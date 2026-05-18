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

#include <folly/io/async/ShmPollerService.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/io/async/BusyPollSharedMemoryTransport.h>
#include <folly/logging/xlog.h>

#include <sched.h>

#ifdef __linux__
#include <unistd.h>
#include <fstream>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

// Conditional diag-statistics: zero overhead in release builds.
#if FOLLY_SHM_DIAG_STATS
#define SHM_STAT(expr) expr
#else
#define SHM_STAT(expr) ((void)0)
#endif

namespace folly {

// ========== CPU Topology ==========

ShmPollerService::CpuTopology ShmPollerService::CpuTopology::detect() {
  CpuTopology topo;

#ifdef __linux__
  // Determine number of CPUs
  int numCpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (numCpus <= 0) {
    numCpus = 1;
  }
  topo.numLogicalCpus = static_cast<size_t>(numCpus);

  // Map (physical_id, core_id) -> first cpu in that core
  struct CoreKey {
    int physicalId;
    int coreId;
    bool operator==(const CoreKey& o) const {
      return physicalId == o.physicalId && coreId == o.coreId;
    }
  };
  struct CoreKeyHash {
    size_t operator()(const CoreKey& k) const {
      return std::hash<int>()(k.physicalId) ^
             (std::hash<int>()(k.coreId) << 16);
    }
  };

  std::unordered_map<CoreKey, std::vector<int>, CoreKeyHash> coreMap;

  for (int cpu = 0; cpu < numCpus; ++cpu) {
    // Read physical_id
    std::string physPath =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/physical_id";
    std::ifstream physFile(physPath);
    int physicalId = 0;
    if (physFile.is_open()) {
      physFile >> physicalId;
    }

    // Read core_id
    std::string corePath =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/core_id";
    std::ifstream coreFile(corePath);
    int coreId = 0;
    if (coreFile.is_open()) {
      coreFile >> coreId;
    }

    coreMap[{physicalId, coreId}].push_back(cpu);
  }

  for (auto& [key, cpus] : coreMap) {
    if (!cpus.empty()) {
      topo.physicalCores.push_back(cpus[0]);
      for (size_t i = 1; i < cpus.size(); ++i) {
        topo.htSiblings.push_back(cpus[i]);
      }
    }
  }

#elif defined(__APPLE__)
  // macOS: use sysctl for CPU counts
  int physicalCpu = 0;
  int logicalCpu = 0;
  size_t len = sizeof(physicalCpu);
  sysctlbyname("hw.physicalcpu", &physicalCpu, &len, nullptr, 0);
  len = sizeof(logicalCpu);
  sysctlbyname("hw.logicalcpu", &logicalCpu, &len, nullptr, 0);

  if (physicalCpu <= 0) physicalCpu = 1;
  if (logicalCpu <= 0) logicalCpu = 1;
  topo.numLogicalCpus = static_cast<size_t>(logicalCpu);

  for (int i = 0; i < physicalCpu; ++i) {
    topo.physicalCores.push_back(i);
  }
  for (int i = physicalCpu; i < logicalCpu; ++i) {
    topo.htSiblings.push_back(i);
  }
#else
  topo.numLogicalCpus = 1;
  topo.physicalCores.push_back(0);
#endif

  XLOG(INFO) << "CpuTopology: " << topo.physicalCores.size()
             << " physical cores, " << topo.htSiblings.size()
             << " HT siblings, " << topo.numLogicalCpus << " logical CPUs";

  return topo;
}

// ========== Lifecycle ==========

ShmPollerService::~ShmPollerService() {
  stopPollers();
}

void ShmPollerService::initDirection(
    DirectionContext& ctx,
    ImportedMemoryProvider& provider,
    const std::string& poolName,
    bool createGqm) {
  auto gqmRegion = provider.createFromPool(
      poolName,
      poolName + "_gqm",
      SharedMemoryGqm::kGqmRegionSize,
      4096);

  if (createGqm) {
    ctx.gqm = ImportedGqm::create(std::move(gqmRegion));
  } else {
    ctx.gqm = ImportedGqm::open(std::move(gqmRegion));
  }

  auto dataRegion = provider.createFromPool(
      poolName,
      poolName + "_data",
      provider.poolRemaining(poolName),
      4096);

  auto* base = static_cast<char*>(dataRegion->data());
  ctx.writeCursor =
      reinterpret_cast<std::atomic<uint64_t>*>(base);
  // readCursor is cross-linked in initFromProvider after both directions init.
  ctx.readCursor = nullptr;

  if (createGqm) {
    ctx.writeCursor->store(0, std::memory_order_relaxed);
    // Also zero the +64 slot (will be used as cross-direction readCursor).
    auto* crossSlot =
        reinterpret_cast<std::atomic<uint64_t>*>(base + kCursorSlotSize);
    crossSlot->store(0, std::memory_order_relaxed);
  }
  ctx.ringBase = base + kControlBlockSize;
  ctx.usableSize = dataRegion->size() - kControlBlockSize;
  ctx.dataRegion = std::move(dataRegion);

  XLOG(INFO) << "ShmPollerService::initDirection pool=" << poolName
             << " gqmCreator=" << createGqm
             << " ringSize=" << ctx.usableSize;
}

void ShmPollerService::initFromProvider(
    ImportedMemoryProvider& provider,
    const std::string& writePool,
    const std::string& readPool,
    bool isGqmCreator,
    uint8_t numLanes) {
  numLanes_ = numLanes;
  lanes_.reserve(numLanes);

  for (uint8_t i = 0; i < numLanes; ++i) {
    auto lane = std::make_unique<LaneContext>();
    auto writePoolLane = folly::sformat("{}_lane{}", writePool, i);
    auto readPoolLane  = folly::sformat("{}_lane{}", readPool, i);

    initDirection(lane->writeCtx, provider, writePoolLane, isGqmCreator);
    initDirection(lane->readCtx, provider, readPoolLane, isGqmCreator);

    // Cross-link readCursors so each cursor is NC-written by one side only.
    //
    // writeCtx.readCursor: flow-control feedback for data we write.
    //   The peer (reader of our data) writes this cursor into *its* memfile.
    //   The peer's memfile is our readCtx.dataRegion, at offset +64.
    lane->writeCtx.readCursor = reinterpret_cast<std::atomic<uint64_t>*>(
        static_cast<char*>(lane->readCtx.dataRegion->data()) +
        kCursorSlotSize);

    // readCtx.readCursor: we update this after consuming peer's data.
    //   We write it into *our* memfile (writeCtx.dataRegion), at offset +64.
    lane->readCtx.readCursor = reinterpret_cast<std::atomic<uint64_t>*>(
        static_cast<char*>(lane->writeCtx.dataRegion->data()) +
        kCursorSlotSize);

    XLOG(INFO) << "ShmPollerService: lane " << i
               << " cross-linked readCursors"
               << " writeCtx.readCursor@"
               << static_cast<void*>(lane->writeCtx.readCursor)
               << " readCtx.readCursor@"
               << static_cast<void*>(lane->readCtx.readCursor);

    lanes_.push_back(std::move(lane));
  }

  XLOG(INFO) << "ShmPollerService::initFromProvider " << (int)numLanes
             << " lanes initialized";
}

// ========== Poller threads ==========

void ShmPollerService::pinThreadToCore(std::thread& t, int coreId) {
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(coreId, &cpuset);
  int rc = pthread_setaffinity_np(
      t.native_handle(), sizeof(cpuset), &cpuset);
  if (rc != 0) {
    XLOG(WARNING) << "Failed to pin poller to core " << coreId
                  << ": " << strerror(rc);
  } else {
    XLOG(INFO) << "Poller pinned to core " << coreId;
  }
#elif defined(__APPLE__)
  // macOS: soft affinity via thread_policy_set
  thread_affinity_policy_data_t policy = {coreId + 1}; // 1-based tag
  thread_policy_set(
      pthread_mach_thread_np(t.native_handle()),
      THREAD_AFFINITY_POLICY,
      reinterpret_cast<thread_policy_t>(&policy), 1);
  XLOG(INFO) << "Poller soft-affined to core tag " << (coreId + 1);
#endif
}

void ShmPollerService::startPollers() {
  auto topo = CpuTopology::detect();

  for (size_t i = 0; i < lanes_.size(); ++i) {
    auto& lane = lanes_[i];
    if (lane->readCtx.running.exchange(true)) {
      continue;
    }

    int coreId = -1;
    if (i < topo.physicalCores.size()) {
      coreId = topo.physicalCores[i]; // prefer physical cores
    } else if (i - topo.physicalCores.size() < topo.htSiblings.size()) {
      coreId = topo.htSiblings[i - topo.physicalCores.size()]; // HT fallback
    }

    lane->readPollerThread = std::thread([this, lane = lane.get()]() {
      XLOG(INFO) << "ShmPollerService: read poller started";
      pollerLoop(lane->readCtx, lane->iobufPool);
      XLOG(INFO) << "ShmPollerService: read poller stopped";
    });

    if (coreId >= 0) {
      pinThreadToCore(lane->readPollerThread, coreId);
      lane->pinnedCore = coreId;
    }

    XLOG(INFO) << "ShmPollerService: lane " << i
               << " poller started, pinnedCore=" << lane->pinnedCore;
  }
}

void ShmPollerService::stopPollers() {
  for (auto& lane : lanes_) {
    lane->readCtx.running.store(false, std::memory_order_release);
    if (lane->readPollerThread.joinable()) {
      lane->readPollerThread.join();
    }
  }
}

void ShmPollerService::pollerLoop(DirectionContext& ctx, IOBufPool& pool) {
  uint64_t localReadCursor = ctx.readCursor->load(
      std::memory_order_relaxed);
  uint32_t idleSpins = 0;

  while (ctx.running.load(std::memory_order_acquire)) {
    auto notif = ctx.gqm->pop();
    if (!notif.has_value()) {
      SHM_STAT(diagStats_.popEmptyCount.fetch_add(1, std::memory_order_relaxed));
      if (++idleSpins > kMaxSpinCount) {
        SHM_STAT(diagStats_.popYieldCount.fetch_add(1, std::memory_order_relaxed));
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        idleSpins = 0;
      }
      continue;
    }
    SHM_STAT(diagStats_.popSuccessCount.fetch_add(1, std::memory_order_relaxed));
    idleSpins = 0;

    // Capture timestamp immediately after successful pop (only for stats)
    [[maybe_unused]] auto popTime = std::chrono::steady_clock::now();

    uint16_t connId = notif->connId;
    uint32_t offset = notif->offset;
    uint16_t length = notif->length;

    SHM_STAT(diagStats_.ioBufAllocCount.fetch_add(1, std::memory_order_relaxed));
    // Use pooled buffer to avoid per-chunk malloc/free.
    // When the Thrift parser releases the IOBuf, the custom deleter
    // returns the buffer to the lane's iobufPool for reuse.
    auto* buf = pool.alloc();
    size_t firstPart = std::min(
        static_cast<size_t>(length),
        ctx.usableSize - static_cast<size_t>(offset));
    std::memcpy(buf, ctx.ringBase + offset, firstPart);
    if (firstPart < length) {
      std::memcpy(
          static_cast<uint8_t*>(buf) + firstPart,
          ctx.ringBase,
          length - firstPart);
    }
    auto chunk = IOBuf::takeOwnership(
        buf, length, iobufPoolDeleter, &pool);

    localReadCursor += length;
    ctx.readCursor->store(localReadCursor, std::memory_order_release);

    // Obtain the target EventBase with a brief shared_lock.  Only connId
    // (not the raw transport pointer) is captured by the lambda; the
    // transport is re-looked-up inside the lambda on the EventBase thread.
    //
    // This prevents use-after-free: the transport pointer is obtained and
    // consumed within a single EventBase event-loop turn, which serialises
    // with closeNow()/unregisterTransport() on the same EventBase.
    //
    // Invariant: in shared mode, BusyPollSharedMemoryTransport must only be
    // destroyed on its own EventBase thread.
    EventBase* evb = nullptr;
    {
      SHM_STAT(diagStats_.sharedLockCount.fetch_add(1, std::memory_order_relaxed));
      std::shared_lock lk(connMu_);
      auto it = connTable_.find(connId);
      if (it == connTable_.end()) {
        XLOG(DBG5) << "ShmPollerService: dropping data for unknown connId="
                    << connId;
        continue;
      }
      evb = it->second.evb;
    }
    evb->runInEventBaseThread(
        [connId, data = std::move(chunk), popTime, this]() mutable {
          SHM_STAT({
            auto dispatchTime = std::chrono::steady_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                dispatchTime - popTime).count();
            diagStats_.recordDispatchLatency(static_cast<uint64_t>(ns));
          });

          BusyPollSharedMemoryTransport* transport = nullptr;
          {
            std::shared_lock lk(connMu_);
            auto it = connTable_.find(connId);
            if (it == connTable_.end()) {
              return; // Transport unregistered — drop data
            }
            transport = it->second.transport;
          }
          // Lock released before onDataReceived to avoid deadlock if the
          // read callback triggers close() (reentrant unique_lock).
          // Safety: EventBase is single-threaded, so no other callback
          // (including closeNow/unregisterTransport) can interleave between
          // the lock release and this call.
          transport->onDataReceived(std::move(data));
        });
  }
}

// ========== ConnId management ==========

uint16_t ShmPollerService::allocateConnId() {
  return nextConnId_.fetch_add(1, std::memory_order_relaxed);
}

void ShmPollerService::registerTransport(
    uint16_t connId,
    BusyPollSharedMemoryTransport* transport,
    EventBase* evb,
    uint8_t laneId) {
  std::unique_lock lk(connMu_);
  connTable_[connId] = ConnEntry{transport, evb, laneId};
  XLOG(INFO) << "ShmPollerService: registered connId=" << connId
             << " laneId=" << (int)laneId;
}

void ShmPollerService::unregisterTransport(uint16_t connId) {
  std::unique_lock lk(connMu_);
  connTable_.erase(connId);
  XLOG(INFO) << "ShmPollerService: unregistered connId=" << connId;
}

// ========== Lane selection ==========

uint8_t ShmPollerService::selectLane() {
  return nextLaneId_.fetch_add(1, std::memory_order_relaxed) % numLanes_;
}

// ========== Write path ==========

void ShmPollerService::writeData(
    uint16_t connId, const void* data, size_t len, uint8_t laneId) {
  [[maybe_unused]] auto writeStart = std::chrono::steady_clock::now();

  auto& ctx = lanes_.at(laneId)->writeCtx;
  const auto* src = static_cast<const uint8_t*>(data);
  size_t remaining = len;

  while (remaining > 0) {
    uint16_t chunkLen = static_cast<uint16_t>(
        std::min(remaining,
                 static_cast<size_t>(GqmNotification::kMaxChunkSize)));

    // Flow control: spin until enough free space (with timeout)
    uint32_t spins = 0;
    auto flowStart = std::chrono::steady_clock::now();
    for (;;) {
      uint64_t w = ctx.writeCursor->load(std::memory_order_relaxed);
      uint64_t r = ctx.readCursor->load(std::memory_order_acquire);
      if (ctx.usableSize - (w - r) >= chunkLen) {
        break;
      }
      if (++spins > kMaxSpinCount) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - flowStart).count();
        if (elapsed >= kWriteTimeoutMs) {
          SHM_STAT(diagStats_.writeTimeoutCount.fetch_add(1, std::memory_order_relaxed));
          throw std::runtime_error(folly::sformat(
              "ShmPollerService::writeData: flow control timeout after {}ms "
              "(connId={}, laneId={}, chunkLen={}, usableSize={}, w={}, r={})",
              elapsed, connId, (int)laneId, chunkLen, ctx.usableSize, w, r));
        }
        SHM_STAT(diagStats_.writeFlowControlYields.fetch_add(
            1, std::memory_order_relaxed));
        sched_yield();
        spins = 0;
      }
    }

    uint64_t cursor = ctx.writeCursor->fetch_add(
        chunkLen, std::memory_order_relaxed);

    // Post-reservation check: ensure our reserved range [cursor, cursor+chunkLen)
    // does not overlap unread data.  The pre-check above is an optimization;
    // this post-check is the correctness gate for concurrent writers.
    spins = 0;
    auto postStart = std::chrono::steady_clock::now();
    while (cursor + chunkLen >
           ctx.readCursor->load(std::memory_order_acquire) + ctx.usableSize) {
      if (++spins > kMaxSpinCount) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - postStart).count();
        if (elapsed >= kWriteTimeoutMs) {
          SHM_STAT(diagStats_.writeTimeoutCount.fetch_add(1, std::memory_order_relaxed));
          throw std::runtime_error(folly::sformat(
              "ShmPollerService::writeData: post-reservation timeout after {}ms "
              "(connId={}, laneId={}, cursor={}, chunkLen={}, usableSize={})",
              elapsed, connId, (int)laneId, cursor, chunkLen, ctx.usableSize));
        }
        SHM_STAT(diagStats_.writeFlowControlYields.fetch_add(
            1, std::memory_order_relaxed));
        sched_yield();
        spins = 0;
      }
    }
    uint32_t offset = static_cast<uint32_t>(cursor % ctx.usableSize);

    size_t firstPart = std::min(
        static_cast<size_t>(chunkLen),
        ctx.usableSize - static_cast<size_t>(offset));
    std::memcpy(ctx.ringBase + offset, src, firstPart);
    if (firstPart < chunkLen) {
      std::memcpy(ctx.ringBase, src + firstPart, chunkLen - firstPart);
    }

    // Release fence: ensure data-region stores (memcpy above) are globally
    // visible before the GQM push notification reaches the reader.
    //
    // Without this fence, the CPU (ARM) or compiler (LTO) may reorder the
    // data writes past the GQM head advancement, causing the reader to pop
    // a valid GQM entry but observe stale data in the ring buffer.
    //
    // On x86 (TSO) this fence compiles to nothing; on ARM it emits dmb ish.
    // The fence pairs with the acquire load in ugqm_pop -> pollerLoop.
    std::atomic_thread_fence(std::memory_order_release);

    ctx.gqm->push(GqmNotification{connId, offset, chunkLen});

    src += chunkLen;
    remaining -= chunkLen;
  }

  SHM_STAT({
    auto writeEnd = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        writeEnd - writeStart).count();
    diagStats_.writeCallCount.fetch_add(1, std::memory_order_relaxed);
    diagStats_.writeSumNs.fetch_add(static_cast<uint64_t>(ns),
                                     std::memory_order_relaxed);
  });
}

} // namespace folly
