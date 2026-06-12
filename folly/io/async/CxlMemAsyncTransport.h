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

#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/CxlMemHWQueue.h>
#include <folly/io/async/CxlMemPayloadSlice.h>
#include <folly/io/async/CxlMemTransportConfig.h>
#include <folly/io/async/WriteFlags.h>
#include <folly/portability/SysUio.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>

namespace folly {

template <typename Backend>
class CxlMemAsyncTransport : public AsyncTransport {
 public:
  using Config = CxlMemAsyncTransportConfig<Backend>;

  explicit CxlMemAsyncTransport(Config config);
  ~CxlMemAsyncTransport() override = default;

  void flushPendingWrites();
  size_t drainInbound(
      size_t maxItems = std::numeric_limits<size_t>::max(),
      uint64_t firstItem = 0,
      bool hasFirstItem = false);

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
  EventBase* getEventBase() const override;

  void setSendTimeout(uint32_t milliseconds) override;
  uint32_t getSendTimeout() const override;
  void getLocalAddress(SocketAddress* address) const override;
  void getPeerAddress(SocketAddress* address) const override;

  bool isEorTrackingEnabled() const override;
  void setEorTracking(bool track) override;
  size_t getAppBytesWritten() const override;
  size_t getRawBytesWritten() const override;
  size_t getAppBytesReceived() const override;
  size_t getRawBytesReceived() const override;
  size_t getAppBytesBuffered() const override;
  size_t getRawBytesBuffered() const override;

 private:
  struct PendingWrite {
    std::string data;
    size_t nextByte{0};
    WriteCallback* callback{nullptr};
    uint64_t reservedOffset{0};
    uint16_t reservedLength{0};
    bool hasReservation{false};
  };

  void validateConfig() const;
  std::string flatten(IOBuf* buf) const;
  bool flushOnePendingWrite(PendingWrite& pending);
  bool publishReservedFrame(PendingWrite& pending);
  void processAcks();
  void closeReadCallback();
  void failPendingWrites(const AsyncSocketException& exception);
  void failWriteImmediately(
      WriteCallback* callback,
      const AsyncSocketException& exception);
  void deliverReadBuffer(uint64_t payloadOffset, uint16_t payloadLength);
  void publishAck(uint64_t consumedCursor);
  size_t bufferedBytes() const;

  Config config_;
  CxlMemPayloadSlice outboundSlice_;
  ReadCallback* readCallback_{nullptr};
  EventBase* eventBase_{nullptr};
  std::deque<PendingWrite> pendingWrites_;
  uint64_t inboundConsumedCursor_{0};
  uint32_t sendTimeout_{0};
  size_t appBytesWritten_{0};
  size_t appBytesReceived_{0};
  bool closed_{false};
  bool error_{false};
  bool writeShutdown_{false};
  bool eorTracking_{false};
};

extern template class CxlMemAsyncTransport<CxlMemStubHWQueueBackend>;
extern template class CxlMemAsyncTransport<CxlMemRealHWQueueBackend>;

} // namespace folly
