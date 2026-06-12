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

#include <folly/io/async/CxlMemPoller.h>

#include <folly/io/async/EventBase.h>

#include <algorithm>
#include <stdexcept>

namespace folly {

template <typename Backend>
CxlMemPollerQueueHandle<Backend>::CxlMemPollerQueueHandle(
    std::shared_ptr<detail::CxlMemPollerQueueEntry<Backend>> entry)
    : entry_(std::move(entry)) {}

template <typename Backend>
CxlMemPollerQueueState CxlMemPollerQueueHandle<Backend>::state() const {
  std::lock_guard<std::mutex> guard(entry_->mutex);
  return entry_->state;
}

template <typename Backend>
void CxlMemPollerQueueHandle<Backend>::close() {
  std::lock_guard<std::mutex> guard(entry_->mutex);
  entry_->state = CxlMemPollerQueueState::CLOSED;
}

template <typename Backend>
CxlMemPoller<Backend>::CxlMemPoller(CxlMemPollerOptions options)
    : options_(options) {}

template <typename Backend>
std::shared_ptr<typename CxlMemPoller<Backend>::QueueHandle>
CxlMemPoller<Backend>::addQueue(
    EventBase* eventBase,
    CxlMemDoorbellQueue<Backend>* queue,
    ItemHandler handler,
    CxlMemPollerQueueOptions options) {
  if (eventBase == nullptr || queue == nullptr || !handler) {
    throw std::invalid_argument("CxlMemPoller requires queue and handler");
  }
  if (options.evbDrainMaxItems == 0) {
    throw std::invalid_argument("CxlMemPoller requires a non-zero item budget");
  }

  auto entry = std::make_shared<QueueEntry>();
  entry->eventBase = eventBase;
  entry->queue = queue;
  entry->handler = std::move(handler);
  entry->options = options;
  entries_.push_back(entry);
  return std::shared_ptr<QueueHandle>(new QueueHandle(std::move(entry)));
}

template <typename Backend>
size_t CxlMemPoller<Backend>::scanOnce() {
  size_t scheduled = 0;
  for (const auto& entry : entries_) {
    uint64_t item = 0;
    {
      std::lock_guard<std::mutex> guard(entry->mutex);
      if (entry->state != CxlMemPollerQueueState::POLLING) {
        continue;
      }
      if (!entry->queue->pop(&item)) {
        continue;
      }
      entry->state = CxlMemPollerQueueState::SCHEDULED;
    }
    scheduleHandoff(entry, item);
    ++scheduled;
  }

  if (scheduled > 0) {
    resetIdleState();
  } else {
    observeEmptyScan();
  }
  return scheduled;
}

template <typename Backend>
void CxlMemPoller<Backend>::notifyReturned() {
  resetIdleState();
}

template <typename Backend>
bool CxlMemPoller<Backend>::isIdleSleeping() const {
  std::lock_guard<std::mutex> guard(idleMutex_);
  return idleSleeping_;
}

template <typename Backend>
uint64_t CxlMemPoller<Backend>::idleSleepCount() const {
  std::lock_guard<std::mutex> guard(idleMutex_);
  return idleSleepCount_;
}

template <typename Backend>
void CxlMemPoller<Backend>::scheduleHandoff(
    std::shared_ptr<QueueEntry> entry,
    uint64_t item) {
  entry->eventBase->runInEventBaseThreadAlwaysEnqueue(
      [this, entry = std::move(entry), item]() noexcept {
        try {
          drainInEventBase(entry, item);
        } catch (...) {
          std::lock_guard<std::mutex> guard(entry->mutex);
          entry->state = CxlMemPollerQueueState::CLOSED;
        }
      });
}

template <typename Backend>
void CxlMemPoller<Backend>::drainInEventBase(
    std::shared_ptr<QueueEntry> entry,
    uint64_t firstItem) {
  {
    std::lock_guard<std::mutex> guard(entry->mutex);
    if (entry->state == CxlMemPollerQueueState::CLOSED) {
      return;
    }
    entry->state = CxlMemPollerQueueState::EVB_OWNED;
  }

  const auto start = std::chrono::steady_clock::now();
  size_t handled = 0;
  bool hasItem = true;
  uint64_t item = firstItem;

  while (true) {
    {
      std::lock_guard<std::mutex> guard(entry->mutex);
      if (entry->state == CxlMemPollerQueueState::CLOSED) {
        return;
      }
    }

    if (!hasItem) {
      if (!entry->queue->pop(&item)) {
        break;
      }
      hasItem = true;
    }

    entry->handler(item);
    hasItem = false;
    ++handled;

    if (handled >= entry->options.evbDrainMaxItems) {
      break;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed >= entry->options.evbDrainMaxDuration) {
      break;
    }
  }

  returnQueue(std::move(entry));
}

template <typename Backend>
void CxlMemPoller<Backend>::returnQueue(std::shared_ptr<QueueEntry> entry) {
  {
    std::lock_guard<std::mutex> guard(entry->mutex);
    if (entry->state == CxlMemPollerQueueState::CLOSED) {
      return;
    }
    entry->state = CxlMemPollerQueueState::RETURNING;
  }
  {
    std::lock_guard<std::mutex> guard(entry->mutex);
    if (entry->state == CxlMemPollerQueueState::RETURNING) {
      entry->state = CxlMemPollerQueueState::POLLING;
    }
  }
  notifyReturned();
}

template <typename Backend>
void CxlMemPoller<Backend>::resetIdleState() {
  std::lock_guard<std::mutex> guard(idleMutex_);
  emptyScans_ = 0;
  idleSleeping_ = false;
}

template <typename Backend>
void CxlMemPoller<Backend>::observeEmptyScan() {
  std::lock_guard<std::mutex> guard(idleMutex_);
  ++emptyScans_;
  if (options_.idleProfile == CxlMemPollerIdleProfile::ADAPTIVE &&
      emptyScans_ >= options_.adaptiveSleepAfterEmptyScans) {
    if (!idleSleeping_) {
      ++idleSleepCount_;
    }
    idleSleeping_ = true;
  }
}

template class CxlMemPollerQueueHandle<CxlMemStubHWQueueBackend>;
template class CxlMemPollerQueueHandle<CxlMemRealHWQueueBackend>;
template class CxlMemPoller<CxlMemStubHWQueueBackend>;
template class CxlMemPoller<CxlMemRealHWQueueBackend>;

} // namespace folly
