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
#include <memory>
#include <string>

#include <folly/io/async/BusyPollSharedMemoryTransport.h>
#include <folly/io/async/GqmInterface.h>
#include <folly/io/async/MemoryProvider.h>

namespace folly {

/**
 * Result of a per-connection (legacy) shared memory handshake.
 */
struct ShmHandshakeResult {
  std::unique_ptr<MemoryRegion> writeRegion;
  std::unique_ptr<MemoryRegion> readRegion;
  std::unique_ptr<GqmInterface> gqmWrite;
  std::unique_ptr<GqmInterface> gqmRead;
};

/**
 * Result of a shared-mode handshake (connId exchange only).
 */
struct ShmSharedHandshakeResult {
  uint16_t localConnId{0};
  uint16_t peerConnId{0};
  uint8_t laneId{0};
};

/**
 * Info exchanged over the bootstrap socket during the SHM handshake.
 */
struct ShmHandshakeInfo {
  std::string writeShmName;
  uint64_t dataRegionSize{0};
  uint64_t dataRegionOffset{0};
  std::string gqmWriteName;
  uint32_t gqmQueueDepth{0};
  uint64_t gqmRegionOffset{0};
  uint64_t gqmRegionSize{0};
  uint32_t maxChunkSize{0};
  std::string writePoolName;

  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

/**
 * Client-side SHM handshake (legacy per-connection mode).
 */
ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

/**
 * Server-side SHM handshake (legacy per-connection mode).
 */
ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

/**
 * Client-side shared-mode handshake.
 * Exchanges connId and laneId with the server over the bootstrap socket.
 * Shared GQM / data regions are pre-initialized by ShmPollerService.
 */
ShmSharedHandshakeResult shmHandshakeClientShared(
    EventBase* evb,
    AsyncTransport* sock,
    uint16_t localConnId,
    uint8_t laneId = 0);

/**
 * Server-side shared-mode handshake.
 */
ShmSharedHandshakeResult shmHandshakeServerShared(
    EventBase* evb,
    AsyncTransport* sock,
    uint16_t localConnId);

} // namespace folly
