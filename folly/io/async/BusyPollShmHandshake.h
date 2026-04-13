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
#include <folly/io/async/SharedMemoryRegion.h>
#include <folly/io/async/fdsock/AsyncFdSocket.h>

namespace folly {

/**
 * Result of a shared memory handshake.
 */
struct ShmHandshakeResult {
  std::unique_ptr<SharedMemoryRegion> writeRegion;
  std::unique_ptr<SharedMemoryRegion> readRegion;
  int peerEventFd{-1};
};

/**
 * Shared memory handshake info exchanged between client and server.
 */
struct ShmHandshakeInfo {
  // Name of the shared memory region that the sender will write to
  std::string writeShmName;
  // Size of the data region
  uint64_t dataRegionSize{0};
  // Magic number for validation
  static constexpr uint32_t kMagic = 0x53484D54; // "SHMT"
};

/**
 * Perform shared memory handshake on the client side.
 *
 * This function:
 * 1. Generates a unique shared memory region name for the client's write region
 * 2. Sends the client's handshake info (region name + data size) to the server
 * 3. Receives the server's handshake info
 * 4. Creates the client's write region and opens the server's write region
 * 5. Creates a local eventfd and sends it to the server via SCM_RIGHTS
 * 6. Receives the server's eventfd via SCM_RIGHTS
 *
 * @param evb EventBase for async operations
 * @param sock Connected AsyncFdSocket for handshake (must be Unix domain socket)
 * @param config Transport configuration
 * @return ShmHandshakeResult with regions and peer eventfd
 */
ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncFdSocket* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

/**
 * Perform shared memory handshake on the server side.
 *
 * Same flow as shmHandshakeClient but from the server's perspective.
 *
 * @param evb EventBase for async operations
 * @param sock Connected AsyncFdSocket for handshake (must be Unix domain socket)
 * @param config Transport configuration
 * @return ShmHandshakeResult with regions and peer eventfd
 */
ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncFdSocket* sock,
    const BusyPollSharedMemoryTransport::Config& config = {});

} // namespace folly
