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

  ctx.ctrl = reinterpret_cast<ShmControlBlock*>(dataRegion->data());
  if (createGqm) {
    ctx.ctrl->writeCursor.store(0, std::memory_order_relaxed);
    ctx.ctrl->readCursor.store(0, std::memory_order_relaxed);
  }
  ctx.ringBase =
      static_cast<char*>(dataRegion->data()) + kControlBlockSize;
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
  initDirection(readCtx_, provider, readPool, !isGqmCreator);
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
  uint64_t localReadCursor = ctx.ctrl->readCursor.load(
      std::memory_order_relaxed);
  uint32_t idleSpins = 0;

  while (ctx.running.load(std::memory_order_acquire)) {
    auto notif = ctx.gqm->pop();
    if (!notif.has_value()) {
      if (++idleSpins > kMaxSpinCount) {
        sched_yield();
        idleSpins = 0;
      }
      continue;
    }
    idleSpins = 0;

    uint16_t connId = notif->connId;
    uint32_t offset = notif->offset;
    uint16_t length = notif->length;

    auto chunk = IOBuf::create(length);
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
    ctx.ctrl->readCursor.store(localReadCursor, std::memory_order_release);

    {
      std::lock_guard<std::mutex> lk(connMu_);
      auto it = connTable_.find(connId);
      if (it == connTable_.end()) {
        XLOG(DBG5) << "ShmPollerService: dropping data for unknown connId="
                    << connId;
        continue;
      }
      auto* transport = it->second.transport;
      auto* evb = it->second.evb;
      evb->runInEventBaseThread(
          [transport, data = std::move(chunk)]() mutable {
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
  std::lock_guard<std::mutex> lk(connMu_);
  connTable_[connId] = ConnEntry{transport, evb};
  XLOG(INFO) << "ShmPollerService: registered connId=" << connId;
}

void ShmPollerService::unregisterTransport(uint16_t connId) {
  std::lock_guard<std::mutex> lk(connMu_);
  connTable_.erase(connId);
  XLOG(INFO) << "ShmPollerService: unregistered connId=" << connId;
}

// ========== Write path ==========

void ShmPollerService::writeData(
    uint16_t connId, const void* data, size_t len) {
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
      uint64_t w = ctx.ctrl->writeCursor.load(std::memory_order_relaxed);
      uint64_t r = ctx.ctrl->readCursor.load(std::memory_order_acquire);
      if (ctx.usableSize - (w - r) >= chunkLen) {
        break;
      }
      if (++spins > kMaxSpinCount) {
        sched_yield();
        spins = 0;
      }
    }

    uint64_t cursor = ctx.ctrl->writeCursor.fetch_add(
        chunkLen, std::memory_order_relaxed);
    uint32_t offset = static_cast<uint32_t>(cursor % ctx.usableSize);

    size_t firstPart = std::min(
        static_cast<size_t>(chunkLen),
        ctx.usableSize - static_cast<size_t>(offset));
    std::memcpy(ctx.ringBase + offset, src, firstPart);
    if (firstPart < chunkLen) {
      std::memcpy(ctx.ringBase, src + firstPart, chunkLen - firstPart);
    }

    ctx.gqm->push(GqmNotification{connId, offset, chunkLen});

    src += chunkLen;
    remaining -= chunkLen;
  }
}

} // namespace folly
