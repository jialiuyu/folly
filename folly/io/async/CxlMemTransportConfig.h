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

#include <folly/io/async/CxlMemDoorbellQueue.h>

#include <cstddef>
#include <cstdint>

namespace folly {

class EventBase;

template <typename Backend>
struct CxlMemAsyncTransportConfig {
  EventBase* eventBase{nullptr};
  uint16_t connId{0};

  unsigned char* outboundPayload{nullptr};
  size_t outboundPayloadSize{0};
  uint64_t outboundPayloadBaseOffset{0};

  unsigned char* inboundPayload{nullptr};
  size_t inboundPayloadSize{0};
  uint64_t inboundPayloadBaseOffset{0};

  CxlMemDoorbellQueue<Backend>* outboundDataQueue{nullptr};
  CxlMemDoorbellQueue<Backend>* outboundAckQueue{nullptr};
  CxlMemDoorbellQueue<Backend>* inboundDataQueue{nullptr};
  CxlMemDoorbellQueue<Backend>* inboundAckQueue{nullptr};
};

} // namespace folly
