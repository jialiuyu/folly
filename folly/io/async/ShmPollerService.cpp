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

namespace folly {

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
    bool isGqmCreator) {
  initDirection(writeCtx_, provider, writePool, isGqmCreator);
  initDirection(readCtx_, provider, readPool, isGqmCreator);

  // Cross-link readCursors so each cursor is NC-written by one side only.
  //
  // writeCtx_.readCursor: flow-control feedback for data we write.
  //   The peer (reader of our data) writes this cursor into *its* memfile.
  //   The peer's memfile is our readCtx_.dataRegion, at offset +64.
  writeCtx_.readCursor = reinterpret_cast<std::atomic<uint64_t>*>(
      static_cast<char*>(readCtx_.dataRegion->data()) + kCursorSlotSize);

  // readCtx_.readCursor: we update this after consuming peer's data.
  //   We write it into *our* memfile (writeCtx_.dataRegion), at offset +64.
  readCtx_.readCursor = reinterpret_cast<std::atomic<uint64_t>*>(
      static_cast<char*>(writeCtx_.dataRegion->data()) + kCursorSlotSize);

  XLOG(INFO) << "ShmPollerService: cross-linked readCursors"
             << " writeCtx.readCursor@"
             << static_cast<void*>(writeCtx_.readCursor)
             << " readCtx.readCursor@"
             << static_cast<void*>(readCtx_.readCursor);
}

// ========== Poller threads ==========

void ShmPollerService::startPollers() {
  if (readCtx_.running.exchange(true)) {
    return;
  }
  readCtx_.pollerThread = std::thread([this]() {
    XLOG(INFO) << "ShmPollerService: read poller started";
    pollerLoop(readCtx_);
    XLOG(INFO) << "ShmPollerService: read poller stopped";
  });
}

void ShmPollerService::stopPollers() {
  readCtx_.running.store(false, std::memory_order_release);
  if (readCtx_.pollerThread.joinable()) {
    readCtx_.pollerThread.join();
  }
}

void ShmPollerService::pollerLoop(DirectionContext& ctx) {
  uint64_t localReadCursor = ctx.readCursor->load(
      std::memory_order_relaxed);
  uint32_t idleSpins = 0;

  while (ctx.running.load(std::memory_order_acquire)) {
    auto notif = ctx.gqm->pop();
    if (!notif.has_value()) {
      diagStats_.popEmptyCount.fetch_add(1, std::memory_order_relaxed);
      if (++idleSpins > kMaxSpinCount) {
        diagStats_.popYieldCount.fetch_add(1, std::memory_order_relaxed);
        sched_yield();
        idleSpins = 0;
      }
      continue;
    }
    diagStats_.popSuccessCount.fetch_add(1, std::memory_order_relaxed);
    idleSpins = 0;

    // Capture timestamp immediately after successful pop
    auto popTime = std::chrono::steady_clock::now();

    uint16_t connId = notif->connId;
    uint32_t offset = notif->offset;
    uint16_t length = notif->length;

    diagStats_.ioBufAllocCount.fetch_add(1, std::memory_order_relaxed);
    auto chunk = IOBuf::create(std::max(size_t(1), static_cast<size_t>(length)));
    size_t firstPart = std::min(
        static_cast<size_t>(length),
        ctx.usableSize - static_cast<size_t>(offset));
    std::memcpy(chunk->writableData(), ctx.ringBase + offset, firstPart);
    if (firstPart < length) {
      std::memcpy(
          chunk->writableData() + firstPart,
          ctx.ringBase,
          length - firstPart);
    }
    chunk->append(length);

    localReadCursor += length;
    ctx.readCursor->store(localReadCursor, std::memory_order_release);

    {
      diagStats_.sharedLockCount.fetch_add(1, std::memory_order_relaxed);
      std::shared_lock lk(connMu_);
      auto it = connTable_.find(connId);
      if (it == connTable_.end()) {
        XLOG(DBG5) << "ShmPollerService: dropping data for unknown connId="
                    << connId;
        continue;
      }
      auto* transport = it->second.transport;
      auto* evb = it->second.evb;
      evb->runInEventBaseThread(
          [transport, data = std::move(chunk), popTime, this]() mutable {
            auto dispatchTime = std::chrono::steady_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                dispatchTime - popTime).count();
            diagStats_.recordDispatchLatency(static_cast<uint64_t>(ns));
            transport->onDataReceived(std::move(data));
          });
    }
  }
}

// ========== ConnId management ==========

uint16_t ShmPollerService::allocateConnId() {
  return nextConnId_.fetch_add(1, std::memory_order_relaxed);
}

void ShmPollerService::registerTransport(
    uint16_t connId,
    BusyPollSharedMemoryTransport* transport,
    EventBase* evb) {
  std::unique_lock lk(connMu_);
  connTable_[connId] = ConnEntry{transport, evb};
  XLOG(INFO) << "ShmPollerService: registered connId=" << connId;
}

void ShmPollerService::unregisterTransport(uint16_t connId) {
  std::unique_lock lk(connMu_);
  connTable_.erase(connId);
  XLOG(INFO) << "ShmPollerService: unregistered connId=" << connId;
}

// ========== Write path ==========

void ShmPollerService::writeData(
    uint16_t connId, const void* data, size_t len) {
  auto writeStart = std::chrono::steady_clock::now();

  auto& ctx = writeCtx_;
  const auto* src = static_cast<const uint8_t*>(data);
  size_t remaining = len;

  while (remaining > 0) {
    uint16_t chunkLen = static_cast<uint16_t>(
        std::min(remaining,
                 static_cast<size_t>(GqmNotification::kMaxChunkSize)));

    // Flow control: spin until enough free space
    uint32_t spins = 0;
    for (;;) {
      uint64_t w = ctx.writeCursor->load(std::memory_order_relaxed);
      uint64_t r = ctx.readCursor->load(std::memory_order_acquire);
      if (ctx.usableSize - (w - r) >= chunkLen) {
        break;
      }
      if (++spins > kMaxSpinCount) {
        diagStats_.writeFlowControlYields.fetch_add(
            1, std::memory_order_relaxed);
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
    while (cursor + chunkLen >
           ctx.readCursor->load(std::memory_order_acquire) + ctx.usableSize) {
      if (++spins > kMaxSpinCount) {
        diagStats_.writeFlowControlYields.fetch_add(
            1, std::memory_order_relaxed);
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

  auto writeEnd = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      writeEnd - writeStart).count();
  diagStats_.writeCallCount.fetch_add(1, std::memory_order_relaxed);
  diagStats_.writeSumNs.fetch_add(static_cast<uint64_t>(ns),
                                   std::memory_order_relaxed);
}

} // namespace folly
