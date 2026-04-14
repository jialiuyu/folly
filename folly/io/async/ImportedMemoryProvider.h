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
  ImportedMemoryRegion(const std::string& name, void* addr, size_t size)
      : name_(name), addr_(addr), size_(size) {}

  void* data() override { return addr_; }
  const void* data() const override { return addr_; }
  size_t size() const override { return size_; }
  const std::string& name() const override { return name_; }

 private:
  std::string name_;
  void* addr_;
  size_t size_;
};

/**
 * MemoryProvider that hands out sub-regions from pre-registered memory pools.
 *
 * Typical usage:
 *   1. At process startup, mmap a 1 GB hugepage region.
 *   2. Call registerPool("pool0", addr, 1GB).
 *   3. Pass this provider to BusyPollSharedMemoryTransport::Config.
 *   4. create()/import() carve sub-regions from the pool via bump allocation.
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

  /**
   * Register a pre-allocated memory pool.
   * @param poolName  Logical name for the pool.
   * @param addr      Start address (caller owns the memory).
   * @param size      Total size in bytes.
   */
  void registerPool(const std::string& poolName, void* addr, size_t size);

  /**
   * Allocate a sub-region from the first pool that has enough space.
   * The returned MemoryRegion does NOT free the memory on destruction.
   */
  std::unique_ptr<MemoryRegion> create(
      const std::string& name, size_t size) override;

  /**
   * Import is identical to create for this provider (both carve from pool).
   */
  std::unique_ptr<MemoryRegion> import(
      const std::string& name, size_t size) override;

 private:
  void* allocateFromPool(size_t size);

  std::mutex mu_;
  std::unordered_map<std::string, Pool> pools_;
};

} // namespace folly
