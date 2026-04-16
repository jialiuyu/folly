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

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/GqmInterface.h>
#include <folly/io/async/MemoryProvider.h>
#include <folly/io/async/SharedMemoryRegion.h>

namespace folly {

class EventBase;

/**
 * SharedMemoryTransport implements AsyncTransport interface using shared
 * memory for high-performance inter-process communication.
 *
 * Communication Model:
 * - Bidirectional communication uses two shared memory regions:
 *   1. Write Region: Owned by this endpoint, written by us, read by peer
 *   2. Read Region: Owned by peer endpoint, written by peer, read by us
 *
 * - Notification mechanism:
 *   After writing data to the shared memory, a notification is sent via
 *   GQM (hardware queue) to inform the peer that data is available.
 *
 * - TCP Handshake:
 *   Since shared memory lacks handshake capability, TCP is used for initial
 *   setup to exchange shared memory region names and configuration.
 *
 * Usage:
 *   // Client side:
 *   auto transport = SharedMemoryTransport::createClient(
 *       evb, tcpSocket, config);
 *
 *   // Server side:
 *   auto transport = SharedMemoryTransport::createServer(
 *       evb, tcpSocket, config);
 */
class SharedMemoryTransport
    : public AsyncTransport,
      private AsyncTimeout,
      private AsyncTransport::ReadCallback {
 public:
  using UniquePtr = std::unique_ptr<SharedMemoryTransport, Destructor>;
  using Config = SharedMemoryTransportConfig;

  /**
   * Create a client-side SharedMemoryTransport.
   * Performs handshake over the TCP socket to establish shared memory.
   *
   * @param evb EventBase to use for async operations
   * @param tcpSocket TCP socket for handshake (will be closed after handshake)
   * @param config Configuration for shared memory regions
   * @return UniquePtr to the created transport
   */
  static UniquePtr createClient(
      EventBase* evb,
      AsyncTransport::UniquePtr tcpSocket,
      const Config& config = {});

  /**
   * Create a server-side SharedMemoryTransport.
   * Performs handshake over the TCP socket to establish shared memory.
   *
   * @param evb EventBase to use for async operations
   * @param tcpSocket TCP socket for handshake (will be closed after handshake)
   * @param config Configuration for shared memory regions
   * @return UniquePtr to the created transport
   */
  static UniquePtr createServer(
      EventBase* evb,
      AsyncTransport::UniquePtr tcpSocket,
      const Config& config = {});

  /**
   * Create a SharedMemoryTransport from already-established shared memory
   * regions. This is useful when the handshake has been done externally.
   *
   * @param evb EventBase to use for async operations
   * @param writeRegion Region to write data to (read by peer)
   * @param readRegion Region to read data from (written by peer)
   * @param gqmInterface GQM interface for notifications (optional)
   * @param config Configuration
   */
  static UniquePtr createFromRegions(
      EventBase* evb,
      std::unique_ptr<MemoryRegion> writeRegion,
      std::unique_ptr<MemoryRegion> readRegion,
      std::shared_ptr<GqmInterface> gqmInterface = nullptr,
      const Config& config = {});

  ~SharedMemoryTransport() override;

  // Non-copyable, non-movable
  SharedMemoryTransport(const SharedMemoryTransport&) = delete;
  SharedMemoryTransport& operator=(const SharedMemoryTransport&) = delete;
  SharedMemoryTransport(SharedMemoryTransport&&) = delete;
  SharedMemoryTransport& operator=(SharedMemoryTransport&&) = delete;

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
  EventBase* getEventBase() const override { return evb_; }

  // EOR tracking
  bool isEorTrackingEnabled() const override { return eorTrackingEnabled_; }
  void setEorTracking(bool track) override { eorTrackingEnabled_ = track; }

  // Byte counters
  size_t getAppBytesWritten() const override { return bytesWritten_; }
  size_t getRawBytesWritten() const override { return bytesWritten_; }
  size_t getAppBytesReceived() const override { return bytesRead_; }
  size_t getRawBytesReceived() const override { return bytesRead_; }

  // ========== SharedMemoryTransport Specific Methods ==========

  /**
   * Get the write region (for testing/debugging)
   */
  MemoryRegion* getWriteRegion() { return writeRegion_.get(); }
  const MemoryRegion* getWriteRegion() const { return writeRegion_.get(); }

  /**
   * Get the read region (for testing/debugging)
   */
  MemoryRegion* getReadRegion() { return readRegion_.get(); }
  const MemoryRegion* getReadRegion() const { return readRegion_.get(); }

  /**
   * Set a callback to be notified when the transport is closed
   */
  void setCloseCallback(folly::Function<void()> callback) {
    closeCallback_ = std::move(callback);
  }

  /**
   * Manually trigger a check for available data (useful for polling)
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
    uint64_t notificationSent{0};
    uint64_t notificationReceived{0};
  };
  Stats getStats() const;

 private:
  // Private constructor
  SharedMemoryTransport(
      EventBase* evb,
      std::unique_ptr<MemoryRegion> writeRegion,
      std::unique_ptr<MemoryRegion> readRegion,
      std::shared_ptr<GqmInterface> gqmInterface,
      const Config& config);

  // Handshake methods
  static UniquePtr performHandshake(
      EventBase* evb,
      AsyncTransport::UniquePtr tcpSocket,
      bool isServer,
      const Config& config);

  bool sendHandshakeInfo(AsyncTransport* socket, const SharedMemoryHandshakeInfo& info);
  bool receiveHandshakeInfo(AsyncTransport* socket, SharedMemoryHandshakeInfo& info);

  // Internal write implementation
  void writeInternal(
      WriteCallback* callback,
      std::unique_ptr<IOBuf> buf,
      WriteFlags flags);

  // Send notification to peer about available data
  void sendNotification(uint32_t offset, uint32_t length);

  // Process pending notifications (called when data might be available)
  void processNotifications();

  // Read data from shared memory and deliver to callback
  void deliverReadData();

  // AsyncTimeout callback - used for polling read region
  void timeoutExpired() noexcept override;

  // AsyncTransport::ReadCallback methods for handshake
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override;
  void readDataAvailable(size_t len) noexcept override;
  void readEOF() noexcept override;
  void readErr(const AsyncSocketException& ex) noexcept override;
  bool isBufferMovable() noexcept override { return true; }
  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override;

  // State
  enum class State {
    HANDSHAKE,    // Performing TCP handshake
    CONNECTED,    // Shared memory established, ready for I/O
    CLOSING,      // Close requested, draining writes
    CLOSED,       // Fully closed
    ERROR         // Error state
  };

  EventBase* evb_;
  std::atomic<State> state_{State::HANDSHAKE};

  // Shared memory regions
  std::unique_ptr<MemoryRegion> writeRegion_;  // We write, peer reads
  std::unique_ptr<MemoryRegion> readRegion_;   // Peer writes, we read

  // GQM notification interface
  std::shared_ptr<GqmInterface> gqmInterface_;

  // Configuration
  Config config_;

  // Writer-local cursor (only the writer advances it)
  uint64_t writeCursor_{0};

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
  mutable std::mutex writeMutex_;

  // Read state
  IOBufQueue readBufQueue_;

  // Handshake state
  AsyncTransport::UniquePtr handshakeSocket_;
  IOBufQueue handshakeReadBuf_;
  bool isServer_;

  // Statistics
  std::atomic<uint64_t> bytesWritten_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> writeCount_{0};
  std::atomic<uint64_t> readCount_{0};
  std::atomic<uint64_t> notificationSent_{0};
  std::atomic<uint64_t> notificationReceived_{0};

  // Other state
  uint32_t sendTimeoutMs_{0};
  bool eorTrackingEnabled_{false};
  folly::Function<void()> closeCallback_;

  // Cached addresses (for getLocalAddress/getPeerAddress)
  mutable SocketAddress localAddress_;
  mutable SocketAddress peerAddress_;
  mutable bool addressesCached_{false};

  // Polling interval for checking read data (when not using notifications)
  static constexpr std::chrono::milliseconds kDefaultPollInterval{1};
};

} // namespace folly
