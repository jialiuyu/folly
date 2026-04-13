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
#include <folly/io/async/SharedMemoryRegion.h>

namespace folly {

/**
 * Result of a shared memory handshake.
 */
struct ShmHandshakeResult {
  std::unique_ptr<SharedMemoryRegion> writeRegion;
  std::unique_ptr<SharedMemoryRegion> readRegion;
  std::unique_ptr<GqmInterface> gqmWrite; // we push, peer pops
  std::unique_ptr<GqmInterface> gqmRead; // peer pushes, we pop
};

/**
 * Shared memory handshake info exchanged between client and server.
 */
struct ShmHandshakeInfo {
  // Name of the shared memory data region that the sender will write to
  std::string writeShmName;
  // Size of the data region
  uint64_t dataRegionSize{0};
  // Name of the GQM queue that the sender will push to (reader pops)
  std::string gqmWriteName;
  // Depth of the GQM queue
  uint32_t gqmQueueDepth{0};
  // Magic number for validation
  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

/**
 * Perform shared memory handshake on the client side.
 *
 * Flow:
 * 1. Generate unique names for our write data region + GQM write queue
 * 2. Send our handshake info (region names + sizes) to server
 * 3. Receive server's handshake info
 * 4. Create our write data region + GQM write queue
 * 5. Open server's write data region as our read region
 * 6. Open server's GQM write queue as our GQM read queue
 * 7. Close the handshake socket
 *
 * @param evb EventBase for async operations
 * @param sock Connected socket for handshake (Unix domain socket preferred)
 * @param config Transport configuration
 * @return ShmHandshakeResult with regions and GQM queues
 */
ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

/**
 * Perform shared memory handshake on the server side.
 *
 * @param evb EventBase for async operations
 * @param sock Connected socket for handshake
 * @param config Transport configuration
 * @return ShmHandshakeResult with regions and GQM queues
 */
ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

} // namespace folly
