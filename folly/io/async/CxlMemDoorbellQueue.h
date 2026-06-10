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

#include <folly/io/async/CxlMemHWQueue.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace folly {

template <typename Backend>
class CxlMemDoorbellQueue {
 public:
  explicit CxlMemDoorbellQueue(std::vector<CxlMemHWQueueConfig> configs) {
    if (configs.empty()) {
      throw std::invalid_argument(
          "CxlMemDoorbellQueue requires at least one HWQueue");
    }
    queues_.reserve(configs.size());
    for (auto config : configs) {
      queues_.push_back(std::make_unique<CxlMemHWQueue<Backend>>(config));
      capacity_ += queues_.back()->capacity();
    }
  }

  CxlMemDoorbellQueue(std::initializer_list<CxlMemHWQueueConfig> configs)
      : CxlMemDoorbellQueue(std::vector<CxlMemHWQueueConfig>(configs)) {}

  bool push(uint64_t item) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (size_t i = 0; i < queues_.size(); ++i) {
      const size_t index = (nextPushQueue_ + i) % queues_.size();
      if (queues_[index]->push(item)) {
        nextPushQueue_ = (index + 1) % queues_.size();
        return true;
      }
    }
    return false;
  }

  bool pop(uint64_t* item) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& queue : queues_) {
      if (queue->pop(item)) {
        return true;
      }
    }
    return false;
  }

  size_t capacity() const { return capacity_; }

 private:
  std::vector<std::unique_ptr<CxlMemHWQueue<Backend>>> queues_;
  size_t capacity_{0};
  size_t nextPushQueue_{0};
  mutable std::mutex mutex_;
};

extern template class CxlMemDoorbellQueue<CxlMemStubHWQueueBackend>;
extern template class CxlMemDoorbellQueue<CxlMemRealHWQueueBackend>;

} // namespace folly
