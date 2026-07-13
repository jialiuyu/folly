# CXL Mem Transport 方案草案

> 状态：讨论草案。本文用于持续维护当前方案、未决问题和待实现内容。  
> 范围：第一阶段只面向 `fbthrift/thrift/perf/cpp2` 的 Rocket benchmark，目标是用 CXL.mem/shm 快速路径替换 benchmark 依赖的 TCP socket 数据面。

## 目标

- 在 `folly/io/async` 中实现一个可替代 `AsyncSocket` 的 `AsyncTransport` 底层传输。
- 数据面使用 CXL.mem 或 shm 上的共享内存 payload ring。
- 控制面使用硬件队列抽象作为 doorbell。
- `fbthrift/thrift/perf/cpp2` 的 Rocket benchmark 通过新 transport 发送 Rocket SETUP 和业务帧。
- socket 链路保留，用于握手、元信息交换和初始化失败时无缝降级。

## 非目标

- 第一阶段不修改 Rocket 协议层。
- 第一阶段不做运行期 CXL 失败后切回 socket。
- 第一阶段不支持 FD passing、TLS/ALPN、通用生产 ThriftServer 接入。
- 第一阶段不把普通共享 cursor 当作跨端正确性发布机制。

## 已确认约束

- 使用两块 region：
  - `c2s`：client 写，server 读。
  - `s2c`：server 写，client 读。
- CXL.mem 不走标准 CXL API，直接打开指向共享内存的设备文件，并按 NC/CC 方式映射。
- 控制面 socket 保留：
  - 初始化和握手走 socket。
  - 初始化失败继续走原 Rocket socket。
  - 运行期 CXL mem transport 出错则关闭连接并暴露 benchmark 错误。
- Rocket 只看到 `folly::AsyncTransport`，不感知底层是 socket 还是 CXL.mem。
- 每个 IO shard 拥有自己的 payload slice；连接严格绑定所属 EventBase/IO 线程。
- 同一个 payload slice 只有一个 writer：该 IO shard 的 EventBase 线程。
- 没有硬件事件源或中断源，整个数据流由轮询驱动。

## 总体架构

```text
fbthrift perf/cpp2
  Client/Server flags
  socket handshake / fallback
        |
        v
folly::CxlMemAsyncTransport
  EventBase 亲和
  pending writes
  per-IO payload slice
        |
        v
DoorbellQueue
  DATA queues
  ACK queues
  多个 HWQueue 拼接
        |
        v
HWQueue backend
  RealHWQueueBackend
  StubHWQueueBackend
        |
        v
CxlMemRegion / ShmRegion
  c2s region
  s2c region
```

## HWQueue 抽象

`HWQueue` 是底层硬件队列封装，不承载 payload，只承载 8B 控制项。

固定约束：

- 管理一段 4KB 内存。
- 固定 item 大小 8B。
- 最大容量 496 项。
- 对外语义只包含 push/pop。
- 真实硬件保证 push/pop 的执行串行性。
- 测试机使用 stub backend。

建议接口语义：

```cpp
bool push(uint64_t item);
bool pop(uint64_t* item);
size_t capacity() const;
```

热路径语义必须写清楚：

- transport 不把 `HWQueue` 当普通内存队列使用，而把它当 publish/consume primitive。
- 发送方 backend 必须保证 payload 写入在 DATA item push 前按硬件要求完成排序。
- 接收方 pop 到 DATA item 后，依赖 CXL.mem 的一致性和顺序语义读取 payload。
- 普通 CPU fence 不能被描述成跨机器可见性保证。

backend traits：

```cpp
struct RealHWQueueBackend {
  static constexpr bool kThreadSafePush = true;
  static constexpr bool kThreadSafePop = true;
};

struct StubHWQueueBackend {
  static constexpr bool kThreadSafePush = false;
  static constexpr bool kThreadSafePop = false;
};
```

stub backend 必须加锁或等价同步，模拟真实硬件的多线程 push/pop 串行能力和 496 水位。

## DoorbellQueue 抽象

`DoorbellQueue` 是多个 `HWQueue` 的高级封装。

职责：

- 由多个 4KB `HWQueue` 拼接，扩展逻辑容量。
- 隐藏单个硬件队列 496 项限制。
- 为 DATA 和 ACK 分离建模。
- 支持队列选择、容量水位、优先级和背压。
- 后续可动态调整 DATA/ACK 优先级。

建议结构：

```text
每个 IO shard:
  dataDoorbell: DoorbellQueue
  ackDoorbell:  DoorbellQueue

每个 DoorbellQueue:
  HWQueue[queues_per_doorbell]
```

DATA 和 ACK 分离的原因：

- DATA 表示 payload 可读。
- ACK 表示 payload 已消费、空间可回收。
- ACK 与 DATA 可以独立设置优先级。
- ACK 可以批量发布，降低控制面开销。

## Doorbell item 编码

DATA item 初始编码：

```text
[63:48] connId
[47:16] payload offset
[15:0]  payload length
```

约束：

- `payload length` 最大 65535 字节。
- 超过 64KB 的 `IOBuf` 必须切成多个 DATA item。
- `payload offset` 使用方向 region 内的全局 offset，避免在 8B item 中编码 shard id。
- `connId` 本地查表得到所属 EventBase、transport 和 IO shard。

ACK item 第一阶段候选编码按 per-IO slice 回收：

```text
[63:48] shardId 或保留字段
[47:0]  consumed cursor
```

由于 ACK queue 与 DATA queue 分离，ACK item 不需要 type bit。

## 内存布局

每个方向一块 region。第一阶段按 IO shard 切 payload slice。

```text
region:
  [0, dataDoorbellBytes)
      DATA DoorbellQueue 区域

  [dataDoorbellBytes, dataDoorbellBytes + ackDoorbellBytes)
      ACK DoorbellQueue 区域

  [metadataStart, payloadStart)
      per-IO slice metadata / debug counters

  [payloadStart, end)
      per-IO payload slices
```

每个 IO shard 的 payload slice：

```text
payload slice:
  writeCursorLocal_       本 EventBase 线程本地 uint64_t
  remoteReadCursorLocal_  从 ACK queue 更新，本地 uint64_t
  pending write queue     空间不足或 doorbell 满时缓存待发送 IOBuf
```

不依赖共享 `readCursor/writeCursor` 做跨端正确性。共享 cursor 可作为 debug/metrics，但不参与覆盖判断。

## TX 热路径

发送发生在 transport 所属 EventBase 线程。

```text
Rocket writeChain()
  -> CxlMemAsyncTransport::writeChain()
  -> 检查本 IO slice free space
  -> 拷贝 IOBuf 到 outbound payload slice
  -> backend sender-side ordering
  -> push DATA item 到 dataDoorbell
  -> 调 WriteCallback::writeSuccess()
```

如果 payload slice 空间不足或 DATA DoorbellQueue 满：

```text
不自旋
IOBuf + WriteCallback 入 pending write queue
标记 shard hasPending
由 poller / EventBase retry flush
```

原因：

- 没有硬件事件源，EventBase 自旋无法推动对端释放空间。
- 自旋会阻塞同一 IO 线程上的其他连接。
- `AsyncTransport` 允许 write callback 异步完成。

## RX 热路径

接收由队列所有权转移模型驱动。

```text
central poller pop 到 DATA 首包
  -> 停止扫描该 queue
  -> 保存 handoff item
  -> runInEventBaseThread()

EventBase callback
  -> 处理 handoff item
  -> 继续 pop 同一 queue
  -> 读 payload
  -> 构造 IOBuf
  -> 调 readBufferAvailable()
  -> 累计 consumed cursor
  -> 批量 publish ACK
  -> 到 empty/budget/time 后归还 queue 给 poller
```

payload copy 默认在 EventBase 线程完成，以保持 Rocket parser 和 transport 状态的线程亲和性。poller 不直接调用 Rocket parser。

## ACK 策略

ACK 不要求每个 DATA item 都发布。

建议批量触发条件：

- 消费字节数达到 `ackEveryBytes`。
- 消费 DATA item 数达到 `ackEveryItems`。
- 本端 pending writes 因空间不足阻塞时，提高 ACK 发布优先级。
- EventBase drain 结束时发布一次最新 consumed cursor。

ACK 延迟只会降低空间回收速度；不能导致 writer 提前覆盖未消费 payload。

## Central Poller 与 Queue Ownership Handoff

central poller 是队列调度器和软中断控制器，不是数据处理线程。

每个底层 `HWQueue` 或逻辑 `DoorbellQueue` 有所有权状态：

```text
POLLING    poller 可以扫描和 pop
SCHEDULED  poller 已 pop 首包，EventBase callback 已排队
EVB_OWNED  EventBase 正在 drain
RETURNING  EventBase 准备归还给 poller
CLOSED     不再扫描
```

状态流：

```text
poller:
  q.state == POLLING
  if q.pop(item):
    q.handoff = item
    q.state = SCHEDULED
    evb->runInEventBaseThread(handleQueueHandoff)

EventBase:
  SCHEDULED -> EVB_OWNED
  handle q.handoff
  while budget/time 未耗尽 and q.pop(item):
    handle item
  if shouldContinueLocally:
    evb->runInLoop(callback)
  else:
    q.state = POLLING
    poller.notifyReturned(q)
```

该模型的关键好处：

- 没有 peek 的硬件队列不会被 poller 和 EventBase 同时 pop。
- 低负载时 IO 线程可以睡眠。
- 高负载时 EventBase 接管 queue 连续 drain，路径接近 IO-local polling。
- EventBase 有 budget/time 限制，避免 CXL queue 独占 IO 线程。

## Poller 空闲策略

central poller 无包可 poll 后采用分层退避：

```text
ACTIVE_SCAN -> SHORT_SPIN -> YIELD -> TIMED_SLEEP -> COLD_SLEEP
```

默认 profile 建议为 `adaptive`：

- hot queue 多扫几轮。
- 短自旋若干微秒。
- yield 若干轮。
- sleep 从小延迟开始，指数退避到上限。
- 任意命中后 reset backoff。
- EventBase 归还 queue 是本进程内事件，可以唤醒 sleeping poller。

远端新 DATA 到达无法唤醒 sleeping poller，只能依赖超时扫描。这是无硬件事件源的根本代价。

## PollerGroup 规模

第一阶段建议参数：

```text
--cxl_poller_threads=0
--cxl_io_per_poller=4
--cxl_hwqueues_per_doorbell=2
```

自动模式：

```text
poller_threads = ceil(io_threads / io_per_poller)
```

经验建议：

- 低延迟：每个 poller 服务 1-2 个 IO shard。
- 默认平衡：每个 poller 服务 4 个 IO shard。
- 省 CPU：每个 poller 服务 8 个 IO shard。
- 第一阶段不建议超过 16 个 IO shard / poller。

## fbthrift perf/cpp2 接入

待实现方向：

- client 增加 `--transport=cxl_mem`。
- client 通过 socket 完成 CXL mem 握手，成功后用 `CxlMemAsyncTransport` 创建 `RocketClientChannel`。
- server 增加启用 CXL mem benchmark 的 flag。
- server 保留 socket accept/握手，用于创建 server 侧 `CxlMemAsyncTransport`。
- 初始化失败时继续使用原 socket Rocket。
- 运行期失败关闭连接。

第一阶段应避免破坏现有 Rocket peeking。server 端握手入口仍需细化。

## 待实现内容

### folly

- [x] 新增 `HWQueue` 抽象和 backend traits。
- [x] 新增 `StubHWQueueBackend`，带锁模拟 4KB/496 项/8B item。
- [x] 新增 `RealHWQueueBackend` 接口骨架，封装真实硬件 push/pop 和 sender-side ordering。
- [x] 新增 `DoorbellQueue<Backend>`，支持多个 `HWQueue` 拼接。
- [x] 新增 DATA/ACK 分离的 queue group。
- [x] 新增 `CxlMemRegion` 或等价类，封装设备文件打开、offset、size、NC/CC 映射。
- [x] 新增 per-IO payload slice 管理。
- [x] 新增 `CxlMemAsyncTransport<QueueBackend>`，实现 `folly::AsyncTransport`。
- [ ] 新增 pending write queue 和 EventBase retry flush。
- [x] 新增 ACK 批量发布和 ACK 消费更新本地回收 cursor。
- [x] 新增 `PollerGroup` 和 Queue Ownership Handoff 状态机。
- [ ] 新增 poller idle profile：busy/adaptive/powersave。
- [x] 新增单元测试：HWQueue stub 容量、DoorbellQueue 拼接、ACK 批量、pending write、ownership handoff。

### fbthrift

- [x] 在 `thrift/perf/cpp2` client 增加 `cxl_mem` transport 选择。
- [ ] 增加 CXL mem benchmark flags：region path、region size、IO slice size、poller threads、hwqueues per doorbell、stub/hardware backend。
- [x] 增加 socket 控制面握手协议。
- [x] 成功握手后创建 `CxlMemAsyncTransport` 并传给 `RocketClientChannel::newChannel()`。
- [x] server 侧增加 benchmark 专用 CXL mem 接入入口。
- [x] 初始化失败时 fallback 到原 Rocket socket。
- [x] 文档化运行命令和降级语义。

## 遗留控制表

| 编号 | 问题 | 当前倾向 | 影响 |
|------|------|----------|------|
| 1 | server 端 CXL 握手入口放在哪里 | benchmark 专用入口，避免破坏 Rocket peeking | fbthrift 改动范围 |
| 2 | 真实 HWQueue sender-side ordering API | backend 封装，transport 不直接碰硬件细节 | 正确性 |
| 3 | 每个 DoorbellQueue 拼几个 HWQueue | 默认 2，可配置 | 控制面容量 |
| 4 | ACK item 具体 bit 编码 | separate ACK queue，48 bit cursor + shard/保留字段 | 兼容大 ring |
| 5 | ACK 批量阈值 | bytes/items/time/backpressure 组合 | 吞吐和回收延迟 |
| 6 | EventBase drain budget | items + time + consecutive drains | 延迟和 CPU 公平性 |
| 7 | poller idle profile 默认参数 | adaptive | 空闲 CPU 和尾延迟 |
| 8 | payload chunk 超过 64KB | 切分多个 DATA item | 大包支持 |
| 9 | FD passing | 第一阶段不支持，遇到 FD write error | benchmark 范围 |
| 10 | shared cursor 是否保留 | 仅 debug/metrics，不参与正确性 | 观测性 |
| 11 | poller-side payload copy | 第一阶段不做 | 后续性能优化 |
| 12 | 多 poller 是否抢同一 queue | 第一阶段固定 queue 归属 poller | 并发复杂度 |
| 13 | stub backend 是否提供无锁 SPSC 模式 | 默认加锁 MPMC，后续可加 SPSC fast stub | 测试真实性与性能 |

## 初始验证策略

- 先用 shm + `StubHWQueueBackend` 跑通单进程/双进程 transport 单测。
- 再跑 fbthrift perf/cpp2 `--transport=cxl_mem --backend=stub`。
- 最后切到真实 CXL device path 和 `RealHWQueueBackend`。
- 每阶段都保留 `--transport=rocket` 原 socket baseline 对照。

## 设计原则

- 热路径不依赖普通共享 cursor 的跨端可见性。
- 跨端控制状态统一通过 HWQueue/DoorbellQueue 传递。
- EventBase 是 transport 状态 owner。
- central poller 是 queue ownership scheduler，不处理 Rocket 复杂状态。
- 没有硬件事件源时，所有唤醒都应被视为轮询策略的一部分，而不是正确性前提。
