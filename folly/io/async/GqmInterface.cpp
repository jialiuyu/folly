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

#include <folly/io/async/gqm_common.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/logging/xlog.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace folly {

namespace {

constexpr size_t kUgqmHeaderBytes() {
  /* Must match struct layout in ugqm_stub.c / vendor ugqm implementation. */
  return 3 * sizeof(uint64_t);
}

uint32_t ugqmLengthFromRegionBytes(size_t regionBytes) {
  if (regionBytes <= kUgqmHeaderBytes()) {
    return 0;
  }
  size_t slots = (regionBytes - kUgqmHeaderBytes()) / sizeof(uint64_t);
  if (slots == 0) {
    return 0;
  }
  if (slots > static_cast<size_t>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(slots);
}

folly::Optional<GqmNotification> ugqmPopNotification(void* queue) {
  uint64_t raw = 0;
  uint64_t ret = ugqm_pop(queue, &raw);
  if (GQM_RET_ERR(ret) == GQM_ERR_EMPTY) {
    return folly::none;
  }
  if (GQM_RET_ERR(ret) != GQM_ERR_OK) {
    return folly::none;
  }
  return GqmNotification::fromUint64(raw);
}

void ugqmPushNotification(void* queue, const GqmNotification& notification) {
  uint64_t msg = notification.toUint64();
  uint64_t ret = ugqm_push(queue, msg);
  if (GQM_RET_ERR(ret) != GQM_ERR_OK) {
    XLOG(ERR) << "ugqm_push failed, err=" << GQM_RET_ERR(ret);
  }
}

} // namespace

// ========== DefaultGqmInterface Implementation ==========

bool DefaultGqmInterface::init(void* mem, size_t size) {
  uint32_t length = ugqmLengthFromRegionBytes(size);
  if (length == 0) {
    return false;
  }
  uint64_t ret = ugqm_withdata_init(mem, length);
  return GQM_RET_ERR(ret) == GQM_ERR_OK;
}

void DefaultGqmInterface::push(const GqmNotification& notification) {
  ugqmPushNotification(queueMem_, notification);
}

folly::Optional<GqmNotification> DefaultGqmInterface::pop() {
  return ugqmPopNotification(queueMem_);
}

// ========== SharedMemoryGqm Implementation ==========

SharedMemoryGqm::SharedMemoryGqm(
    const std::string& name,
    int fd,
    void* mappedAddr,
    size_t totalSize,
    bool isCreator)
    : name_(name),
      fd_(fd),
      mappedAddr_(mappedAddr),
      totalSize_(totalSize),
      isCreator_(isCreator) {
  XLOG(DBG5) << "SharedMemoryGqm created: " << name_;
}

SharedMemoryGqm::~SharedMemoryGqm() {
  if (mappedAddr_ != nullptr && mappedAddr_ != MAP_FAILED) {
    if (isCreator_) {
      uint64_t dret = ugqm_deinit(mappedAddr_);
      if (GQM_RET_ERR(dret) != GQM_ERR_OK) {
        XLOG(WARN) << "ugqm_deinit failed, err=" << GQM_RET_ERR(dret);
      }
    }
    ::munmap(mappedAddr_, totalSize_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  if (isCreator_) {
    ::shm_unlink(name_.c_str());
  }
  XLOG(DBG5) << "SharedMemoryGqm destroyed: " << name_;
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

  uint64_t iret = ugqm_withdata_init(
      mappedAddr, static_cast<uint32_t>(queueDepth));
  if (GQM_RET_ERR(iret) != GQM_ERR_OK) {
    ::munmap(mappedAddr, totalSize);
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error(folly::sformat(
        "Failed to ugqm_withdata_init GQM shared memory {} err={}",
        name,
        GQM_RET_ERR(iret)));
  }

  return std::unique_ptr<SharedMemoryGqm>(
      new SharedMemoryGqm(name, fd, mappedAddr, totalSize, true));
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
      new SharedMemoryGqm(name, fd, mappedAddr, totalSize, false));
}

void SharedMemoryGqm::push(const GqmNotification& notification) {
  ugqmPushNotification(mappedAddr_, notification);
}

folly::Optional<GqmNotification> SharedMemoryGqm::pop() {
  return ugqmPopNotification(mappedAddr_);
}

// ========== ImportedGqm Implementation ==========

ImportedGqm::ImportedGqm(
    std::unique_ptr<MemoryRegion> region, bool isCreator)
    : region_(std::move(region)), isCreator_(isCreator) {}

std::unique_ptr<ImportedGqm> ImportedGqm::create(
    std::unique_ptr<MemoryRegion> region) {
  uint32_t length = ugqmLengthFromRegionBytes(region->size());
  if (length == 0) {
    throw std::runtime_error(folly::sformat(
        "ImportedGqm::create: region '{}' too small for ugqm",
        region->name()));
  }
  uint64_t iret = ugqm_withdata_init(region->data(), length);
  if (GQM_RET_ERR(iret) != GQM_ERR_OK) {
    throw std::runtime_error(folly::sformat(
        "ImportedGqm::create: ugqm_withdata_init failed for region '{}' err={}",
        region->name(),
        GQM_RET_ERR(iret)));
  }
  XLOG(DBG5) << "ImportedGqm created: " << region->name()
             << ", offset=" << region->offset()
             << ", size=" << region->size();
  return std::unique_ptr<ImportedGqm>(
      new ImportedGqm(std::move(region), true));
}

std::unique_ptr<ImportedGqm> ImportedGqm::open(
    std::unique_ptr<MemoryRegion> region) {
  XLOG(DBG5) << "ImportedGqm opened: " << region->name()
             << ", offset=" << region->offset()
             << ", size=" << region->size();
  return std::unique_ptr<ImportedGqm>(
      new ImportedGqm(std::move(region), false));
}

void ImportedGqm::push(const GqmNotification& notification) {
  ugqmPushNotification(region_->data(), notification);
}

folly::Optional<GqmNotification> ImportedGqm::pop() {
  return ugqmPopNotification(region_->data());
}

ImportedGqm::~ImportedGqm() {
  if (isCreator_ && region_ && region_->data()) {
    uint64_t dret = ugqm_deinit(region_->data());
    if (GQM_RET_ERR(dret) != GQM_ERR_OK) {
      XLOG(WARN) << "ugqm_deinit failed, err=" << GQM_RET_ERR(dret);
    }
  }
}

} // namespace folly
