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

#include <folly/io/async/ImportedMemoryProvider.h>

#include <cstring>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/logging/xlog.h>

namespace folly {

void ImportedMemoryProvider::registerPool(
    const std::string& poolName, void* addr, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  pools_[poolName] = Pool{addr, size, 0};
  XLOG(DBG5) << "ImportedMemoryProvider: registered pool '" << poolName
             << "', base=" << addr << ", size=" << size;
}

// ---- Legacy MemoryProvider interface (first-fit, no pool selection) ----

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::create(
    const std::string& name, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [poolName, pool] : pools_) {
    if (pool.allocated + size <= pool.totalSize) {
      size_t off = pool.allocated;
      void* ptr = static_cast<char*>(pool.baseAddr) + off;
      pool.allocated += size;
      std::memset(ptr, 0, size);
      XLOG(DBG5) << "create '" << name << "' from pool '" << poolName
                 << "', offset=" << off << ", size=" << size;
      return std::make_unique<ImportedMemoryRegion>(name, ptr, size, off);
    }
  }
  throw std::runtime_error(folly::sformat(
      "ImportedMemoryProvider::create: no pool has {} bytes available", size));
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::import(
    const std::string& name, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [poolName, pool] : pools_) {
    if (pool.allocated + size <= pool.totalSize) {
      size_t off = pool.allocated;
      void* ptr = static_cast<char*>(pool.baseAddr) + off;
      pool.allocated += size;
      XLOG(DBG5) << "import '" << name << "' from pool '" << poolName
                 << "', offset=" << off << ", size=" << size;
      return std::make_unique<ImportedMemoryRegion>(name, ptr, size, off);
    }
  }
  throw std::runtime_error(folly::sformat(
      "ImportedMemoryProvider::import: no pool has {} bytes available", size));
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::createAligned(
    const std::string& name, size_t size, size_t alignment) {
  return createFromPool("", name, size, alignment);
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::importAtOffset(
    const std::string& poolName,
    const std::string& regionName,
    size_t offset,
    size_t size) {
  return importFromPool(poolName, regionName, offset, size);
}

// ---- Pool-aware allocation (CXL path) ----

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::createFromPool(
    const std::string& poolName,
    const std::string& regionName,
    size_t size,
    size_t alignment) {
  std::lock_guard<std::mutex> lock(mu_);

  auto allocate = [&](Pool& pool, const std::string& pName)
      -> std::unique_ptr<MemoryRegion> {
    size_t alignedOff = (pool.allocated + alignment - 1) & ~(alignment - 1);
    if (alignedOff + size > pool.totalSize) {
      return nullptr;
    }
    void* ptr = static_cast<char*>(pool.baseAddr) + alignedOff;
    pool.allocated = alignedOff + size;
    std::memset(ptr, 0, size);
    XLOG(DBG5) << "createFromPool '" << regionName << "' in pool '" << pName
               << "', offset=" << alignedOff << ", size=" << size
               << ", align=" << alignment;
    return std::make_unique<ImportedMemoryRegion>(
        regionName, ptr, size, alignedOff);
  };

  if (!poolName.empty()) {
    auto it = pools_.find(poolName);
    if (it == pools_.end()) {
      throw std::runtime_error(folly::sformat(
          "ImportedMemoryProvider::createFromPool: pool '{}' not found",
          poolName));
    }
    auto region = allocate(it->second, poolName);
    if (!region) {
      throw std::runtime_error(folly::sformat(
          "ImportedMemoryProvider::createFromPool: pool '{}' has insufficient "
          "space for {} bytes (aligned {})",
          poolName, size, alignment));
    }
    return region;
  }

  // No pool specified — first fit
  for (auto& [pName, pool] : pools_) {
    auto region = allocate(pool, pName);
    if (region) {
      return region;
    }
  }
  throw std::runtime_error(folly::sformat(
      "ImportedMemoryProvider::createFromPool: no pool has {} bytes available "
      "(aligned {})",
      size, alignment));
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::importFromPool(
    const std::string& poolName,
    const std::string& regionName,
    size_t offset,
    size_t size) {
  std::lock_guard<std::mutex> lock(mu_);

  auto resolveFromPool = [&](Pool& pool) -> std::unique_ptr<MemoryRegion> {
    if (offset + size > pool.totalSize) {
      return nullptr;
    }
    void* ptr = static_cast<char*>(pool.baseAddr) + offset;
    return std::make_unique<ImportedMemoryRegion>(
        regionName, ptr, size, offset);
  };

  if (!poolName.empty()) {
    auto it = pools_.find(poolName);
    if (it == pools_.end()) {
      throw std::runtime_error(folly::sformat(
          "ImportedMemoryProvider::importFromPool: pool '{}' not found",
          poolName));
    }
    auto region = resolveFromPool(it->second);
    if (!region) {
      throw std::runtime_error(folly::sformat(
          "ImportedMemoryProvider::importFromPool: offset {} + size {} exceeds "
          "pool '{}' total size {}",
          offset, size, poolName, it->second.totalSize));
    }
    XLOG(DBG5) << "importFromPool '" << regionName << "' in pool '" << poolName
               << "', offset=" << offset << ", size=" << size;
    return region;
  }

  // No pool specified — try all pools
  for (auto& [pName, pool] : pools_) {
    auto region = resolveFromPool(pool);
    if (region) {
      XLOG(DBG5) << "importFromPool '" << regionName << "' in pool '" << pName
                 << "', offset=" << offset << ", size=" << size;
      return region;
    }
  }
  throw std::runtime_error(folly::sformat(
      "ImportedMemoryProvider::importFromPool: no pool can resolve offset={}, "
      "size={}",
      offset, size));
}

size_t ImportedMemoryProvider::poolRemaining(const std::string& poolName) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = pools_.find(poolName);
  if (it == pools_.end()) {
    throw std::runtime_error(folly::sformat(
        "ImportedMemoryProvider::poolRemaining: pool '{}' not found",
        poolName));
  }
  return it->second.totalSize - it->second.allocated;
}

} // namespace folly
