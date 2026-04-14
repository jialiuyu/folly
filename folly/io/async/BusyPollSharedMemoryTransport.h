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
#include <deque>
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
#include <folly/io/async/EventBase.h>
#include <folly/io/async/GqmInterface.h>
#include <folly/io/async/SharedMemoryRegion.h>

namespace folly {

/**
 * BusyPollSharedMemoryTransport implements AsyncTransport using shared memory
 * with GQM-based notification for ultra-low-latency inter-process
 * communication.
 *
 * Communication Model:
 * - Bidirectional communication uses two pairs of (data region + GQM queue):
 *   1. Write side: data writeRegion + gqmWrite (we push, peer pops)
 *   2. Read side:  data readRegion  + gqmRead  (peer pushes, we pop)
 *
 * - Notification mechanism:
 *   After writing data to the shared memory, gqm_push() sends a notification
 *   with (offset, length). The reader polls gqm_pop() to detect new data.
 *   Both push and pop are pure user-space atomic operations — zero syscall.
 *
 * - Polling strategy:
 *   Configurable via PollingMode:
 *   - BUSY_POLL:    Dedicated thread spins on gqm_pop (lowest latency)
 *   - ADAPTIVE:     NAPI-style hybrid: spin under load, sleep when idle
 *   - DEDICATED_CORE: Like BUSY_POLL but pinned to a specific CPU core
 *   - EVENTBASE:    No extra thread; checked via EventBase loop callback
 *
 * Usage:
 *   auto transport = BusyPollSharedMemoryTransport::create(
 *       evb, std::move(writeRegion), std::move(readRegion),
 *       std::move(gqmWrite), std::move(gqmRead), config);
 */
class BusyPollSharedMemoryTransport : public AsyncTransport {
 public:
  using UniquePtr = std::unique_ptr<BusyPollSharedMemoryTransport, Destructor>;

  /**
   * Polling strategy mode.
   */
  enum class PollingMode {
    /// Pure busy-poll: dedicated thread spins on gqm_pop. Lowest latency.
    BUSY_POLL,
    /// NAPI-style adaptive: spin under high load, futex_wait when idle.
    ADAPTIVE,
    /// Dedicated core: busy-poll pinned to a specific CPU core.
    DEDICATED_CORE,
    /// No extra thread; GQM checked via EventBase runInLoop callback.
    EVENTBASE,
  };

  /**
   * Configuration for BusyPollSharedMemoryTransport.
   */
  struct Config {
    // Size of each shared memory data region (default: 4MB)
    size_t dataRegionSize = 4 * 1024 * 1024;
    // Name prefix for shared memory regions
    std::string shmNamePrefix = "/thrift_shm_";
    // Polling mode
    PollingMode pollingMode = PollingMode::ADAPTIVE;
    // CPU core to pin for DEDICATED_CORE mode (-1 = no pinning)
    int pinnedCore = -1;

    // --- Adaptive polling parameters (ADAPTIVE mode only) ---
    // Max spin iterations before falling back to sleep
    uint32_t spinLimit = 1000;
    // Consecutive hits threshold to stay in spin mode
    uint32_t highLoadThreshold = 10;
    // futex_wait timeout in microseconds when sleeping
    uint32_t sleepTimeoutUs = 100;

    // Enable debug logging
    bool debugLogging = false;
  };

  /**
   * Create a BusyPollSharedMemoryTransport from established shared memory
   * regions and GQM queues. Called after the handshake completes.
   *
   * @param evb EventBase to use for async operations
   * @param writeRegion Data region to write to (read by peer)
   * @param readRegion Data region to read from (written by peer)
   * @param gqmWrite GQM queue for write notifications (we push, peer pops)
   * @param gqmRead GQM queue for read notifications (peer pushes, we pop)
   * @param config Configuration
   */
  static UniquePtr create(
      EventBase* evb,
      std::unique_ptr<SharedMemoryRegion> writeRegion,
      std::unique_ptr<SharedMemoryRegion> readRegion,
      std::unique_ptr<GqmInterface> gqmWrite,
      std::unique_ptr<GqmInterface> gqmRead,
      const Config& config = {});

  ~BusyPollSharedMemoryTransport() override;

  // Non-copyable, non-movable
  BusyPollSharedMemoryTransport(const BusyPollSharedMemoryTransport&) = delete;
  BusyPollSharedMemoryTransport& operator=(const BusyPollSharedMemoryTransport&) =
      delete;

  // ========== AsyncTransport Interface Implementation ==========

  void setReadCB(ReadCallback* callback) override;
  ReadCallback* getReadCallback() const override;

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

  void close() override;
  void closeNow() override;
  void closeWithReset() override;
  void shutdownWrite() override;
  void shutdownWriteNow() override;

  bool good() const override;
  bool readable() const override;
  bool writable() const override;
  bool connecting() const override;
  bool error() const override;

  void attachEventBase(EventBase* eventBase) override;
  void detachEventBase() override;
  bool isDetachable() const override;

  void setSendTimeout(uint32_t milliseconds) override;
  uint32_t getSendTimeout() const override;

  void getLocalAddress(SocketAddress* address) const override;
  void getPeerAddress(SocketAddress* address) const override;

  bool isEorTrackingEnabled() const override { return eorTrackingEnabled_; }
  void setEorTracking(bool track) override { eorTrackingEnabled_ = track; }

  size_t getAppBytesWritten() const override { return bytesWritten_; }
  size_t getRawBytesWritten() const override { return bytesWritten_; }
  size_t getAppBytesReceived() const override { return bytesRead_; }
  size_t getRawBytesReceived() const override { return bytesRead_; }

  // ========== BusyPollSharedMemoryTransport Specific Methods ==========

  /**
   * Manually trigger a check for available data via GQM.
   */
  void checkForAvailableData();

  /**
   * Check GQM for notifications and deliver data if available.
   * Called by the poller thread or by EventBase loop callback.
   * Returns true if data was delivered.
   */
  bool pollAndDeliver();

  /**
   * Get the GQM read interface (for external poller integration).
   */
  GqmInterface* getGqmRead() { return gqmRead_.get(); }

  /**
   * Get statistics
   */
  struct Stats {
    uint64_t bytesWritten{0};
    uint64_t bytesRead{0};
    uint64_t writeCount{0};
    uint64_t readCount{0};
    uint64_t gqmPushCount{0};
    uint64_t gqmPopCount{0};
    uint64_t pollCycles{0};
    uint64_t sleepCount{0};
  };
  Stats getStats() const;

 private:
  // Private constructor - use create() factory
  BusyPollSharedMemoryTransport(
      EventBase* evb,
      std::unique_ptr<SharedMemoryRegion> writeRegion,
      std::unique_ptr<SharedMemoryRegion> readRegion,
      std::unique_ptr<GqmInterface> gqmWrite,
      std::unique_ptr<GqmInterface> gqmRead,
      const Config& config);

  // Internal methods
  void writeInternal(
      WriteCallback* callback,
      std::unique_ptr<IOBuf> buf,
      WriteFlags flags);
  void deliverReadData();
  void signalPeer();

  // Poller thread management
  void startPollerThread();
  void stopPollerThread();

  // Polling strategies
  void pollerLoopBusyPoll();
  void pollerLoopAdaptive();
  void pollerLoopDedicatedCore();

  // EventBase-integrated polling
  void registerEventBasePoll();
  void unregisterEventBasePoll();

  // Wakeup fd helpers for adaptive mode
  // On Linux uses eventfd; on other platforms uses pipe.
  // createWakeupFds populates readFd and writeFd.
  // On Linux readFd == writeFd (eventfd is bidirectional).
  static void createWakeupFds(int& readFd, int& writeFd);
  static void closeEventFd(int fd);
  static bool writeEventFd(int fd);
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

  // Shared memory data regions
  std::unique_ptr<SharedMemoryRegion> writeRegion_; // We write, peer reads
  std::unique_ptr<SharedMemoryRegion> readRegion_; // Peer writes, we read

  // GQM notification queues (pure user-space, no syscall)
  std::unique_ptr<GqmInterface> gqmWrite_; // We push, peer pops
  std::unique_ptr<GqmInterface> gqmRead_; // Peer pushes, we pop

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
  mutable std::mutex writeMutex_;

  // Read state
  IOBufQueue readBufQueue_;

  // Poller thread (for BUSY_POLL, ADAPTIVE, DEDICATED_CORE modes)
  std::thread pollerThread_;
  std::atomic<bool> pollerRunning_{false};

  // Adaptive mode: eventfd for waking poller from sleep
  // (only used in ADAPTIVE mode, -1 in other modes)
  int wakeupFd_{-1}; // read end (eventfd on Linux, pipe read on others)
  int wakeupFdWrite_{-1}; // write end (same as wakeupFd_ on Linux, pipe write on others)
  std::atomic<bool> pollerSleeping_{false};

  // EventBase-integrated polling (EVENTBASE mode)
  class PollLoopCallback : public EventBase::LoopCallback {
   public:
    explicit PollLoopCallback(BusyPollSharedMemoryTransport& transport)
        : transport_(transport) {}
    void runLoopCallback() noexcept override;
   private:
    BusyPollSharedMemoryTransport& transport_;
  };
  std::unique_ptr<PollLoopCallback> pollLoopCb_;
  bool evbPollRegistered_{false};

  // Statistics
  std::atomic<uint64_t> bytesWritten_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> writeCount_{0};
  std::atomic<uint64_t> readCount_{0};
  std::atomic<uint64_t> gqmPushCount_{0};
  std::atomic<uint64_t> gqmPopCount_{0};
  std::atomic<uint64_t> pollCycles_{0};
  std::atomic<uint64_t> sleepCount_{0};

  // Other state
  uint32_t sendTimeoutMs_{0};
  bool eorTrackingEnabled_{false};
  folly::Function<void()> closeCallback_;

  // Cached addresses
  mutable SocketAddress localAddress_;
  mutable SocketAddress peerAddress_;
};

} // namespace folly
