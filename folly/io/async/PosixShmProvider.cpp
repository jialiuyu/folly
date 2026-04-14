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

#include <folly/io/async/PosixShmProvider.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <folly/Format.h>
#include <folly/logging/xlog.h>

namespace folly {

// ---------- PosixShmRegion ----------

PosixShmRegion::PosixShmRegion(
    const std::string& name,
    int fd,
    void* mappedAddr,
    size_t size,
    bool isCreator)
    : name_(name),
      fd_(fd),
      mappedAddr_(mappedAddr),
      size_(size),
      isCreator_(isCreator) {
  XLOG(DBG5) << "PosixShmRegion created: " << name_
             << ", size=" << size_ << ", isCreator=" << isCreator_;
}

PosixShmRegion::~PosixShmRegion() {
  if (mappedAddr_ != nullptr && mappedAddr_ != MAP_FAILED) {
    ::munmap(mappedAddr_, size_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  if (isCreator_) {
    ::shm_unlink(name_.c_str());
  }
  XLOG(DBG5) << "PosixShmRegion destroyed: " << name_;
}

// ---------- PosixShmProvider ----------

std::unique_ptr<MemoryRegion> PosixShmProvider::create(
    const std::string& name, size_t size) {
  int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  if (fd < 0) {
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::create shm_open failed for {}: {}",
        name,
        strerror(errno)));
  }

  if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
    int savedErrno = errno;
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::create ftruncate failed for {}: {}",
        name,
        strerror(savedErrno)));
  }

  void* addr =
      ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    int savedErrno = errno;
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::create mmap failed for {}: {}",
        name,
        strerror(savedErrno)));
  }

  std::memset(addr, 0, size);

  return std::unique_ptr<MemoryRegion>(
      new PosixShmRegion(name, fd, addr, size, true));
}

std::unique_ptr<MemoryRegion> PosixShmProvider::import(
    const std::string& name, size_t size) {
  int fd = ::shm_open(name.c_str(), O_RDWR, 0666);
  if (fd < 0) {
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::import shm_open failed for {}: {}",
        name,
        strerror(errno)));
  }

  struct stat st;
  if (::fstat(fd, &st) < 0) {
    int savedErrno = errno;
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::import fstat failed for {}: {}",
        name,
        strerror(savedErrno)));
  }

  if (static_cast<size_t>(st.st_size) < size) {
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::import region {} too small: {} < {}",
        name,
        st.st_size,
        size));
  }

  void* addr =
      ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    int savedErrno = errno;
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "PosixShmProvider::import mmap failed for {}: {}",
        name,
        strerror(savedErrno)));
  }

  return std::unique_ptr<MemoryRegion>(
      new PosixShmRegion(name, fd, addr, size, false));
}

} // namespace folly
