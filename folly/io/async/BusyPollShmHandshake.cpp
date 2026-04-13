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

#ifdef __linux__
#include <sys/eventfd.h>
#endif
#include <fcntl.h>
#include <unistd.h>

namespace folly {

namespace {

// Handshake header: magic (4) + name_len (4) + name (variable) + data_size (8)
constexpr size_t kHandshakeMagicSize = 4;
constexpr size_t kHandshakeNameLenSize = 4;
constexpr size_t kHandshakeDataSizeFieldSize = 8;

// Serialize handshake info to IOBuf
std::unique_ptr<IOBuf> serializeHandshakeInfo(const ShmHandshakeInfo& info) {
  size_t bufSize = kHandshakeMagicSize + kHandshakeNameLenSize +
      info.writeShmName.size() + kHandshakeDataSizeFieldSize;
  auto buf = IOBuf::create(bufSize);
  io::Appender appender(buf.get(), 0);

  // Write magic
  appender.writeBE<uint32_t>(ShmHandshakeInfo::kMagic);
  // Write name length and name
  appender.writeBE<uint32_t>(static_cast<uint32_t>(info.writeShmName.size()));
  appender.push(
      reinterpret_cast<const uint8_t*>(info.writeShmName.data()),
      info.writeShmName.size());
  // Write data region size
  appender.writeBE<uint64_t>(info.dataRegionSize);

  return buf;
}

// Deserialize handshake info from IOBuf
bool deserializeHandshakeInfo(
    const IOBuf* buf, ShmHandshakeInfo& info) {
  io::Cursor cursor(buf);

  // Read and verify magic
  auto magic = cursor.readBE<uint32_t>();
  if (magic != ShmHandshakeInfo::kMagic) {
    XLOG(ERR) << "Invalid handshake magic: " << magic;
    return false;
  }

  // Read name
  auto nameLen = cursor.readBE<uint32_t>();
  if (nameLen > 256) { // Sanity check
    XLOG(ERR) << "Handshake name too long: " << nameLen;
    return false;
  }
  info.writeShmName.resize(nameLen);
  cursor.pull(info.writeShmName.data(), nameLen);

  // Read data region size
  info.dataRegionSize = cursor.readBE<uint64_t>();

  return true;
}

// Create a unique shm name using timestamp and pid
std::string generateShmName(
    const std::string& prefix, bool isServer, uint64_t id) {
  return folly::sformat(
      "{}{}_{:x}", prefix, isServer ? "s" : "c", id);
}

// Synchronous write on a socket (for handshake)
bool syncWrite(AsyncFdSocket* sock, std::unique_ptr<IOBuf> buf) {
  class SyncWriteCallback : public AsyncTransport::WriteCallback {
   public:
    void writeSuccess() noexcept override { done_ = true; }
    void writeErr(size_t, const AsyncSocketException& ex) noexcept override {
      error_ = ex.what();
    }
    std::atomic<bool> done_{false};
    std::string error_;
  };

  auto cb = std::make_unique<SyncWriteCallback>();
  sock->writeChain(cb.get(), std::move(buf));

  // Wait for write to complete (simplified polling)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!cb->done_.load(std::memory_order_relaxed)) {
    if (std::chrono::steady_clock::now() > deadline) {
      XLOG(ERR) << "Handshake write timeout";
      return false;
    }
    // Brief yield to avoid burning CPU during handshake
    std::this_thread::yield();
  }

  if (!cb->error_.empty()) {
    XLOG(ERR) << "Handshake write error: " << cb->error_;
    return false;
  }
  return true;
}

// Synchronous read from a socket (for handshake)
bool syncRead(
    EventBase* evb,
    AsyncFdSocket* sock,
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
    // Drive the EventBase to process I/O
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

// Create an eventfd (or pipe on macOS) and return read/write fds
std::pair<int, int> createEventFdPair() {
#ifdef __linux__
  int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw std::runtime_error(
        folly::sformat("Failed to create eventfd: {}", strerror(errno)));
  }
  return {fd, fd}; // eventfd is bidirectional
#else
  int pipefd[2];
  if (::pipe(pipefd) < 0) {
    throw std::runtime_error(
        folly::sformat("Failed to create pipe: {}", strerror(errno)));
  }
  for (int i = 0; i < 2; ++i) {
    int flags = ::fcntl(pipefd[i], F_GETFL);
    ::fcntl(pipefd[i], F_SETFL, flags | O_NONBLOCK);
  }
  return {pipefd[0], pipefd[1]}; // {read_end, write_end}
#endif
}

} // namespace

// ========== Client Handshake ==========

ShmHandshakeResult shmHandshakeClient(
    EventBase* evb,
    AsyncFdSocket* sock,
    const BusyPollSharedMemoryTransport::Config& config) {
  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // Step 1: Create our handshake info
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName = generateShmName(config.shmNamePrefix, false, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;

  // Step 2: Send our handshake info
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(sock, std::move(sendBuf))) {
    throw std::runtime_error("Client handshake: failed to send info");
  }

  // Step 3: Receive peer's handshake info
  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, kHandshakeMagicSize)) {
    throw std::runtime_error("Client handshake: failed to read peer info");
  }
  auto readBuf = readQueue.move();
  ShmHandshakeInfo peerInfo;
  if (!deserializeHandshakeInfo(readBuf.get(), peerInfo)) {
    throw std::runtime_error("Client handshake: invalid peer info");
  }

  // Step 4: Create shared memory regions
  // Our write region (peer reads from this)
  auto writeRegion = SharedMemoryRegion::create(
      myInfo.writeShmName, config.dataRegionSize, true);
  // Our read region (peer writes to this, we read from it)
  auto readRegion = SharedMemoryRegion::create(
      peerInfo.writeShmName, peerInfo.dataRegionSize, false);

  // Step 5: Create eventfd pair and send read end to peer via SCM_RIGHTS
  auto [localReadFd, localWriteFd] = createEventFdPair();
  // The read fd will be watched by our EventHandler (EventBase)
  // The write fd will be used by the busy-poll thread to signal EventBase

  // Send our localReadFd to the peer so they can signal us after writing
  // We send the write-end of a pipe pair on macOS (peer writes to it,
  // our EventBase reads from the read-end). On Linux with eventfd,
  // we send the same fd since eventfd is bidirectional.
  int fdToSend =
#ifdef __linux__
      localReadFd; // eventfd: peer writes to it, we read from it
#else
      localWriteFd; // pipe: peer writes to write-end, we read from read-end
#endif

  {
    SocketFds::ToSend fdsToSend;
    fdsToSend.push_back(
        std::make_shared<const folly::File>(fdToSend));
    SocketFds socketFds(std::move(fdsToSend));
    sock->injectSocketSeqNumIntoFdsToSend(&socketFds);

    // Send a small data message along with the FDs
    auto dummyBuf = IOBuf::create(1);
    io::Appender appender(dummyBuf.get(), 0);
    appender.writeBE<uint8_t>(0x01); // FD marker byte

    sock->writeChainWithFds(
        nullptr, std::move(dummyBuf), std::move(socketFds));
  }

  // Step 6: Receive peer's eventfd via SCM_RIGHTS
  int peerEventFd = -1;
  {
    IOBufQueue fdReadQueue;
    if (!syncRead(evb, sock, fdReadQueue, 1)) {
      throw std::runtime_error("Client handshake: failed to read peer FDs");
    }
    auto peerFds = sock->popNextReceivedFds();
    auto received = peerFds.releaseReceived();
    if (received.size() >= 1) {
      peerEventFd = received[0].fd();
      // Release the fd from the File object so it's not closed on destruction
      received[0].release();
    } else {
      throw std::runtime_error("Client handshake: no peer eventfd received");
    }
  }

  // Step 7: Close the handshake socket
  sock->close();

  XLOG(DBG) << "Client handshake complete: writeRegion="
            << myInfo.writeShmName << ", readRegion="
            << peerInfo.writeShmName;

  return ShmHandshakeResult{
      std::move(writeRegion),
      std::move(readRegion),
      peerEventFd};
}

// ========== Server Handshake ==========

ShmHandshakeResult shmHandshakeServer(
    EventBase* evb,
    AsyncFdSocket* sock,
    const BusyPollSharedMemoryTransport::Config& config) {
  uint64_t uniqueId = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

  // Step 1: Receive client's handshake info
  IOBufQueue readQueue;
  if (!syncRead(evb, sock, readQueue, kHandshakeMagicSize)) {
    throw std::runtime_error("Server handshake: failed to read client info");
  }
  auto readBuf = readQueue.move();
  ShmHandshakeInfo clientInfo;
  if (!deserializeHandshakeInfo(readBuf.get(), clientInfo)) {
    throw std::runtime_error("Server handshake: invalid client info");
  }

  // Step 2: Create our handshake info
  ShmHandshakeInfo myInfo;
  myInfo.writeShmName = generateShmName(config.shmNamePrefix, true, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;

  // Step 3: Send our handshake info
  auto sendBuf = serializeHandshakeInfo(myInfo);
  if (!syncWrite(sock, std::move(sendBuf))) {
    throw std::runtime_error("Server handshake: failed to send info");
  }

  // Step 4: Create shared memory regions
  // Our write region (client reads from this)
  auto writeRegion = SharedMemoryRegion::create(
      myInfo.writeShmName, config.dataRegionSize, true);
  // Our read region (client writes to this, we read from it)
  auto readRegion = SharedMemoryRegion::create(
      clientInfo.writeShmName, clientInfo.dataRegionSize, false);

  // Step 5: Create eventfd pair and send to client via SCM_RIGHTS
  auto [localReadFd, localWriteFd] = createEventFdPair();

  int fdToSend =
#ifdef __linux__
      localReadFd;
#else
      localWriteFd;
#endif

  {
    SocketFds::ToSend fdsToSend;
    fdsToSend.push_back(
        std::make_shared<const folly::File>(fdToSend));
    SocketFds socketFds(std::move(fdsToSend));
    sock->injectSocketSeqNumIntoFdsToSend(&socketFds);

    auto dummyBuf = IOBuf::create(1);
    io::Appender appender(dummyBuf.get(), 0);
    appender.writeBE<uint8_t>(0x01);

    sock->writeChainWithFds(
        nullptr, std::move(dummyBuf), std::move(socketFds));
  }

  // Step 6: Receive client's eventfd via SCM_RIGHTS
  int peerEventFd = -1;
  {
    IOBufQueue fdReadQueue;
    if (!syncRead(evb, sock, fdReadQueue, 1)) {
      throw std::runtime_error("Server handshake: failed to read client FDs");
    }
    auto peerFds = sock->popNextReceivedFds();
    auto received = peerFds.releaseReceived();
    if (received.size() >= 1) {
      peerEventFd = received[0].fd();
      received[0].release();
    } else {
      throw std::runtime_error("Server handshake: no client eventfd received");
    }
  }

  // Step 7: Close the handshake socket
  sock->close();

  XLOG(DBG) << "Server handshake complete: writeRegion="
            << myInfo.writeShmName << ", readRegion="
            << clientInfo.writeShmName;

  return ShmHandshakeResult{
      std::move(writeRegion),
      std::move(readRegion),
      peerEventFd};
}

} // namespace folly
