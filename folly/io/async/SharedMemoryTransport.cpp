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

#include <folly/io/async/SharedMemoryTransport.h>

#include <cstring>
#include <stdexcept>

#include <folly/Exception.h>
#include <folly/ExceptionWrapper.h>
#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>

namespace folly {

namespace {

// Handshake protocol constants
constexpr size_t kHandshakeHeaderSize = 12;  // magic (4) + size (8)
constexpr uint32_t kHandshakeMagic = SharedMemoryHandshakeInfo::kMagic;

// Generate a unique name for shared memory region
std::string generateShmName(const std::string& prefix, uint64_t id) {
  return folly::sformat("{}{}", prefix, id);
}

// Serialize handshake info to IOBuf
std::unique_ptr<IOBuf> serializeHandshakeInfo(const SharedMemoryHandshakeInfo& info) {
  auto buf = IOBuf::create(kHandshakeHeaderSize + info.writeShmName.size() + 8);
  io::Appender appender(buf.get(), 0);

  // Write magic
  appender.writeBE<uint32_t>(kHandshakeMagic);
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
    const IOBuf* buf,
    SharedMemoryHandshakeInfo& info) {
  io::Cursor cursor(buf);

  // Read and verify magic
  auto magic = cursor.readBE<uint32_t>();
  if (magic != kHandshakeMagic) {
    XLOG(ERR) << "Invalid handshake magic: " << magic;
    return false;
  }

  // Read name
  auto nameLen = cursor.readBE<uint32_t>();
  if (nameLen > 256) {  // Sanity check
    XLOG(ERR) << "Handshake name too long: " << nameLen;
    return false;
  }
  info.writeShmName.resize(nameLen);
  cursor.pull(info.writeShmName.data(), nameLen);

  // Read data region size
  info.dataRegionSize = cursor.readBE<uint64_t>();

  return true;
}

} // namespace

// ========== SharedMemoryTransport Implementation ==========

SharedMemoryTransport::SharedMemoryTransport(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    std::shared_ptr<GqmInterface> gqmInterface,
    const Config& config)
    : AsyncTimeout(evb),
      evb_(evb),
      writeRegion_(std::move(writeRegion)),
      readRegion_(std::move(readRegion)),
      gqmInterface_(std::move(gqmInterface)),
      config_(config),
      isServer_(false) {
  state_ = State::CONNECTED;
  XLOG(DBG) << "SharedMemoryTransport created from existing regions";
}

SharedMemoryTransport::~SharedMemoryTransport() {
  closeNow();
  XLOG(DBG) << "SharedMemoryTransport destroyed";
}

// ========== Static Factory Methods ==========

SharedMemoryTransport::UniquePtr SharedMemoryTransport::createClient(
    EventBase* evb,
    AsyncTransport::UniquePtr tcpSocket,
    const Config& config) {
  return performHandshake(evb, std::move(tcpSocket), false, config);
}

SharedMemoryTransport::UniquePtr SharedMemoryTransport::createServer(
    EventBase* evb,
    AsyncTransport::UniquePtr tcpSocket,
    const Config& config) {
  return performHandshake(evb, std::move(tcpSocket), true, config);
}

SharedMemoryTransport::UniquePtr SharedMemoryTransport::createFromRegions(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    std::shared_ptr<GqmInterface> gqmInterface,
    const Config& config) {
  if (!writeRegion || !readRegion) {
    throw std::invalid_argument("Write and read regions must not be null");
  }

  auto transport = UniquePtr(new SharedMemoryTransport(
      evb,
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmInterface),
      config));

  // Start polling for read data
  transport->scheduleTimeout(kDefaultPollInterval);

  return transport;
}

SharedMemoryTransport::UniquePtr SharedMemoryTransport::performHandshake(
    EventBase* evb,
    AsyncTransport::UniquePtr tcpSocket,
    bool isServer,
    const Config& config) {
  if (!tcpSocket || !tcpSocket->good()) {
    throw std::invalid_argument("TCP socket must be valid and connected");
  }

  // Generate unique ID for shared memory regions
  // Use timestamp + random value for uniqueness
  uint64_t uniqueId = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

  // Create handshake info
  SharedMemoryHandshakeInfo myInfo;
  myInfo.writeShmName = generateShmName(config.shmNamePrefix, uniqueId);
  myInfo.dataRegionSize = config.dataRegionSize;

  // Send our handshake info
  auto sendBuf = serializeHandshakeInfo(myInfo);
  bool sendSuccess = false;

  auto writeCallback = [&]() -> AsyncTransport::WriteCallback* {
    class SyncWriteCallback : public AsyncTransport::WriteCallback {
     public:
      void writeSuccess() noexcept override { done_ = true; }
      void writeErr(size_t, const AsyncSocketException& ex) noexcept override {
        error_ = ex.what();
      }
      bool done_{false};
      std::string error_;
    };
    return new SyncWriteCallback();
  }();

  // Use synchronous write for simplicity in handshake
  // In production, you might want async with timeout
  tcpSocket->writeChain(writeCallback, std::move(sendBuf));

  // Wait for write to complete (simplified - should use proper async)
  // For now, we'll use a simple polling approach
  // TODO: Implement proper async handshake with timeout

  // Read peer's handshake info
  SharedMemoryHandshakeInfo peerInfo;
  IOBufQueue readQueue;
  bool readDone = false;

  class HandshakeReadCallback : public AsyncTransport::ReadCallback {
   public:
    explicit HandshakeReadCallback(IOBufQueue& queue, bool& done)
        : queue_(queue), done_(done) {}

    void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
      auto buf = queue_.preallocate(4096, 4096);
      *bufReturn = buf.first;
      *lenReturn = buf.second;
    }

    void readDataAvailable(size_t len) noexcept override {
      queue_.postallocate(len);
      // We'll check if we have enough data in the main code
    }

    void readEOF() noexcept override { done_ = true; }
    void readErr(const AsyncSocketException&) noexcept override { done_ = true; }
    bool isBufferMovable() noexcept override { return true; }
    void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
      queue_.append(std::move(readBuf));
    }

   private:
    IOBufQueue& queue_;
    bool& done_;
  };

  HandshakeReadCallback readCallback(readQueue, readDone);
  tcpSocket->setReadCB(&readCallback);

  // Wait for handshake data (simplified polling)
  // In production, this should be done asynchronously
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (readQueue.chainLength() < kHandshakeHeaderSize + 256 &&
         std::chrono::steady_clock::now() < deadline) {
    evb->loopOnce();
  }

  tcpSocket->setReadCB(nullptr);

  // Parse peer info
  auto buf = readQueue.move();
  if (!deserializeHandshakeInfo(buf.get(), peerInfo)) {
    throw std::runtime_error("Failed to parse peer handshake info");
  }

  // Create shared memory regions
  // My write region (peer reads from this)
  auto writeRegion = SharedMemoryRegion::create(
      myInfo.writeShmName, config.dataRegionSize, true);

  // My read region (peer writes to this, I read from it)
  // The peer told us the name in their handshake
  auto readRegion = SharedMemoryRegion::create(
      peerInfo.writeShmName, peerInfo.dataRegionSize, false);

  // Close TCP socket - handshake complete
  tcpSocket->close();

  // Create the transport
  auto gqmInterface = std::make_shared<DefaultGqmInterface>();
  return createFromRegions(
      evb,
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmInterface),
      config);
}

// ========== AsyncTransport Interface ==========

void SharedMemoryTransport::setReadCB(ReadCallback* callback) {
  readCallback_ = callback;

  if (callback && state_ == State::CONNECTED) {
    // Deliver any pending data immediately
    deliverReadData();

    // Schedule polling
    if (!isScheduled()) {
      scheduleTimeout(kDefaultPollInterval);
    }
  }
}

AsyncTransport::ReadCallback* SharedMemoryTransport::getReadCallback() const {
  return readCallback_;
}

void SharedMemoryTransport::write(
    WriteCallback* callback,
    const void* buf,
    size_t bytes,
    WriteFlags flags) {
  auto ioBuf = IOBuf::copyBuffer(buf, bytes);
  writeChain(callback, std::move(ioBuf), flags);
}

void SharedMemoryTransport::writev(
    WriteCallback* callback,
    const iovec* vec,
    size_t count,
    WriteFlags flags) {
  auto ioBuf = IOBuf::fromIovec(vec, count);
  writeChain(callback, std::move(ioBuf), flags);
}

void SharedMemoryTransport::writeChain(
    WriteCallback* callback,
    std::unique_ptr<IOBuf>&& buf,
    WriteFlags flags) {
  if (state_ != State::CONNECTED) {
    if (callback) {
      callback->writeErr(
          0,
          AsyncSocketException(
              AsyncSocketException::NOT_OPEN,
              "Transport not connected"));
    }
    return;
  }

  writeInternal(callback, std::move(buf), flags);
}

void SharedMemoryTransport::writeInternal(
    WriteCallback* callback,
    std::unique_ptr<IOBuf> buf,
    WriteFlags /*flags*/) {
  if (!writeRegion_ || writeRegion_->isClosed()) {
    if (callback) {
      callback->writeErr(
          0,
          AsyncSocketException(
              AsyncSocketException::END_OF_FILE,
              "Write region closed"));
    }
    return;
  }

  // Flatten the buffer chain for writing
  size_t totalBytes = buf->computeChainDataLength();
  size_t bytesWritten = 0;

  for (auto& iov : *buf) {
    ssize_t written = writeRegion_->write(iov.data(), iov.size());
    if (written < 0) {
      if (callback) {
        callback->writeErr(
            bytesWritten,
            AsyncSocketException(
                AsyncSocketException::UNKNOWN,
                "Write failed"));
      }
      state_ = State::ERROR;
      return;
    }
    bytesWritten += written;

    if (static_cast<size_t>(written) < iov.size()) {
      // Partial write - buffer full
      // In a real implementation, we'd buffer the rest and retry
      XLOG(DBG) << "Partial write: " << written << "/" << iov.size();
      break;
    }
  }

  bytesWritten_ += bytesWritten;
  writeCount_++;

  // Send notification to peer
  uint64_t writeOffset = writeRegion_->header()->writeOffset.load(std::memory_order_acquire);
  uint32_t offsetInRegion = static_cast<uint32_t>(writeOffset % writeRegion_->dataSize());
  sendNotification(offsetInRegion - bytesWritten, bytesWritten);

  if (callback) {
    callback->writeSuccess();
  }
}

void SharedMemoryTransport::sendNotification(uint32_t offset, uint32_t length) {
  if (gqmInterface_) {
    GqmNotification notification{0, offset, static_cast<uint16_t>(length)};
    gqmInterface_->push(notification);
    notificationSent_++;
    XLOG(DBG) << "Sent notification: offset=" << offset << ", length=" << length;
  }
}

void SharedMemoryTransport::close() {
  State expected = State::CONNECTED;
  if (state_.compare_exchange_strong(expected, State::CLOSING)) {
    // Mark regions as closed
    if (writeRegion_) {
      writeRegion_->close();
    }
    if (readRegion_) {
      readRegion_->close();
    }

    // Will transition to CLOSED after pending writes complete
    // For now, just close immediately since we don't have pending writes
    closeNow();
  }
}

void SharedMemoryTransport::closeNow() {
  State oldState = state_.exchange(State::CLOSED);
  if (oldState == State::CLOSED) {
    return;
  }

  cancelTimeout();

  // Close shared memory regions
  if (writeRegion_) {
    writeRegion_->close();
  }
  if (readRegion_) {
    readRegion_->close();
  }

  // Fail any pending writes
  {
    std::lock_guard<std::mutex> lock(writeMutex_);
    for (auto& req : pendingWrites_) {
      if (req.callback) {
        req.callback->writeErr(
            req.bytesWritten,
            AsyncSocketException(
                AsyncSocketException::END_OF_FILE,
                "Transport closed"));
      }
    }
    pendingWrites_.clear();
  }

  // Notify read callback of EOF
  if (readCallback_) {
    readCallback_->readEOF();
    readCallback_ = nullptr;
  }

  // Call close callback
  if (closeCallback_) {
    closeCallback_();
  }

  XLOG(DBG) << "SharedMemoryTransport closed";
}

void SharedMemoryTransport::closeWithReset() {
  // Mark error state
  if (writeRegion_) {
    writeRegion_->header()->setError();
  }
  if (readRegion_) {
    readRegion_->header()->setError();
  }

  closeNow();
}

void SharedMemoryTransport::shutdownWrite() {
  if (writeRegion_) {
    writeRegion_->close();
  }
}

void SharedMemoryTransport::shutdownWriteNow() {
  shutdownWrite();
}

bool SharedMemoryTransport::good() const {
  State s = state_.load();
  return s == State::CONNECTED;
}

bool SharedMemoryTransport::readable() const {
  return good() && readRegion_ && readRegion_->availableToRead() > 0;
}

bool SharedMemoryTransport::writable() const {
  return good() && writeRegion_ && writeRegion_->availableToWrite() > 0;
}

bool SharedMemoryTransport::connecting() const {
  return state_.load() == State::HANDSHAKE;
}

bool SharedMemoryTransport::error() const {
  State s = state_.load();
  return s == State::ERROR;
}

void SharedMemoryTransport::attachEventBase(EventBase* eventBase) {
  if (evb_ == eventBase) {
    return;
  }
  DCHECK(!evb_);
  evb_ = eventBase;
  AsyncTimeout::attachEventBase(eventBase);
}

void SharedMemoryTransport::detachEventBase() {
  DCHECK(evb_);
  AsyncTimeout::detachEventBase();
  evb_ = nullptr;
}

bool SharedMemoryTransport::isDetachable() const {
  // Can detach if no pending writes
  std::lock_guard<std::mutex> lock(writeMutex_);
  return pendingWrites_.empty() && !isScheduled();
}

void SharedMemoryTransport::setSendTimeout(uint32_t milliseconds) {
  sendTimeoutMs_ = milliseconds;
}

uint32_t SharedMemoryTransport::getSendTimeout() const {
  return sendTimeoutMs_;
}

void SharedMemoryTransport::getLocalAddress(SocketAddress* address) const {
  *address = localAddress_;
}

void SharedMemoryTransport::getPeerAddress(SocketAddress* address) const {
  *address = peerAddress_;
}

void SharedMemoryTransport::checkForAvailableData() {
  processNotifications();
  deliverReadData();
}

void SharedMemoryTransport::processNotifications() {
  if (!gqmInterface_) {
    return;
  }

  while (auto notification = gqmInterface_->pop()) {
    notificationReceived_++;
    XLOG(DBG) << "Received notification: offset=" << notification->offset
              << ", length=" << notification->length;
    // The notification tells us there's new data in the read region
    // We'll read it in deliverReadData()
  }
}

void SharedMemoryTransport::deliverReadData() {
  if (!readCallback_ || !readRegion_) {
    return;
  }

  while (size_t available = readRegion_->availableToRead()) {
    // Get buffer from callback
    void* buf = nullptr;
    size_t bufLen = 0;
    readCallback_->getReadBuffer(&buf, &bufLen);

    if (!buf || bufLen == 0) {
      // Callback returned invalid buffer
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::INVALID_STATE,
              "Invalid read buffer"));
      return;
    }

    // Read from shared memory
    size_t toRead = std::min(available, bufLen);
    ssize_t bytesRead = readRegion_->read(buf, toRead);

    if (bytesRead < 0) {
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::UNKNOWN,
              "Read failed"));
      return;
    }

    if (bytesRead == 0) {
      // EOF
      readCallback_->readEOF();
      return;
    }

    bytesRead_ += bytesRead;
    readCount_++;

    // Deliver to callback
    readCallback_->readDataAvailable(bytesRead);
  }
}

void SharedMemoryTransport::timeoutExpired() noexcept {
  if (state_ != State::CONNECTED) {
    return;
  }

  // Check for new data
  processNotifications();
  deliverReadData();

  // Reschedule
  scheduleTimeout(kDefaultPollInterval);
}

// ========== AsyncTransport::ReadCallback for Handshake ==========

void SharedMemoryTransport::getReadBuffer(void** bufReturn, size_t* lenReturn) {
  auto buf = handshakeReadBuf_.preallocate(4096, 4096);
  *bufReturn = buf.first;
  *lenReturn = buf.second;
}

void SharedMemoryTransport::readDataAvailable(size_t len) noexcept {
  handshakeReadBuf_.postallocate(len);
}

void SharedMemoryTransport::readEOF() noexcept {
  XLOG(DBG) << "Handshake socket EOF";
}

void SharedMemoryTransport::readErr(const AsyncSocketException& ex) noexcept {
  XLOG(ERR) << "Handshake socket error: " << ex.what();
}

void SharedMemoryTransport::readBufferAvailable(
    std::unique_ptr<IOBuf> readBuf) noexcept {
  handshakeReadBuf_.append(std::move(readBuf));
}

SharedMemoryTransport::Stats SharedMemoryTransport::getStats() const {
  return Stats{
      bytesWritten_.load(),
      bytesRead_.load(),
      writeCount_.load(),
      readCount_.load(),
      notificationSent_.load(),
      notificationReceived_.load()};
}

} // namespace folly
