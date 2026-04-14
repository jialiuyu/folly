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

#include <mutex>
#include <unordered_map>

#include <folly/io/async/MemoryProvider.h>

namespace folly {

/**
 * MemoryRegion backed by caller-supplied memory.
 *
 * The caller retains ownership of the underlying memory; this region
 * does NOT free/munmap on destruction.
 */
class ImportedMemoryRegion : public MemoryRegion {
 public:
  ImportedMemoryRegion(
      const std::string& name, void* addr, size_t size, size_t offset)
      : name_(name), addr_(addr), size_(size), offset_(offset) {}

  void* data() override { return addr_; }
  const void* data() const override { return addr_; }
  size_t size() const override { return size_; }
  const std::string& name() const override { return name_; }
  size_t offset() const override { return offset_; }

 private:
  std::string name_;
  void* addr_;
  size_t size_;
  size_t offset_;
};

/**
 * MemoryProvider that hands out sub-regions from pre-registered memory pools.
 * Designed for CXL device files or pre-allocated hugepage regions.
 *
 * Typical usage (benchmark):
 *   1. At process start, mmap two 1 GB CXL device files.
 *   2. registerPool("s2c", addr0, 1GB) — server→client direction.
 *      registerPool("c2s", addr1, 1GB) — client→server direction.
 *   3. Pass this provider + pool names to BusyPollSharedMemoryTransport::Config.
 *   4. The handshake allocates data regions and GQM regions from the
 *      appropriate pools via createFromPool / importFromPool.
 *
 * Thread-safe: multiple connections can allocate concurrently.
 */
class ImportedMemoryProvider : public MemoryProvider {
 public:
  struct Pool {
    void* baseAddr{nullptr};
    size_t totalSize{0};
    size_t allocated{0};
  };

  void registerPool(const std::string& poolName, void* addr, size_t size);

  // --- MemoryProvider interface ---

  /** Allocate from the first pool with enough space (legacy path). */
  std::unique_ptr<MemoryRegion> create(
      const std::string& name, size_t size) override;

  /** Legacy import — bump-allocates (only correct for single-connection). */
  std::unique_ptr<MemoryRegion> import(
      const std::string& name, size_t size) override;

  std::unique_ptr<MemoryRegion> createAligned(
      const std::string& name, size_t size, size_t alignment) override;

  /**
   * Import at a known offset within a specific pool.
   * No bump allocation — the offset was determined by the creator side
   * and communicated via handshake.
   */
  std::unique_ptr<MemoryRegion> importAtOffset(
      const std::string& poolName,
      const std::string& regionName,
      size_t offset,
      size_t size) override;

  bool usesSharedBackingStore() const override { return true; }

  // --- Pool-aware allocation (CXL path) ---

  /**
   * Bump-allocate from a specific named pool with alignment.
   * The returned region records its offset within the pool.
   */
  std::unique_ptr<MemoryRegion> createFromPool(
      const std::string& poolName,
      const std::string& regionName,
      size_t size,
      size_t alignment = 1);

  /**
   * Access a specific pool at a known offset (no allocation).
   */
  std::unique_ptr<MemoryRegion> importFromPool(
      const std::string& poolName,
      const std::string& regionName,
      size_t offset,
      size_t size);

  /**
   * Return remaining unallocated bytes in the named pool.
   */
  size_t poolRemaining(const std::string& poolName);

 private:
  std::mutex mu_;
  std::unordered_map<std::string, Pool> pools_;
};

} // namespace folly
