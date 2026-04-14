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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <folly/Optional.h>

namespace folly {

/**
 * GQM descriptor: 64 bits encoding (offset:32 | length:32).
 *
 * In the ring-queue model, each GQM entry IS one data chunk descriptor
 * pointing into the flat shared memory data region.  GQM's internal
 * atomic push/pop provides ordering and flow control, so the data region
 * itself needs no writeOffset/readOffset management.
 */
struct GqmNotification {
  uint32_t offset; // Byte offset into the data region
  uint32_t length; // Chunk length in bytes

  static constexpr uint32_t kMaxChunkSize = 64 * 1024; // 64 KB

  uint64_t toUint64() const {
    return (static_cast<uint64_t>(offset) << 32) | length;
  }

  static GqmNotification fromUint64(uint64_t value) {
    return GqmNotification{
        static_cast<uint32_t>(value >> 32),
        static_cast<uint32_t>(value & 0xFFFFFFFF)};
  }
};

/**
 * Abstract GQM ring-queue interface.
 *
 * In the SHM transport, GQM serves as the primary data ring queue:
 * push/pop of uint64 descriptors (offset+length) provides ordering and
 * backpressure.  The underlying implementation may be a hardware queue
 * (DefaultGqmInterface) or a software POSIX-shm queue (SharedMemoryGqm).
 */
class GqmInterface {
 public:
  virtual ~GqmInterface() = default;

  /**
   * Push a notification to the queue.
   * The message must fit in 64 bits.
   */
  virtual void push(const GqmNotification& notification) = 0;

  /**
   * Pop a notification from the queue.
   * Returns folly::none if no notification is available.
   */
  virtual folly::Optional<GqmNotification> pop() = 0;

  /**
   * Check if the queue is empty without consuming any notifications.
   */
  virtual bool empty() = 0;
};

/**
 * Default GQM implementation using the provided C functions.
 *
 * IMPORTANT: The following C functions must be linked:
 *   int  gqm_init(void* queue, size_t size);
 *   void gqm_push(void* queue, void* msg, size_t len);
 *   void* gqm_pop(void* queue);
 *
 * - gqm_init: Initialize a memory region as a GQM queue. Returns 0 on success.
 * - gqm_push: Atomically push a message to the specified queue. Thread-safe.
 * - gqm_pop: Atomically pop a message from the specified queue. Returns pointer
 *   to 64-bit message, or nullptr if empty. The message is consumed (read-once).
 *
 * All functions require a queue pointer obtained from gqm_init.
 */
class DefaultGqmInterface : public GqmInterface {
 public:
  /**
   * Construct with a pointer to an already-initialized GQM queue region.
   */
  explicit DefaultGqmInterface(void* queueMem) : queueMem_(queueMem) {}

  /**
   * Initialize a memory region as a GQM queue.
   * @param mem Pointer to the shared memory region
   * @param size Size of the region in bytes
   * @return true on success
   */
  static bool init(void* mem, size_t size);

  void push(const GqmNotification& notification) override;
  folly::Optional<GqmNotification> pop() override;
  bool empty() override;

 private:
  void* queueMem_;
};

/**
 * SharedMemoryGqm: A GQM queue backed by a POSIX shared memory region.
 *
 * The queue is created in shared memory so both the writer and reader
 * processes can access it without syscalls. push/pop are pure user-space
 * atomic operations.
 *
 * Usage:
 *   // Writer side (creates the queue):
 *   auto gqm = SharedMemoryGqm::create("/my_gqm", 1024);
 *   gqm->push({offset, length});
 *
 *   // Reader side (opens existing queue):
 *   auto gqm = SharedMemoryGqm::open("/my_gqm", 1024);
 *   auto notif = gqm->pop();
 */
class SharedMemoryGqm : public GqmInterface {
 public:
  /**
   * Default queue depth (number of 64-bit entries).
   */
  static constexpr size_t kDefaultQueueDepth = 1024;

  /**
   * Create a new GQM queue in shared memory.
   * The region is created with shm_open and initialized with gqm_init.
   *
   * @param name Shared memory name (e.g., "/thrift_gqm_0")
   * @param queueDepth Number of 64-bit entries in the queue
   * @return Unique pointer to the GQM interface
   */
  static std::unique_ptr<SharedMemoryGqm> create(
      const std::string& name,
      size_t queueDepth = kDefaultQueueDepth);

  /**
   * Open an existing GQM queue in shared memory.
   *
   * @param name Shared memory name
   * @param queueDepth Number of entries (must match the creation size)
   * @return Unique pointer to the GQM interface
   */
  static std::unique_ptr<SharedMemoryGqm> open(
      const std::string& name,
      size_t queueDepth = kDefaultQueueDepth);

  ~SharedMemoryGqm() override;

  SharedMemoryGqm(const SharedMemoryGqm&) = delete;
  SharedMemoryGqm& operator=(const SharedMemoryGqm&) = delete;

  void push(const GqmNotification& notification) override;
  folly::Optional<GqmNotification> pop() override;
  bool empty() override;

  /**
   * Get the shared memory region name.
   */
  const std::string& name() const { return name_; }

  /**
   * Get the raw memory pointer (for gqm_init or direct access).
   */
  void* data() { return mappedAddr_; }

 private:
  SharedMemoryGqm(
      const std::string& name,
      int fd,
      void* mappedAddr,
      size_t totalSize,
      bool isCreator);

  std::string name_;
  int fd_;
  void* mappedAddr_;
  size_t totalSize_;
  bool isCreator_;
};

/**
 * Null GQM implementation for testing or when notifications are not needed.
 */
class NullGqmInterface : public GqmInterface {
 public:
  void push(const GqmNotification& notification) override {
    (void)notification;
  }

  folly::Optional<GqmNotification> pop() override { return folly::none; }

  bool empty() override { return true; }
};

} // namespace folly
