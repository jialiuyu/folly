# SHM 回包队头阻塞缓解方案

## 背景

当前共享内存传输路径复用了 fbthrift/Rocket 标准的 `AsyncTransport`
响应模型。这保证了 socket transport 和 SHM transport 的行为兼容，但
也意味着 ThreadManager 派发的 RPC 回包必须先回到 IO EventBase，之后
才能真正写入 SHM。

对于 `download()` 这类非常小的 benchmark RPC，这会制造可避免的队头
阻塞：

```text
SHM poller
  -> IO EventBase 投递请求并做 Rocket dispatch
  -> ThreadManager worker 执行 handler
  -> HandlerCallback 完成响应
  -> ReplyQueue
  -> IO EventBase drain ReplyQueue
  -> Rocket send path
  -> BusyPollSharedMemoryTransport::writeChain()
  -> ShmPollerService::writeData()
```

SHM 数据面本身不需要 fd readiness，也不需要 epoll 风格的写事件所有权。
当前瓶颈来自 Rocket connection 和 response-channel 控制面仍然绑定在
IO EventBase 上。

## 当前证据

当前性能 benchmark service 有两种不同的执行模型：

- `sum`、`noop`、`onewayNoop` 带有 `@cpp.ProcessInEbThreadUnsafe`。
- `download`、`upload`、`streamDownload` 没有该注解，因此走
  ThreadManager dispatch。

生成代码也体现了这个区别：

- EventBase 方法通过 `async_eb_*` 在线执行。
- ThreadManager 方法通过 `processInThread()` 和 `async_tm_*` 执行。
- 如果 worker 线程完成响应时不在 IO EventBase 线程上，响应会通过
  `HandlerCallbackBase::putMessageInReplyQueue()` 入队。

在 shared SHM 模式下，接收数据由 `ShmPollerService` 通过
`EventBase::runInEventBaseThread()` 投递，响应写入最终进入
`BusyPollSharedMemoryTransport::writeInternal()`，再委托给
`ShmPollerService::writeData()`。

因此，一个 ThreadManager 派发的 SHM RPC 仍然依赖 IO EventBase 完成
最终回包写入。

## 问题定义

对于小 RPC，ThreadManager 调度和 ReplyQueue 回跳的成本会盖过真正的
handler 工作。在 `--io_threads=1` 和 `--shm_lanes=1` 时，同一个 IO
EventBase 同时负责：

- inbound SHM delivery callback，
- Rocket frame 解析和 dispatch，
- ReplyQueue drain，
- Rocket response framing 和 write batching，
- outbound SHM 写入。

一旦 IO EventBase 忙碌，worker 已完成的响应会堆在 ReplyQueue drain
点后面。这解释了 `download()` 相对 `sum` 这类 EventBase-only RPC 的
低稳态 QPS 和超长尾延迟。

## 目标

1. 恢复 benchmark `download()` 在简单内存响应场景下的性能。
2. 保持 socket transport 行为不变。
3. 除非显式开启，保持 legacy SHM 行为不变。
4. 为通用 worker-thread SHM 回包优化提供演进路径，同时不破坏 Rocket
   ordering、lifetime 和 backpressure 语义。

## 非目标

- 快速修复阶段不修改 folly 公开 API。
- 不让所有 RPC handler 都跑在 IO EventBase 上。
- 不绕过 Rocket framing 或 response metadata。
- 不支持 socket worker-direct write；该优化只针对 SHM。

## 方案 A：让轻量 SHM Benchmark RPC 在 IO EventBase 上执行

### 摘要

给 `download()` 添加 `@cpp.ProcessInEbThreadUnsafe`，并实现对应的
`async_eb_download()` handler。这样会把这个极轻量响应 handler 移到
IO EventBase 上执行，与当前 `sum()` 的执行模型一致。

### 预期路径

```text
SHM poller
  -> IO EventBase 投递请求并做 Rocket dispatch
  -> async_eb_download()
  -> HandlerCallback::result()
  -> Rocket send path
  -> BusyPollSharedMemoryTransport::writeChain()
  -> ShmPollerService::writeData()
```

该路径移除了 worker hop 和 ReplyQueue 回跳。

### 实现形态

在 `fbthrift/thrift/perf/cpp2/if/StreamApi.thrift` 中：

```thrift
@cpp.ProcessInEbThreadUnsafe
ApiBase.Chunk2 download();
```

在 benchmark handler 中增加 EventBase handler 实现：

```cpp
void async_eb_download(
    apache::thrift::HandlerCallbackPtr<std::unique_ptr<Chunk2>> callback)
    override {
  stats_->add(kUpload_);
  callback->result(std::make_unique<Chunk2>(chunk_));
}
```

具体返回对象构造方式应以重新生成后的签名和本地代码风格为准。

### 收益

- 改动最小。
- 可以直接验证根因假设。
- 为 `download()` 移除 ThreadManager 调度和 ReplyQueue 回跳。
- 不影响 socket，也不影响一般 worker-dispatched RPC。

### 风险

- `ProcessInEbThreadUnsafe` 会禁用 queue timeout 和部分 overload
  protection。
- handler 必须非常快，不能阻塞，不能持有可能阻塞的锁。
- 这不是面向真实 worker-thread RPC 的通用解法。

### 验收标准

- 生成代码将 `download()` 路由到 `async_eb_download()`。
- 生成代码中 `download()` 不再调用 `processInThread()`。
- `download()` QPS 接近 `sum()` 的同一数量级。
- 在相同 benchmark 配置下，P99/P99.9 不再出现秒级长尾。

## 方案 B：增加 SHM Worker-Direct Reply Fast Path

### 摘要

增加一个 SHM 专用回包路径，使 worker 线程完成响应后，可以把已经序列化
好的 Rocket response frame 提交到 SHM，而不是先等待 IO EventBase drain
ReplyQueue。

这是通用修复方向，但它触及的语义约束比方案 A 更严格。

### 关键不变量

任何 worker-direct reply 路径都必须保持：

- 单 connection 内的字节流顺序，
- Rocket stream response ordering，
- write callback 和错误语义，
- connection close / unregister 的生命周期安全，
- SHM ring 满时的 backpressure 行为，
- 与非 SHM transport 的兼容性。

### 建议架构

为 shared-mode Rocket connection 引入一个只用于 SHM 的 reply writer：

```text
worker thread
  -> serialize response payload
  -> Rocket frame bytes
  -> ShmDirectReplyWriter
  -> ShmPollerService::writeData()
```

这个 writer 只应在底层 transport 是 shared-mode
`BusyPollSharedMemoryTransport` 时暴露。

相比在通用 transport 中硬编码特殊逻辑，更推荐增加 transport capability
检查：

```cpp
bool supportsThreadSafeShmDirectReply() const;
```

默认实现返回 false。shared-mode SHM 在满足 ordering 和 lifetime 规则后
可以 opt in。

### 阶段 B1：实验性 Direct Writer

使用 per-connection mutex 保护完整 response-frame 写入：

```text
worker
  -> build full Rocket frame IOBuf chain
  -> lock connection SHM write mutex
  -> write all chunks through ShmPollerService::writeData()
  -> unlock
```

性质：

- 容易推理。
- 防止多个 worker 的 frame 片段互相穿插。
- direct-write 范围较窄。

限制：

- SHM flow control 时 worker 可能阻塞或 spin。
- 多个响应同时写同一 connection 时，mutex 竞争会降低收益。
- 错误处理和 close race 必须严格防护。

### 阶段 B2：Per-Connection SHM Reply Queue

将 worker 侧直接 spin 改成 per-connection MPSC queue，并由非 IO writer /
drainer 负责写 SHM：

```text
worker
  -> build full Rocket frame IOBuf chain
  -> enqueue into connection SHM reply queue

SHM reply drainer
  -> dequeue in order
  -> ShmPollerService::writeData()
```

性质：

- 在不让 worker 卡在 SHM flow control 的前提下保持响应顺序。
- backpressure 更显式。
- 回包 drain 不依赖 IO EventBase。

限制：

- 基础设施更多。
- 需要与 connection close 做关闭协调。
- 需要 metrics 和有界队列策略。

### Fallback 规则

以下情况应回退到现有 ReplyQueue 路径：

- transport 不是 shared-mode SHM，
- 响应携带 FD 或依赖 socket-only feature，
- connection 正在关闭，
- direct writer backpressure 超过有界阈值，
- ordering 状态不确定，
- feature flag 禁用该优化。

### 必要指标

增加以下 counters：

- direct reply attempts，
- direct reply successes，
- direct reply fallbacks，
- direct reply write failures，
- SHM write flow-control yields，
- direct reply queue depth，
- ReplyQueue enqueue / drain counts，
- IO EventBase notification queue size，如果可获取。

## 推荐推进顺序

### 第 1 步：先实现方案 A

只对 `download()` 使用 EventBase-handler 路径。这是验证诊断并恢复轻量
payload response benchmark 性能的最快方式。

### 第 2 步：补充诊断指标

实现方案 B 前，先增加足够可观测性来证明耗时位置：

- 统计进入 ReplyQueue 的 worker replies，
- 测量 IO EventBase drain 前的 ReplyQueue 等待时间，
- 测量 SHM write latency 和 flow-control yields，
- 测量 poller 到 EventBase 的 dispatch latency。

### 第 3 步：在 Feature Flag 后面原型化方案 B

只为简单 request-response payload 实现 worker-direct SHM reply。socket、
legacy SHM、streaming、sink、bidi、携带 FD 的响应和复杂错误路径继续走
现有 response path。

### 第 4 步：不变量测试通过后再扩展

只有通过 ordering、close-race、backpressure、multi-client 和 mixed-RPC
测试后，才能扩大 direct path 覆盖范围。

## 测试计划

### 静态验证

- 确认生成的 `download()` 代码使用 `async_eb_download()`。
- 确认生成的 `download()` setup 不再调用 `processInThread()`。
- 确认 `sum()` 和 `download()` 具有一致的 executor metadata。

### Benchmark 验证

在方案 A 前后运行同一组 benchmark 配置：

```text
--transport=shm
--download_weight=1
--chunk_size=1024
--max_outstanding_ops=100
--io_threads=1
--shm_lanes=1
```

预期：

- 相比当前 worker-dispatched `download()`，QPS 显著提升，
- server 侧不再出现明显的 0/100 QPS 交替模式，
- P99 和 P99.9 不再是秒级延迟。

### 回归验证

- socket transport benchmark 仍然正常。
- shared SHM benchmark 中 `sum`、`noop`、`upload`、`streamDownload` 仍然正常。
- legacy SHM fallback path 不变。
- multi-client 测试保持响应正确性。

### 方案 B 专项测试

- 同一 connection 上多个 worker 同时回包时 frame 顺序保持正确。
- pending direct replies 期间 connection close 不访问已释放状态。
- SHM ring 满时不会让 CPU worker 无限 spin。
- direct path 在禁用或不支持时正确 fallback。

## 开放问题

1. Direct SHM replies 应该做在 Rocket 下面作为 `AsyncTransport` capability，
   还是做在 Rocket connection 层？
2. 阶段 B1 是否允许 worker 线程阻塞在 SHM flow control 上，还是应该立即
   enqueue 到 drainer？
3. 回退到现有 IO EventBase ReplyQueue 路径前，direct writer 的可接受
   backpressure 阈值是多少？
4. 第一个 direct-path 原型应支持哪些 Rocket response 变体：只支持普通
   request-response，还是也支持 exception？

## 建议

先为 benchmark `download()` RPC 实现方案 A。这是最小改动，并且直接解决
当前性能断崖。

将方案 B 作为独立的 transport/Rocket 设计工作推进。SHM 可以支持
worker-direct 数据路径，但实现必须显式保持 Rocket 的 connection ordering、
lifetime 和 backpressure 语义。
