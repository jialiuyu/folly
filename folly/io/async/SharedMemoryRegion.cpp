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

#include <folly/io/async/SharedMemoryRegion.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include <folly/Format.h>
#include <folly/Likely.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

namespace folly {

// ========== SharedMemoryRegion Implementation ==========

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::create(
    const std::string& name,
    size_t dataSize,
    bool create) {
  int fd = -1;
  void* mappedAddr = nullptr;
  size_t totalSize = kHeaderSize + dataSize;

  // Create or open shared memory
  if (create) {
    // Create new shared memory region
    fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
      XLOG(ERR) << "Failed to create shared memory " << name << ": "
                << strerror(errno);
      throw std::runtime_error(
          folly::sformat("Failed to create shared memory {}: {}", name, strerror(errno)));
    }

    // Set the size
    if (::ftruncate(fd, totalSize) < 0) {
      ::close(fd);
      ::shm_unlink(name.c_str());
      throw std::runtime_error(
          folly::sformat("Failed to set size of shared memory {}: {}", name, strerror(errno)));
    }
  } else {
    // Open existing shared memory region
    fd = ::shm_open(name.c_str(), O_RDWR, 0666);
    if (fd < 0) {
      throw std::runtime_error(
          folly::sformat("Failed to open shared memory {}: {}", name, strerror(errno)));
    }

    // Verify size
    struct stat st;
    if (::fstat(fd, &st) < 0) {
      ::close(fd);
      throw std::runtime_error(
          folly::sformat("Failed to stat shared memory {}: {}", name, strerror(errno)));
    }

    if (static_cast<size_t>(st.st_size) < totalSize) {
      ::close(fd);
      throw std::runtime_error(
          folly::sformat("Shared memory {} is too small: {} < {}", name, st.st_size, totalSize));
    }
  }

  // Map into memory
  mappedAddr = ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mappedAddr == MAP_FAILED) {
    int savedErrno = errno;
    ::close(fd);
    if (create) {
      ::shm_unlink(name.c_str());
    }
    throw std::runtime_error(
        folly::sformat("Failed to mmap shared memory {}: {}", name, strerror(savedErrno)));
  }

  // Initialize header if creating
  if (create) {
    SharedMemoryRegionHeader* header =
        reinterpret_cast<SharedMemoryRegionHeader*>(mappedAddr);
    new (header) SharedMemoryRegionHeader();
    header->dataSize = dataSize;
  }

  return std::unique_ptr<SharedMemoryRegion>(
      new SharedMemoryRegion(name, fd, mappedAddr, totalSize, create));
}

SharedMemoryRegion::SharedMemoryRegion(
    const std::string& name,
    int fd,
    void* mappedAddr,
    size_t totalSize,
    bool isCreator)
    : name_(name),
      fd_(fd),
      mappedAddr_(mappedAddr),
      totalSize_(totalSize),
      header_(reinterpret_cast<SharedMemoryRegionHeader*>(mappedAddr)),
      data_(reinterpret_cast<char*>(mappedAddr) + kHeaderSize),
      isCreator_(isCreator) {
  XLOG(DBG5) << "SharedMemoryRegion created: " << name_
             << ", dataSize=" << header_->dataSize
             << ", isCreator=" << isCreator_;
}

SharedMemoryRegion::~SharedMemoryRegion() {
  if (mappedAddr_ != nullptr && mappedAddr_ != MAP_FAILED) {
    ::munmap(mappedAddr_, totalSize_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  if (isCreator_) {
    ::shm_unlink(name_.c_str());
  }
  XLOG(DBG5) << "SharedMemoryRegion destroyed: " << name_;
}

size_t SharedMemoryRegion::availableToRead() const {
  uint64_t writeOffset = header_->writeOffset.load(std::memory_order_acquire);
  uint64_t readOffset = header_->readOffset.load(std::memory_order_acquire);

  if (writeOffset >= readOffset) {
    return writeOffset - readOffset;
  } else {
    // Wraparound case - should not happen with our implementation
    return 0;
  }
}

size_t SharedMemoryRegion::availableToWrite() const {
  uint64_t writeOffset = header_->writeOffset.load(std::memory_order_acquire);
  uint64_t readOffset = header_->readOffset.load(std::memory_order_acquire);

  // We keep one byte empty to distinguish full from empty
  if (writeOffset >= readOffset) {
    return header_->dataSize - (writeOffset - readOffset) - 1;
  } else {
    return readOffset - writeOffset - 1;
  }
}

ssize_t SharedMemoryRegion::write(const void* buf, size_t len) {
  if (header_->isClosed() || header_->hasError()) {
    errno = EPIPE;
    return -1;
  }

  size_t available = availableToWrite();
  if (available == 0) {
    // No space available
    return 0;
  }

  size_t toWrite = std::min(len, available);
  uint64_t writeOffset = header_->writeOffset.load(std::memory_order_acquire);
  uint64_t offsetInRegion = writeOffset % header_->dataSize;

  // Handle wraparound
  size_t firstChunk = std::min(toWrite, header_->dataSize - offsetInRegion);
  ::memcpy(static_cast<char*>(data_) + offsetInRegion, buf, firstChunk);

  if (firstChunk < toWrite) {
    // Wrap around to beginning
    ::memcpy(data_, static_cast<const char*>(buf) + firstChunk, toWrite - firstChunk);
  }

  // Update write offset
  header_->writeOffset.store(writeOffset + toWrite, std::memory_order_release);

  return toWrite;
}

ssize_t SharedMemoryRegion::read(void* buf, size_t len) {
  if (header_->hasError()) {
    errno = EIO;
    return -1;
  }

  size_t available = availableToRead();
  if (available == 0) {
    if (header_->isClosed()) {
      return 0; // EOF
    }
    return 0; // No data available
  }

  size_t toRead = std::min(len, available);
  uint64_t readOffset = header_->readOffset.load(std::memory_order_acquire);
  uint64_t offsetInRegion = readOffset % header_->dataSize;

  // Handle wraparound
  size_t firstChunk = std::min(toRead, header_->dataSize - offsetInRegion);
  ::memcpy(buf, static_cast<const char*>(data_) + offsetInRegion, firstChunk);

  if (firstChunk < toRead) {
    // Wrap around from beginning
    ::memcpy(static_cast<char*>(buf) + firstChunk, data_, toRead - firstChunk);
  }

  // Update read offset
  header_->readOffset.store(readOffset + toRead, std::memory_order_release);

  return toRead;
}

void SharedMemoryRegion::close() {
  header_->setClosed();
}

void SharedMemoryRegion::setReaderEventFd(int fd) {
  header_->readerEventFd.store(fd, std::memory_order_release);
}

int SharedMemoryRegion::getReaderEventFd() const {
  return header_->readerEventFd.load(std::memory_order_acquire);
}

} // namespace folly
