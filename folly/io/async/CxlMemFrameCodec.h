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
#include <limits>
#include <stdexcept>

namespace folly {

struct CxlMemDataFrame {
  uint16_t connId{0};
  uint64_t payloadOffset{0};
  uint16_t payloadLength{0};
};

class CxlMemFrameCodec {
 public:
  static constexpr size_t kMaxDataPayloadLength =
      std::numeric_limits<uint16_t>::max();
  static constexpr uint64_t kMaxDataPayloadOffset = (uint64_t{1} << 32) - 1;
  static constexpr uint64_t kMaxAckCursor = (uint64_t{1} << 48) - 1;

  static uint64_t encodeDataItem(
      uint16_t connId,
      uint64_t payloadOffset,
      uint16_t payloadLength) {
    if (payloadOffset > kMaxDataPayloadOffset) {
      throw std::invalid_argument("CXL mem DATA payload offset is too large");
    }
    if (payloadLength == 0) {
      throw std::invalid_argument("CXL mem DATA payload length is zero");
    }
    return (uint64_t{connId} << 48) | (payloadOffset << 16) | payloadLength;
  }

  static CxlMemDataFrame decodeDataItem(uint64_t item) {
    return CxlMemDataFrame{
        static_cast<uint16_t>(item >> 48),
        (item >> 16) & kMaxDataPayloadOffset,
        static_cast<uint16_t>(item)};
  }

  static uint64_t encodeAckItem(uint64_t consumedCursor) {
    if (consumedCursor > kMaxAckCursor) {
      throw std::invalid_argument("CXL mem ACK cursor is too large");
    }
    return consumedCursor;
  }

  static uint64_t decodeAckItem(uint64_t item) {
    return item & kMaxAckCursor;
  }
};

} // namespace folly
