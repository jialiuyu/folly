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

#include <folly/io/async/CxlMemDoorbellQueue.h>

#include <folly/portability/GTest.h>

#include <cstdint>
#include <vector>

namespace folly {
namespace {

using StubDoorbellQueue = CxlMemDoorbellQueue<CxlMemStubHWQueueBackend>;

struct DoorbellMemory {
  DoorbellMemory()
      : first(kCxlMemHWQueueBytes), second(kCxlMemHWQueueBytes) {}

  std::vector<unsigned char> first;
  std::vector<unsigned char> second;

  std::vector<CxlMemHWQueueConfig> configs() {
    return {
        CxlMemHWQueueConfig{first.data(), first.size()},
        CxlMemHWQueueConfig{second.data(), second.size()},
    };
  }
};

} // namespace

TEST(CxlMemDoorbellQueue, sumsUnderlyingQueueCapacity) {
  DoorbellMemory memory;
  StubDoorbellQueue queue(memory.configs());

  EXPECT_EQ(2 * kCxlMemHWQueueCapacity, queue.capacity());
}

TEST(CxlMemDoorbellQueue, drainsLowerQueuesBeforeLaterQueues) {
  DoorbellMemory memory;
  StubDoorbellQueue queue(memory.configs());

  ASSERT_TRUE(queue.push(1));
  ASSERT_TRUE(queue.push(2));
  ASSERT_TRUE(queue.push(3));

  uint64_t item = 0;
  ASSERT_TRUE(queue.pop(&item));
  EXPECT_EQ(1, item);
  ASSERT_TRUE(queue.pop(&item));
  EXPECT_EQ(3, item);
  ASSERT_TRUE(queue.pop(&item));
  EXPECT_EQ(2, item);
  EXPECT_FALSE(queue.pop(&item));
}

TEST(CxlMemDoorbellQueue, continuesWritingAfterOneQueueCapacity) {
  DoorbellMemory memory;
  StubDoorbellQueue queue(memory.configs());

  const size_t capacity = queue.capacity();
  for (size_t i = 0; i < capacity; ++i) {
    ASSERT_TRUE(queue.push(i)) << i;
  }
  EXPECT_FALSE(queue.push(capacity));

  std::vector<bool> seen(capacity);
  for (size_t i = 0; i < capacity; ++i) {
    uint64_t item = 0;
    ASSERT_TRUE(queue.pop(&item)) << i;
    ASSERT_LT(item, capacity);
    EXPECT_FALSE(seen[item]) << item;
    seen[item] = true;
  }

  uint64_t item = 0;
  EXPECT_FALSE(queue.pop(&item));
}

} // namespace folly
