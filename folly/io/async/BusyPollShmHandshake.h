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

#include <memory>
#include <string>

#include <folly/io/async/BusyPollSharedMemoryTransport.h>
#include <folly/io/async/GqmInterface.h>
#include <folly/io/async/MemoryProvider.h>

namespace folly {

/**
 * Result of a shared memory handshake.
 */
struct ShmHandshakeResult {
  std::unique_ptr<MemoryRegion> writeRegion;
  std::unique_ptr<MemoryRegion> readRegion;
  std::unique_ptr<GqmInterface> gqmWrite;
  std::unique_ptr<GqmInterface> gqmRead;
};

/**
 * Info exchanged over the bootstrap socket during the SHM handshake.
 */
struct ShmHandshakeInfo {
  std::string writeShmName;
  uint64_t dataRegionSize{0};
  std::string gqmWriteName;
  uint32_t gqmQueueDepth{0};
  uint32_t maxChunkSize{0};

  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

/**
 * Client-side SHM handshake.
 *
 * Uses config.memoryProvider (falls back to PosixShmProvider) to
 * create/import data regions.
 */
ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

/**
 * Server-side SHM handshake.
 */
ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

} // namespace folly
