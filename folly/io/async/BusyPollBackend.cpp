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

#include <folly/io/async/BusyPollBackend.h>

#include <algorithm>
#include <cerrno>
#include <thread>

#include <folly/io/async/EventUtil.h>
#include <folly/portability/Event.h>

namespace folly {

struct BusyPollBackend::EventInfo {
  static void freeFunction(void* v) {
    delete static_cast<EventInfo*>(v);
  }

  struct event* ev{nullptr};
  std::chrono::steady_clock::time_point expiration;
  bool timerScheduled{false};
};

int BusyPollBackend::eb_event_base_loop(int flags) {
  const bool waitForEvents = (flags & EVLOOP_NONBLOCK) == 0;
  while (true) {
    if (loopBreak_) {
      loopBreak_ = false;
      return 0;
    }

    if (processExpiredTimers()) {
      return 0;
    }

    if (!hasPendingTimers()) {
      return 1;
    }

    if (!waitForEvents) {
      return 0;
    }

    std::this_thread::yield();
  }
}

int BusyPollBackend::eb_event_base_loopbreak() {
  loopBreak_ = true;
  return 0;
}

int BusyPollBackend::eb_event_add(Event& event, const struct timeval* timeout) {
  auto* ev = event.getEvent();
  CHECK(ev != nullptr);
  CHECK(!(event_ref_flags(ev) & ~EVLIST_ALL));

  if (timeout != nullptr || (ev->ev_events & EV_TIMEOUT)) {
    return addTimerEvent(event, timeout);
  }

  errno = ENOTSUP;
  return -1;
}

int BusyPollBackend::eb_event_del(Event& event) {
  if (!event.eb_ev_base()) {
    errno = EINVAL;
    return -1;
  }

  auto* ev = event.getEvent();
  if (event_ref_flags(ev) & EVLIST_TIMEOUT) {
    return removeTimerEvent(event);
  }

  if (event_ref_flags(ev) & EVLIST_ACTIVE) {
    event_ref_flags(ev) &= ~EVLIST_ACTIVE;
    return 0;
  }

  errno = EINVAL;
  return -1;
}

bool BusyPollBackend::eb_event_active(Event& event, int res) {
  auto* ev = event.getEvent();
  if (ev == nullptr) {
    return false;
  }

  event_ref_flags(ev) |= EVLIST_ACTIVE;
  ev->ev_res = static_cast<short>(res);
  (*event_ref_callback(ev))(
      static_cast<int>(ev->ev_fd), ev->ev_res, event_ref_arg(ev));
  event_ref_flags(ev) &= ~EVLIST_ACTIVE;
  return true;
}

BusyPollBackend::EventInfo* BusyPollBackend::getOrCreateInfo(Event& event) {
  auto* info = static_cast<EventInfo*>(event.getUserData());
  if (info == nullptr) {
    info = new EventInfo();
    event.setUserData(info, EventInfo::freeFunction);
  } else {
    CHECK_EQ(event.getFreeFunction(), EventInfo::freeFunction);
  }
  info->ev = event.getEvent();
  return info;
}

int BusyPollBackend::addTimerEvent(
    Event& event, const struct timeval* timeout) {
  if (timeout == nullptr) {
    static const struct timeval immediate{0, 0};
    timeout = &immediate;
  }

  auto* ev = event.getEvent();
  auto* info = getOrCreateInfo(event);

  info->expiration = std::chrono::steady_clock::now() +
      std::chrono::seconds{timeout->tv_sec} +
      std::chrono::microseconds{timeout->tv_usec};

  if (!info->timerScheduled) {
    timers_.push_back(info);
    info->timerScheduled = true;
  }

  event_ref_flags(ev) |= EVLIST_TIMEOUT;
  return 0;
}

int BusyPollBackend::removeTimerEvent(Event& event) {
  auto* ev = event.getEvent();
  auto* info = static_cast<EventInfo*>(event.getUserData());
  if (info == nullptr || !info->timerScheduled) {
    errno = EINVAL;
    return -1;
  }

  timers_.erase(std::remove(timers_.begin(), timers_.end(), info), timers_.end());
  info->timerScheduled = false;
  event_ref_flags(ev) &= ~EVLIST_TIMEOUT;
  return 0;
}

bool BusyPollBackend::processExpiredTimers() {
  auto now = std::chrono::steady_clock::now();
  auto* info = nextExpiredTimer(now);
  if (info == nullptr) {
    return false;
  }

  timers_.erase(std::remove(timers_.begin(), timers_.end(), info), timers_.end());
  info->timerScheduled = false;

  auto* ev = info->ev;
  ev->ev_res = EV_TIMEOUT;
  auto internal = event_ref_flags(ev) & EVLIST_INTERNAL;
  event_ref_flags(ev).get() = internal ? EVLIST_INTERNAL : EVLIST_INIT;
  (*event_ref_callback(ev))(
      static_cast<int>(ev->ev_fd), ev->ev_res, event_ref_arg(ev));
  return true;
}

BusyPollBackend::EventInfo* BusyPollBackend::nextExpiredTimer(
    std::chrono::steady_clock::time_point now) {
  EventInfo* earliest = nullptr;
  for (auto* info : timers_) {
    if (info == nullptr || !info->timerScheduled) {
      continue;
    }
    if (info->expiration > now) {
      continue;
    }
    if (earliest == nullptr || info->expiration < earliest->expiration) {
      earliest = info;
    }
  }
  return earliest;
}

bool BusyPollBackend::hasPendingTimers() const {
  return std::any_of(timers_.begin(), timers_.end(), [](auto* info) {
    return info != nullptr && info->timerScheduled;
  });
}

} // namespace folly
