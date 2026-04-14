# Folly + FBThrift 共享内存传输层：数据流与线程模型深度分析

> 分支：`fix/shm-transport-review-fixes`
> 核心目标：为 Thrift RPC 新增基于 POSIX 共享内存 + GQM 通知的超低延迟传输层，替代 TCP socket

---

## 目录

1. [架构总览](#1-架构总览)
2. [数据流图](#2-数据流图)
   - [2.1 连接建立（Handshake）](#21-连接建立handshake)
   - [2.2 稳态数据传输](#22-稳态数据传输)
   - [2.3 连接关闭](#23-连接关闭)
3. [线程模型](#3-线程模型)
   - [3.1 ThriftServer 双线程池](#31-thriftserver-双线程池)
   - [3.2 SHM Poller 线程四种策略](#32-shm-poller-线程四种策略)
   - [3.3 线程交互时序](#33-线程交互时序)
4. [关键代码逐行解析](#4-关键代码逐行解析)
   - [4.1 共享内存环形缓冲区](#41-共享内存环形缓冲区)
   - [4.2 GQM 通知队列](#42-gqm-通知队列)
   - [4.3 BusyPollTransport 写路径](#43-busypolltransport-写路径)
   - [4.4 BusyPollTransport 读路径](#44-busypolltransport-读路径)
   - [4.5 ADAPTIVE 自适应轮询](#45-adaptive-自适应轮询)
   - [4.6 ThriftServer 集成点](#46-thriftserver-集成点)
5. [内存布局](#5-内存布局)
6. [同步机制](#6-同步机制)
7. [涉及文件清单](#7-涉及文件清单)

---

## 1. 架构总览

本次修改在 Folly 基础库和 FBThrift RPC 框架之间新增了一套完整的共享内存传输方案，架构分为三层：

```
┌─────────────────────────────────────────────────────────────────┐
│                     FBThrift (应用层)                           │
│  Cpp2Worker::createThriftTransport() — TCP → SHM 透明升级      │
├─────────────────────────────────────────────────────────────────┤
│                     Folly (传输层)                              │
│                                                                 │
│  ┌──────────────────────┐  ┌──────────────────────────────────┐ │
│  │ SharedMemoryTransport│  │ BusyPollSharedMemoryTransport    │ │
│  │  (基础版, 基于定时器) │  │  (高性能版, 基于独立 poller)     │ │
│  │  AsyncTimeout polling│  │  4 种 PollingMode 策略           │ │
│  └──────────┬───────────┘  └──────────┬───────────────────────┘ │
│             │                         │                        │
│  ┌──────────▼─────────────────────────▼───────────────────────┐ │
│  │              SharedMemoryRegion (POSIX shm)                 │ │
│  │  环形缓冲区 + atomic offset + flags                         │ │
│  └──────────────────────┬─────────────────────────────────────┘ │
│                         │                                      │
│  ┌──────────────────────▼─────────────────────────────────────┐ │
│  │              GqmInterface (通知队列)                        │ │
│  │  SharedMemoryGqm / DefaultGqmInterface / NullGqmInterface  │ │
│  └────────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│                     POSIX / Linux 内核                         │
│  shm_open + mmap (共享内存)  |  eventfd/pipe (唤醒)           │
└─────────────────────────────────────────────────────────────────┘
```

存在两种 `AsyncTransport` 实现：

| 实现 | 文件 | 轮询方式 | 适用场景 |
|------|------|---------|---------|
| `SharedMemoryTransport` | `SharedMemoryTransport.h/.cpp` | `AsyncTimeout` 1ms 定时器轮询 | 简单场景、调试 |
| `BusyPollSharedMemoryTransport` | `BusyPollSharedMemoryTransport.h/.cpp` | 独立 poller 线程 + 4 种策略 | 生产环境（Cpp2Worker 使用此版本） |

两者都实现 `folly::AsyncTransport` 接口，对上层 Thrift 协议栈完全透明。

---

## 2. 数据流图

### 2.1 连接建立（Handshake）

Handshake 的核心思路：**先通过 TCP socket 交换 SHM/GQM 名称参数，再关闭 TCP，切换到纯共享内存通信。**

```
Client Process                                           Server Process
─────────────                                           ─────────────
ThriftChannel / Connect                                 ThriftServer Accept
  │                                                       │
  │  TCP connect ──────────────────────────────────►  TCP accept
  │                                                       │
  │  onNewConnectionThatMayThrow(sock)                     │
  │                                                       │
  │  createThriftTransport(sock)                           │
  │  ┌────────────────────────────┐                       │
  │  │ server_->getUseShmTransport│──► true               │
  │  │                            │                       │
  │  │ shmHandshakeServer(evb,    │                       │
  │  │   sock, shmConfig)         │                       │
  │  └────────────────────────────┘                       │
  │                                                       │
  │  ════════ Handshake Protocol ════════                 │
  │                                                       │
  │  Step 1: Client → Server                              │
  │  ┌──────────────────────────────────────────┐         │
  │  │ Length-prefixed frame:                    │         │
  │  │   [payload_len: uint32 BE]                │         │
  │  │   [magic: 0x53484D54 "SHMT"]              │         │
  │  │   [version: 1]                            │         │
  │  │   [writeShmNameLen: uint32 BE]            │         │
  │  │   [writeShmName: bytes]                   │         │
  │  │   [dataRegionSize: uint64 BE]             │         │
  │  │   [gqmWriteNameLen: uint32 BE]            │         │
  │  │   [gqmWriteName: bytes]                   │         │
  │  │   [gqmQueueDepth: uint32 BE]              │         │
  │  └──────────────────────────────────────────┘         │
  │                                                       │
  │  Step 2: Server → Client (same format)                │
  │                                                       │
  │  ════════ Resource Creation ════════                  │
  │                                                       │
  │  Client creates:                    Server creates:    │
  │  ├─ SharedMemoryRegion::create(      ├─ SMR::create(  │
  │  │    clientWriteShm, true)           │    srvWrite,true│
  │  ├─ SharedMemoryGqm::create(          ├─ SMG::create( │
  │  │    clientGqm)                       │    srvGqm)    │
  │  │                                    │               │
  │  Client opens:                      Server opens:     │
  │  ├─ SharedMemoryRegion::create(      ├─ SMR::create( │
  │  │    srvWriteShm, false)             │    cliWrite,  │
  │  ├─ SharedMemoryGqm::open(           │    false)     │
  │  │    srvGqm)                         ├─ SMG::open(  │
  │                                     │    cliGqm)     │
  │  sock->close()                        sock->close()   │
  │                                                       │
  │  ════════ Transport Created ════════                   │
  │                                                       │
  │  BusyPollSharedMemoryTransport::create(evb,           │
  │    writeRegion, readRegion, gqmWrite, gqmRead, config)│
  │    → starts poller thread (ADAPTIVE mode)              │
  │                                                       │
  │  return convertToShared(transport)  ──► Cpp2Connection│
  │                                            Rocket 协议│
  │                                            解码/编码   │
```

握手协议的关键代码在 `BusyPollShmHandshake.cpp`：

**Server 侧握手流程** (`shmHandshakeServer`, L299-350):

```cpp
// Step 1: 阻塞式读取客户端握手帧
ShmHandshakeInfo clientInfo;
if (!readFramedHandshake(evb, sock, clientInfo)) {
  throw std::runtime_error("Server handshake: failed to read client info");
}

// Step 2: 生成服务端唯一名称
ShmHandshakeInfo myInfo;
myInfo.writeShmName = generateShmName(config.shmNamePrefix, true, uniqueId);
myInfo.gqmWriteName = generateShmName("/thrift_gqm_", true, uniqueId);

// Step 3: 发送服务端握手帧（syncWrite 内部用 evb->loopOnce 驱动）
auto sendBuf = serializeHandshakeInfo(myInfo);
if (!syncWrite(evb, sock, std::move(sendBuf))) { ... }

// Step 4: 创建 SHM 数据区域 — 自己的写区域(create=true), 对方的写区域(open=false)
auto writeRegion = SharedMemoryRegion::create(myInfo.writeShmName, ..., true);
auto readRegion  = SharedMemoryRegion::create(clientInfo.writeShmName, ..., false);

// Step 5: 创建 GQM 队列 — 自己的推送队列(create), 对方的推送队列(open)
auto gqmWrite = SharedMemoryGqm::create(myInfo.gqmWriteName, ...);
auto gqmRead  = SharedMemoryGqm::open(clientInfo.gqmWriteName, ...);

// Step 6: 关闭 TCP socket — 后续纯走 SHM
sock->close();
```

名称生成确保全局唯一（`BusyPollShmHandshake.cpp:108-112`）：

```cpp
std::string generateShmName(const std::string& prefix, bool isServer, uint64_t id) {
  return folly::sformat("{}{}_{:x}_{}", prefix, isServer ? "s" : "c", id, ::getpid());
  // 例如: "/thrift_shm_s_18a3f2b1c4d_12345"
}
```

握手使用 `syncRead`/`syncWrite` 辅助函数，在 EventBase 上通过 `loopOnce(EVLOOP_NONBLOCK)` 驱动异步 socket 完成同步式收发，并带有 5 秒超时保护（L132-133）。

### 2.2 稳态数据传输

握手完成后，数据完全通过共享内存传输，不再经过内核网络栈：

```
Client Process                                    Server Process
══════════════                                  ══════════════

  ┌──────────────────┐                          ┌──────────────────┐
  │ writeRegion (A)  │──── memcpy ────►         │ readRegion (A)   │  ← Server 读
  │ Client writes    │                          │ Server reads     │
  └──────────────────┘                          └──────────────────┘
         │                                              ▲
         │ gqmWrite_(A) → push({offset, len})           │ gqmRead_(A) → pop()
         │  (pure userspace atomic)                     │  (pure userspace atomic)
         │                                              │
         │         ┌─────────────────────┐              │
         └────────►│  GQM Queue (A)      │──────────────┘
                   │  Client push        │
                   │  Server pop         │
                   │  depth = 1024       │
                   └─────────────────────┘


  ┌──────────────────┐                          ┌──────────────────┐
  │ readRegion (B)   │◄──── memcpy ────         │ writeRegion (B)  │  ← Server 写
  │ Client reads     │                          │ Server writes    │
  └──────────────────┘                          └──────────────────┘
         ▲                                              │
         │ gqmRead_(B) → pop()                          │ gqmWrite_(B) → push({offset, len})
         │                                              │
         │         ┌─────────────────────┐              │
         └─────────│  GQM Queue (B)      │◄─────────────┘
                   │  Server push        │
                   │  Client pop         │
                   └─────────────────────┘
```

**每个方向需要一对资源**：
- 1 个 `SharedMemoryRegion`（环形缓冲区，默认 4MB）
- 1 个 `GqmInterface`（通知队列，默认深度 1024）

双向通信共需要 2 个 SHM 区域 + 2 个 GQM 队列。

### 2.3 连接关闭

```
close()
  ├─ state_: CONNECTED → CLOSING (CAS)
  ├─ writeRegion_->close()  → header flags |= kFlagClosed
  ├─ readRegion_->close()   → header flags |= kFlagClosed
  └─ closeNow()
      ├─ state_: → CLOSED (exchange)
      ├─ stopPollerThread()
      │   ├─ pollerRunning_ = false (release)
      │   ├─ writeEventFd(wakeupFdWrite_)  // 唤醒 ADAPTIVE 模式下可能睡眠的 poller
      │   └─ pollerThread_.join()
      ├─ unregisterEventBasePoll()
      ├─ close(wakeupFd_) / close(wakeupFdWrite_)
      ├─ fail pending writes (writeErr callback)
      ├─ readCallback_->readEOF()
      └─ closeCallback_()

closeWithReset()
  ├─ writeRegion_->header()->setError()  → flags |= kFlagError
  ├─ readRegion_->header()->setError()
  └─ closeNow()
```

`SharedMemoryRegion` 析构时（`SharedMemoryRegion.cpp:127-138`），`isCreator_ == true` 的一方负责 `shm_unlink` 清理 POSIX SHM 名称。

---

## 3. 线程模型

### 3.1 ThriftServer 双线程池

```
┌──────────────────────────────────────────────────────────────────┐
│                         ThriftServer                             │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │             I/O Thread Pool (IOThreadPoolExecutor)           │ │
│  │  numIOWorkerThreads = 通常 = CPU 核数                        │ │
│  │                                                              │ │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐ │ │
│  │  │  Cpp2Worker #0 │  │  Cpp2Worker #1 │  │  Cpp2Worker #N │ │ │
│  │  │  + EventBase   │  │  + EventBase   │  │  + EventBase   │ │ │
│  │  │                │  │                │  │                │ │ │
│  │  │  ┌──────────┐  │  │  ┌──────────┐  │  │  ┌──────────┐  │ │ │
│  │  │  │SHM Poller│  │  │  │SHM Poller│  │  │  │SHM Poller│  │ │ │
│  │  │  │Thread #1 │  │  │  │Thread #2 │  │  │  │Thread #K │  │ │ │
│  │  │  └─────┬────┘  │  │  └─────┬────┘  │  │  └─────┬────┘  │ │ │
│  │  │        │       │  │        │       │  │        │       │ │ │
│  │  │  Connections   │  │  Connections   │  │  Connections   │ │ │
│  │  │  (round-robin  │  │  (round-robin  │  │  (round-robin  │ │ │
│  │  │   accept)      │  │   accept)      │  │   accept)      │ │ │
│  │  └────────┼───────┘  └────────┼───────┘  └────────┼───────┘ │ │
│  │           │                   │                   │          │ │
│  │           └───────────────────┼───────────────────┘          │ │
│  │                               │ dispatchRequest()            │ │
│  └───────────────────────────────┼──────────────────────────────┘ │
│                                  ▼                                │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │             CPU Thread Pool (ThreadManager / Executor)       │ │
│  │  numCPUWorkerThreads = 通常 = CPU 核数                        │ │
│  │                                                              │ │
│  │  ┌───────┐  ┌───────┐  ┌───────┐  ┌───────┐                │ │
│  │  │Thread │  │Thread │  │Thread │  │Thread │   ...           │ │
│  │  │  #0   │  │  #1   │  │  #2   │  │  #3   │                │ │
│  │  │       │  │       │  │       │  │       │                │ │
│  │  │业务   │  │业务   │  │业务   │  │业务   │                │ │
│  │  │Handler│  │Handler│  │Handler│  │Handler│                │ │
│  │  └───────┘  └───────┘  └───────┘  └───────┘                │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

**各线程职责**：

| 线程 | 职责 | SHM 传输中的角色 |
|------|------|------------------|
| **I/O Worker (EventBase)** | 网络 I/O、连接管理、协议解析 | poller 发现数据后，通过 `runInEventBaseThread` 调度到此线程执行 `deliverReadData()` |
| **SHM Poller Thread**（每个连接独立） | 无（传统 TCP 不需要） | 新增线程，轮询 GQM 队列检测新数据 |
| **CPU Worker** | 执行业务逻辑 Handler | 不变 |

### 3.2 SHM Poller 线程四种策略

定义在 `BusyPollSharedMemoryTransport::PollingMode`（`.h:73-82`）：

#### 策略 A：BUSY_POLL — 纯自旋（`.cpp:549-561`）

```cpp
void BusyPollSharedMemoryTransport::pollerLoopBusyPoll() {
  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;
    pollAndDeliver();       // gqmRead_->pop() + deliverReadData()
    __builtin_ia32_pause(); // x86_64: PAUSE 指令降低功耗
  }
}
```

- 适用：对延迟极度敏感、可独占 CPU 核心的场景
- 代价：100% CPU 占用

#### 策略 B：ADAPTIVE — NAPI 风格自适应（`.cpp:565-621`）

```cpp
void BusyPollSharedMemoryTransport::pollerLoopAdaptive() {
  uint32_t consecutiveHits = 0;
  uint32_t spinCount = 0;

  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;
    bool found = pollAndDeliver();

    if (found) {
      consecutiveHits++;     // 连续命中，保持在 spin 模式
      spinCount = 0;
    } else {
      consecutiveHits = 0;
      spinCount++;           // 连续未命中，累计空闲次数
    }

    if (consecutiveHits >= config_.highLoadThreshold) {
      // 高负载：保持 spin（仅 PAUSE）
      __builtin_ia32_pause();
    } else if (spinCount > config_.spinLimit) {
      // 空闲太久：阻塞等待唤醒
      pollerSleeping_.store(true, std::memory_order_release);
      sleepCount_++;

      struct pollfd pfd;
      pfd.fd = wakeupFd_;
      pfd.events = POLLIN;
      int timeoutMs = std::max(1, (int)(config_.sleepTimeoutUs / 1000));
      ::poll(&pfd, 1, timeoutMs);   // 阻塞在 eventfd/pipe 上
      drainEventFd(wakeupFd_);       // 清空唤醒信号

      pollerSleeping_.store(false, std::memory_order_release);
      spinCount = 0;
    } else {
      // 短暂空闲：spin + PAUSE
      __builtin_ia32_pause();
    }
  }
}
```

**状态转换图**：

```
           consecutiveHits >= highLoadThreshold
          ┌──────────────────────────────────────┐
          │                                      │
          ▼                                      │
     ┌─────────┐    consecutiveHits == 0    ┌────┴─────┐
     │  SPIN   │───────────────────────────►│  YIELD   │
     │ (PAUSE) │                             │ (PAUSE)  │
     └─────────┘                             └────┬─────┘
                                                 │
                                        spinCount > spinLimit
                                                 │
                                                 ▼
                                            ┌─────────┐
                                            │  SLEEP  │
                                            │ (poll)  │
                                            └────┬────┘
                                                 │
                                    唤醒 or timeout
                                                 │
                                                 ▼
                                            ┌─────────┐
                                            │  YIELD  │
                                            └─────────┘
```

**默认参数**（`.h:97-103`）：

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `spinLimit` | 1000 | 连续空转多少次后进入睡眠 |
| `highLoadThreshold` | 10 | 连续命中多少次后锁定 spin 模式 |
| `sleepTimeoutUs` | 100 | `poll()` 超时（微秒） |

#### 策略 C：DEDICATED_CORE — 绑核自旋（`.cpp:625-656`）

```cpp
void BusyPollSharedMemoryTransport::pollerLoopDedicatedCore() {
#ifdef __linux__
  if (config_.pinnedCore >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(config_.pinnedCore, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  }
#endif
  // 与 BUSY_POLL 相同的自旋逻辑
  while (pollerRunning_.load(std::memory_order_relaxed)) {
    pollCycles_++;
    pollAndDeliver();
    __builtin_ia32_pause();
  }
}
```

- 与 BUSY_POLL 相同的自旋逻辑，但通过 `pthread_setaffinity_np` 绑定到指定 CPU 核心
- 避免缓存迁移（cache migration），提供最稳定的延迟

#### 策略 D：EVENTBASE — 无额外线程（`.cpp:660-687`）

```cpp
void BusyPollSharedMemoryTransport::PollLoopCallback::runLoopCallback() noexcept {
  if (transport_.state_ != State::CONNECTED || !transport_.evbPollRegistered_) return;
  transport_.pollAndDeliver();
  // 重新注册到下一次 EventBase 循环
  if (transport_.evbPollRegistered_ && transport_.evb_) {
    transport_.evb_->runInLoop(this);
  }
}
```

- 注册为 `EventBase::LoopCallback`，每次 EventBase 事件循环迭代时检查 GQM
- **零额外线程**，但延迟取决于 EventBase 循环频率
- 适合低吞吐、对延迟不敏感的场景

### 3.3 线程交互时序

以 Server 接收一个 RPC 请求并返回响应为例（ADAPTIVE 模式）：

```
Client EventBase    SHM (mem)     Server Poller      Server EventBase      CPU Thread
      │                │              Thread                 │                   │
      │ writeChain()   │                │                     │                   │
      ├─► writeInternal()              │                     │                   │
      │   ├─ SMR::write()              │                     │                   │
      │   │  memcpy(data+off, buf)     │                     │                   │
      │   │  writeOffset.store(+n,     │                     │                   │
      │   │    memory_order_release)   │                     │                   │
      │   └─ signalPeer()              │                     │                   │
      │      gqmWrite_->push({off,len})│                     │                   │
      │                │               │                     │                   │
      │                │    [SPIN]     │                     │                   │
      │                │◄──gqmRead_    │                     │                   │
      │                │    ->pop()    │                     │                   │
      │                │    (hit!)     │                     │                   │
      │                │               ├─ pollAndDeliver()   │                   │
      │                │               │                     │                   │
      │                │               │  not in EVB thread? │                   │
      │                │               ├─ evb_->runInEventBaseThread(            │
      │                │               │    [this]() {       │                   │
      │                │               │      deliverReadData();                │
      │                │               │    })               │                   │
      │                │               │                     ▼                   │
      │                │               │              deliverReadData()          │
      │                │               │              ├─ readRegion_->read()    │
      │                │◄──────────────┼─ memcpy ──────┤  memcpy(buf, data+off) │
      │                │               │              ├─ readCallback_->        │
      │                │               │              │  readDataAvailable(n)   │
      │                │               │              ├─ Rocket frame decode    │
      │                │               │              ├─ dispatchRequest()      │
      │                │               │              └──────────────────────►  │
      │                │               │                                 Handler::execute()
      │                │               │                                      │
      │                │               │                ◄──── Response ────────┤
      │                │               │                     writeChain()       │
      │                │               │                     ├─ SMR::write()    │
      │                │◄── memcpy ────┼─────────────────────┤  memcpy(data)    │
      │                │               │                     └─ gqmWrite_->push()│
      │  ◄── gqmRead_->pop()           │                     │                   │
      │  ◄── memcpy (readRegion)       │                     │                   │
      │  readDataAvailable()           │                     │                   │
      │  Rocket decode + app callback  │                     │                   │
```

**关键线程切换点**：Poller 线程 → EventBase 线程（`BusyPollSharedMemoryTransport.cpp:462-470`）：

```cpp
bool BusyPollSharedMemoryTransport::pollAndDeliver() {
  // 1. 消费所有 GQM 通知
  while (auto notif = gqmRead_->pop()) {
    gqmPopCount_++;
    hasNotification = true;
  }

  if (hasNotification || readRegion_->availableToRead() > 0) {
    // 2. 确保在 EventBase 线程上交付数据（线程安全）
    if (evb_ && evb_->isInEventBaseThread()) {
      deliverReadData();                              // 同步调用
    } else if (evb_) {
      evb_->runInEventBaseThread([this]() {           // 跨线程调度
        if (state_ == State::CONNECTED && readCallback_) {
          deliverReadData();
        }
      });
    }
    return true;
  }
  return false;
}
```

---

## 4. 关键代码逐行解析

### 4.1 共享内存环形缓冲区

**Header 结构**（`SharedMemoryRegion.h:43-68`）：

```cpp
struct SharedMemoryRegionHeader {
  alignas(64) std::atomic<uint64_t> writeOffset{0};  // Producer 更新（cache line 隔离）
  alignas(64) std::atomic<uint64_t> readOffset{0};   // Consumer 更新（cache line 隔离）
  uint64_t dataSize{0};                              // 数据区大小（创建后不变）
  std::atomic<uint64_t> flags{0};                    // 状态标志 (CLOSED/ERROR)
  std::atomic<int32_t> readerEventFd{-1};            // 唤醒 fd（-1 = 未设置）
  uint8_t reserved[20]{};                            // 填充到 64 字节对齐

  static constexpr uint64_t kFlagClosed = 1 << 0;
  static constexpr uint64_t kFlagError  = 1 << 1;
};
```

**创建**（`SharedMemoryRegion.cpp:36-107`）：

```cpp
std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::create(
    const std::string& name, size_t dataSize, bool create) {
  size_t totalSize = kHeaderSize + dataSize;

  if (create) {
    // O_CREAT | O_EXCL 确保原子创建，防止竞争
    fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    ::ftruncate(fd, totalSize);              // 设置大小
  } else {
    fd = ::shm_open(name.c_str(), O_RDWR, 0666);  // 打开已存在的
    ::fstat(fd, &st);                        // 验证大小
  }

  // MAP_SHARED: 写入对其他进程可见
  mappedAddr = ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (create) {
    // placement new 初始化 header
    new (reinterpret_cast<SharedMemoryRegionHeader*>(mappedAddr)) SharedMemoryRegionHeader();
    header->dataSize = dataSize;
  }
}
```

**写入**（`SharedMemoryRegion.cpp:164-193`）：

```cpp
ssize_t SharedMemoryRegion::write(const void* buf, size_t len) {
  if (header_->isClosed() || header_->hasError()) return -1;

  size_t available = availableToWrite();
  if (available == 0) return 0;                        // 背压：缓冲区满

  size_t toWrite = std::min(len, available);
  uint64_t writeOffset = header_->writeOffset.load(std::memory_order_acquire);
  uint64_t offsetInRegion = writeOffset % header_->dataSize;

  // 处理环形缓冲区 wraparound
  size_t firstChunk = std::min(toWrite, header_->dataSize - offsetInRegion);
  ::memcpy(static_cast<char*>(data_) + offsetInRegion, buf, firstChunk);

  if (firstChunk < toWrite) {
    // 跨越尾部 → 回绕到头部继续写
    ::memcpy(data_, static_cast<const char*>(buf) + firstChunk, toWrite - firstChunk);
  }

  // release 保证上面的 memcpy 在 read 之前可见
  header_->writeOffset.store(writeOffset + toWrite, std::memory_order_release);
  return toWrite;
}
```

**读取**（`SharedMemoryRegion.cpp:195-226`）逻辑对称：

```cpp
ssize_t SharedMemoryRegion::read(void* buf, size_t len) {
  size_t available = availableToRead();
  if (available == 0) {
    if (header_->isClosed()) return 0;    // EOF
    return 0;
  }

  size_t toRead = std::min(len, available);
  uint64_t readOffset = header_->readOffset.load(std::memory_order_acquire);
  uint64_t offsetInRegion = readOffset % header_->dataSize;

  // 同样处理 wraparound
  size_t firstChunk = std::min(toRead, header_->dataSize - offsetInRegion);
  ::memcpy(buf, static_cast<const char*>(data_) + offsetInRegion, firstChunk);

  if (firstChunk < toRead) {
    ::memcpy(static_cast<char*>(buf) + firstChunk, data_, toRead - firstChunk);
  }

  header_->readOffset.store(readOffset + toRead, std::memory_order_release);
  return toRead;
}
```

**环形缓冲区示意**：

```
  data_ (4MB by default)
  ┌──────────────────────────────────────────────┐
  │ [consumed]  [unread data]      [free space]  │
  │             ↑                   ↑            │
  │        readOffset          writeOffset       │
  │             │                   │            │
  │             │◄── available ──►  │            │
  │             │    to read        │            │
  │             │                   │            │
  │    wraparound: offset = absoluteOffset % dataSize          │
  └──────────────────────────────────────────────┘

  空间判断:
  availableToWrite = dataSize - (writeOffset - readOffset) - 1   // 保留 1 字节区分满/空
  availableToRead  = writeOffset - readOffset
```

### 4.2 GQM 通知队列

**消息格式**（`GqmInterface.h:32-45`）：

```cpp
struct GqmNotification {
  uint32_t offset;  // 数据在 SHM 中的起始偏移
  uint32_t length;  // 数据长度

  uint64_t toUint64() const {
    return (static_cast<uint64_t>(offset) << 32) | length;  // 打包为 64-bit
  }

  static GqmNotification fromUint64(uint64_t value) {
    return GqmNotification{
        static_cast<uint32_t>(value >> 32),      // 高 32 位 = offset
        static_cast<uint32_t>(value & 0xFFFFFFFF)}; // 低 32 位 = length
  }
};
```

**三级实现体系**：

```
GqmInterface (抽象基类)
  ├── DefaultGqmInterface  — 调用外部 C 函数 gqm_init/push/pop
  │                         适用于有硬件队列库的环境
  ├── SharedMemoryGqm      — POSIX shm + gqm_init
  │                         纯软件实现，零硬件依赖
  │                         create(): shm_open + mmap + gqm_init
  │                         open():   shm_open + mmap
  │                         push/pop: 调用 gqm_push/gqm_pop
  └── NullGqmInterface     — 空实现，push/pop 为 no-op
                            用于测试
```

**SharedMemoryGqm 创建**（`GqmInterface.cpp:97-147`）：

```cpp
std::unique_ptr<SharedMemoryGqm> SharedMemoryGqm::create(
    const std::string& name, size_t queueDepth) {
  // 每个 entry 8 字节 (uint64_t)，额外 128 个 entry 给 GQM 元数据用
  size_t totalSize = (queueDepth + 128) * sizeof(uint64_t);

  int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  ::ftruncate(fd, totalSize);
  void* mappedAddr = ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  // 用外部 gqm_init 初始化队列结构
  if (gqm_init(mappedAddr, totalSize) != 0) {
    throw std::runtime_error("Failed to gqm_init GQM shared memory");
  }
}
```

**push/pop 通过外部 `gqm_push`/`gqm_pop` C 函数实现**（`GqmInterface.cpp:35-40`）：

```cpp
extern "C" {
int  gqm_init(void* queue, size_t size);     // 初始化队列
void gqm_push(void* queue, void* msg, size_t len);  // 原子入队
void* gqm_pop(void* queue);                  // 原子出队，空返回 nullptr
int  gqm_empty(void* queue);                 // 检查是否为空
}
```

这些是纯用户空间原子操作，**零 syscall**。

### 4.3 BusyPollTransport 写路径

**入口**：`writeChain()`（`.cpp:216-231`）→ `writeInternal()`（`.cpp:233-284`）

```cpp
void BusyPollSharedMemoryTransport::writeInternal(
    WriteCallback* callback, std::unique_ptr<IOBuf> buf, WriteFlags) {

  size_t bytesWritten = 0;

  // 遍历 IOBuf 链中的每个 segment，逐段写入共享内存
  for (auto& iov : *buf) {
    ssize_t written = writeRegion_->write(iov.data(), iov.size());
    if (written < 0) {
      callback->writeErr(bytesWritten, AsyncSocketException(..., "Write failed"));
      state_ = State::ERROR;
      return;
    }
    bytesWritten += written;

    if (static_cast<size_t>(written) < iov.size()) {
      // 背压：共享内存缓冲区已满，无法写入全部数据
      callback->writeErr(bytesWritten,
          AsyncSocketException(..., "Shared memory buffer full"));
      return;  // 注意：当前实现不缓冲未写入部分，直接报错
    }
  }

  bytesWritten_ += bytesWritten;
  writeCount_++;

  // 通知对端：零 syscall
  signalPeer();

  if (callback) callback->writeSuccess();
}
```

**signalPeer**（`.cpp:286-297`）：

```cpp
void BusyPollSharedMemoryTransport::signalPeer() {
  if (gqmWrite_) {
    uint64_t writeOff = writeRegion_->header()->writeOffset.load(
        std::memory_order_acquire);
    uint32_t offset = static_cast<uint32_t>(writeOff % writeRegion_->dataSize());
    uint32_t len = static_cast<uint32_t>(
        writeCount_.load(std::memory_order_relaxed));
    gqmWrite_->push({offset, len});   // 纯用户空间原子操作
    gqmPushCount_++;
  }
}
```

**写路径完整数据流**：

```
writeChain(IOBuf)
  → for (iov : *buf)
    → writeRegion_->write(iov.data, iov.size)
      → memcpy(data_ + offsetInRegion, buf, firstChunk)     // 内核不参与
      → memcpy(data_, buf + firstChunk, toWrite - firstChunk) // wraparound
      → writeOffset.store(+n, memory_order_release)          // 内存屏障
  → signalPeer()
    → gqmWrite_->push({offset, len})                        // 零 syscall 通知
  → callback->writeSuccess()                                // 同步完成
```

### 4.4 BusyPollTransport 读路径

**交付数据**（`.cpp:477-514`）：

```cpp
void BusyPollSharedMemoryTransport::deliverReadData() {
  if (!readCallback_ || !readRegion_) return;

  while (size_t available = readRegion_->availableToRead()) {
    void* buf = nullptr;
    size_t bufLen = 0;
    readCallback_->getReadBuffer(&buf, &bufLen);   // 从上层获取读缓冲区

    if (!buf || bufLen == 0) {
      readCallback_->readErr(AsyncSocketException(..., "Invalid read buffer"));
      return;
    }

    size_t toRead = std::min(available, bufLen);
    ssize_t bytesRead = readRegion_->read(buf, toRead);

    if (bytesRead < 0) {
      readCallback_->readErr(AsyncSocketException(..., "Read failed"));
      return;
    }
    if (bytesRead == 0) {
      readCallback_->readEOF();
      return;
    }

    bytesRead_ += bytesRead;
    readCount_++;
    readCallback_->readDataAvailable(bytesRead);   // 通知上层有数据到达
  }
}
```

**读路径完整数据流**：

```
Poller Thread:
  gqmRead_->pop()                                  // 原子出队，零 syscall
    → hasNotification = true
    → evb_->runInEventBaseThread([this]() { ... })  // 跨线程调度

Server EventBase Thread:
  deliverReadData()
    → while (available = readRegion_->availableToRead())
      → readCallback_->getReadBuffer(&buf, &bufLen)
      → readRegion_->read(buf, toRead)
        → memcpy(buf, data_ + offsetInRegion, firstChunk)     // 内核不参与
        → readOffset.store(+n, memory_order_release)
      → readCallback_->readDataAvailable(bytesRead)
        → Rocket 协议帧解码
        → dispatchRequest() → CPU Thread Pool
```

### 4.5 ADAPTIVE 自适应轮询

唤醒机制需要跨平台支持（`.cpp:42-100`）：

```cpp
void BusyPollSharedMemoryTransport::createWakeupFds(int& readFd, int& writeFd) {
#ifdef __linux__
  int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  readFd = fd;         // eventfd 是双向的，读写同一个 fd
  writeFd = fd;
#else
  int pipefd[2];
  ::pipe(pipefd);
  // 设置非阻塞
  ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
  ::fcntl(pipefd[1], F_SETFL, ::fcntl(pipefd[1], F_GETFL) | O_NONBLOCK);
  readFd = pipefd[0];  // 读端
  writeFd = pipefd[1]; // 写端
#endif
}
```

写入唤醒信号：

```cpp
bool BusyPollSharedMemoryTransport::writeEventFd(int fd) {
  if (fd < 0) return false;
#ifdef __linux__
  uint64_t val = 1;
  return ::write(fd, &val, sizeof(val)) == sizeof(val);
#else
  uint8_t c = 1;
  return ::write(fd, &c, sizeof(c)) == sizeof(c);
#endif
}
```

**注意**：Linux 上 `wakeupFd_ == wakeupFdWrite_`（同一个 eventfd），关闭时需要避免 double-close（`.cpp:324-331`）：

```cpp
if (wakeupFdWrite_ >= 0 && wakeupFdWrite_ != wakeupFd_) {
  ::close(wakeupFdWrite_);   // macOS: 关闭 pipe 写端
}
wakeupFdWrite_ = -1;
closeEventFd(wakeupFd_);     // 关闭 eventfd 或 pipe 读端
wakeupFd_ = -1;
```

### 4.6 ThriftServer 集成点

**Cpp2Worker::createThriftTransport()**（`Cpp2Worker.cpp:273-309`）：

```cpp
std::shared_ptr<folly::AsyncTransport> Cpp2Worker::createThriftTransport(
    folly::AsyncTransport::UniquePtr sock) {

  if (server_ && server_->getUseShmTransport()) {
    try {
      // 配置 ADAPTIVE 模式
      folly::BusyPollSharedMemoryTransport::Config shmConfig;
      shmConfig.pollingMode =
          folly::BusyPollSharedMemoryTransport::PollingMode::ADAPTIVE;

      // 执行 SHM 握手
      auto shmResult = folly::shmHandshakeServer(getEventBase(), sock.get(), shmConfig);

      // 创建 BusyPoll Transport
      auto shmTransport = folly::BusyPollSharedMemoryTransport::create(
          getEventBase(),
          std::move(shmResult.writeRegion),   // Server 写区域
          std::move(shmResult.readRegion),    // Client 写区域（Server 读取）
          std::move(shmResult.gqmWrite),      // Server 通知队列（Server push）
          std::move(shmResult.gqmRead),       // Client 通知队列（Server pop）
          shmConfig);

      // 转为 shared_ptr 给 Thrift 上层使用
      return apache::thrift::transport::detail::convertToShared(
          folly::AsyncTransport::UniquePtr(shmTransport.release()));

    } catch (const std::exception& ex) {
      FB_LOG(ERROR) << "SHM handshake failed, falling back to TCP: " << ex.what();
      // 优雅降级到普通 TCP
    }
  }

  // 原有 TCP 路径
  return apache::thrift::transport::detail::convertToShared(std::move(sock));
}
```

**ThriftServer 配置**（`ThriftServer.h:1968,2721-2722`）：

```cpp
class ThriftServer {
  bool useShmTransport_{false};       // 默认关闭

  void setUseShmTransport(bool use) { useShmTransport_ = use; }
  bool getUseShmTransport() const { return useShmTransport_; }
};
```

使用时只需一行：

```cpp
server->setUseShmTransport(true);
```

---

## 5. 内存布局

### 共享内存数据区

```
物理内存 layout (mmap):
┌─────────────────────────────────────────────────────────┐
│ SharedMemoryRegionHeader (≈ 64 bytes, cache-line aligned)│
│ ┌───────────────────────────────────────────────────┐   │
│ │ [Cache Line 0] alignas(64)                        │   │
│ │   atomic<uint64_t> writeOffset                    │   │
│ │   = Producer 写游标                               │   │
│ ├───────────────────────────────────────────────────┤   │
│ │ [Cache Line 1] alignas(64)                        │   │
│ │   atomic<uint64_t> readOffset                     │   │
│ │   = Consumer 读游标                               │   │
│ ├───────────────────────────────────────────────────┤   │
│ │ uint64_t dataSize                                 │   │
│ │ atomic<uint64_t> flags (CLOSED | ERROR)           │   │
│ │ atomic<int32_t>  readerEventFd (-1 or valid)      │   │
│ │ uint8_t reserved[20]                              │   │
│ └───────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────┤
│ Data Region (default 4MB = 4194304 bytes)               │
│                                                         │
│  Virtual addressing:                                     │
│    physicalOffset = absoluteOffset % dataSize           │
│                                                         │
│  ┌─────────────────────────────────────────────┐        │
│  │ [consumed] [  unread data  ] [   free   ]   │        │
│  │     ▲            ▲               ▲          │        │
│  │  readOffset   readOffset      writeOffset   │        │
│  │  (old)       (current)        (current)     │        │
│  └─────────────────────────────────────────────┘        │
│                                                         │
│  Wraparound handling:                                   │
│    if (offsetInRegion + toWrite > dataSize)             │
│      firstChunk = dataSize - offsetInRegion             │
│      secondChunk = toWrite - firstChunk                 │
│      memcpy(data_ + offsetInRegion, buf, firstChunk)   │
│      memcpy(data_, buf + firstChunk, secondChunk)      │
└─────────────────────────────────────────────────────────┘

每个连接需要 2 个这样的区域（双向各一个）。
```

### GQM 队列

```
GQM Queue (SharedMemoryGqm):
┌─────────────────────────────────────────┐
│ [GQM internal metadata]                 │
│  ~ 128 * 8 = 1024 bytes overhead       │
├─────────────────────────────────────────┤
│ Entry 0: uint64_t (offset:32|len:32)   │
│ Entry 1: uint64_t                       │
│ Entry 2: uint64_t                       │
│ ...                                     │
│ Entry 1023: uint64_t                    │
│                                         │
│ Default: 1024 entries                   │
│ Total: (1024 + 128) * 8 = 9216 bytes   │
│ Operations: gqm_push / gqm_pop          │
│   (atomic CAS, zero syscall)            │
└─────────────────────────────────────────┘

每个连接需要 2 个队列（双向各一个）。
```

### 每连接资源开销

| 资源 | 大小 | 数量 |
|------|------|------|
| SharedMemoryRegion (data) | Header + 4MB | 2 (双向) |
| GQM Queue | ~9KB | 2 (双向) |
| Poller Thread (ADAPTIVE) | 1 thread + eventfd | 1 |

---

## 6. 同步机制

### 内存序（Memory Ordering）

```
Writer (Client)                                    Reader (Server)
─────────────                                    ─────────────
memcpy(data_ + off, buf, len)                      │
  ↓                                                │
writeOffset.store(+n, memory_order_release)         │
  ↓ (release barrier)                               │
  ═══════════════════════════════════════════════  │
  ↓ (acquire barrier)                               ↓
                          readOffset.load(acquire)
                                ↓
                          memcpy(buf, data_ + off, len)
```

| 操作 | Memory Order | 位置 |
|------|-------------|------|
| `writeOffset.store(release)` | release | `SharedMemoryRegion.cpp:190` |
| `readOffset.load(acquire)` | acquire | `SharedMemoryRegion.cpp:141,153,177,210` |
| `flags.fetch_or(release)` | release | `SharedMemoryRegion.h:66-67` |
| `flags.load(acquire)` | acquire | `SharedMemoryRegion.h:64-65` |
| `pollerRunning_.load(relaxed)` | relaxed | `BusyPollSharedMemoryTransport.cpp:551,573,645` |
| `pollerRunning_.store(release)` | release | `BusyPollSharedMemoryTransport.cpp:537` |
| `pollerSleeping_.store(release)` | release | `BusyPollSharedMemoryTransport.cpp:593,609` |

### False Sharing 防护

```cpp
struct SharedMemoryRegionHeader {
  alignas(64) std::atomic<uint64_t> writeOffset{0};  // Cache Line 1 (Producer 只写)
  alignas(64) std::atomic<uint64_t> readOffset{0};   // Cache Line 2 (Consumer 只写)
  // ...
};
```

- `writeOffset` 和 `readOffset` 各占独立的 64 字节 cache line
- Producer 更新 `writeOffset` 不会导致 Consumer 的 `readOffset` cache line 失效
- 反之亦然

### 跨线程数据交付

Poller 线程发现数据后，**必须**在 EventBase 线程上调用 `deliverReadData()`（`.cpp:462-470`）：

```cpp
if (evb_ && evb_->isInEventBaseThread()) {
  deliverReadData();                    // 已在正确线程，直接调用
} else if (evb_) {
  evb_->runInEventBaseThread([this]() { // 跨线程调度到 EventBase
    if (state_ == State::CONNECTED && readCallback_) {
      deliverReadData();
    }
  });
}
```

这保证了 `readCallback_->readDataAvailable()` 始终在 EventBase 线程上执行，避免 Thrift 上层的并发问题。

---

## 7. 涉及文件清单

### Folly 侧（新增文件）

| 文件 | 行数 | 职责 |
|------|------|------|
| `folly/io/async/SharedMemoryRegion.h` | 215 | SHM 环形缓冲区 Header 定义 + Region 类 |
| `folly/io/async/SharedMemoryRegion.cpp` | 240 | shm_open/mmap/write/read 实现 |
| `folly/io/async/GqmInterface.h` | 209 | GQM 通知抽象接口 + 三种实现声明 |
| `folly/io/async/GqmInterface.cpp` | 215 | SharedMemoryGqm/DefaultGqm 实现 |
| `folly/io/async/BusyPollSharedMemoryTransport.h` | 339 | 高性能 Transport：4 种 PollingMode + 状态机 |
| `folly/io/async/BusyPollSharedMemoryTransport.cpp` | 704 | 写/读路径 + 4 种 poller 循环 + eventfd/pipe 跨平台 |
| `folly/io/async/BusyPollShmHandshake.h` | 89 | 握手函数声明 + ShmHandshakeInfo/Result |
| `folly/io/async/BusyPollShmHandshake.cpp` | 352 | 客户端/服务端握手协议 + syncRead/syncWrite |
| `folly/io/async/SharedMemoryTransport.h` | 334 | 基础版 Transport（AsyncTimeout 轮询） |
| `folly/io/async/SharedMemoryTransport.cpp` | 647 | 基础版实现（performHandshake + 定时器 poll） |

### FBThrift 侧（修改文件）

| 文件 | 修改位置 | 内容 |
|------|---------|------|
| `thrift/lib/cpp2/server/Cpp2Worker.cpp:25-26` | include 新增 | `BusyPollSharedMemoryTransport.h`, `BusyPollShmHandshake.h` |
| `thrift/lib/cpp2/server/Cpp2Worker.cpp:273-298` | `createThriftTransport()` | SHM 握手 + Transport 创建 + 异常回退 TCP |
| `thrift/lib/cpp2/server/ThriftServer.h:1968` | 成员变量 | `bool useShmTransport_{false}` |
| `thrift/lib/cpp2/server/ThriftServer.h:2716-2722` | 公开接口 | `setUseShmTransport()` / `getUseShmTransport()` |
| `thrift/perf/cpp2/server/Server.cpp` | 性能测试 | `FLAGS_shm` 配置 SHM 开关 |
