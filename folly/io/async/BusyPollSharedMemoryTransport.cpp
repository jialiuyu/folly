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

#include <folly/io/async/BusyPollSharedMemoryTransport.h>

#include <cstring>
#include <stdexcept>

#include <folly/Exception.h>
#include <folly/ExceptionWrapper.h>
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

// ========== Platform-specific eventfd helpers ==========

void BusyPollSharedMemoryTransport::closeEventFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool BusyPollSharedMemoryTransport::drainEventFd(int fd) {
  if (fd < 0) {
    return false;
  }
#ifdef __linux__
  uint64_t val;
  while (::read(fd, &val, sizeof(val)) == sizeof(val)) {
    // drain all
  }
#else
  // Pipe: drain all bytes
  uint8_t buf[64];
  while (::read(fd, buf, sizeof(buf)) > 0) {
    // drain all
  }
#endif
  return true;
}

// ========== BusyPollSharedMemoryTransport Implementation ==========

BusyPollSharedMemoryTransport::BusyPollSharedMemoryTransport(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    int localEventFd,
    int peerEventFd,
    const Config& config)
    : EventHandler(evb, folly::NetworkSocket::fromFd(localEventFd)),
      evb_(evb),
      writeRegion_(std::move(writeRegion)),
      readRegion_(std::move(readRegion)),
      localEventFd_(localEventFd),
      peerEventFd_(peerEventFd),
      config_(config) {
  state_ = State::CONNECTED;
  XLOG(DBG) << "BusyPollSharedMemoryTransport created";
}

BusyPollSharedMemoryTransport::~BusyPollSharedMemoryTransport() {
  closeNow();
  XLOG(DBG) << "BusyPollSharedMemoryTransport destroyed";
}

// ========== Static Factory Method ==========

BusyPollSharedMemoryTransport::UniquePtr
BusyPollSharedMemoryTransport::create(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    int peerEventFd,
    const Config& config) {
  if (!writeRegion || !readRegion) {
    throw std::invalid_argument("Write and read regions must not be null");
  }

  // Create local notification fd (eventfd on Linux, pipe on macOS)
  int localEventFd = -1;
  int localEventFdWrite = -1;

#ifdef __linux__
  localEventFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (localEventFd < 0) {
    throw std::runtime_error(
        folly::sformat("Failed to create eventfd: {}", strerror(errno)));
  }
  // eventfd is bidirectional - same fd for read and write
  localEventFdWrite = localEventFd;
#else
  // macOS: use pipe for notification
  int notifyPipe[2];
  if (::pipe(notifyPipe) < 0) {
    throw std::runtime_error(
        folly::sformat("Failed to create notify pipe: {}", strerror(errno)));
  }
  for (int i = 0; i < 2; ++i) {
    int flags = ::fcntl(notifyPipe[i], F_GETFL);
    ::fcntl(notifyPipe[i], F_SETFL, flags | O_NONBLOCK);
  }
  localEventFd = notifyPipe[0]; // read end (EventBase watches this)
  localEventFdWrite = notifyPipe[1]; // write end (busy-poll thread writes)
#endif

  auto transport = UniquePtr(new BusyPollSharedMemoryTransport(
      evb,
      std::move(writeRegion),
      std::move(readRegion),
      localEventFd,
      peerEventFd,
      config));

  // Store the write end on macOS (same as localEventFd on Linux)
  transport->localEventFdWrite_ = localEventFdWrite;

  // Register EventHandler on local eventfd/pipe read end
  transport->registerHandler(EventHandler::READ | EventHandler::PERSIST);

  // Start busy-poll thread
  if (config.enableBusyPoll) {
    transport->startBusyPollThread();
  }

  return transport;
}

// ========== AsyncTransport Interface ==========

void BusyPollSharedMemoryTransport::setReadCB(ReadCallback* callback) {
  readCallback_ = callback;

  if (callback && state_ == State::CONNECTED) {
    // Deliver any pending data immediately
    deliverReadData();
  }
}

AsyncTransport::ReadCallback*
BusyPollSharedMemoryTransport::getReadCallback() const {
  return readCallback_;
}

void BusyPollSharedMemoryTransport::write(
    WriteCallback* callback,
    const void* buf,
    size_t bytes,
    WriteFlags flags) {
  auto ioBuf = IOBuf::copyBuffer(buf, bytes);
  writeChain(callback, std::move(ioBuf), flags);
}

void BusyPollSharedMemoryTransport::writev(
    WriteCallback* callback,
    const iovec* vec,
    size_t count,
    WriteFlags flags) {
  auto ioBuf = IOBuf::fromIovec(vec, count);
  writeChain(callback, std::move(ioBuf), flags);
}

void BusyPollSharedMemoryTransport::writeChain(
    WriteCallback* callback,
    std::unique_ptr<IOBuf>&& buf,
    WriteFlags flags) {
  if (state_ != State::CONNECTED) {
    if (callback) {
      callback->writeErr(
          0,
          AsyncSocketException(
              AsyncSocketException::NOT_OPEN, "Transport not connected"));
    }
    return;
  }

  writeInternal(callback, std::move(buf), flags);
}

void BusyPollSharedMemoryTransport::writeInternal(
    WriteCallback* callback,
    std::unique_ptr<IOBuf> buf,
    WriteFlags /*flags*/) {
  if (!writeRegion_ || writeRegion_->isClosed()) {
    if (callback) {
      callback->writeErr(
          0,
          AsyncSocketException(
              AsyncSocketException::END_OF_FILE, "Write region closed"));
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
                AsyncSocketException::UNKNOWN, "Write failed"));
      }
      state_ = State::ERROR;
      return;
    }
    bytesWritten += written;

    if (static_cast<size_t>(written) < iov.size()) {
      // Partial write - buffer full
      // Buffer the rest in pendingWrites_ for retry
      XLOG(DBG) << "Partial write: " << written << "/" << iov.size();
      // For now, fail the write since shared memory ring buffer is full
      if (callback) {
        callback->writeErr(
            bytesWritten,
            AsyncSocketException(
                AsyncSocketException::UNKNOWN, "Shared memory buffer full"));
      }
      return;
    }
  }

  bytesWritten_ += bytesWritten;
  writeCount_++;

  // Signal peer that new data is available
  signalPeer();

  if (callback) {
    callback->writeSuccess();
  }
}

void BusyPollSharedMemoryTransport::signalPeer() {
  if (peerEventFd_ >= 0) {
#ifdef __linux__
    uint64_t val = 1;
    auto ret = ::write(peerEventFd_, &val, sizeof(val));
    if (ret != sizeof(val)) {
      XLOG(DBG) << "Failed to signal peer eventfd: " << strerror(errno);
    }
#else
    uint8_t c = 1;
    auto ret = ::write(peerEventFd_, &c, sizeof(c));
    if (ret != sizeof(c)) {
      XLOG(DBG) << "Failed to signal peer pipe: " << strerror(errno);
    }
#endif
    peerNotifications_++;
  }
}

void BusyPollSharedMemoryTransport::close() {
  State expected = State::CONNECTED;
  if (state_.compare_exchange_strong(expected, State::CLOSING)) {
    if (writeRegion_) {
      writeRegion_->close();
    }
    if (readRegion_) {
      readRegion_->close();
    }
    closeNow();
  }
}

void BusyPollSharedMemoryTransport::closeNow() {
  State oldState = state_.exchange(State::CLOSED);
  if (oldState == State::CLOSED) {
    return;
  }

  // Stop busy-poll thread
  stopBusyPollThread();

  // Unregister EventHandler
  unregisterHandler();

  // Close notification fds
  closeEventFd(localEventFd_);
  localEventFd_ = -1;
  if (localEventFdWrite_ >= 0) {
    ::close(localEventFdWrite_);
    localEventFdWrite_ = -1;
  }
  // Note: peerEventFd_ is owned by the caller; we don't close it here
  // because it may be shared. The handshake code manages its lifetime.
  peerEventFd_ = -1;

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
                AsyncSocketException::END_OF_FILE, "Transport closed"));
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

  XLOG(DBG) << "BusyPollSharedMemoryTransport closed";
}

void BusyPollSharedMemoryTransport::closeWithReset() {
  if (writeRegion_) {
    writeRegion_->header()->setError();
  }
  if (readRegion_) {
    readRegion_->header()->setError();
  }
  closeNow();
}

void BusyPollSharedMemoryTransport::shutdownWrite() {
  if (writeRegion_) {
    writeRegion_->close();
  }
}

void BusyPollSharedMemoryTransport::shutdownWriteNow() {
  shutdownWrite();
}

bool BusyPollSharedMemoryTransport::good() const {
  return state_.load() == State::CONNECTED;
}

bool BusyPollSharedMemoryTransport::readable() const {
  return good() && readRegion_ && readRegion_->availableToRead() > 0;
}

bool BusyPollSharedMemoryTransport::writable() const {
  return good() && writeRegion_ && writeRegion_->availableToWrite() > 0;
}

bool BusyPollSharedMemoryTransport::connecting() const {
  return false;
}

bool BusyPollSharedMemoryTransport::error() const {
  return state_.load() == State::ERROR;
}

void BusyPollSharedMemoryTransport::attachEventBase(EventBase* eventBase) {
  if (evb_ == eventBase) {
    return;
  }
  DCHECK(!evb_);
  evb_ = eventBase;
  EventHandler::attachEventBase(eventBase);
}

void BusyPollSharedMemoryTransport::detachEventBase() {
  DCHECK(evb_);
  EventHandler::detachEventBase();
  evb_ = nullptr;
}

bool BusyPollSharedMemoryTransport::isDetachable() const {
  std::lock_guard<std::mutex> lock(writeMutex_);
  return pendingWrites_.empty();
}

void BusyPollSharedMemoryTransport::setSendTimeout(uint32_t milliseconds) {
  sendTimeoutMs_ = milliseconds;
}

uint32_t BusyPollSharedMemoryTransport::getSendTimeout() const {
  return sendTimeoutMs_;
}

void BusyPollSharedMemoryTransport::getLocalAddress(
    SocketAddress* address) const {
  *address = localAddress_;
}

void BusyPollSharedMemoryTransport::getPeerAddress(
    SocketAddress* address) const {
  *address = peerAddress_;
}

void BusyPollSharedMemoryTransport::checkForAvailableData() {
  deliverReadData();
}

// ========== Busy-Poll Thread ==========

void BusyPollSharedMemoryTransport::startBusyPollThread() {
  busyPollRunning_ = true;
  dataDelivered_ = false;

  busyPollThread_ = std::thread([this]() {
    uint64_t lastOffset = readRegion_->header()->writeOffset.load(
        std::memory_order_acquire);

    XLOG(DBG) << "Busy-poll thread started";

    while (busyPollRunning_.load(std::memory_order_relaxed)) {
      uint64_t writeOffset = readRegion_->header()->writeOffset.load(
          std::memory_order_acquire);

      if (writeOffset != lastOffset) {
        // New data available - signal EventBase
        lastOffset = writeOffset;
        busyPollWakeups_++;

        // Write to local notification fd to wake EventBase
        int notifyFd =
#ifdef __linux__
            localEventFd_;
#else
            localEventFdWrite_;
#endif
        if (notifyFd >= 0) {
#ifdef __linux__
          uint64_t val = 1;
          ::write(notifyFd, &val, sizeof(val));
#else
          uint8_t c = 1;
          ::write(notifyFd, &c, sizeof(c));
#endif
        }

        // Wait until EventBase processes the data before resuming spin
        // This prevents flooding the eventfd
        int spinCount = 0;
        while (!dataDelivered_.exchange(false, std::memory_order_acq_rel)) {
          if (!busyPollRunning_.load(std::memory_order_relaxed)) {
            return;
          }
#if defined(__x86_64__)
          __builtin_ia32_pause();
#else
          // Yield periodically on non-x86 platforms
          if (++spinCount > 100) {
            std::this_thread::yield();
            spinCount = 0;
          }
#endif
        }
      }

      // CPU hint for spin-wait
#if defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }

    XLOG(DBG) << "Busy-poll thread stopped";
  });
}

void BusyPollSharedMemoryTransport::stopBusyPollThread() {
  busyPollRunning_.store(false, std::memory_order_release);
  // Unblock busy-poll thread if it's waiting for data delivery
  dataDelivered_.store(true, std::memory_order_release);
  if (busyPollThread_.joinable()) {
    busyPollThread_.join();
  }
}

// ========== EventHandler Callback ==========

void BusyPollSharedMemoryTransport::handlerReady(
    uint16_t events) noexcept {
  if (events & EventHandler::READ) {
    // Drain the notification fd
    drainEventFd(localEventFd_);

    // Deliver data to read callback
    deliverReadData();

    // Signal busy-poll thread that we've processed the data
    dataDelivered_.store(true, std::memory_order_release);
  }
}

// ========== Data Delivery ==========

void BusyPollSharedMemoryTransport::deliverReadData() {
  if (!readCallback_ || !readRegion_) {
    return;
  }

  while (size_t available = readRegion_->availableToRead()) {
    // Get buffer from callback
    void* buf = nullptr;
    size_t bufLen = 0;
    readCallback_->getReadBuffer(&buf, &bufLen);

    if (!buf || bufLen == 0) {
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::INVALID_STATE, "Invalid read buffer"));
      return;
    }

    // Read from shared memory
    size_t toRead = std::min(available, bufLen);
    ssize_t bytesRead = readRegion_->read(buf, toRead);

    if (bytesRead < 0) {
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::UNKNOWN, "Read failed"));
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

// ========== Statistics ==========

BusyPollSharedMemoryTransport::Stats
BusyPollSharedMemoryTransport::getStats() const {
  return Stats{
      bytesWritten_.load(),
      bytesRead_.load(),
      writeCount_.load(),
      readCount_.load(),
      busyPollWakeups_.load(),
      peerNotifications_.load()};
}

} // namespace folly
