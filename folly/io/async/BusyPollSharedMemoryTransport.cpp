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
#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>
#include <folly/portability/SysUio.h>

#ifdef __linux__
#include <sys/eventfd.h>
#include <sched.h>
#include <pthread.h>
#endif
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace folly {

// ========== Platform-specific eventfd helpers (for ADAPTIVE mode wakeup) ==========

void BusyPollSharedMemoryTransport::createWakeupFds(
    int& readFd, int& writeFd) {
#ifdef __linux__
  int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    throw std::runtime_error(
        folly::sformat("Failed to create eventfd: {}", strerror(errno)));
  }
  readFd = fd;
  writeFd = fd;
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
  readFd = pipefd[0];
  writeFd = pipefd[1];
#endif
}

void BusyPollSharedMemoryTransport::closeEventFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool BusyPollSharedMemoryTransport::writeEventFd(int fd) {
  if (fd < 0) {
    return false;
  }
#ifdef __linux__
  uint64_t val = 1;
  return ::write(fd, &val, sizeof(val)) == sizeof(val);
#else
  uint8_t c = 1;
  return ::write(fd, &c, sizeof(c)) == sizeof(c);
#endif
}

bool BusyPollSharedMemoryTransport::drainEventFd(int fd) {
  if (fd < 0) {
    return false;
  }
#ifdef __linux__
  uint64_t val;
  while (::read(fd, &val, sizeof(val)) == sizeof(val)) {
  }
#else
  uint8_t buf[64];
  while (::read(fd, buf, sizeof(buf)) > 0) {
  }
#endif
  return true;
}

// ========== BusyPollSharedMemoryTransport Implementation ==========

BusyPollSharedMemoryTransport::BusyPollSharedMemoryTransport(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    std::unique_ptr<GqmInterface> gqmWrite,
    std::unique_ptr<GqmInterface> gqmRead,
    const Config& config)
    : evb_(evb),
      writeRegion_(std::move(writeRegion)),
      readRegion_(std::move(readRegion)),
      gqmWrite_(std::move(gqmWrite)),
      gqmRead_(std::move(gqmRead)),
      config_(config) {
  state_ = State::CONNECTED;
  XLOG(DBG5) << "BusyPollSharedMemoryTransport created, mode="
             << static_cast<int>(config_.pollingMode);
}

BusyPollSharedMemoryTransport::~BusyPollSharedMemoryTransport() {
  closeNow();
  XLOG(DBG5) << "BusyPollSharedMemoryTransport destroyed";
}

// ========== Static Factory Method ==========

BusyPollSharedMemoryTransport::UniquePtr
BusyPollSharedMemoryTransport::create(
    EventBase* evb,
    std::unique_ptr<SharedMemoryRegion> writeRegion,
    std::unique_ptr<SharedMemoryRegion> readRegion,
    std::unique_ptr<GqmInterface> gqmWrite,
    std::unique_ptr<GqmInterface> gqmRead,
    const Config& config) {
  if (!writeRegion || !readRegion || !gqmWrite || !gqmRead) {
    throw std::invalid_argument(
        "Write/read regions and GQM queues must not be null");
  }

  auto transport = UniquePtr(new BusyPollSharedMemoryTransport(
      evb,
      std::move(writeRegion),
      std::move(readRegion),
      std::move(gqmWrite),
      std::move(gqmRead),
      config));

  // Start the appropriate polling mode
  switch (config.pollingMode) {
    case PollingMode::BUSY_POLL:
      transport->startPollerThread();
      break;
    case PollingMode::ADAPTIVE:
      createWakeupFds(transport->wakeupFd_, transport->wakeupFdWrite_);
      transport->startPollerThread();
      break;
    case PollingMode::DEDICATED_CORE:
      transport->startPollerThread();
      break;
    case PollingMode::EVENTBASE:
      transport->registerEventBasePoll();
      break;
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
  std::unique_ptr<IOBuf> head;
  for (size_t i = 0; i < count; ++i) {
    auto buf = IOBuf::copyBuffer(vec[i].iov_base, vec[i].iov_len);
    if (head) {
      head->prependChain(std::move(buf));
    } else {
      head = std::move(buf);
    }
  }
  if (head) {
    writeChain(callback, std::move(head), flags);
  } else if (callback) {
    callback->writeSuccess();
  }
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
      XLOG(DBG3) << "Partial write: " << written << "/" << iov.size();
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

  // Notify peer via GQM (pure user-space, zero syscall)
  signalPeer();

  if (callback) {
    callback->writeSuccess();
  }
}

void BusyPollSharedMemoryTransport::signalPeer() {
  if (gqmWrite_) {
    uint64_t writeOff =
        writeRegion_->header()->writeOffset.load(std::memory_order_acquire);
    uint32_t offset =
        static_cast<uint32_t>(writeOff % writeRegion_->dataSize());
    uint32_t len =
        static_cast<uint32_t>(writeCount_.load(std::memory_order_relaxed));
    gqmWrite_->push({offset, len});
    gqmPushCount_++;
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

  // Stop poller thread
  stopPollerThread();

  // Unregister EventBase polling
  unregisterEventBasePoll();

  // Close wakeup fds (ADAPTIVE mode)
  // On Linux, wakeupFd_ == wakeupFdWrite_ (same eventfd), avoid double-close.
  if (wakeupFdWrite_ >= 0 && wakeupFdWrite_ != wakeupFd_) {
    ::close(wakeupFdWrite_);
  }
  wakeupFdWrite_ = -1;
  closeEventFd(wakeupFd_);
  wakeupFd_ = -1;

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

  XLOG(DBG5) << "BusyPollSharedMemoryTransport closed";
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
}

void BusyPollSharedMemoryTransport::detachEventBase() {
  DCHECK(evb_);
  unregisterEventBasePoll();
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
  pollAndDeliver();
}

// ========== Core: poll GQM and deliver data ==========

bool BusyPollSharedMemoryTransport::pollAndDeliver() {
  bool hasNotification = false;

  while (auto notif = gqmRead_->pop()) {
    gqmPopCount_++;
    hasNotification = true;
  }

  if (hasNotification || readRegion_->availableToRead() > 0) {
    // Always deliver data on the EventBase thread for thread safety
    if (evb_ && evb_->isInEventBaseThread()) {
      deliverReadData();
    } else if (evb_) {
      evb_->runInEventBaseThread([this]() {
        if (state_ == State::CONNECTED && readCallback_) {
          deliverReadData();
        }
      });
    }
    return true;
  }

  return false;
}

void BusyPollSharedMemoryTransport::deliverReadData() {
  if (!readCallback_ || !readRegion_) {
    return;
  }

  while (size_t available = readRegion_->availableToRead()) {
    void* buf = nullptr;
    size_t bufLen = 0;
    readCallback_->getReadBuffer(&buf, &bufLen);

    if (!buf || bufLen == 0) {
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::INVALID_STATE, "Invalid read buffer"));
      return;
    }

    size_t toRead = std::min(available, bufLen);
    ssize_t bytesRead = readRegion_->read(buf, toRead);

    if (bytesRead < 0) {
      readCallback_->readErr(
          AsyncSocketException(
              AsyncSocketException::UNKNOWN, "Read failed"));
      return;
    }

    if (bytesRead == 0) {
      readCallback_->readEOF();
      return;
    }

    bytesRead_ += bytesRead;
    readCount_++;

    readCallback_->readDataAvailable(bytesRead);
  }
}

// ========== Poller Thread Management ==========

void BusyPollSharedMemoryTransport::startPollerThread() {
  pollerRunning_ = true;

  switch (config_.pollingMode) {
    case PollingMode::BUSY_POLL:
      pollerThread_ = std::thread([this]() { pollerLoopBusyPoll(); });
      break;
    case PollingMode::ADAPTIVE:
      pollerThread_ = std::thread([this]() { pollerLoopAdaptive(); });
      break;
    case PollingMode::DEDICATED_CORE:
      pollerThread_ = std::thread([this]() { pollerLoopDedicatedCore(); });
      break;
    default:
      break;
  }
}

void BusyPollSharedMemoryTransport::stopPollerThread() {
  pollerRunning_.store(false, std::memory_order_release);
  // Wake up poller if sleeping (ADAPTIVE mode)
  if (wakeupFdWrite_ >= 0) {
    writeEventFd(wakeupFdWrite_);
  }
  if (pollerThread_.joinable()) {
    pollerThread_.join();
  }
}

// ========== Strategy A: Pure Busy-Poll ==========

void BusyPollSharedMemoryTransport::pollerLoopBusyPoll() {
  XLOG(DBG5) << "Busy-poll thread started (pure spin)";
  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;
    pollAndDeliver();
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
  }
  XLOG(DBG5) << "Busy-poll thread stopped";
}

// ========== Strategy B: NAPI-style Adaptive Hybrid ==========

void BusyPollSharedMemoryTransport::pollerLoopAdaptive() {
  XLOG(DBG5) << "Adaptive poller thread started (spin="
             << config_.spinLimit << ", highLoad=" << config_.highLoadThreshold
             << ", sleep=" << config_.sleepTimeoutUs << "us)";

  uint32_t consecutiveHits = 0;
  uint32_t spinCount = 0;

  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;

    bool found = pollAndDeliver();

    if (found) {
      consecutiveHits++;
      spinCount = 0;
    } else {
      consecutiveHits = 0;
      spinCount++;
    }

    if (consecutiveHits >= config_.highLoadThreshold) {
#if defined(__x86_64__)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    } else if (spinCount > config_.spinLimit) {
      pollerSleeping_.store(true, std::memory_order_release);
      sleepCount_++;

      if (wakeupFd_ >= 0) {
        struct pollfd pfd;
        pfd.fd = wakeupFd_;
        pfd.events = POLLIN;
        int timeoutMs = std::max(
            1, static_cast<int>(config_.sleepTimeoutUs / 1000));
        ::poll(&pfd, 1, timeoutMs);
        drainEventFd(wakeupFd_);
      } else {
        std::this_thread::sleep_for(
            std::chrono::microseconds(config_.sleepTimeoutUs));
      }

      pollerSleeping_.store(false, std::memory_order_release);
      spinCount = 0;
    } else {
#if defined(__x86_64__)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
  }

  XLOG(DBG5) << "Adaptive poller thread stopped";
}

// ========== Strategy C: Dedicated Core ==========

void BusyPollSharedMemoryTransport::pollerLoopDedicatedCore() {
  XLOG(DBG5) << "Dedicated core poller started, core="
             << config_.pinnedCore;

#ifdef __linux__
  if (config_.pinnedCore >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.pinnedCore, &cpuset);
    int rc = pthread_setaffinity_np(
        pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      XLOG(ERR) << "Failed to pin to core " << config_.pinnedCore
                << ": " << strerror(rc);
    } else {
      XLOG(INFO) << "Poller pinned to core " << config_.pinnedCore;
    }
  }
#endif

  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;
    pollAndDeliver();
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
  }

  XLOG(DBG5) << "Dedicated core poller stopped";
}

// ========== Strategy D: EventBase-Integrated ==========

void BusyPollSharedMemoryTransport::PollLoopCallback::runLoopCallback()
    noexcept {
  if (transport_.state_ != State::CONNECTED || !transport_.evbPollRegistered_) {
    return;
  }
  transport_.pollAndDeliver();
  if (transport_.evbPollRegistered_ &&
      transport_.state_ == State::CONNECTED &&
      transport_.evb_) {
    transport_.evb_->runInLoop(this);
  }
}

void BusyPollSharedMemoryTransport::registerEventBasePoll() {
  if (!evb_ || evbPollRegistered_) {
    return;
  }
  evbPollRegistered_ = true;
  pollLoopCb_ = std::make_unique<PollLoopCallback>(*this);
  evb_->runInLoop(pollLoopCb_.get());
}

void BusyPollSharedMemoryTransport::unregisterEventBasePoll() {
  evbPollRegistered_ = false;
  if (pollLoopCb_) {
    pollLoopCb_->cancelLoopCallback();
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
      gqmPushCount_.load(),
      gqmPopCount_.load(),
      pollCycles_.load(),
      sleepCount_.load()};
}

} // namespace folly
