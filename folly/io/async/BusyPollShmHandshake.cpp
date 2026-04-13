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

#include <folly/io/async/BusyPollShmHandshake.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>

namespace folly {

namespace {

// Serialize handshake info to IOBuf
std::unique_ptr<IOBuf> serializeHandshakeInfo(const ShmHandshakeInfo& info) {
  size_t bufSize = 4 + // magic
      4 + info.writeShmName.size() + // name len + name
      8 + // dataRegionSize
      4 + info.gqmWriteName.size() + // gqm name len + name
      4; // gqmQueueDepth
  auto buf = IOBuf::create(bufSize);
  io::Appender appender(buf.get(), 0);

  appender.writeBE<uint32_t>(ShmHandshakeInfo::kMagic);
  appender.writeBE<uint32_t>(static_cast<uint32_t>(info.writeShmName.size()));
  appender.push(
      reinterpret_cast<const uint8_t*>(info.writeShmName.data()),
      info.writeShmName.size());
  appender.writeBE<uint64_t>(info.dataRegionSize);
  appender.writeBE<uint32_t>(static_cast<uint32_t>(info.gqmWriteName.size()));
  appender.push(
      reinterpret_cast<const uint8_t*>(info.gqmWriteName.data()),
      info.gqmWriteName.size());
  appender.writeBE<uint32_t>(info.gqmQueueDepth);

  return buf;
}

// Deserialize handshake info from IOBuf
bool deserializeHandshakeInfo(const IOBuf* buf, ShmHandshakeInfo& info) {
  io::Cursor cursor(buf);

  auto magic = cursor.readBE<uint32_t>();
  if (magic != ShmHandshakeInfo::kMagic) {
    XLOG(ERR) << "Invalid handshake magic: " << magic;
    return false;
  }

  auto nameLen = cursor.readBE<uint32_t>();
  if (nameLen > 256) {
    XLOG(ERR) << "Handshake name too long: " << nameLen;
    return false;
  }
  info.writeShmName.resize(nameLen);
  cursor.pull(info.writeShmName.data(), nameLen);

  info.dataRegionSize = cursor.readBE<uint64_t>();

  auto gqmNameLen = cursor.readBE<uint32_t>();
  if (gqmNameLen > 256) {
    XLOG(ERR) << "GQM name too long: " << gqmNameLen;
    return false;
  }
  info.gqmWriteName.resize(gqmNameLen);
  cursor.pull(info.gqmWriteName.data(), gqmNameLen);

  info.gqmQueueDepth = cursor.readBE<uint32_t>();

  return true;
}

// Generate a unique shm name
std::string generateShmName(
    const std::string& prefix, bool isServer, uint64_t id) {
  return folly::sformat("{}{}_{:x}", prefix, isServer ? "s" : "c", id);
}

// Synchronous write on a socket (for handshake)
bool syncWrite(
    EventBase* evb,
    AsyncTransport* sock,
    std::unique_ptr<IOBuf> buf) {
  class SyncWriteCallback : public AsyncTransport::WriteCallback {
   public:
    void writeSuccess() noexcept override { done_ = true; }
    void writeErr(size_t, const AsyncSocketException& ex) noexcept override {
      error_ = ex.what();
    }
    std::atomic<bool> done_{false};
    std::string error_;
  };

  SyncWriteCallback cb;
  sock->writeChain(&cb, std::move(buf));

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!cb.done_.load(std::memory_order_relaxed)) {
    if (std::chrono::steady_clock::now() > deadline) {
      XLOG(ERR) << "Handshake write timeout";
      return false;
    }
    evb->loopOnce(EVLOOP_NONBLOCK);
  }

  if (!cb.error_.empty()) {
    XLOG(ERR) << "Handshake write error: " << cb.error_;
    return false;
  }
  return true;
}

// Synchronous read from a socket (for handshake)
bool syncRead(
    EventBase* evb,
    AsyncTransport* sock,
    IOBufQueue& queue,
    size_t minBytes) {
  class SyncReadCallback : public AsyncTransport::ReadCallback {
   public:
    explicit SyncReadCallback(IOBufQueue& q) : queue_(q) {}

    void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
      auto buf = queue_.preallocate(4096, 4096);
      *bufReturn = buf.first;
      *lenReturn = buf.second;
    }

    void readDataAvailable(size_t len) noexcept override {
      queue_.postallocate(len);
    }

    void readEOF() noexcept override { eof_ = true; }
    void readErr(const AsyncSocketException& ex) noexcept override {
      error_ = ex.what();
    }
    bool isBufferMovable() noexcept override { return true; }
    void readBufferAvailable(
        std::unique_ptr<IOBuf> readBuf) noexcept override {
      queue_.append(std::move(readBuf));
    }

    IOBufQueue& queue_;
    std::atomic<bool> eof_{false};
    std::string error_;
  };

  SyncReadCallback readCb(queue);
  sock->setReadCB(&readCb);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (queue.chainLength() < minBytes && !readCb.eof_ &&
         std::chrono::steady_clock::now() < deadline) {
    evb->loopOnce(EVLOOP_NONBLOCK);
  }

  sock->setReadCB(nullptr);

  if (!readCb.error_.empty()) {
    XLOG(ERR) << "Handshake read error: " << readCb.error_;
    return false;
  }
  if (readCb.eof_) {
    XLOG(ERR) << "Handshake read EOF";
    return false;
  }
  return queue.chainLength() >= minBytes;
}

} // namespace

// ========== Client Handshake ==========

ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config) {
  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // Step 1: Create our handshake info
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName =
      generateShmName(config.shmNamePrefix, false, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;
  myInfo.gqmWriteName =
      generateShmName("/thrift_gqm_", false, uniqueId);
  myInfo.gqmQueueDepth = SharedMemoryGqm::kDefaultQueueDepth;

  // Step 2: Send our handshake info
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error("Client handshake: failed to send info");
  }

  // Step 3: Receive peer's handshake info
  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, 4)) { // at least magic
    throw std::runtime_error("Client handshake: failed to read peer info");
  }
  auto readBuf = readQueue.move();
  ShmHandshakeInfo peerInfo;
  if (!deserializeHandshakeInfo(readBuf.get(), peerInfo)) {
    throw std::runtime_error("Client handshake: invalid peer info");
  }

  // Step 4: Create shared memory data regions
  auto writeRegion = SharedMemoryRegion::create(
      myInfo.writeShmName, config.dataRegionSize, true);
  auto readRegion = SharedMemoryRegion::create(
      peerInfo.writeShmName, peerInfo.dataRegionSize, false);

  // Step 5: Create GQM queues
  auto gqmWrite = SharedMemoryGqm::create(
      myInfo.gqmWriteName, myInfo.gqmQueueDepth);
  auto gqmRead = SharedMemoryGqm::open(
      peerInfo.gqmWriteName, peerInfo.gqmQueueDepth);

  // Step 6: Close the handshake socket
  sock->close();

  XLOG(DBG) << "Client handshake complete: writeRegion="
            << myInfo.writeShmName << ", gqmWrite=" << myInfo.gqmWriteName;

  return ShmHandshakeResult{
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmWrite),
      std::move(gqmRead)};
}

// ========== Server Handshake ==========

ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config) {
  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // Step 1: Receive client's handshake info
  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, 4)) {
    throw std::runtime_error("Server handshake: failed to read client info");
  }
  auto readBuf = readQueue.move();
  ShmHandshakeInfo clientInfo;
  if (!deserializeHandshakeInfo(readBuf.get(), clientInfo)) {
    throw std::runtime_error("Server handshake: invalid client info");
  }

  // Step 2: Create our handshake info
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName =
      generateShmName(config.shmNamePrefix, true, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;
  myInfo.gqmWriteName =
      generateShmName("/thrift_gqm_", true, uniqueId);
  myInfo.gqmQueueDepth = SharedMemoryGqm::kDefaultQueueDepth;

  // Step 3: Send our handshake info
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error("Server handshake: failed to send info");
  }

  // Step 4: Create shared memory data regions
  auto writeRegion = SharedMemoryRegion::create(
      myInfo.writeShmName, config.dataRegionSize, true);
  auto readRegion = SharedMemoryRegion::create(
      clientInfo.writeShmName, clientInfo.dataRegionSize, false);

  // Step 5: Create GQM queues
  auto gqmWrite = SharedMemoryGqm::create(
      myInfo.gqmWriteName, myInfo.gqmQueueDepth);
  auto gqmRead = SharedMemoryGqm::open(
      clientInfo.gqmWriteName, clientInfo.gqmQueueDepth);

  // Step 6: Close the handshake socket
  sock->close();

  XLOG(DBG) << "Server handshake complete: writeRegion="
            << myInfo.writeShmName << ", gqmWrite=" << myInfo.gqmWriteName;

  return ShmHandshakeResult{
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmWrite),
      std::move(gqmRead)};
}

} // namespace folly
