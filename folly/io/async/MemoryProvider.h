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
#include <memory>
#include <string>

namespace folly {

/**
 * Abstract interface for a mapped memory region used by SHM transport.
 *
 * A MemoryRegion represents a contiguous block of memory that is accessible
 * by both the writer and reader processes. The underlying backing store
 * (POSIX shm, CXL device memory, pre-allocated hugepages, etc.) is
 * determined by the MemoryProvider that created it.
 */
class MemoryRegion {
 public:
  virtual ~MemoryRegion() = default;

  /**
   * Pointer to the start of the usable data area.
   */
  virtual void* data() = 0;
  virtual const void* data() const = 0;

  /**
   * Size of the usable data area in bytes.
   */
  virtual size_t size() const = 0;

  /**
   * Logical name of this region (used during handshake so the peer can
   * import the same region).
   */
  virtual const std::string& name() const = 0;
};

/**
 * Factory interface for creating and importing MemoryRegions.
 *
 * Implementations map to different backing stores:
 *   - PosixShmProvider  : POSIX shm_open + mmap  (current default)
 *   - ImportedMemoryProvider : caller-supplied pre-allocated pool
 *   - (future) CxlMemoryProvider : CXL device memory
 */
class MemoryProvider {
 public:
  virtual ~MemoryProvider() = default;

  /**
   * Create a new named region of the given size.
   * The creator owns cleanup (e.g. shm_unlink).
   */
  virtual std::unique_ptr<MemoryRegion> create(
      const std::string& name, size_t size) = 0;

  /**
   * Import (open) an existing region created by another process/side.
   * The importer does NOT unlink the backing store on destruction.
   */
  virtual std::unique_ptr<MemoryRegion> import(
      const std::string& name, size_t size) = 0;
};

} // namespace folly
