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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <folly/Portability.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/GqmInterface.h>

namespace folly {

/**
 * SharedMemoryRegionHeader defines the structure of the shared memory header.
 *
 * Memory layout:
 * +------------------+
 * | Header (64 bytes)|
 * +------------------+
 * | Data Region      |
 * | (configurable)   |
 * +------------------+
 *
 * Header layout (64 bytes):
 * - write_offset (8 bytes): Offset where writer will write next
 * - read_offset (8 bytes): Offset where reader has read up to
 * - data_size (8 bytes): Size of the data region
 * - flags (8 bytes): Flags for synchronization
 * - reserved (32 bytes): Reserved for future use
 */
struct SharedMemoryRegionHeader {
  // Atomic write offset - updated by the writer
  alignas(64) std::atomic<uint64_t> writeOffset{0};
  // Atomic read offset - updated by the reader
  alignas(64) std::atomic<uint64_t> readOffset{0};
  // Size of the data region (fixed after creation)
  uint64_t dataSize{0};
  // Flags for state synchronization
  std::atomic<uint64_t> flags{0};
  // Eventfd file descriptor for writer-to-reader notification.
  // The reader creates an eventfd and stores its fd here.
  // The writer signals this eventfd after writing data.
  // -1 means no eventfd notification is set up.
  std::atomic<int32_t> readerEventFd{-1};
  // Padding to maintain 64-byte header alignment
  uint8_t reserved[20]{};

  // Flag values
  static constexpr uint64_t kFlagClosed = 1 << 0;
  static constexpr uint64_t kFlagError = 1 << 1;

  bool isClosed() const { return (flags.load(std::memory_order_acquire) & kFlagClosed) != 0; }
  bool hasError() const { return (flags.load(std::memory_order_acquire) & kFlagError) != 0; }
  void setClosed() { flags.fetch_or(kFlagClosed, std::memory_order_release); }
  void setError() { flags.fetch_or(kFlagError, std::memory_order_release); }
};

/**
 * SharedMemoryRegion represents a single shared memory buffer for one-way
 * communication. For bidirectional communication, two regions are needed.
 */
class SharedMemoryRegion {
 public:
  // Header size is 64 bytes
  static constexpr size_t kHeaderSize = 64;

  /**
   * Creates a new shared memory region with the given name and size.
   * The region will be created if it doesn't exist, or opened if it does.
   *
   * @param name The name for the shared memory region (used for shm_open)
   * @param size The size of the data region (excluding header)
   * @param create If true, create the region; if false, open existing
   */
  static std::unique_ptr<SharedMemoryRegion> create(
      const std::string& name,
      size_t dataSize,
      bool create = true);

  ~SharedMemoryRegion();

  // Non-copyable, non-movable
  SharedMemoryRegion(const SharedMemoryRegion&) = delete;
  SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
  SharedMemoryRegion(SharedMemoryRegion&&) = delete;
  SharedMemoryRegion& operator=(SharedMemoryRegion&&) = delete;

  /**
   * Get pointer to the header (read-only for external use)
   */
  const SharedMemoryRegionHeader* header() const { return header_; }

  /**
   * Get pointer to the data region
   */
  void* data() { return data_; }
  const void* data() const { return data_; }

  /**
   * Get the size of the data region
   */
  size_t dataSize() const { return header_->dataSize; }

  /**
   * Get the name of the shared memory region
   */
  const std::string& name() const { return name_; }

  /**
   * Write data to the shared memory region.
   * Returns the number of bytes written, or -1 on error.
   * This is a non-blocking write that will write as much as possible.
   */
  ssize_t write(const void* buf, size_t len);

  /**
   * Read data from the shared memory region.
   * Returns the number of bytes read, or -1 on error.
   * This is a non-blocking read that will read as much as available.
   */
  ssize_t read(void* buf, size_t len);

  /**
   * Get the number of bytes available to read
   */
  size_t availableToRead() const;

  /**
   * Get the number of bytes that can be written
   */
  size_t availableToWrite() const;

  /**
   * Close the shared memory region (marks it as closed in header)
   */
  void close();

  /**
   * Check if the region is closed
   */
  bool isClosed() const { return header_->isClosed(); }

  /**
   * Get the file descriptor for the shared memory
   */
  int fd() const { return fd_; }

  /**
   * Set the eventfd that the writer should signal after writing data.
   * This is called by the reader of this region.
   */
  void setReaderEventFd(int fd);

  /**
   * Get the eventfd that the writer should signal after writing data.
   * Returns -1 if no eventfd is set.
   */
  int getReaderEventFd() const;

 private:
  SharedMemoryRegion(
      const std::string& name,
      int fd,
      void* mappedAddr,
      size_t totalSize);

  std::string name_;
  int fd_;
  void* mappedAddr_;
  size_t totalSize_;
  SharedMemoryRegionHeader* header_;
  void* data_;
};

/**
 * SharedMemoryTransportConfig holds configuration for SharedMemoryTransport.
 */
struct SharedMemoryTransportConfig {
  // Size of each shared memory data region (default: 1MB)
  size_t dataRegionSize = 1024 * 1024;
  // Name prefix for shared memory regions
  std::string shmNamePrefix = "/thrift_shm_";
  // Enable debug logging
  bool debugLogging = false;
};

/**
 * SharedMemoryHandshakeInfo contains information exchanged during TCP
 * handshake to establish shared memory connections.
 */
struct SharedMemoryHandshakeInfo {
  // Name of the shared memory region that the sender will write to
  // (and the receiver will read from)
  std::string writeShmName;
  // Size of the data region
  uint64_t dataRegionSize{0};
  // Magic number for validation
  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

} // namespace folly
