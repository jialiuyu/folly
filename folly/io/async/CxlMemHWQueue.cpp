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

#include <cstring>
#include <stdexcept>

namespace folly {
namespace {

constexpr uint64_t kQueueMagic = 0x434d485751303031;
constexpr size_t kMagicOffset = 0;
constexpr size_t kHeadOffset = 8;
constexpr size_t kTailOffset = 16;
constexpr size_t kItemsOffset = 24;

uint64_t loadU64(const unsigned char* memory, size_t offset) {
  uint64_t value = 0;
  std::memcpy(&value, memory + offset, sizeof(value));
  return value;
}

void storeU64(unsigned char* memory, size_t offset, uint64_t value) {
  std::memcpy(memory + offset, &value, sizeof(value));
}

void checkConfig(CxlMemHWQueueConfig config) {
  if (config.memory == nullptr) {
    throw std::invalid_argument("CxlMemHWQueue requires non-null memory");
  }
  if (config.length < kCxlMemHWQueueBytes) {
    throw std::invalid_argument("CxlMemHWQueue requires at least 4KB memory");
  }
  if (config.capacity != kCxlMemHWQueueCapacity) {
    throw std::invalid_argument("CxlMemHWQueue capacity is fixed at 496 items");
  }
}

} // namespace

namespace detail {

CxlMemStubHWQueueStorage::CxlMemStubHWQueueStorage(
    CxlMemHWQueueConfig config)
    : memory_(static_cast<unsigned char*>(config.memory)),
      capacity_(config.capacity) {
  checkConfig(config);

  if (loadU64(memory_, kMagicOffset) != kQueueMagic) {
    storeU64(memory_, kMagicOffset, kQueueMagic);
    storeU64(memory_, kHeadOffset, 0);
    storeU64(memory_, kTailOffset, 0);
  }
}

bool CxlMemStubHWQueueStorage::push(uint64_t item) {
  const uint64_t head = loadU64(memory_, kHeadOffset);
  const uint64_t tail = loadU64(memory_, kTailOffset);
  if (tail - head >= capacity_) {
    return false;
  }

  storeU64(memory_, kItemsOffset + ((tail % capacity_) * sizeof(item)), item);
  storeU64(memory_, kTailOffset, tail + 1);
  return true;
}

bool CxlMemStubHWQueueStorage::pop(uint64_t* item) {
  if (item == nullptr) {
    throw std::invalid_argument("CxlMemHWQueue::pop requires non-null item");
  }

  const uint64_t head = loadU64(memory_, kHeadOffset);
  const uint64_t tail = loadU64(memory_, kTailOffset);
  if (head == tail) {
    return false;
  }

  const uint64_t value = loadU64(
      memory_, kItemsOffset + ((head % capacity_) * sizeof(value)));
  storeU64(memory_, kHeadOffset, head + 1);
  *item = value;
  return true;
}

size_t CxlMemStubHWQueueStorage::capacity() const {
  return capacity_;
}

CxlMemRealHWQueueStorage::CxlMemRealHWQueueStorage(
    CxlMemHWQueueConfig config)
    : capacity_(config.capacity) {
  checkConfig(config);
}

bool CxlMemRealHWQueueStorage::push(uint64_t) {
  throw std::logic_error("CxlMemRealHWQueueBackend is not implemented yet");
}

bool CxlMemRealHWQueueStorage::pop(uint64_t*) {
  throw std::logic_error("CxlMemRealHWQueueBackend is not implemented yet");
}

size_t CxlMemRealHWQueueStorage::capacity() const {
  return capacity_;
}

} // namespace detail

template class CxlMemHWQueue<CxlMemStubHWQueueBackend>;
template class CxlMemHWQueue<CxlMemRealHWQueueBackend>;

} // namespace folly
