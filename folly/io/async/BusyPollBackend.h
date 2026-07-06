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

#include <chrono>
#include <vector>

#include <folly/io/async/EventBaseBackendBase.h>

namespace folly {

class BusyPollBackend : public EventBaseBackendBase {
 public:
  BusyPollBackend() = default;
  ~BusyPollBackend() override = default;

  event_base* getEventBase() override { return nullptr; }

  int eb_event_base_loop(int flags) override;
  int eb_event_base_loopbreak() override;

  int eb_event_add(Event& event, const struct timeval* timeout) override;
  int eb_event_del(Event& event) override;

  bool eb_event_active(Event& event, int res) override;

 private:
  struct EventInfo;

  EventInfo* getOrCreateInfo(Event& event);
  int addTimerEvent(Event& event, const struct timeval* timeout);
  int removeTimerEvent(Event& event);
  bool processExpiredTimers();
  EventInfo* nextExpiredTimer(std::chrono::steady_clock::time_point now);
  bool hasPendingTimers() const;

  bool loopBreak_{false};
  std::vector<EventInfo*> timers_;
};

} // namespace folly
