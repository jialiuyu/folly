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

#include <folly/io/async/MemoryProvider.h>

namespace folly {

/**
 * MemoryRegion backed by POSIX shared memory (shm_open + mmap).
 */
class PosixShmRegion : public MemoryRegion {
 public:
  ~PosixShmRegion() override;

  PosixShmRegion(const PosixShmRegion&) = delete;
  PosixShmRegion& operator=(const PosixShmRegion&) = delete;

  void* data() override { return mappedAddr_; }
  const void* data() const override { return mappedAddr_; }
  size_t size() const override { return size_; }
  const std::string& name() const override { return name_; }

 private:
  friend class PosixShmProvider;

  PosixShmRegion(
      const std::string& name,
      int fd,
      void* mappedAddr,
      size_t size,
      bool isCreator);

  std::string name_;
  int fd_;
  void* mappedAddr_;
  size_t size_;
  bool isCreator_;
};

/**
 * MemoryProvider that allocates regions via POSIX shm_open / mmap.
 *
 * create() calls shm_open(O_CREAT|O_EXCL), ftruncate, mmap.
 * import() calls shm_open(O_RDWR), mmap (no unlink on destroy).
 */
class PosixShmProvider : public MemoryProvider {
 public:
  std::unique_ptr<MemoryRegion> create(
      const std::string& name, size_t size) override;

  std::unique_ptr<MemoryRegion> import(
      const std::string& name, size_t size) override;
};

} // namespace folly
