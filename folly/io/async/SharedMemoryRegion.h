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

#include <cstdint>
#include <string>

namespace folly {

/**
 * Configuration for shared-memory transport setup.
 */
struct SharedMemoryTransportConfig {
  size_t dataRegionSize = 4 * 1024 * 1024;
  std::string shmNamePrefix = "/thrift_shm_";
  bool debugLogging = false;
};

/**
 * Handshake info exchanged over the bootstrap socket.
 */
struct SharedMemoryHandshakeInfo {
  std::string writeShmName;
  uint64_t dataRegionSize{0};
  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

} // namespace folly
