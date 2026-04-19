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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/GqmInterface.h>
#include <folly/io/async/ImportedMemoryProvider.h>

#include <mutex>
#include <vector>

namespace folly {

class BusyPollSharedMemoryTransport;

/**
 * Per-direction control cursors placed at the start of each data region.
 *
 * In NC+CC mixed mode (one side noncacheable, one side cacheable-coherent),
 * writeCursor and readCursor must live in DIFFERENT memory regions so that
 * each cursor is only NC-written by one side.  The layout per memfile is:
 *
 *   [+0,  +64)   writeCursor — written by this memfile's NC owner
 *   [+64, +128)  cross-direction readCursor — also written by this NC owner
 *
 * The "cross-direction readCursor" at +64 feeds back consumption progress
 * for the OPPOSITE direction's data ring.  This ensures the readCursor
 * is always NC-written (into the reader's own memfile) and CC-read by the
 * writer on the other memfile.
 */
static constexpr size_t kCursorSlotSize = 64; // one cacheline per cursor
static constexpr size_t kControlBlockSize = 2 * kCursorSlotSize; // 128 bytes

/**
 * Set FOLLY_SHM_DIAG_STATS=1 to enable per-iteration diagnostic counters.
 * Default: enabled in debug builds, disabled in release for zero overhead.
 */
#if !defined(FOLLY_SHM_DIAG_STATS)
#if !defined(NDEBUG)
#define FOLLY_SHM_DIAG_STATS 1
#else
#define FOLLY_SHM_DIAG_STATS 0
#endif
#endif

/**
 * ShmPollerService: shared GQM + data ring manager with poller dispatch.
 *
 * Per direction there is exactly ONE GQM queue and ONE data ring buffer,
 * shared across all connections.  One poller thread per direction pops
 * GQM entries, memcpys data into IOBufs, advances readCursor, and
 * dispatches to the correct transport via connId.
 *
 * Thread safety:
 *   - writeData() is called from arbitrary IO threads (thread-safe via
 *     atomic writeCursor + GQM push).
 *   - pollerLoop() runs on a dedicated thread per direction.
 *   - registerTransport/unregisterTransport are mutex-protected.
 *   - allocateConnId() is lock-free (atomic increment).
 */
class ShmPollerService {
 public:
  struct DirectionContext {
    std::unique_ptr<MemoryRegion> dataRegion;
    std::unique_ptr<GqmInterface> gqm;

    // In NC+CC mode these may point into different memfiles.
    // writeCursor lives in this direction's own memfile (+0).
    // readCursor lives in the opposite direction's memfile (+64),
    // cross-linked by initFromProvider after both directions init.
    std::atomic<uint64_t>* writeCursor{nullptr};
    std::atomic<uint64_t>* readCursor{nullptr};

    char* ringBase{nullptr};
    size_t usableSize{0};
    std::thread pollerThread;
    std::atomic<bool> running{false};
  };

  ShmPollerService() = default;
  ~ShmPollerService();

  ShmPollerService(const ShmPollerService&) = delete;
  ShmPollerService& operator=(const ShmPollerService&) = delete;

  /**
   * Initialize both directions from an ImportedMemoryProvider.
   *
   * For each direction, allocates:
   *   1. GQM region (32KB, 4KB-aligned) from the named pool
   *   2. Data region (remaining pool space) from the named pool
   *
   * The GQM creator calls ugqm_withdata_init; the peer calls ImportedGqm::open.
   *
   * @param provider  The ImportedMemoryProvider with registered pools
   * @param writePool Pool name for this side's write direction
   * @param readPool  Pool name for this side's read direction
   * @param isGqmCreator  true if this side should gqm_init (typically
   *                      the side that "owns" the pool)
   */
  void initFromProvider(
      ImportedMemoryProvider& provider,
      const std::string& writePool,
      const std::string& readPool,
      bool isGqmCreator = true);

  /**
   * Start poller threads for the read direction.
   * Must be called after initFromProvider().
   */
  void startPollers();

  /**
   * Stop poller threads.  Called from destructor or explicitly.
   */
  void stopPollers();

  /**
   * Allocate a unique connection ID (monotonically increasing, 1-based).
   */
  uint16_t allocateConnId();

  /**
   * Register a transport for dispatch.  The poller will route data
   * for this connId to the given transport on the given EventBase.
   */
  void registerTransport(
      uint16_t connId,
      BusyPollSharedMemoryTransport* transport,
      EventBase* evb);

  /**
   * Unregister a transport.  In-flight data for this connId will be
   * silently discarded by the poller.
   */
  void unregisterTransport(uint16_t connId);

  /**
   * Write data to the shared ring buffer and push a GQM notification.
   * Called from any IO thread.  Blocks (spin+yield) if the ring is full.
   *
   * @param connId  Connection identifier
   * @param data    Payload pointer
   * @param len     Payload length (will be split into chunks)
   */
  void writeData(uint16_t connId, const void* data, size_t len);

  /**
   * Access the write-direction context (for diagnostics / testing).
   */
  DirectionContext& writeContext() { return writeCtx_; }
  const DirectionContext& writeContext() const { return writeCtx_; }

  DirectionContext& readContext() { return readCtx_; }
  const DirectionContext& readContext() const { return readCtx_; }

  // ========== Diagnostics ==========

  struct DiagStats {
    // Dispatch latency: time from GQM pop to IO-thread lambda execution
    std::atomic<uint64_t> dispatchCount{0};
    std::atomic<uint64_t> dispatchSumNs{0};
    // Power-of-2 histogram: bucket i covers [2^(i+8), 2^(i+9)) ns
    static constexpr int kDispatchBucketOffset = 9;
    static constexpr int kDispatchNumBuckets = 20;
    std::atomic<uint64_t> dispatchBuckets_[kDispatchNumBuckets]{};

    // GQM pop idle: how many empty-pops per successful pop
    std::atomic<uint64_t> popSuccessCount{0};
    std::atomic<uint64_t> popEmptyCount{0};
    std::atomic<uint64_t> popYieldCount{0};

    // Write path
    std::atomic<uint64_t> writeCallCount{0};
    std::atomic<uint64_t> writeSumNs{0};
    std::atomic<uint64_t> writeFlowControlYields{0};
    std::atomic<uint64_t> writeTimeoutCount{0};

    // Per-message overhead
    std::atomic<uint64_t> sharedLockCount{0};
    std::atomic<uint64_t> ioBufAllocCount{0};

    void recordDispatchLatency(uint64_t ns) {
      dispatchCount.fetch_add(1, std::memory_order_relaxed);
      dispatchSumNs.fetch_add(ns, std::memory_order_relaxed);
      int idx = 0;
      if (ns >= (1ULL << kDispatchBucketOffset)) {
        idx = 63 - __builtin_clzll(ns) - kDispatchBucketOffset + 1;
        if (idx < 0) idx = 0;
        if (idx >= kDispatchNumBuckets) idx = kDispatchNumBuckets - 1;
      }
      dispatchBuckets_[idx].fetch_add(1, std::memory_order_relaxed);
    }
  };

  DiagStats& diagStats() { return diagStats_; }
  const DiagStats& diagStats() const { return diagStats_; }

 private:
  static constexpr uint32_t kMaxSpinCount = 1024;
  static constexpr uint32_t kYieldCount = 64;
  static constexpr uint32_t kWriteTimeoutMs = 5000; // 5s backpressure timeout

  void pollerLoop(DirectionContext& ctx);

  void initDirection(
      DirectionContext& ctx,
      ImportedMemoryProvider& provider,
      const std::string& poolName,
      bool createGqm);

  // IOBuf buffer pool: reclaims buffers after the Thrift parser releases
  // them, avoiding per-chunk malloc/free in the poller hot path.
  // Thread-safe: pop() runs on poller thread, push() on arbitrary IO threads.
  struct IOBufPool {
    static constexpr size_t kBufSize = GqmNotification::kMaxChunkSize + 1;
    static constexpr size_t kPoolCapacity = 128;

    std::mutex mu;
    std::vector<void*> freeList;

    void* alloc() {
      {
        std::lock_guard<std::mutex> lk(mu);
        if (!freeList.empty()) {
          void* p = freeList.back();
          freeList.pop_back();
          return p;
        }
      }
      return std::malloc(kBufSize);
    }

    void push(void* p) {
      std::lock_guard<std::mutex> lk(mu);
      if (freeList.size() < kPoolCapacity) {
        freeList.push_back(p);
      } else {
        std::free(p);
      }
    }

    ~IOBufPool() {
      for (void* p : freeList) {
        std::free(p);
      }
    }
  };

  static void iobufPoolDeleter(void* buf, size_t /*size*/, void* ctx) {
    static_cast<IOBufPool*>(ctx)->push(buf);
  }

  DirectionContext writeCtx_;
  DirectionContext readCtx_;

  struct ConnEntry {
    BusyPollSharedMemoryTransport* transport{nullptr};
    EventBase* evb{nullptr};
  };
  mutable std::shared_mutex connMu_;
  std::unordered_map<uint16_t, ConnEntry> connTable_;
  std::atomic<uint16_t> nextConnId_{1};

  IOBufPool iobufPool_;
  DiagStats diagStats_;
};

} // namespace folly
