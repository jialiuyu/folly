/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace folly {

inline constexpr size_t kCxlMemHWQueueBytes = 4096;
inline constexpr size_t kCxlMemHWQueueCapacity = 496;

struct CxlMemHWQueueConfig {
  void* memory{nullptr};
  size_t length{0};
  size_t capacity{kCxlMemHWQueueCapacity};
};

struct CxlMemStubHWQueueBackend {
  static constexpr bool kThreadSafePush = false;
  static constexpr bool kThreadSafePop = false;
};

struct CxlMemRealHWQueueBackend {
  static constexpr bool kThreadSafePush = true;
  static constexpr bool kThreadSafePop = true;
};

namespace detail {

class CxlMemStubHWQueueStorage {
 public:
  explicit CxlMemStubHWQueueStorage(CxlMemHWQueueConfig config);

  bool push(uint64_t item);
  bool pop(uint64_t* item);
  size_t capacity() const;

 private:
  unsigned char* memory_{nullptr};
  size_t capacity_{kCxlMemHWQueueCapacity};
};

class CxlMemRealHWQueueStorage {
 public:
  explicit CxlMemRealHWQueueStorage(CxlMemHWQueueConfig config);

  bool push(uint64_t item);
  bool pop(uint64_t* item);
  size_t capacity() const;

 private:
  size_t capacity_{kCxlMemHWQueueCapacity};
};

template <typename Backend>
struct CxlMemHWQueueStorageFor;

template <>
struct CxlMemHWQueueStorageFor<CxlMemStubHWQueueBackend> {
  using type = CxlMemStubHWQueueStorage;
};

template <>
struct CxlMemHWQueueStorageFor<CxlMemRealHWQueueBackend> {
  using type = CxlMemRealHWQueueStorage;
};

} // namespace detail

template <typename Backend>
class CxlMemHWQueue {
 public:
  explicit CxlMemHWQueue(CxlMemHWQueueConfig config) : storage_(config) {}

  bool push(uint64_t item) {
    if constexpr (Backend::kThreadSafePush && Backend::kThreadSafePop) {
      return storage_.push(item);
    } else {
      std::lock_guard<std::mutex> guard(mutex_);
      return storage_.push(item);
    }
  }

  bool pop(uint64_t* item) {
    if constexpr (Backend::kThreadSafePush && Backend::kThreadSafePop) {
      return storage_.pop(item);
    } else {
      std::lock_guard<std::mutex> guard(mutex_);
      return storage_.pop(item);
    }
  }

  size_t capacity() const { return storage_.capacity(); }

 private:
  using Storage = typename detail::CxlMemHWQueueStorageFor<Backend>::type;

  Storage storage_;
  mutable std::mutex mutex_;
};

extern template class CxlMemHWQueue<CxlMemStubHWQueueBackend>;
extern template class CxlMemHWQueue<CxlMemRealHWQueueBackend>;

} // namespace folly
