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

#include <folly/io/async/CxlMemAsyncTransport.h>

#include <folly/io/async/CxlMemFrameCodec.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace folly {

namespace {

AsyncSocketException notOpenException() {
  return AsyncSocketException(
      AsyncSocketException::NOT_OPEN, "CXL mem transport is closed");
}

AsyncSocketException shutdownException() {
  return AsyncSocketException(
      AsyncSocketException::INVALID_STATE,
      "CXL mem transport write side is shutdown");
}

AsyncSocketException closedWithPendingWritesException() {
  return AsyncSocketException(
      AsyncSocketException::NOT_OPEN,
      "CXL mem transport closed with pending writes");
}

} // namespace

template <typename Backend>
CxlMemAsyncTransport<Backend>::CxlMemAsyncTransport(Config config)
    : config_(config),
      outboundSlice_(CxlMemPayloadSliceConfig{
          config.outboundPayloadBaseOffset,
          config.outboundPayloadSize}),
      eventBase_(config.eventBase) {
  validateConfig();
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::setReadCB(ReadCallback* callback) {
  readCallback_ = callback;
}

template <typename Backend>
typename CxlMemAsyncTransport<Backend>::ReadCallback*
CxlMemAsyncTransport<Backend>::getReadCallback() const {
  return readCallback_;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::write(
    WriteCallback* callback,
    const void* buf,
    size_t bytes,
    WriteFlags flags) {
  (void)flags;
  if (bytes == 0) {
    if (callback != nullptr) {
      callback->writeSuccess();
    }
    return;
  }
  writeChain(callback, IOBuf::copyBuffer(buf, bytes));
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::writev(
    WriteCallback* callback,
    const iovec* vec,
    size_t count,
    WriteFlags flags) {
  (void)flags;
  size_t length = 0;
  for (size_t i = 0; i < count; ++i) {
    length += vec[i].iov_len;
  }

  std::string data;
  data.reserve(length);
  for (size_t i = 0; i < count; ++i) {
    data.append(static_cast<const char*>(vec[i].iov_base), vec[i].iov_len);
  }
  writeChain(callback, IOBuf::copyBuffer(data));
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::writeChain(
    WriteCallback* callback,
    std::unique_ptr<IOBuf>&& buf,
    WriteFlags flags) {
  (void)flags;
  if (!good()) {
    failWriteImmediately(callback, notOpenException());
    return;
  }
  if (writeShutdown_) {
    failWriteImmediately(callback, shutdownException());
    return;
  }

  std::string data = flatten(buf.get());
  if (data.empty()) {
    if (callback != nullptr) {
      callback->writeSuccess();
    }
    return;
  }

  PendingWrite pending;
  pending.data = std::move(data);
  pending.callback = callback;
  pendingWrites_.push_back(std::move(pending));
  flushPendingWrites();
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::flushPendingWrites() {
  if (closed_) {
    return;
  }
  processAcks();

  while (!pendingWrites_.empty()) {
    if (!flushOnePendingWrite(pendingWrites_.front())) {
      return;
    }
    PendingWrite completed = std::move(pendingWrites_.front());
    pendingWrites_.pop_front();
    appBytesWritten_ += completed.data.size();
    if (completed.callback != nullptr) {
      completed.callback->writeSuccess();
    }
  }
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::drainInbound(
    size_t maxItems,
    uint64_t firstItem,
    bool hasFirstItem) {
  if (closed_ || maxItems == 0) {
    return 0;
  }

  size_t drained = 0;
  uint64_t item = firstItem;
  bool hasItem = hasFirstItem;
  while (drained < maxItems) {
    if (!hasItem && !config_.inboundDataQueue->pop(&item)) {
      break;
    }
    hasItem = false;
    const CxlMemDataFrame frame = CxlMemFrameCodec::decodeDataItem(item);
    if (frame.connId == config_.connId && frame.payloadLength != 0) {
      deliverReadBuffer(frame.payloadOffset, frame.payloadLength);
      ++drained;
    }
  }

  if (drained > 0) {
    publishAck(inboundConsumedCursor_);
  }
  return drained;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::close() {
  closed_ = true;
  writeShutdown_ = true;
  closeReadCallback();
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::closeNow() {
  closed_ = true;
  writeShutdown_ = true;
  closeReadCallback();
  failPendingWrites(closedWithPendingWritesException());
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::shutdownWrite() {
  writeShutdown_ = true;
  flushPendingWrites();
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::shutdownWriteNow() {
  writeShutdown_ = true;
  failPendingWrites(shutdownException());
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::good() const {
  return !closed_ && !error_;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::readable() const {
  return good() && readCallback_ != nullptr;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::writable() const {
  return good() && !writeShutdown_;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::connecting() const {
  return false;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::error() const {
  return error_;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::attachEventBase(EventBase* eventBase) {
  eventBase_ = eventBase;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::detachEventBase() {
  eventBase_ = nullptr;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::isDetachable() const {
  return pendingWrites_.empty() && readCallback_ == nullptr;
}

template <typename Backend>
EventBase* CxlMemAsyncTransport<Backend>::getEventBase() const {
  return eventBase_;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::setSendTimeout(uint32_t milliseconds) {
  sendTimeout_ = milliseconds;
}

template <typename Backend>
uint32_t CxlMemAsyncTransport<Backend>::getSendTimeout() const {
  return sendTimeout_;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::getLocalAddress(
    SocketAddress* address) const {
  if (address != nullptr) {
    *address = SocketAddress();
  }
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::getPeerAddress(
    SocketAddress* address) const {
  if (address != nullptr) {
    *address = SocketAddress();
  }
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::isEorTrackingEnabled() const {
  return eorTracking_;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::setEorTracking(bool track) {
  eorTracking_ = track;
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getAppBytesWritten() const {
  return appBytesWritten_;
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getRawBytesWritten() const {
  return appBytesWritten_;
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getAppBytesReceived() const {
  return appBytesReceived_;
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getRawBytesReceived() const {
  return appBytesReceived_;
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getAppBytesBuffered() const {
  return bufferedBytes();
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::getRawBytesBuffered() const {
  return bufferedBytes();
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::validateConfig() const {
  if (config_.outboundPayload == nullptr || config_.inboundPayload == nullptr ||
      config_.outboundPayloadSize == 0 || config_.inboundPayloadSize == 0 ||
      config_.outboundDataQueue == nullptr ||
      config_.outboundAckQueue == nullptr ||
      config_.inboundDataQueue == nullptr || config_.inboundAckQueue == nullptr) {
    throw std::invalid_argument("CxlMemAsyncTransport requires valid queues");
  }
  if (config_.outboundPayloadSize > CxlMemFrameCodec::kMaxAckCursor ||
      config_.inboundPayloadSize > CxlMemFrameCodec::kMaxAckCursor) {
    throw std::invalid_argument("CXL mem payload slice is too large");
  }
  if (config_.outboundPayloadBaseOffset >
      CxlMemFrameCodec::kMaxDataPayloadOffset) {
    throw std::invalid_argument("CXL mem outbound base offset is too large");
  }
  if (config_.inboundPayloadBaseOffset >
      CxlMemFrameCodec::kMaxDataPayloadOffset) {
    throw std::invalid_argument("CXL mem inbound base offset is too large");
  }
}

template <typename Backend>
std::string CxlMemAsyncTransport<Backend>::flatten(IOBuf* buf) const {
  std::string data;
  if (buf == nullptr) {
    return data;
  }
  data.reserve(buf->computeChainDataLength());
  for (auto range : *buf) {
    data.append(
        reinterpret_cast<const char*>(range.data()),
        static_cast<size_t>(range.size()));
  }
  return data;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::flushOnePendingWrite(
    PendingWrite& pending) {
  while (pending.nextByte < pending.data.size()) {
    if (pending.hasReservation) {
      if (!publishReservedFrame(pending)) {
        return false;
      }
      continue;
    }

    const size_t remaining = pending.data.size() - pending.nextByte;
    const size_t frameLength =
        std::min(remaining, CxlMemFrameCodec::kMaxDataPayloadLength);
    uint64_t payloadOffset = 0;
    if (!outboundSlice_.reserve(frameLength, &payloadOffset)) {
      return false;
    }

    pending.reservedOffset = payloadOffset;
    pending.reservedLength = static_cast<uint16_t>(frameLength);
    pending.hasReservation = true;
    const size_t sliceOffset = static_cast<size_t>(
        payloadOffset - config_.outboundPayloadBaseOffset);
    std::memcpy(
        config_.outboundPayload + sliceOffset,
        pending.data.data() + pending.nextByte,
        frameLength);

    if (!publishReservedFrame(pending)) {
      return false;
    }
  }
  return true;
}

template <typename Backend>
bool CxlMemAsyncTransport<Backend>::publishReservedFrame(
    PendingWrite& pending) {
  const uint64_t item = CxlMemFrameCodec::encodeDataItem(
      config_.connId, pending.reservedOffset, pending.reservedLength);
  if (!config_.outboundDataQueue->push(item)) {
    return false;
  }

  outboundSlice_.commit(pending.reservedOffset, pending.reservedLength);
  pending.nextByte += pending.reservedLength;
  pending.reservedOffset = 0;
  pending.reservedLength = 0;
  pending.hasReservation = false;
  return true;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::processAcks() {
  uint64_t item = 0;
  while (config_.outboundAckQueue->pop(&item)) {
    outboundSlice_.releaseThrough(CxlMemFrameCodec::decodeAckItem(item));
  }
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::closeReadCallback() {
  ReadCallback* callback = readCallback_;
  readCallback_ = nullptr;
  if (callback != nullptr) {
    callback->readEOF();
  }
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::failPendingWrites(
    const AsyncSocketException& exception) {
  while (!pendingWrites_.empty()) {
    PendingWrite pending = std::move(pendingWrites_.front());
    pendingWrites_.pop_front();
    if (pending.callback != nullptr) {
      pending.callback->writeErr(pending.nextByte, exception);
    }
  }
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::failWriteImmediately(
    WriteCallback* callback,
    const AsyncSocketException& exception) {
  if (callback != nullptr) {
    callback->writeErr(0, exception);
  }
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::deliverReadBuffer(
    uint64_t payloadOffset,
    uint16_t payloadLength) {
  if (payloadOffset < config_.inboundPayloadBaseOffset) {
    error_ = true;
    return;
  }
  const uint64_t relativeOffset = payloadOffset - config_.inboundPayloadBaseOffset;
  if (relativeOffset + payloadLength > config_.inboundPayloadSize) {
    error_ = true;
    return;
  }

  const auto* data = config_.inboundPayload + relativeOffset;
  if (readCallback_ != nullptr && readCallback_->isBufferMovable()) {
    readCallback_->readBufferAvailable(IOBuf::copyBuffer(data, payloadLength));
  } else if (readCallback_ != nullptr) {
    void* buffer = nullptr;
    size_t bufferLength = 0;
    readCallback_->getReadBuffer(&buffer, &bufferLength);
    if (buffer == nullptr || bufferLength < payloadLength) {
      error_ = true;
      readCallback_->readErr(AsyncSocketException(
          AsyncSocketException::BAD_ARGS,
          "CXL mem read callback returned a small buffer"));
      return;
    }
    std::memcpy(buffer, data, payloadLength);
    readCallback_->readDataAvailable(payloadLength);
  }

  inboundConsumedCursor_ += payloadLength;
  appBytesReceived_ += payloadLength;
}

template <typename Backend>
void CxlMemAsyncTransport<Backend>::publishAck(uint64_t consumedCursor) {
  config_.inboundAckQueue->push(CxlMemFrameCodec::encodeAckItem(consumedCursor));
}

template <typename Backend>
size_t CxlMemAsyncTransport<Backend>::bufferedBytes() const {
  size_t total = 0;
  for (const auto& pending : pendingWrites_) {
    total += pending.data.size() - pending.nextByte;
  }
  return total;
}

template class CxlMemAsyncTransport<CxlMemStubHWQueueBackend>;
template class CxlMemAsyncTransport<CxlMemRealHWQueueBackend>;

} // namespace folly
