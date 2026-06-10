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

#include <folly/io/async/CxlMemHWQueue.h>

#include <folly/portability/GTest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace folly {
namespace {

using StubHWQueue = CxlMemHWQueue<CxlMemStubHWQueueBackend>;

std::vector<unsigned char> makeQueueMemory(size_t size = kCxlMemHWQueueBytes) {
  return std::vector<unsigned char>(size);
}

CxlMemHWQueueConfig makeQueueConfig(std::vector<unsigned char>& memory) {
  return CxlMemHWQueueConfig{memory.data(), memory.size()};
}

} // namespace

TEST(CxlMemHWQueue, rejectsMemorySmallerThanFourKB) {
  auto memory = makeQueueMemory(kCxlMemHWQueueBytes - 1);

  EXPECT_THROW(StubHWQueue(makeQueueConfig(memory)), std::invalid_argument);
}

TEST(CxlMemHWQueue, hasFixedStubCapacityAndReportsFull) {
  auto memory = makeQueueMemory();
  StubHWQueue queue(makeQueueConfig(memory));

  EXPECT_EQ(kCxlMemHWQueueCapacity, queue.capacity());
  for (size_t i = 0; i < queue.capacity(); ++i) {
    EXPECT_TRUE(queue.push(i));
  }
  EXPECT_FALSE(queue.push(queue.capacity()));
}

TEST(CxlMemHWQueue, popEmptyReturnsFalseAndLeavesOutputUnchanged) {
  auto memory = makeQueueMemory();
  StubHWQueue queue(makeQueueConfig(memory));

  uint64_t item = 0xfeedface;
  EXPECT_FALSE(queue.pop(&item));
  EXPECT_EQ(0xfeedface, item);
}

TEST(CxlMemHWQueue, popsInFifoOrder) {
  auto memory = makeQueueMemory();
  StubHWQueue queue(makeQueueConfig(memory));

  ASSERT_TRUE(queue.push(11));
  ASSERT_TRUE(queue.push(22));

  uint64_t item = 0;
  ASSERT_TRUE(queue.pop(&item));
  EXPECT_EQ(11, item);
  ASSERT_TRUE(queue.pop(&item));
  EXPECT_EQ(22, item);
  EXPECT_FALSE(queue.pop(&item));
}

TEST(CxlMemHWQueue, stubBackendSupportsConcurrentPushAndPop) {
  auto memory = makeQueueMemory();
  StubHWQueue queue(makeQueueConfig(memory));

  constexpr size_t kProducerCount = 4;
  constexpr size_t kConsumerCount = 2;
  constexpr size_t kItemsPerProducer = 1000;
  constexpr size_t kTotalItems = kProducerCount * kItemsPerProducer;

  std::atomic<size_t> consumed{0};
  std::atomic<size_t> duplicateItems{0};
  std::atomic<size_t> outOfRangeItems{0};
  std::atomic<bool> timedOut{false};
  std::vector<std::atomic<bool>> seen(kTotalItems);
  for (auto& slot : seen) {
    slot.store(false, std::memory_order_relaxed);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto isTimedOut = [&] {
    if (std::chrono::steady_clock::now() > deadline) {
      timedOut.store(true, std::memory_order_release);
      return true;
    }
    return false;
  };

  std::vector<std::thread> producers;
  producers.reserve(kProducerCount);
  for (size_t producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&, producer] {
      for (size_t i = 0; i < kItemsPerProducer; ++i) {
        const uint64_t item = producer * kItemsPerProducer + i;
        while (!queue.push(item)) {
          if (isTimedOut()) {
            return;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::thread> consumers;
  consumers.reserve(kConsumerCount);
  for (size_t consumer = 0; consumer < kConsumerCount; ++consumer) {
    consumers.emplace_back([&] {
      while (consumed.load(std::memory_order_acquire) < kTotalItems) {
        uint64_t item = 0;
        if (queue.pop(&item)) {
          if (item >= kTotalItems) {
            outOfRangeItems.fetch_add(1, std::memory_order_relaxed);
          } else if (seen[item].exchange(true, std::memory_order_acq_rel)) {
            duplicateItems.fetch_add(1, std::memory_order_relaxed);
          }
          consumed.fetch_add(1, std::memory_order_release);
          continue;
        }
        if (isTimedOut()) {
          return;
        }
        std::this_thread::yield();
      }
    });
  }

  for (auto& producer : producers) {
    producer.join();
  }
  for (auto& consumer : consumers) {
    consumer.join();
  }

  ASSERT_FALSE(timedOut.load(std::memory_order_acquire));
  EXPECT_EQ(kTotalItems, consumed.load(std::memory_order_acquire));
  EXPECT_EQ(0, duplicateItems.load(std::memory_order_relaxed));
  EXPECT_EQ(0, outOfRangeItems.load(std::memory_order_relaxed));
  for (size_t i = 0; i < kTotalItems; ++i) {
    EXPECT_TRUE(seen[i].load(std::memory_order_acquire)) << i;
  }
}

} // namespace folly
