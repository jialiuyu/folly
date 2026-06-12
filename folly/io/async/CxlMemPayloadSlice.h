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
#include <cstdint>

namespace folly {

struct CxlMemPayloadSliceConfig {
  uint64_t baseOffset{0};
  size_t length{0};
};

class CxlMemPayloadSlice {
 public:
  explicit CxlMemPayloadSlice(CxlMemPayloadSliceConfig config);

  bool reserve(size_t len, uint64_t* offset);
  void commit(uint64_t offset, size_t len);
  void releaseThrough(uint64_t remoteReadCursor);
  size_t freeBytes() const;

  uint64_t writeCursor() const { return writeCursorLocal_; }
  uint64_t remoteReadCursor() const { return remoteReadCursorLocal_; }
  uint64_t baseOffset() const { return baseOffset_; }
  size_t length() const { return length_; }

 private:
  uint64_t offsetForCursor(uint64_t cursor) const;

  uint64_t baseOffset_{0};
  size_t length_{0};
  uint64_t writeCursorLocal_{0};
  uint64_t reservedCursorLocal_{0};
  uint64_t remoteReadCursorLocal_{0};
  uint64_t pendingOffset_{0};
  size_t pendingLength_{0};
  uint64_t pendingEndCursor_{0};
  bool hasPendingReservation_{false};
};

} // namespace folly
