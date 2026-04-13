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

#include <folly/io/async/GqmInterface.h>

#include <cstring>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/logging/xlog.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace folly {

// ========== External GQM C functions ==========
// These functions should be provided by your hardware queue library.
extern "C" {
int gqm_init(void* mem, size_t size);
void gqm_push(void* msg, size_t len);
void* gqm_pop();
}

// ========== DefaultGqmInterface Implementation ==========

bool DefaultGqmInterface::init(void* mem, size_t size) {
  return gqm_init(mem, size) == 0;
}

void DefaultGqmInterface::push(const GqmNotification& notification) {
  uint64_t msg = notification.toUint64();
  gqm_push(&msg, sizeof(msg));
}

folly::Optional<GqmNotification> DefaultGqmInterface::pop() {
  void* result = gqm_pop();
  if (result == nullptr) {
    return folly::none;
  }

  uint64_t msg;
  std::memcpy(&msg, result, sizeof(msg));
  return GqmNotification::fromUint64(msg);
}

// ========== SharedMemoryGqm Implementation ==========

SharedMemoryGqm::SharedMemoryGqm(
    const std::string& name,
    int fd,
    void* mappedAddr,
    size_t totalSize)
    : name_(name),
      fd_(fd),
      mappedAddr_(mappedAddr),
      totalSize_(totalSize) {
  XLOG(DBG) << "SharedMemoryGqm created: " << name_;
}

SharedMemoryGqm::~SharedMemoryGqm() {
  if (mappedAddr_ != nullptr && mappedAddr_ != MAP_FAILED) {
    ::munmap(mappedAddr_, totalSize_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  XLOG(DBG) << "SharedMemoryGqm destroyed: " << name_;
}

std::unique_ptr<SharedMemoryGqm> SharedMemoryGqm::create(
    const std::string& name,
    size_t queueDepth) {
  // Each entry is 64 bits (8 bytes). Add some overhead for GQM metadata.
  // We allocate queueDepth * 8 bytes for entries, plus a page for metadata.
  size_t entrySize = sizeof(uint64_t);
  size_t totalSize = (queueDepth + 128) * entrySize; // extra for GQM overhead

  // Create shared memory region
  int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  if (fd < 0) {
    throw std::runtime_error(folly::sformat(
        "Failed to create GQM shared memory {}: {}", name, strerror(errno)));
  }

  // Set size
  if (::ftruncate(fd, totalSize) < 0) {
    int savedErrno = errno;
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "Failed to set size of GQM shared memory {}: {}",
        name,
        strerror(savedErrno)));
  }

  // Map
  void* mappedAddr =
      ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mappedAddr == MAP_FAILED) {
    int savedErrno = errno;
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "Failed to mmap GQM shared memory {}: {}",
        name,
        strerror(savedErrno)));
  }

  // Initialize with gqm_init
  if (gqm_init(mappedAddr, totalSize) != 0) {
    ::munmap(mappedAddr, totalSize);
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "Failed to gqm_init GQM shared memory {}", name));
  }

  return std::unique_ptr<SharedMemoryGqm>(
      new SharedMemoryGqm(name, fd, mappedAddr, totalSize));
}

std::unique_ptr<SharedMemoryGqm> SharedMemoryGqm::open(
    const std::string& name,
    size_t queueDepth) {
  size_t entrySize = sizeof(uint64_t);
  size_t totalSize = (queueDepth + 128) * entrySize;

  // Open existing shared memory region
  int fd = ::shm_open(name.c_str(), O_RDWR, 0666);
  if (fd < 0) {
    throw std::runtime_error(folly::sformat(
        "Failed to open GQM shared memory {}: {}", name, strerror(errno)));
  }

  // Verify size
  struct stat st;
  if (::fstat(fd, &st) < 0) {
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "Failed to stat GQM shared memory {}: {}", name, strerror(errno)));
  }

  if (static_cast<size_t>(st.st_size) < totalSize) {
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "GQM shared memory {} is too small: {} < {}",
        name,
        st.st_size,
        totalSize));
  }

  // Map
  void* mappedAddr =
      ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mappedAddr == MAP_FAILED) {
    int savedErrno = errno;
    ::close(fd);
    throw std::runtime_error(folly::sformat(
        "Failed to mmap GQM shared memory {}: {}",
        name,
        strerror(savedErrno)));
  }

  return std::unique_ptr<SharedMemoryGqm>(
      new SharedMemoryGqm(name, fd, mappedAddr, totalSize));
}

void SharedMemoryGqm::push(const GqmNotification& notification) {
  uint64_t msg = notification.toUint64();
  gqm_push(&msg, sizeof(msg));
}

folly::Optional<GqmNotification> SharedMemoryGqm::pop() {
  void* result = gqm_pop();
  if (result == nullptr) {
    return folly::none;
  }

  uint64_t msg;
  std::memcpy(&msg, result, sizeof(msg));
  return GqmNotification::fromUint64(msg);
}

} // namespace folly
