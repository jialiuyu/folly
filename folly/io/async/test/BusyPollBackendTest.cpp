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

#include <atomic>
#include <thread>

#include <folly/io/async/Epoll.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace {

EventBase makeBusyPollEventBase() {
  EventBase::Options options;
  options.setBackendFactory(
      [] { return std::make_unique<BusyPollBackend>(); });
  options.setNotificationQueueMode(
      EventBase::NotificationQueueMode::ManualPoll);
  return EventBase{std::move(options)};
}

} // namespace

TEST(BusyPollBackendTest, defaultEventBaseKeepsPollableBackend) {
  EventBase evb;
#if FOLLY_HAS_EPOLL
  EXPECT_NE(-1, evb.getBackend()->getPollableFd());
#else
  EXPECT_NE(nullptr, evb.getBackend());
#endif
}

TEST(BusyPollBackendTest, busyPollBackendHasNoPollableFd) {
  auto evb = makeBusyPollEventBase();
  EXPECT_EQ(-1, evb.getBackend()->getPollableFd());
}

TEST(BusyPollBackendTest, manualNotificationQueueRequiresExplicitPoll) {
  auto evb = makeBusyPollEventBase();
  std::atomic<bool> ran{false};

  std::thread producer([&] {
    evb.runInEventBaseThread([&] { ran.store(true, std::memory_order_release); });
  });
  producer.join();

  EXPECT_GT(evb.getNotificationQueueSize(), 0);
  EXPECT_FALSE(ran.load(std::memory_order_acquire));

  evb.loopPollSetup();
  EXPECT_TRUE(evb.pollNotificationQueue());
  evb.loopPollCleanup();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_EQ(0, evb.getNotificationQueueSize());
}

} // namespace folly
