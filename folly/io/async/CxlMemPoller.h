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

#include <folly/io/async/CxlMemDoorbellQueue.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace folly {

class EventBase;

enum class CxlMemPollerQueueState : uint8_t {
  POLLING,
  SCHEDULED,
  EVB_OWNED,
  RETURNING,
  CLOSED,
};

enum class CxlMemPollerIdleProfile : uint8_t {
  ACTIVE,
  ADAPTIVE,
};

struct CxlMemPollerQueueOptions {
  size_t evbDrainMaxItems{64};
  std::chrono::microseconds evbDrainMaxDuration{50};
};

struct CxlMemPollerOptions {
  CxlMemPollerIdleProfile idleProfile{CxlMemPollerIdleProfile::ADAPTIVE};
  size_t adaptiveSleepAfterEmptyScans{3};
};

template <typename Backend>
class CxlMemPoller;

namespace detail {

template <typename Backend>
struct CxlMemPollerQueueEntry {
  EventBase* eventBase{nullptr};
  CxlMemDoorbellQueue<Backend>* queue{nullptr};
  std::function<void(uint64_t)> handler;
  CxlMemPollerQueueOptions options;
  mutable std::mutex mutex;
  CxlMemPollerQueueState state{CxlMemPollerQueueState::POLLING};
};

} // namespace detail

template <typename Backend>
class CxlMemPollerQueueHandle {
 public:
  CxlMemPollerQueueState state() const;
  void close();

 private:
  friend class CxlMemPoller<Backend>;

  explicit CxlMemPollerQueueHandle(
      std::shared_ptr<detail::CxlMemPollerQueueEntry<Backend>> entry);

  std::shared_ptr<detail::CxlMemPollerQueueEntry<Backend>> entry_;
};

template <typename Backend>
class CxlMemPoller {
 public:
  using ItemHandler = std::function<void(uint64_t)>;
  using QueueHandle = CxlMemPollerQueueHandle<Backend>;

  explicit CxlMemPoller(CxlMemPollerOptions options = {});

  std::shared_ptr<QueueHandle> addQueue(
      EventBase* eventBase,
      CxlMemDoorbellQueue<Backend>* queue,
      ItemHandler handler,
      CxlMemPollerQueueOptions options = {});

  size_t scanOnce();
  void notifyReturned();
  bool isIdleSleeping() const;
  uint64_t idleSleepCount() const;

 private:
  using QueueEntry = detail::CxlMemPollerQueueEntry<Backend>;

  void scheduleHandoff(std::shared_ptr<QueueEntry> entry, uint64_t item);
  void drainInEventBase(std::shared_ptr<QueueEntry> entry, uint64_t firstItem);
  void returnQueue(std::shared_ptr<QueueEntry> entry);
  void resetIdleState();
  void observeEmptyScan();

  CxlMemPollerOptions options_;
  std::vector<std::shared_ptr<QueueEntry>> entries_;
  mutable std::mutex idleMutex_;
  size_t emptyScans_{0};
  uint64_t idleSleepCount_{0};
  bool idleSleeping_{false};
};

extern template class CxlMemPollerQueueHandle<CxlMemStubHWQueueBackend>;
extern template class CxlMemPollerQueueHandle<CxlMemRealHWQueueBackend>;
extern template class CxlMemPoller<CxlMemStubHWQueueBackend>;
extern template class CxlMemPoller<CxlMemRealHWQueueBackend>;

} // namespace folly
