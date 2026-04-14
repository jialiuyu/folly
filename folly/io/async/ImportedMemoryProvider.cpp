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

#include <stdexcept>

#include <folly/Format.h>
#include <folly/logging/xlog.h>

namespace folly {

void ImportedMemoryProvider::registerPool(
    const std::string& poolName, void* addr, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  pools_[poolName] = Pool{addr, size, 0};
  XLOG(DBG5) << "ImportedMemoryProvider: registered pool '" << poolName
             << "', size=" << size;
}

void* ImportedMemoryProvider::allocateFromPool(size_t size) {
  for (auto& [poolName, pool] : pools_) {
    if (pool.allocated + size <= pool.totalSize) {
      void* ptr =
          static_cast<char*>(pool.baseAddr) + pool.allocated;
      pool.allocated += size;
      XLOG(DBG5) << "ImportedMemoryProvider: allocated " << size
                 << " bytes from pool '" << poolName
                 << "', remaining=" << (pool.totalSize - pool.allocated);
      return ptr;
    }
  }
  return nullptr;
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::create(
    const std::string& name, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  void* addr = allocateFromPool(size);
  if (!addr) {
    throw std::runtime_error(folly::sformat(
        "ImportedMemoryProvider::create: no pool has {} bytes available", size));
  }
  std::memset(addr, 0, size);
  return std::make_unique<ImportedMemoryRegion>(name, addr, size);
}

std::unique_ptr<MemoryRegion> ImportedMemoryProvider::import(
    const std::string& name, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  void* addr = allocateFromPool(size);
  if (!addr) {
    throw std::runtime_error(folly::sformat(
        "ImportedMemoryProvider::import: no pool has {} bytes available",
        size));
  }
  return std::make_unique<ImportedMemoryRegion>(name, addr, size);
}

} // namespace folly
