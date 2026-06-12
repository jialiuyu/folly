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

#include <folly/io/async/CxlMemPayloadSlice.h>

#include <algorithm>
#include <stdexcept>

namespace folly {

CxlMemPayloadSlice::CxlMemPayloadSlice(CxlMemPayloadSliceConfig config)
    : baseOffset_(config.baseOffset), length_(config.length) {
  if (length_ == 0) {
    throw std::invalid_argument("CxlMemPayloadSlice requires non-zero length");
  }
}

bool CxlMemPayloadSlice::reserve(size_t len, uint64_t* offset) {
  if (offset == nullptr) {
    throw std::invalid_argument("CxlMemPayloadSlice::reserve requires offset");
  }
  if (len == 0) {
    *offset = offsetForCursor(reservedCursorLocal_);
    return true;
  }
  if (hasPendingReservation_ || len > length_) {
    return false;
  }

  uint64_t startCursor = reservedCursorLocal_;
  const size_t position = static_cast<size_t>(startCursor % length_);
  if (len > length_ - position) {
    startCursor += length_ - position;
  }

  const uint64_t endCursor = startCursor + len;
  if (endCursor - remoteReadCursorLocal_ > length_) {
    return false;
  }

  pendingOffset_ = offsetForCursor(startCursor);
  pendingLength_ = len;
  pendingEndCursor_ = endCursor;
  hasPendingReservation_ = true;
  reservedCursorLocal_ = endCursor;
  *offset = pendingOffset_;
  return true;
}

void CxlMemPayloadSlice::commit(uint64_t offset, size_t len) {
  if (!hasPendingReservation_ || offset != pendingOffset_ ||
      len != pendingLength_) {
    throw std::invalid_argument("CxlMemPayloadSlice commit does not match reservation");
  }

  writeCursorLocal_ = pendingEndCursor_;
  hasPendingReservation_ = false;
  pendingOffset_ = 0;
  pendingLength_ = 0;
  pendingEndCursor_ = 0;
}

void CxlMemPayloadSlice::releaseThrough(uint64_t remoteReadCursor) {
  remoteReadCursorLocal_ = std::max(
      remoteReadCursorLocal_, std::min(remoteReadCursor, writeCursorLocal_));
}

size_t CxlMemPayloadSlice::freeBytes() const {
  const uint64_t used = reservedCursorLocal_ - remoteReadCursorLocal_;
  if (used >= length_) {
    return 0;
  }
  return length_ - static_cast<size_t>(used);
}

uint64_t CxlMemPayloadSlice::offsetForCursor(uint64_t cursor) const {
  return baseOffset_ + (cursor % length_);
}

} // namespace folly
