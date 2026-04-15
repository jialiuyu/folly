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
#include <folly/io/async/MemoryProvider.h>

namespace folly {

class ShmPollerService;

/**
 * BusyPollSharedMemoryTransport implements AsyncTransport over shared memory
 * with GQM-based ring-queue notification for ultra-low-latency IPC.
 *
 * Communication model (per direction):
 *   MemoryRegion  – flat data buffer (no internal ring-buffer logic)
 *   GqmInterface  – ring queue of (offset, length) descriptors
 *
 * The writer splits each write into ≤ kMaxChunkSize chunks, copies them
 * into the data region at a locally-maintained writeCursor, and pushes
 * one GQM descriptor per chunk.  The reader pops GQM descriptors and
 * copies the referenced bytes out of the data region.
 *
 * Flow control: dataRegionSize >= gqmDepth * maxChunkSize guarantees
 * that as long as GQM push succeeds, the data region will not be
 * over-written.  When GQM is full the writer blocks (backpressure).
 */
class BusyPollSharedMemoryTransport : public AsyncTransport {
 public:
  using UniquePtr = std::unique_ptr<BusyPollSharedMemoryTransport, Destructor>;

  enum class PollingMode {
    BUSY_POLL,
    ADAPTIVE,
    DEDICATED_CORE,
    EVENTBASE,
  };

  struct Config {
    size_t dataRegionSize = 4 * 1024 * 1024;
    std::string shmNamePrefix = "/thrift_shm_";
    PollingMode pollingMode = PollingMode::ADAPTIVE;
    int pinnedCore = -1;

    uint32_t spinLimit = 1000;
    uint32_t highLoadThreshold = 10;
    uint32_t sleepTimeoutUs = 100;

    uint16_t maxChunkSize = GqmNotification::kMaxChunkSize;

    std::shared_ptr<MemoryProvider> memoryProvider;

    // Pool names for ImportedMemoryProvider (CXL device file path).
    // writePoolName: pool where this side allocates write data + GQM.
    // readPoolName:  pool where peer's write data + GQM reside (import).
    std::string writePoolName;
    std::string readPoolName;

    bool debugLogging = false;
  };

  /**
   * Create from pre-established memory regions and GQM queues (after
   * the handshake completes).  Legacy per-connection mode.
   */
  static UniquePtr create(
      EventBase* evb,
      std::unique_ptr<MemoryRegion> writeDataRegion,
      std::unique_ptr<MemoryRegion> readDataRegion,
      std::unique_ptr<GqmInterface> gqmWrite,
      std::unique_ptr<GqmInterface> gqmRead);

  static UniquePtr create(
      EventBase* evb,
      std::unique_ptr<MemoryRegion> writeDataRegion,
      std::unique_ptr<MemoryRegion> readDataRegion,
      std::unique_ptr<GqmInterface> gqmWrite,
      std::unique_ptr<GqmInterface> gqmRead,
      const Config& config);

  /**
   * Create a lightweight transport backed by a shared ShmPollerService.
   * The transport does not own GQM/data regions or poller threads.
   */
  static UniquePtr createShared(
      EventBase* evb,
      ShmPollerService* pollerService,
      uint16_t localConnId,
      uint16_t peerConnId);

  ~BusyPollSharedMemoryTransport() override;

  BusyPollSharedMemoryTransport(const BusyPollSharedMemoryTransport&) = delete;
  BusyPollSharedMemoryTransport& operator=(
      const BusyPollSharedMemoryTransport&) = delete;

  // ========== AsyncTransport interface ==========
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
  EventBase* getEventBase() const override { return evb_; }

  bool isEorTrackingEnabled() const override { return eorTrackingEnabled_; }
  void setEorTracking(bool track) override { eorTrackingEnabled_ = track; }

  size_t getAppBytesWritten() const override { return bytesWritten_; }
  size_t getRawBytesWritten() const override { return bytesWritten_; }
  size_t getAppBytesReceived() const override { return bytesRead_; }
  size_t getRawBytesReceived() const override { return bytesRead_; }

  // ========== SHM-specific ==========
  void checkForAvailableData();
  bool pollAndDeliver();

  /**
   * Called by ShmPollerService poller thread (via EventBase dispatch).
   * Delivers data to the readCallback using readBufferAvailable (zero-copy)
   * when supported, falling back to getReadBuffer + memcpy otherwise.
   */
  void onDataReceived(std::unique_ptr<IOBuf> data);

  GqmInterface* getGqmRead() { return gqmRead_.get(); }
  uint16_t localConnId() const { return localConnId_; }
  uint16_t peerConnId() const { return peerConnId_; }

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
  BusyPollSharedMemoryTransport(
      EventBase* evb,
      std::unique_ptr<MemoryRegion> writeDataRegion,
      std::unique_ptr<MemoryRegion> readDataRegion,
      std::unique_ptr<GqmInterface> gqmWrite,
      std::unique_ptr<GqmInterface> gqmRead,
      const Config& config);

  BusyPollSharedMemoryTransport(
      EventBase* evb,
      ShmPollerService* pollerService,
      uint16_t localConnId,
      uint16_t peerConnId);

  void writeInternal(
      WriteCallback* callback,
      std::unique_ptr<IOBuf> buf,
      WriteFlags flags);
  void deliverReadData();

  void startPollerThread();
  void stopPollerThread();

  void pollerLoopBusyPoll();
  void pollerLoopAdaptive();
  void pollerLoopDedicatedCore();

  void registerEventBasePoll();
  void unregisterEventBasePoll();

  static void createWakeupFds(int& readFd, int& writeFd);
  static void closeEventFd(int fd);
  static bool writeEventFd(int fd);
  static bool drainEventFd(int fd);

  enum class State { CONNECTED, CLOSING, CLOSED, ERROR };

  EventBase* evb_;
  std::atomic<State> state_{State::CONNECTED};

  ShmPollerService* pollerService_{nullptr};
  uint16_t localConnId_{0};
  uint16_t peerConnId_{0};

  // Flat data regions (no internal ring-buffer logic)
  std::unique_ptr<MemoryRegion> writeDataRegion_;
  std::unique_ptr<MemoryRegion> readDataRegion_;

  // GQM ring queues carry (offset, length) descriptors
  std::unique_ptr<GqmInterface> gqmWrite_;
  std::unique_ptr<GqmInterface> gqmRead_;

  Config config_;

  // Writer-local cursor (not shared; only the writer advances it)
  uint64_t writeCursor_{0};

  ReadCallback* readCallback_{nullptr};

  struct WriteRequest {
    WriteCallback* callback{nullptr};
    std::unique_ptr<IOBuf> buffer;
    size_t bytesWritten{0};
    size_t totalBytes{0};
  };
  std::deque<WriteRequest> pendingWrites_;
  mutable std::mutex writeMutex_;

  IOBufQueue readBufQueue_;

  std::thread pollerThread_;
  std::atomic<bool> pollerRunning_{false};

  int wakeupFd_{-1};
  int wakeupFdWrite_{-1};
  std::atomic<bool> pollerSleeping_{false};

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

  std::atomic<uint64_t> bytesWritten_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> writeCount_{0};
  std::atomic<uint64_t> readCount_{0};
  std::atomic<uint64_t> gqmPushCount_{0};
  std::atomic<uint64_t> gqmPopCount_{0};
  std::atomic<uint64_t> pollCycles_{0};
  std::atomic<uint64_t> sleepCount_{0};

  uint32_t sendTimeoutMs_{0};
  bool eorTrackingEnabled_{false};
  folly::Function<void()> closeCallback_;

  mutable SocketAddress localAddress_;
  mutable SocketAddress peerAddress_;
};

} // namespace folly
