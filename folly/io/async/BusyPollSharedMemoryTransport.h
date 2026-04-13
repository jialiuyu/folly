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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/SharedMemoryRegion.h>
#include <folly/net/NetworkSocket.h>

namespace folly {

class EventBase;

/**
 * BusyPollSharedMemoryTransport implements AsyncTransport using shared memory
 * with busy-polling for ultra-low-latency inter-process communication.
 *
 * Communication Model:
 * - Bidirectional communication uses two shared memory regions:
 *   1. Write Region: Owned by this endpoint, written by us, read by peer
 *   2. Read Region: Owned by peer endpoint, written by peer, read by us
 *
 * - Notification mechanism:
 *   A dedicated busy-poll thread continuously checks the read region's
 *   atomic writeOffset for new data. When new data is detected, the thread
 *   signals a local eventfd (or pipe on macOS) which wakes up the EventBase
 *   via EventHandler. This achieves near-zero detection latency.
 *
 * - After writing data, the peer is notified via its eventfd (passed during
 *   handshake) so it can process the data promptly even without busy-polling.
 *
 * Usage:
 *   // After handshake completes:
 *   auto transport = BusyPollSharedMemoryTransport::create(
 *       evb, std::move(writeRegion), std::move(readRegion), peerEventFd, config);
 */
class BusyPollSharedMemoryTransport
    : public AsyncTransport,
      private EventHandler {
 public:
  using UniquePtr = std::unique_ptr<BusyPollSharedMemoryTransport, Destructor>;

  /**
   * Configuration for BusyPollSharedMemoryTransport.
   */
  struct Config {
    // Size of each shared memory data region (default: 4MB)
    size_t dataRegionSize = 4 * 1024 * 1024;
    // Name prefix for shared memory regions
    std::string shmNamePrefix = "/thrift_shm_";
    // Enable busy-poll thread (if false, relies on peer eventfd notification)
    bool enableBusyPoll = true;
    // Enable debug logging
    bool debugLogging = false;
  };

  /**
   * Create a BusyPollSharedMemoryTransport from already-established shared
   * memory regions. This is called after the handshake completes.
   *
   * @param evb EventBase to use for async operations
   * @param writeRegion Region to write data to (read by peer)
   * @param readRegion Region to read data from (written by peer)
   * @param peerEventFd Eventfd to signal peer after writing data (-1 if none)
   * @param config Configuration
   */
  static UniquePtr create(
      EventBase* evb,
      std::unique_ptr<SharedMemoryRegion> writeRegion,
      std::unique_ptr<SharedMemoryRegion> readRegion,
      int peerEventFd,
      const Config& config = {});

  ~BusyPollSharedMemoryTransport() override;

  // Non-copyable, non-movable
  BusyPollSharedMemoryTransport(const BusyPollSharedMemoryTransport&) = delete;
  BusyPollSharedMemoryTransport& operator=(const BusyPollSharedMemoryTransport&) =
      delete;
  BusyPollSharedMemoryTransport(BusyPollSharedMemoryTransport&&) = delete;
  BusyPollSharedMemoryTransport& operator=(BusyPollSharedMemoryTransport&&) =
      delete;

  // ========== AsyncTransport Interface Implementation ==========

  // Read methods
  void setReadCB(ReadCallback* callback) override;
  ReadCallback* getReadCallback() const override;

  // Write methods
  void write(
      WriteCallback* callback,
      const void* buf,
      size_t bytes,
      WriteFlags flags = WriteFlags::NONE) override;
  void writev(
      WriteCallback* callback,
      const iovec* vec,
      size_t count,
      WriteFlags flags = WriteFlags::NONE) override;
  void writeChain(
      WriteCallback* callback,
      std::unique_ptr<IOBuf>&& buf,
      WriteFlags flags = WriteFlags::NONE) override;

  // Connection management
  void close() override;
  void closeNow() override;
  void closeWithReset() override;
  void shutdownWrite() override;
  void shutdownWriteNow() override;

  // State queries
  bool good() const override;
  bool readable() const override;
  bool writable() const override;
  bool connecting() const override;
  bool error() const override;

  // EventBase management
  void attachEventBase(EventBase* eventBase) override;
  void detachEventBase() override;
  bool isDetachable() const override;

  // Timeout management
  void setSendTimeout(uint32_t milliseconds) override;
  uint32_t getSendTimeout() const override;

  // Address methods
  void getLocalAddress(SocketAddress* address) const override;
  void getPeerAddress(SocketAddress* address) const override;

  // EOR tracking
  bool isEorTrackingEnabled() const override { return eorTrackingEnabled_; }
  void setEorTracking(bool track) override { eorTrackingEnabled_ = track; }

  // Byte counters
  size_t getAppBytesWritten() const override { return bytesWritten_; }
  size_t getRawBytesWritten() const override { return bytesWritten_; }
  size_t getAppBytesReceived() const override { return bytesRead_; }
  size_t getRawBytesReceived() const override { return bytesRead_; }

  // ========== BusyPollSharedMemoryTransport Specific Methods ==========

  /**
   * Manually trigger a check for available data
   */
  void checkForAvailableData();

  /**
   * Get statistics
   */
  struct Stats {
    uint64_t bytesWritten{0};
    uint64_t bytesRead{0};
    uint64_t writeCount{0};
    uint64_t readCount{0};
    uint64_t busyPollWakeups{0};
    uint64_t peerNotifications{0};
  };
  Stats getStats() const;

 private:
  // Private constructor - use create() factory
  BusyPollSharedMemoryTransport(
      EventBase* evb,
      std::unique_ptr<SharedMemoryRegion> writeRegion,
      std::unique_ptr<SharedMemoryRegion> readRegion,
      int localEventFd,
      int peerEventFd,
      const Config& config);

  // EventHandler callback - called when local eventfd is readable
  void handlerReady(uint16_t events) noexcept override;

  // Internal methods
  void writeInternal(
      WriteCallback* callback,
      std::unique_ptr<IOBuf> buf,
      WriteFlags flags);
  void deliverReadData();
  void signalPeer();
  void startBusyPollThread();
  void stopBusyPollThread();

  // Create a notification fd (eventfd on Linux, pipe on macOS)
  static void closeEventFd(int fd);
  static bool drainEventFd(int fd);

  // State
  enum class State {
    CONNECTED, // Shared memory established, ready for I/O
    CLOSING, // Close requested, draining writes
    CLOSED, // Fully closed
    ERROR // Error state
  };

  EventBase* evb_;
  std::atomic<State> state_{State::CONNECTED};

  // Shared memory regions
  std::unique_ptr<SharedMemoryRegion> writeRegion_; // We write, peer reads
  std::unique_ptr<SharedMemoryRegion> readRegion_; // Peer writes, we read

  // Local notification fd (eventfd/pipe[0]) - BusyPollThread → EventBase
  int localEventFd_{-1};
  // On macOS with pipe, the write end is stored here
  int localEventFdWrite_{-1};
  // Peer notification fd - we signal this after writing data
  int peerEventFd_{-1};

  // Configuration
  Config config_;

  // Read callback
  ReadCallback* readCallback_{nullptr};

  // Write state
  struct WriteRequest {
    WriteCallback* callback{nullptr};
    std::unique_ptr<IOBuf> buffer;
    size_t bytesWritten{0};
    size_t totalBytes{0};
  };
  std::deque<WriteRequest> pendingWrites_;
  std::mutex writeMutex_;

  // Read state
  IOBufQueue readBufQueue_;

  // Busy-poll thread
  std::thread busyPollThread_;
  std::atomic<bool> busyPollRunning_{false};
  std::atomic<bool> dataDelivered_{false};

  // Statistics
  std::atomic<uint64_t> bytesWritten_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> writeCount_{0};
  std::atomic<uint64_t> readCount_{0};
  std::atomic<uint64_t> busyPollWakeups_{0};
  std::atomic<uint64_t> peerNotifications_{0};

  // Other state
  uint32_t sendTimeoutMs_{0};
  bool eorTrackingEnabled_{false};
  folly::Function<void()> closeCallback_;

  // Cached addresses
  mutable SocketAddress localAddress_;
  mutable SocketAddress peerAddress_;
};

} // namespace folly
