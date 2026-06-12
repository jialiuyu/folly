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
#include <folly/io/async/CxlMemPollerGroup.h>

#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>

#include <event2/event.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace folly {
namespace {

using StubDoorbellQueue = CxlMemDoorbellQueue<CxlMemStubHWQueueBackend>;
using StubPoller = CxlMemPoller<CxlMemStubHWQueueBackend>;

struct QueueMemory {
  explicit QueueMemory(size_t count = 1)
      : blocks(count, std::vector<unsigned char>(kCxlMemHWQueueBytes)) {}

  std::vector<CxlMemHWQueueConfig> configs() {
    std::vector<CxlMemHWQueueConfig> result;
    result.reserve(blocks.size());
    for (auto& block : blocks) {
      result.push_back(CxlMemHWQueueConfig{block.data(), block.size()});
    }
    return result;
  }

  std::vector<std::vector<unsigned char>> blocks;
};

void pump(EventBase& eventBase) {
  eventBase.loopOnce(EVLOOP_NONBLOCK);
}

} // namespace

TEST(CxlMemPoller, stopsScanningQueueAfterSchedulingHandoff) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  std::vector<uint64_t> handled;
  auto handle = poller.addQueue(
      &eventBase, &queue, [&](uint64_t item) { handled.push_back(item); });

  ASSERT_TRUE(queue.push(10));
  ASSERT_TRUE(queue.push(11));

  EXPECT_EQ(1, poller.scanOnce());
  EXPECT_EQ(CxlMemPollerQueueState::SCHEDULED, handle->state());
  EXPECT_EQ(0, poller.scanOnce());

  pump(eventBase);
  EXPECT_EQ((std::vector<uint64_t>{10, 11}), handled);
  EXPECT_EQ(CxlMemPollerQueueState::POLLING, handle->state());
}

TEST(CxlMemPoller, eventBaseCallbackProcessesHandoffFirstItem) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  std::vector<uint64_t> handled;
  auto handle = poller.addQueue(
      &eventBase, &queue, [&](uint64_t item) { handled.push_back(item); });

  ASSERT_TRUE(queue.push(99));

  EXPECT_EQ(1, poller.scanOnce());
  EXPECT_TRUE(handled.empty());
  pump(eventBase);

  EXPECT_EQ((std::vector<uint64_t>{99}), handled);
  EXPECT_EQ(CxlMemPollerQueueState::POLLING, handle->state());
}

TEST(CxlMemPoller, returnsQueueAfterDrainSoPollerCanScanAgain) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  std::vector<uint64_t> handled;
  auto handle = poller.addQueue(
      &eventBase, &queue, [&](uint64_t item) { handled.push_back(item); });

  ASSERT_TRUE(queue.push(1));
  EXPECT_EQ(1, poller.scanOnce());
  pump(eventBase);
  EXPECT_EQ(CxlMemPollerQueueState::POLLING, handle->state());

  ASSERT_TRUE(queue.push(2));
  EXPECT_EQ(1, poller.scanOnce());
  pump(eventBase);

  EXPECT_EQ((std::vector<uint64_t>{1, 2}), handled);
}

TEST(CxlMemPoller, drainsOnlyWithinItemBudget) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  CxlMemPollerQueueOptions options;
  options.evbDrainMaxItems = 3;
  std::vector<uint64_t> handled;
  auto handle = poller.addQueue(
      &eventBase,
      &queue,
      [&](uint64_t item) { handled.push_back(item); },
      options);

  for (uint64_t item = 1; item <= 5; ++item) {
    ASSERT_TRUE(queue.push(item));
  }

  EXPECT_EQ(1, poller.scanOnce());
  pump(eventBase);
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), handled);
  EXPECT_EQ(CxlMemPollerQueueState::POLLING, handle->state());

  EXPECT_EQ(1, poller.scanOnce());
  pump(eventBase);
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3, 4, 5}), handled);
}

TEST(CxlMemPoller, closedQueueIsNotScanned) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  std::vector<uint64_t> handled;
  auto handle = poller.addQueue(
      &eventBase, &queue, [&](uint64_t item) { handled.push_back(item); });

  handle->close();
  ASSERT_TRUE(queue.push(1));

  EXPECT_EQ(0, poller.scanOnce());
  EXPECT_TRUE(handled.empty());
  EXPECT_EQ(CxlMemPollerQueueState::CLOSED, handle->state());
}

TEST(CxlMemPoller, adaptiveIdleSleepsAndLocalReturnWakesIt) {
  EventBase eventBase;
  QueueMemory queueMemory;
  StubDoorbellQueue queue(queueMemory.configs());
  StubPoller poller;
  auto handle = poller.addQueue(&eventBase, &queue, [](uint64_t) {});

  EXPECT_EQ(0, poller.scanOnce());
  EXPECT_EQ(0, poller.scanOnce());
  EXPECT_EQ(0, poller.scanOnce());
  EXPECT_TRUE(poller.isIdleSleeping());
  EXPECT_GT(poller.idleSleepCount(), 0);

  poller.notifyReturned();
  EXPECT_FALSE(poller.isIdleSleeping());
  EXPECT_EQ(CxlMemPollerQueueState::POLLING, handle->state());
}

TEST(CxlMemPollerGroup, autoSizesPollersFromIoShards) {
  CxlMemPollerGroupConfig config;
  config.pollerThreads = 0;
  config.ioPerPoller = 4;
  CxlMemPollerGroup<CxlMemStubHWQueueBackend> group(9, config);

  EXPECT_EQ(3, group.pollerCount());
  EXPECT_EQ(0, group.pollerIndexForShard(0));
  EXPECT_EQ(1, group.pollerIndexForShard(4));
  EXPECT_EQ(2, group.pollerIndexForShard(8));
}

} // namespace folly
