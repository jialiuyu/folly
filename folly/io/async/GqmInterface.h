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
#include <folly/io/async/MemoryProvider.h>

namespace folly {

/**
 * GQM descriptor: 64 bits encoding connId:16 | offset:32 | length:16.
 *
 * Bit layout:
 *   [63:48]  connId  — 16 bits, max 65535 connections per poller
 *   [47:16]  offset  — 32 bits, max 4 GB (covers 1 GB pool)
 *   [15:0]   length  — 16 bits, max 65535 bytes (~64 KB per chunk)
 *
 * In the shared-GQM model a single queue serves all connections.  The
 * poller extracts connId to dispatch data to the correct transport.
 * For legacy per-connection mode, connId is set to 0.
 */
struct GqmNotification {
  uint16_t connId{0}; // Connection identifier (0 for legacy mode)
  uint32_t offset{0}; // Byte offset into the data region
  uint16_t length{0}; // Chunk length in bytes

  static constexpr uint16_t kMaxChunkSize = 65535;

  uint64_t toUint64() const {
    return (static_cast<uint64_t>(connId) << 48) |
           (static_cast<uint64_t>(offset) << 16) |
           length;
  }

  static GqmNotification fromUint64(uint64_t value) {
    return GqmNotification{
        static_cast<uint16_t>(value >> 48),
        static_cast<uint32_t>((value >> 16) & 0xFFFFFFFF),
        static_cast<uint16_t>(value & 0xFFFF)};
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
  static constexpr size_t kDefaultQueueDepth = 496;

  /**
   * GQM region size: enough for kDefaultQueueDepth entries.
   * The internal block size is determined by gqm_init; we reserve
   * sufficient space and enforce 4 KB alignment at the allocation site.
   */
  static constexpr size_t kGqmRegionSize = 32 * 1024; // 32 KB

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
 * GQM backed by a MemoryRegion from ImportedMemoryProvider.
 *
 * Used when both processes share the same physical backing store (CXL):
 * the GQM lives in the pre-mapped device file alongside the data regions.
 *
 *   Creator side:  ImportedGqm::create(region)  — calls gqm_init.
 *   Importer side: ImportedGqm::open(region)    — attaches only.
 */
class ImportedGqm : public GqmInterface {
 public:
  static std::unique_ptr<ImportedGqm> create(
      std::unique_ptr<MemoryRegion> region);
  static std::unique_ptr<ImportedGqm> open(
      std::unique_ptr<MemoryRegion> region);

  ~ImportedGqm() override = default;

  ImportedGqm(const ImportedGqm&) = delete;
  ImportedGqm& operator=(const ImportedGqm&) = delete;

  void push(const GqmNotification& notification) override;
  folly::Optional<GqmNotification> pop() override;
  bool empty() override;

  const std::string& name() const { return region_->name(); }
  size_t offset() const { return region_->offset(); }

 private:
  ImportedGqm(std::unique_ptr<MemoryRegion> region, bool isCreator);

  std::unique_ptr<MemoryRegion> region_;
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
