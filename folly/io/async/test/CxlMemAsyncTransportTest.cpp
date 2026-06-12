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

#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace folly {
namespace {

using StubTransport = CxlMemAsyncTransport<CxlMemStubHWQueueBackend>;
using StubDoorbellQueue = CxlMemDoorbellQueue<CxlMemStubHWQueueBackend>;

class CollectReadCallback : public AsyncTransport::ReadCallback {
 public:
  bool isBufferMovable() noexcept override { return true; }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buffer_;
    *lenReturn = sizeof(buffer_);
  }

  void readDataAvailable(size_t len) noexcept override {
    reads.emplace_back(buffer_, len);
  }

  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    readBuf->coalesce();
    reads.emplace_back(
        reinterpret_cast<const char*>(readBuf->data()), readBuf->length());
  }

  void readEOF() noexcept override { ++eofCount; }

  void readErr(const AsyncSocketException&) noexcept override { ++errCount; }

  std::vector<std::string> reads;
  size_t eofCount{0};
  size_t errCount{0};

 private:
  char buffer_[4096];
};

class CountingWriteCallback : public AsyncTransport::WriteCallback {
 public:
  void writeSuccess() noexcept override { ++successCount; }

  void writeErr(size_t bytesWritten, const AsyncSocketException&) noexcept
      override {
    lastBytesWritten = bytesWritten;
    ++errCount;
  }

  size_t successCount{0};
  size_t errCount{0};
  size_t lastBytesWritten{0};
};

struct QueueMemory {
  explicit QueueMemory(size_t count = 1)
      : blocks(count, std::vector<unsigned char>(kCxlMemHWQueueBytes)) {}

  std::vector<CxlMemHWQueueConfig> configs() {
    std::vector<CxlMemHWQueueConfig> result;
    result.reserve(blocks.size());
    for (auto& block : blocks) {
      result.push_back(CxlMemHWQueueConfig{block.data(), block.size()});
    }
    return result;
  }

  std::vector<std::vector<unsigned char>> blocks;
};

class TransportPair {
 public:
  explicit TransportPair(size_t payloadSize = 256)
      : c2sPayload_(payloadSize),
        s2cPayload_(payloadSize),
        c2sData_(c2sDataMemory_.configs()),
        c2sAck_(c2sAckMemory_.configs()),
        s2cData_(s2cDataMemory_.configs()),
        s2cAck_(s2cAckMemory_.configs()) {
    CxlMemAsyncTransportConfig<CxlMemStubHWQueueBackend> clientConfig;
    clientConfig.eventBase = &eventBase_;
    clientConfig.connId = 1;
    clientConfig.outboundPayload = c2sPayload_.data();
    clientConfig.outboundPayloadSize = c2sPayload_.size();
    clientConfig.outboundPayloadBaseOffset = 0;
    clientConfig.inboundPayload = s2cPayload_.data();
    clientConfig.inboundPayloadSize = s2cPayload_.size();
    clientConfig.inboundPayloadBaseOffset = 0;
    clientConfig.outboundDataQueue = &c2sData_;
    clientConfig.outboundAckQueue = &c2sAck_;
    clientConfig.inboundDataQueue = &s2cData_;
    clientConfig.inboundAckQueue = &s2cAck_;

    CxlMemAsyncTransportConfig<CxlMemStubHWQueueBackend> serverConfig;
    serverConfig.eventBase = &eventBase_;
    serverConfig.connId = 1;
    serverConfig.outboundPayload = s2cPayload_.data();
    serverConfig.outboundPayloadSize = s2cPayload_.size();
    serverConfig.outboundPayloadBaseOffset = 0;
    serverConfig.inboundPayload = c2sPayload_.data();
    serverConfig.inboundPayloadSize = c2sPayload_.size();
    serverConfig.inboundPayloadBaseOffset = 0;
    serverConfig.outboundDataQueue = &s2cData_;
    serverConfig.outboundAckQueue = &s2cAck_;
    serverConfig.inboundDataQueue = &c2sData_;
    serverConfig.inboundAckQueue = &c2sAck_;

    client = std::make_unique<StubTransport>(clientConfig);
    server = std::make_unique<StubTransport>(serverConfig);
  }

  std::unique_ptr<IOBuf> buffer(const std::string& data) {
    return IOBuf::copyBuffer(data);
  }

  EventBase eventBase_;
  QueueMemory c2sDataMemory_;
  QueueMemory c2sAckMemory_;
  QueueMemory s2cDataMemory_;
  QueueMemory s2cAckMemory_;
  std::vector<unsigned char> c2sPayload_;
  std::vector<unsigned char> s2cPayload_;
  StubDoorbellQueue c2sData_;
  StubDoorbellQueue c2sAck_;
  StubDoorbellQueue s2cData_;
  StubDoorbellQueue s2cAck_;
  std::unique_ptr<StubTransport> client;
  std::unique_ptr<StubTransport> server;
};

std::string join(const std::vector<std::string>& parts) {
  std::string result;
  for (const auto& part : parts) {
    result += part;
  }
  return result;
}

} // namespace

TEST(CxlMemAsyncTransport, transfersSmallIOBufBetweenPeers) {
  TransportPair pair;
  CollectReadCallback readCallback;
  CountingWriteCallback writeCallback;
  pair.server->setReadCB(&readCallback);

  pair.client->writeChain(&writeCallback, pair.buffer("hello"));
  EXPECT_EQ(1, writeCallback.successCount);

  EXPECT_EQ(1, pair.server->drainInbound());
  ASSERT_EQ(1, readCallback.reads.size());
  EXPECT_EQ("hello", readCallback.reads[0]);
}

TEST(CxlMemAsyncTransport, deliversMultipleIOBufsInOrder) {
  TransportPair pair;
  CollectReadCallback readCallback;
  CountingWriteCallback firstCallback;
  CountingWriteCallback secondCallback;
  pair.server->setReadCB(&readCallback);

  pair.client->writeChain(&firstCallback, pair.buffer("abc"));
  pair.client->writeChain(&secondCallback, pair.buffer("def"));
  EXPECT_EQ(1, firstCallback.successCount);
  EXPECT_EQ(1, secondCallback.successCount);

  EXPECT_EQ(2, pair.server->drainInbound());
  ASSERT_EQ(2, readCallback.reads.size());
  EXPECT_EQ("abc", readCallback.reads[0]);
  EXPECT_EQ("def", readCallback.reads[1]);
}

TEST(CxlMemAsyncTransport, dataQueueFullDefersWriteCallbackUntilFlush) {
  TransportPair pair;
  CountingWriteCallback writeCallback;

  for (size_t i = 0; i < kCxlMemHWQueueCapacity; ++i) {
    ASSERT_TRUE(pair.c2sData_.push(CxlMemFrameCodec::encodeDataItem(9, 0, 1)));
  }

  pair.client->writeChain(&writeCallback, pair.buffer("z"));
  EXPECT_EQ(0, writeCallback.successCount);
  EXPECT_EQ(1, pair.client->getAppBytesBuffered());

  uint64_t ignored = 0;
  ASSERT_TRUE(pair.c2sData_.pop(&ignored));
  pair.client->flushPendingWrites();
  EXPECT_EQ(1, writeCallback.successCount);
  EXPECT_EQ(0, pair.client->getAppBytesBuffered());
}

TEST(CxlMemAsyncTransport, payloadFullDefersWriteUntilAckReleasesSpace) {
  TransportPair pair(8);
  CountingWriteCallback firstCallback;
  CountingWriteCallback secondCallback;

  pair.client->writeChain(&firstCallback, pair.buffer("12345678"));
  EXPECT_EQ(1, firstCallback.successCount);

  pair.client->writeChain(&secondCallback, pair.buffer("x"));
  EXPECT_EQ(0, secondCallback.successCount);
  EXPECT_EQ(1, pair.client->getAppBytesBuffered());

  ASSERT_TRUE(pair.c2sAck_.push(CxlMemFrameCodec::encodeAckItem(8)));
  pair.client->flushPendingWrites();
  EXPECT_EQ(1, secondCallback.successCount);
  EXPECT_EQ(0, pair.client->getAppBytesBuffered());
}

TEST(CxlMemAsyncTransport, splitsPayloadsLargerThanDataItemLength) {
  TransportPair pair(128 * 1024);
  CollectReadCallback readCallback;
  CountingWriteCallback writeCallback;
  pair.server->setReadCB(&readCallback);
  const std::string payload(70000, 'a');

  pair.client->writeChain(&writeCallback, pair.buffer(payload));
  EXPECT_EQ(1, writeCallback.successCount);

  EXPECT_EQ(2, pair.server->drainInbound());
  EXPECT_EQ(payload, join(readCallback.reads));
}

TEST(CxlMemAsyncTransport, closeNowFailsPendingWrite) {
  TransportPair pair;
  CountingWriteCallback writeCallback;
  for (size_t i = 0; i < kCxlMemHWQueueCapacity; ++i) {
    ASSERT_TRUE(pair.c2sData_.push(CxlMemFrameCodec::encodeDataItem(9, 0, 1)));
  }

  pair.client->writeChain(&writeCallback, pair.buffer("z"));
  ASSERT_EQ(0, writeCallback.successCount);

  pair.client->closeNow();
  EXPECT_EQ(1, writeCallback.errCount);
  EXPECT_EQ(0, writeCallback.lastBytesWritten);
  EXPECT_FALSE(pair.client->good());
}

} // namespace folly
