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
#include <folly/io/async/ImportedMemoryProvider.h>
#include <folly/io/async/PosixShmProvider.h>
#include <folly/logging/xlog.h>

namespace folly {

namespace {

constexpr uint32_t kHandshakeVersion = 3; // v3: pool offsets + GQM on shared mem
constexpr size_t kFrameHeaderSize = 4;

// ---- Serialization helpers ----

void writeString(io::Appender& a, const std::string& s) {
  a.writeBE<uint32_t>(static_cast<uint32_t>(s.size()));
  a.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool readString(io::Cursor& c, std::string& s, uint32_t maxLen = 256) {
  auto len = c.readBE<uint32_t>();
  if (len > maxLen) {
    XLOG(ERR) << "Handshake string too long: " << len;
    return false;
  }
  s.resize(len);
  c.pull(s.data(), len);
  return true;
}

std::unique_ptr<IOBuf> serializeHandshakeInfo(const ShmHandshakeInfo& info) {
  size_t payloadSize =
      4 + // magic
      4 + // version
      (4 + info.writeShmName.size()) +
      8 + // dataRegionSize
      8 + // dataRegionOffset
      (4 + info.gqmWriteName.size()) +
      4 + // gqmQueueDepth
      8 + // gqmRegionOffset
      8 + // gqmRegionSize
      4 + // maxChunkSize
      (4 + info.writePoolName.size());

  auto buf = IOBuf::create(kFrameHeaderSize + payloadSize);
  io::Appender appender(buf.get(), 0);

  appender.writeBE<uint32_t>(static_cast<uint32_t>(payloadSize));
  appender.writeBE<uint32_t>(ShmHandshakeInfo::kMagic);
  appender.writeBE<uint32_t>(kHandshakeVersion);
  writeString(appender, info.writeShmName);
  appender.writeBE<uint64_t>(info.dataRegionSize);
  appender.writeBE<uint64_t>(info.dataRegionOffset);
  writeString(appender, info.gqmWriteName);
  appender.writeBE<uint32_t>(info.gqmQueueDepth);
  appender.writeBE<uint64_t>(info.gqmRegionOffset);
  appender.writeBE<uint64_t>(info.gqmRegionSize);
  appender.writeBE<uint32_t>(info.maxChunkSize);
  writeString(appender, info.writePoolName);

  return buf;
}

bool deserializeHandshakeInfo(const IOBuf* buf, ShmHandshakeInfo& info) {
  io::Cursor cursor(buf);

  auto magic = cursor.readBE<uint32_t>();
  if (magic != ShmHandshakeInfo::kMagic) {
    XLOG(ERR) << "Invalid handshake magic: 0x" << std::hex << magic;
    return false;
  }

  auto version = cursor.readBE<uint32_t>();
  if (version < 1 || version > kHandshakeVersion) {
    XLOG(ERR) << "Unsupported handshake version: " << version;
    return false;
  }

  if (!readString(cursor, info.writeShmName)) return false;
  info.dataRegionSize = cursor.readBE<uint64_t>();

  if (version >= 3) {
    info.dataRegionOffset = cursor.readBE<uint64_t>();
  }

  if (!readString(cursor, info.gqmWriteName)) return false;
  info.gqmQueueDepth = cursor.readBE<uint32_t>();

  if (version >= 3) {
    info.gqmRegionOffset = cursor.readBE<uint64_t>();
    info.gqmRegionSize = cursor.readBE<uint64_t>();
  }

  if (version >= 2) {
    info.maxChunkSize = cursor.readBE<uint32_t>();
  } else {
    info.maxChunkSize = GqmNotification::kMaxChunkSize;
  }

  if (version >= 3) {
    if (!readString(cursor, info.writePoolName)) return false;
  }

  return true;
}

std::string generateShmName(
    const std::string& prefix, bool isServer, uint64_t id) {
  return folly::sformat(
      "{}{}_{:x}_{}", prefix, isServer ? "s" : "c", id, ::getpid());
}

// ---- Synchronous socket helpers (handshake only) ----

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
  while (!cb.done_.load(std::memory_order_relaxed) && cb.error_.empty()) {
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

bool readFramedHandshake(
    EventBase* evb,
    AsyncTransport* sock,
    ShmHandshakeInfo& info) {
  IOBufQueue readQueue;

  if (!syncRead(evb, sock, readQueue, kFrameHeaderSize)) {
    XLOG(ERR) << "Failed to read handshake frame header";
    return false;
  }

  io::Cursor headerCursor(readQueue.front());
  uint32_t payloadLen = headerCursor.readBE<uint32_t>();
  if (payloadLen > 4096) {
    XLOG(ERR) << "Handshake payload too large: " << payloadLen;
    return false;
  }

  size_t totalNeeded = kFrameHeaderSize + payloadLen;
  if (readQueue.chainLength() < totalNeeded) {
    if (!syncRead(evb, sock, readQueue, totalNeeded)) {
      XLOG(ERR) << "Failed to read full handshake payload";
      return false;
    }
  }

  readQueue.trimStart(kFrameHeaderSize);
  auto payloadBuf = readQueue.move();
  return deserializeHandshakeInfo(payloadBuf.get(), info);
}

MemoryProvider& resolveProvider(
    const BusyPollSharedMemoryTransport::Config& config) {
  static PosixShmProvider defaultProvider;
  if (config.memoryProvider) {
    return *config.memoryProvider;
  }
  return defaultProvider;
}

} // namespace

// ========== Client Handshake ==========

ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncTransport* sock,
    const BusyPollSharedMemoryTransport::Config& config) {
  auto& provider = resolveProvider(config);
  bool sharedBacking = provider.usesSharedBackingStore();

  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // ---- Build local info ----
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName =
      generateShmName(config.shmNamePrefix, false, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;
  myInfo.gqmWriteName =
      generateShmName("/thrift_gqm_", false, uniqueId);
  myInfo.gqmQueueDepth = SharedMemoryGqm::kDefaultQueueDepth;
  myInfo.maxChunkSize = config.maxChunkSize;
  myInfo.writePoolName = config.writePoolName;

  // ---- Allocate my write data + GQM ----
  std::unique_ptr<MemoryRegion> writeRegion;
  std::unique_ptr<GqmInterface> gqmWrite;

  if (sharedBacking) {
    auto& imported = static_cast<ImportedMemoryProvider&>(provider);
    writeRegion = imported.createFromPool(
        config.writePoolName, myInfo.writeShmName, config.dataRegionSize, 1);
    myInfo.dataRegionOffset = writeRegion->offset();

    auto gqmRegion = imported.createFromPool(
        config.writePoolName, myInfo.gqmWriteName,
        SharedMemoryGqm::kGqmRegionSize, 4096);
    myInfo.gqmRegionOffset = gqmRegion->offset();
    myInfo.gqmRegionSize = gqmRegion->size();
    gqmWrite = ImportedGqm::create(std::move(gqmRegion));
  } else {
    writeRegion =
        provider.create(myInfo.writeShmName, config.dataRegionSize);
    gqmWrite = SharedMemoryGqm::create(
        myInfo.gqmWriteName, myInfo.gqmQueueDepth);
  }

  // ---- Exchange ----
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error("Client handshake: failed to send info");
  }

  ShmHandshakeInfo peerInfo;
  if (!readFramedHandshake(evb, sock, peerInfo)) {
    throw std::runtime_error("Client handshake: failed to read peer info");
  }

  // ---- Import peer's write region + GQM ----
  std::unique_ptr<MemoryRegion> readRegion;
  std::unique_ptr<GqmInterface> gqmRead;

  if (sharedBacking) {
    auto& imported = static_cast<ImportedMemoryProvider&>(provider);
    readRegion = imported.importFromPool(
        config.readPoolName,
        peerInfo.writeShmName,
        peerInfo.dataRegionOffset,
        peerInfo.dataRegionSize);

    auto gqmRegion = imported.importFromPool(
        config.readPoolName,
        peerInfo.gqmWriteName,
        peerInfo.gqmRegionOffset,
        peerInfo.gqmRegionSize);
    gqmRead = ImportedGqm::open(std::move(gqmRegion));
  } else {
    readRegion =
        provider.import(peerInfo.writeShmName, peerInfo.dataRegionSize);
    gqmRead = SharedMemoryGqm::open(
        peerInfo.gqmWriteName, peerInfo.gqmQueueDepth);
  }

  sock->close();

  XLOG(DBG5) << "Client handshake complete: writeRegion="
             << myInfo.writeShmName
             << " (offset=" << myInfo.dataRegionOffset << ")"
             << ", maxChunkSize=" << myInfo.maxChunkSize
             << ", sharedBacking=" << sharedBacking;

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
  auto& provider = resolveProvider(config);
  bool sharedBacking = provider.usesSharedBackingStore();

  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // ---- Read client info first ----
  ShmHandshakeInfo clientInfo;
  if (!readFramedHandshake(evb, sock, clientInfo)) {
    throw std::runtime_error("Server handshake: failed to read client info");
  }

  // ---- Build my info ----
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName =
      generateShmName(config.shmNamePrefix, true, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;
  myInfo.gqmWriteName =
      generateShmName("/thrift_gqm_", true, uniqueId);
  myInfo.gqmQueueDepth = SharedMemoryGqm::kDefaultQueueDepth;
  myInfo.maxChunkSize = config.maxChunkSize;
  myInfo.writePoolName = config.writePoolName;

  // ---- Allocate my write data + GQM ----
  std::unique_ptr<MemoryRegion> writeRegion;
  std::unique_ptr<GqmInterface> gqmWrite;

  if (sharedBacking) {
    auto& imported = static_cast<ImportedMemoryProvider&>(provider);
    writeRegion = imported.createFromPool(
        config.writePoolName, myInfo.writeShmName, config.dataRegionSize, 1);
    myInfo.dataRegionOffset = writeRegion->offset();

    auto gqmRegion = imported.createFromPool(
        config.writePoolName, myInfo.gqmWriteName,
        SharedMemoryGqm::kGqmRegionSize, 4096);
    myInfo.gqmRegionOffset = gqmRegion->offset();
    myInfo.gqmRegionSize = gqmRegion->size();
    gqmWrite = ImportedGqm::create(std::move(gqmRegion));
  } else {
    writeRegion =
        provider.create(myInfo.writeShmName, config.dataRegionSize);
    gqmWrite = SharedMemoryGqm::create(
        myInfo.gqmWriteName, myInfo.gqmQueueDepth);
  }

  // ---- Send my info ----
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error("Server handshake: failed to send info");
  }

  // ---- Import client's write region + GQM ----
  std::unique_ptr<MemoryRegion> readRegion;
  std::unique_ptr<GqmInterface> gqmRead;

  if (sharedBacking) {
    auto& imported = static_cast<ImportedMemoryProvider&>(provider);
    readRegion = imported.importFromPool(
        config.readPoolName,
        clientInfo.writeShmName,
        clientInfo.dataRegionOffset,
        clientInfo.dataRegionSize);

    auto gqmRegion = imported.importFromPool(
        config.readPoolName,
        clientInfo.gqmWriteName,
        clientInfo.gqmRegionOffset,
        clientInfo.gqmRegionSize);
    gqmRead = ImportedGqm::open(std::move(gqmRegion));
  } else {
    readRegion =
        provider.import(clientInfo.writeShmName, clientInfo.dataRegionSize);
    gqmRead = SharedMemoryGqm::open(
        clientInfo.gqmWriteName, clientInfo.gqmQueueDepth);
  }

  sock->close();

  XLOG(DBG5) << "Server handshake complete: writeRegion="
             << myInfo.writeShmName
             << " (offset=" << myInfo.dataRegionOffset << ")"
             << ", maxChunkSize=" << myInfo.maxChunkSize
             << ", sharedBacking=" << sharedBacking;

  return ShmHandshakeResult{
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmWrite),
      std::move(gqmRead)};
}

// ========== Shared-mode handshakes (connId exchange only) ==========

namespace {

constexpr uint32_t kSharedHandshakeMagic = 0x53484D53; // "SHMS"

std::unique_ptr<IOBuf> serializeConnId(uint16_t connId) {
  auto buf = IOBuf::create(kFrameHeaderSize + 8);
  io::Appender appender(buf.get(), 0);
  appender.writeBE<uint32_t>(8);
  appender.writeBE<uint32_t>(kSharedHandshakeMagic);
  appender.writeBE<uint16_t>(connId);
  appender.writeBE<uint16_t>(0); // reserved
  return buf;
}

bool deserializeConnId(const IOBuf* buf, uint16_t& connId) {
  io::Cursor cursor(buf);
  auto magic = cursor.readBE<uint32_t>();
  if (magic != kSharedHandshakeMagic) {
    XLOG(ERR) << "Invalid shared handshake magic: 0x" << std::hex << magic;
    return false;
  }
  connId = cursor.readBE<uint16_t>();
  return true;
}

} // namespace

ShmSharedHandshakeResult shmHandshakeClientShared(
    EventBase* evb,
    AsyncTransport* sock,
    uint16_t localConnId) {
  auto sendBuf = serializeConnId(localConnId);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error(
        "Shared handshake client: failed to send connId");
  }

  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, kFrameHeaderSize)) {
    throw std::runtime_error(
        "Shared handshake client: failed to read frame header");
  }

  io::Cursor headerCursor(readQueue.front());
  uint32_t payloadLen = headerCursor.readBE<uint32_t>();
  size_t totalNeeded = kFrameHeaderSize + payloadLen;
  if (readQueue.chainLength() < totalNeeded) {
    if (!syncRead(evb, sock, readQueue, totalNeeded)) {
      throw std::runtime_error(
          "Shared handshake client: failed to read payload");
    }
  }

  readQueue.trimStart(kFrameHeaderSize);
  auto payloadBuf = readQueue.move();
  uint16_t peerConnId = 0;
  if (!deserializeConnId(payloadBuf.get(), peerConnId)) {
    throw std::runtime_error(
        "Shared handshake client: failed to deserialize peer connId");
  }

  sock->close();

  XLOG(DBG5) << "Shared handshake client complete: localConnId="
             << localConnId << ", peerConnId=" << peerConnId;

  return ShmSharedHandshakeResult{localConnId, peerConnId};
}

ShmSharedHandshakeResult shmHandshakeServerShared(
    EventBase* evb,
    AsyncTransport* sock,
    uint16_t localConnId) {
  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, kFrameHeaderSize)) {
    throw std::runtime_error(
        "Shared handshake server: failed to read frame header");
  }

  io::Cursor headerCursor(readQueue.front());
  uint32_t payloadLen = headerCursor.readBE<uint32_t>();
  size_t totalNeeded = kFrameHeaderSize + payloadLen;
  if (readQueue.chainLength() < totalNeeded) {
    if (!syncRead(evb, sock, readQueue, totalNeeded)) {
      throw std::runtime_error(
          "Shared handshake server: failed to read payload");
    }
  }

  readQueue.trimStart(kFrameHeaderSize);
  auto payloadBuf = readQueue.move();
  uint16_t peerConnId = 0;
  if (!deserializeConnId(payloadBuf.get(), peerConnId)) {
    throw std::runtime_error(
        "Shared handshake server: failed to deserialize peer connId");
  }

  auto sendBuf = serializeConnId(localConnId);
  if (!syncWrite(evb, sock, std::move(sendBuf))) {
    throw std::runtime_error(
        "Shared handshake server: failed to send connId");
  }

  sock->close();

  XLOG(DBG5) << "Shared handshake server complete: localConnId="
             << localConnId << ", peerConnId=" << peerConnId;

  return ShmSharedHandshakeResult{localConnId, peerConnId};
}

} // namespace folly
