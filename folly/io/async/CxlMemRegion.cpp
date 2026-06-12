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

#include <folly/io/async/CxlMemRegion.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace folly {
namespace {

void throwSystemError(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

CxlMemRegion::CxlMemRegion(CxlMemRegionConfig config)
    : path_(std::move(config.path)),
      size_(config.size),
      cacheCoherentMapping_(config.cacheCoherentMapping) {
  if (path_.empty()) {
    throw std::invalid_argument("CxlMemRegion requires a path");
  }
  if (size_ == 0) {
    throw std::invalid_argument("CxlMemRegion requires a non-zero size");
  }

  const int flags = O_RDWR | (config.createIfMissing ? O_CREAT : 0);
  fd_ = ::open(path_.c_str(), flags, 0600);
  if (fd_ == -1) {
    throwSystemError("open CxlMemRegion");
  }

  try {
    struct stat st;
    if (::fstat(fd_, &st) == -1) {
      throwSystemError("stat CxlMemRegion");
    }

    if (static_cast<size_t>(st.st_size) < size_) {
      if (!config.createIfMissing) {
        throw std::invalid_argument("CxlMemRegion file is smaller than mapping");
      }
      if (::ftruncate(fd_, static_cast<off_t>(size_)) == -1) {
        throwSystemError("resize CxlMemRegion");
      }
    }

    void* mapping = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapping == MAP_FAILED) {
      throwSystemError("map CxlMemRegion");
    }
    data_ = static_cast<unsigned char*>(mapping);
  } catch (...) {
    close();
    throw;
  }
}

CxlMemRegion::~CxlMemRegion() {
  close();
}

CxlMemRegion::CxlMemRegion(CxlMemRegion&& other) noexcept {
  *this = std::move(other);
}

CxlMemRegion& CxlMemRegion::operator=(CxlMemRegion&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close();
  path_ = std::move(other.path_);
  data_ = other.data_;
  size_ = other.size_;
  fd_ = other.fd_;
  cacheCoherentMapping_ = other.cacheCoherentMapping_;

  other.data_ = nullptr;
  other.size_ = 0;
  other.fd_ = -1;
  other.cacheCoherentMapping_ = false;
  return *this;
}

void CxlMemRegion::close() noexcept {
  if (data_ != nullptr) {
    ::munmap(data_, size_);
    data_ = nullptr;
  }
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
  size_ = 0;
}

} // namespace folly
