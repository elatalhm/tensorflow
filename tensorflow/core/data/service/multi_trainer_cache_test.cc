/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/data/service/multi_trainer_cache.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/random.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/status_matchers.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"

namespace tensorflow {
namespace data {
namespace {

using ::tensorflow::testing::IsOkAndHolds;
using ::tensorflow::testing::StatusIs;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::Pointee;
using ::testing::UnorderedElementsAreArray;

class InfiniteRange {
 public:
  StatusOr<int64_t> operator()() { return next_++; }

 private:
  // No need to guard this variable because only one thread can write the cache.
  int64_t next_ = 0;
};

class TensorDataset {
 public:
  StatusOr<Tensor> operator()() const { return Tensor("Test Tensor"); }
};

class SlowDataset {
 public:
  explicit SlowDataset(absl::Duration delay) : delay_(delay) {}

  StatusOr<Tensor> operator()() {
    Env::Default()->SleepForMicroseconds(absl::ToInt64Microseconds(delay_));
    return Tensor("Test Tensor");
  }

 private:
  absl::Duration delay_;
};

template <class T>
class ElementOrErrorDataset {
 public:
  explicit ElementOrErrorDataset(const std::vector<StatusOr<T>>& elements)
      : elements_(elements) {}

  StatusOr<T> operator()() {
    if (next_ >= elements_.size()) {
      return errors::OutOfRange("Out of range.");
    }

    return elements_[next_++];
  }

 private:
  const std::vector<StatusOr<T>> elements_;
  int64_t next_ = 0;
};

template <class T>
struct SizeofFn {
  size_t operator()(const T& x) const { return sizeof(x); }
};

template <>
struct SizeofFn<Tensor> {
  size_t operator()(const Tensor& x) const { return x.TotalBytes(); }
};

std::vector<int64_t> GetRange(const size_t range) {
  std::vector<int64_t> result;
  for (int64_t i = 0; i < range; ++i) {
    result.push_back(i);
  }
  return result;
}

bool SequenceIsIncreasing(const std::vector<int64_t> sequence) {
  for (int i = 1; i < sequence.size(); ++i) {
    if (sequence[i - 1] > sequence[i - 1]) {
      return false;
    }
  }
  return true;
}

TEST(MultiTrainerCacheTest, GetFromOneTrainer) {
  const size_t num_elements = 10;
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/1024, InfiniteRange(), SizeofFn<int64_t>());
  for (size_t i = 0; i < num_elements; ++i) {
    EXPECT_THAT(cache.Get("Trainer ID"), IsOkAndHolds(Pointee(i)));
  }
}

TEST(MultiTrainerCacheTest, GetFromMultipleTrainers) {
  const size_t num_elements = 10;
  const size_t num_trainers = 10;

  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/1024, InfiniteRange(), SizeofFn<int64_t>());
  for (size_t i = 0; i < num_elements; ++i) {
    // All the readers get the same element in one step.
    for (size_t j = 0; j < num_trainers; ++j) {
      const std::string trainer_id = absl::StrCat("Trainer ", j);
      EXPECT_THAT(cache.Get(trainer_id), IsOkAndHolds(Pointee(i)));
    }
  }
}

TEST(MultiTrainerCacheTest, SlowTrainersSkipData) {
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/5 * sizeof(int64_t), InfiniteRange(),
      SizeofFn<int64_t>());
  EXPECT_THAT(cache.Get("Fast trainer 1"), IsOkAndHolds(Pointee(0)));
  EXPECT_THAT(cache.Get("Fast trainer 2"), IsOkAndHolds(Pointee(0)));
  EXPECT_THAT(cache.Get("Slow trainer 1"), IsOkAndHolds(Pointee(0)));
  EXPECT_THAT(cache.Get("Slow trainer 2"), IsOkAndHolds(Pointee(0)));

  for (int i = 1; i < 20; ++i) {
    EXPECT_THAT(cache.Get("Fast trainer 1"), IsOkAndHolds(Pointee(i)));
    EXPECT_THAT(cache.Get("Fast trainer 2"), IsOkAndHolds(Pointee(i)));
  }

  // When 19 is cached, 14 must have been discarded.
  EXPECT_THAT(cache.Get("Slow trainer 1"), IsOkAndHolds(Pointee(Gt(14))));
  EXPECT_THAT(cache.Get("Slow trainer 2"), IsOkAndHolds(Pointee(Gt(14))));

  for (int i = 20; i < 100; ++i) {
    EXPECT_THAT(cache.Get("Fast trainer 1"), IsOkAndHolds(Pointee(i)));
    EXPECT_THAT(cache.Get("Fast trainer 2"), IsOkAndHolds(Pointee(i)));
  }

  // When 99 is cached, 94 must have been discarded.
  EXPECT_THAT(cache.Get("Slow trainer 1"), IsOkAndHolds(Pointee(Gt(94))));
  EXPECT_THAT(cache.Get("Slow trainer 2"), IsOkAndHolds(Pointee(Gt(94))));
}

TEST(MultiTrainerCacheTest, NewTrainersStartLate) {
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/5 * sizeof(int64_t), InfiniteRange(),
      SizeofFn<int64_t>());
  for (int i = 0; i < 100; ++i) {
    EXPECT_THAT(cache.Get("Old trainer"), IsOkAndHolds(Pointee(i)));
  }

  // New trainers start to read after the first trainer has finished.
  for (int j = 0; j < 100; ++j) {
    EXPECT_THAT(cache.Get(absl::StrCat("New trainer ", j)),
                IsOkAndHolds(Pointee(Gt(94))));
  }
}

TEST(MultiTrainerCacheTest, AlternateTrainerExtendsCache) {
  // The cache size is smaller than one int64_t.
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/sizeof(int64_t), InfiniteRange(),
      SizeofFn<int64_t>());
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(0)));
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(1)));
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(2)));

  // When 2 is cached, 0 must have been discarded.
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(0))));
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(1))));
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(2))));

  // When 3 is cached, 1 must have been discarded.
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(Gt(1))));
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(Gt(2))));
  EXPECT_THAT(cache.Get("Trainer 1"), IsOkAndHolds(Pointee(Gt(3))));

  // When 4 is cached, 2 must have been discarded.
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(2))));
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(3))));
  EXPECT_THAT(cache.Get("Trainer 2"), IsOkAndHolds(Pointee(Gt(4))));

  // When 5 is cached, 3 must have been discarded.
  EXPECT_THAT(cache.Get("Trainer 3"), IsOkAndHolds(Pointee(Gt(3))));
  EXPECT_THAT(cache.Get("Trainer 3"), IsOkAndHolds(Pointee(Gt(4))));
  EXPECT_THAT(cache.Get("Trainer 3"), IsOkAndHolds(Pointee(Gt(5))));
}

TEST(MultiTrainerCacheTest, ConcurrentReaders) {
  size_t num_trainers = 10;
  size_t num_elements_to_read = 200;
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/3 * sizeof(int64_t), InfiniteRange(),
      SizeofFn<int64_t>());

  std::vector<std::vector<int64_t>> results;
  std::vector<std::unique_ptr<Thread>> reader_threads;
  results.reserve(num_trainers);
  for (size_t i = 0; i < num_trainers; ++i) {
    results.emplace_back();
    std::vector<int64_t>& result = results.back();
    reader_threads.push_back(absl::WrapUnique(Env::Default()->StartThread(
        /*thread_options=*/{}, /*name=*/absl::StrCat("Trainer_", i),
        [&cache, num_elements_to_read, &result]() {
          for (size_t i = 0; i < num_elements_to_read; ++i) {
            // Randomly slows down some trainers.
            if (random::New64() % 5 == 0) {
              Env::Default()->SleepForMicroseconds(2000);
            }
            TF_ASSERT_OK_AND_ASSIGN(std::shared_ptr<const int64_t> next,
                                    cache.Get(absl::StrCat("Trainer_", i)));
            result.push_back(*next);
          }
        })));
  }

  for (auto& thread : reader_threads) {
    thread.reset();
  }

  // Verifies all trainers can read `num_elements_to_read` elements.
  EXPECT_EQ(results.size(), num_trainers);
  for (const std::vector<int64_t>& result : results) {
    EXPECT_EQ(result.size(), num_elements_to_read);
    EXPECT_TRUE(SequenceIsIncreasing(result));
  }
}

TEST(MultiTrainerCacheTest, ConcurrentReadersFromOneTrainer) {
  size_t num_trainers = 10;
  size_t num_elements_to_read = 100;
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/3 * sizeof(int64_t), InfiniteRange(),
      SizeofFn<int64_t>());

  mutex mu;
  std::vector<int64_t> results;  // Guarded by `mu`.
  std::vector<std::unique_ptr<Thread>> reader_threads;
  for (size_t i = 0; i < num_trainers; ++i) {
    reader_threads.push_back(absl::WrapUnique(Env::Default()->StartThread(
        /*thread_options=*/{}, /*name=*/absl::StrCat("Thread_", i),
        [&cache, num_elements_to_read, &results, &mu]() {
          for (size_t i = 0; i < num_elements_to_read; ++i) {
            // Randomly slows down some trainers.
            if (random::New64() % 5 == 0) {
              Env::Default()->SleepForMicroseconds(1000);
            }
            TF_ASSERT_OK_AND_ASSIGN(std::shared_ptr<const int64_t> next,
                                    cache.Get("Trainer ID"));
            mutex_lock l(mu);
            results.push_back(*next);
          }
        })));
  }

  for (auto& thread : reader_threads) {
    thread.reset();
  }
  // Verifies the readers have read all elements because they have the same
  // trainer ID.
  EXPECT_THAT(results, UnorderedElementsAreArray(GetRange(1000)));
}

TEST(MultiTrainerCacheTest, Cancel) {
  size_t num_trainers = 10;
  MultiTrainerCache<Tensor> cache(
      /*max_cache_size_bytes=*/1000, TensorDataset(), SizeofFn<Tensor>());
  EXPECT_FALSE(cache.IsCancelled());

  mutex mu;
  Status status;  // Guarded by `mu`.
  std::vector<std::unique_ptr<Thread>> reader_threads;
  for (size_t i = 0; i < num_trainers; ++i) {
    reader_threads.push_back(absl::WrapUnique(Env::Default()->StartThread(
        /*thread_options=*/{}, /*name=*/absl::StrCat("Trainer_", i),
        [&cache, &status, &mu]() {
          for (int j = 0; true; ++j) {
            StatusOr<std::shared_ptr<const Tensor>> tensor =
                cache.Get(absl::StrCat("Trainer_", j % 1000));
            {
              mutex_lock l(mu);
              status = tensor.status();
            }
            if (!tensor.status().ok()) {
              return;
            }
            test::ExpectEqual(*tensor.ValueOrDie(), Tensor("Test Tensor"));
          }
        })));
  }

  Env::Default()->SleepForMicroseconds(1000000);
  cache.Cancel(errors::Cancelled("Cancelled"));
  for (auto& thread : reader_threads) {
    thread.reset();
  }

  mutex_lock l(mu);
  EXPECT_THAT(status, StatusIs(error::CANCELLED));
  EXPECT_THAT(cache.Get("New trainer"), StatusIs(error::CANCELLED));
  EXPECT_TRUE(cache.IsCancelled());
}

TEST(MultiTrainerCacheTest, Errors) {
  ElementOrErrorDataset<std::string> dataset(std::vector<StatusOr<std::string>>{
      std::string("First element"),
      errors::Cancelled("Cancelled"),
      std::string("Second element"),
      errors::InvalidArgument("InvalidArgument"),
      std::string("Third element"),
      errors::Unavailable("Unavailable"),
  });
  MultiTrainerCache<std::string> cache(
      /*max_cache_size_bytes=*/1000, dataset,
      [](const std::string& s) { return s.capacity(); });

  EXPECT_THAT(cache.Get("Trainer ID"),
              IsOkAndHolds(Pointee(std::string("First element"))));
  EXPECT_THAT(cache.Get("Trainer ID"), StatusIs(error::CANCELLED));
  EXPECT_THAT(cache.Get("Trainer ID"),
              IsOkAndHolds(Pointee(std::string("Second element"))));
  EXPECT_THAT(cache.Get("Trainer ID"), StatusIs(error::INVALID_ARGUMENT));
  EXPECT_THAT(cache.Get("Trainer ID"),
              IsOkAndHolds(Pointee(std::string("Third element"))));
  EXPECT_THAT(cache.Get("Trainer ID"), StatusIs(error::UNAVAILABLE));

  // Errors are not stored in the cache.
  EXPECT_THAT(cache.Get("New Trainer"),
              IsOkAndHolds(Pointee(std::string("First element"))));
  EXPECT_THAT(cache.Get("New Trainer"),
              IsOkAndHolds(Pointee(std::string("Second element"))));
  EXPECT_THAT(cache.Get("New Trainer"),
              IsOkAndHolds(Pointee(std::string("Third element"))));
}

TEST(MultiTrainerCacheTest, CacheSizeIsTooSmall) {
  // The cache size is smaller than one int64_t.
  MultiTrainerCache<int64_t> cache(
      /*max_cache_size_bytes=*/1, InfiniteRange(), SizeofFn<int64_t>());
  EXPECT_THAT(cache.Get("Trainer ID"),
              StatusIs(error::INVALID_ARGUMENT,
                       HasSubstr("tf.data service element size is larger than "
                                 "cache size in bytes.")));
}

TEST(MultiTrainerCacheTest, TrainerIDMustBeNonEmpty) {
  MultiTrainerCache<Tensor> cache(
      /*max_cache_size_bytes=*/1000, TensorDataset(), SizeofFn<Tensor>());
  EXPECT_THAT(
      cache.Get(""),
      StatusIs(
          error::INTERNAL,
          "tf.data service multi-trainer cache trainer ID must be non-empty."));
}

}  // namespace
}  // namespace data
}  // namespace tensorflow
