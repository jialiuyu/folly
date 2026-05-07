# BusyPoll 共享内存传输轮询优化技术报告

报告日期：2026-05-08

## 摘要

本文围绕当前 `BusyPollSharedMemoryTransport` / `ShmPollerService` 的轮询式共享内存传输路径，梳理传统高性能网络系统中的轮询组织方式，并分析其中哪些经验可以迁移到 ub.mem / CXL-like 共享内存传输场景。

本项目的关键约束是：共享内存数据路径没有专用智能网卡，也通常没有可依赖的中断语义。因此问题不是“是否轮询”，而是“如何轮询”：既要保持微秒级低延迟，又要控制 CPU 消耗、尾延迟、连接公平性和多核扩展性。

当前 shared mode 已经接近一个 kernel-bypass data plane：

- 每个方向、每个 lane 有一个共享 GQM 通知队列和一个数据 ring。
- 每个 lane 有一个专用 read poller 线程。
- 写端通过原子 cursor 预留 ring 空间，将 payload 拷贝到 ring，然后 push 一个 64-bit notification。
- 读端 poller pop notification，将 payload 拷贝到 `IOBuf`，推进 `readCursor`，再投递到目标 `EventBase`。
- 现有诊断指标已经覆盖 dispatch latency、pop success/empty count、write time、flow-control yield、shared-lock count 和 IOBuf allocation count。

综合 NAPI、DPDK、AF_XDP、netmap、VPP、IX、ZygOS、Shenango、Caladan、Snap、Metronome 等系统的经验，最值得优先吸收的方向如下：

| 优先级 | 优化方向 | 主要收益 | 粗略收益预估 |
|---:|---|---|---|
| P0 | 有预算的 batch polling 和 grouped dispatch | 降低高请求率下每条消息的 poll、lock、lambda 固定成本 | 小 RPC 吞吐 +15% 到 +40%；dispatch 开销下降 10% 到 30% |
| P0 | NAPI-style 自适应 idle policy | 避免固定 100 us sleep 带来的尾延迟尖刺，同时降低空闲 CPU | 稀疏流量 P99/P99.9 改善；空闲 CPU 下降 30% 到 90%，取决于策略 |
| P1 | lane affinity 和拓扑感知放置 | 降低跨核/cache 干扰和 hot-lane 不均衡 | 饱和多 client 场景吞吐 +10% 到 +30% |
| P1 | 连接 dispatch 快路径 | 降低每个 received chunk 上的 shared mutex 成本 | 高 message-rate 场景 +5% 到 +20% |
| P1 | 批量 IOBuf pool 与 prefetch | 降低 hot path allocator/cache miss 成本 | 64 B 到 4 KB payload 场景 +5% 到 +15% |
| P2 | per-connection 公平性与背压遥测 | 避免热连接占满共享队列影响冷连接 | 主要改善尾延迟稳定性，不一定提升峰值吞吐 |

这些收益数字不是实验结论，而是基于现有代码路径和相似网络轮询系统的工程预估，应通过后续 benchmark 验证。

## 范围与非目标

本文关注：

- 轮询架构综述：NAPI、DPDK、AF_XDP、netmap、VPP、IX、ZygOS、Shenango、Caladan、Snap、Metronome。
- 这些架构对 ub.mem / CXL-like 共享内存传输的可迁移经验。
- 可量化指标与基于 `fbthrift/thrift/perf/cpp2` 的 benchmark 方案。

本文不包含：

- 软件详细设计。
- API 签名、类图、具体 patch 或实现步骤。
- 对当前 memory ordering、cursor ownership、handshake 协议的重新设计。

说明：用户提到的 `dsyOS` 未检索到明确对应的网络轮询系统。本文按最接近、且与微秒级 kernel-bypass RPC 调度高度相关的 `ZygOS` 处理。

## 当前传输路径基线

当前共享内存路径的数据平面比较清晰：

| 组件 | 当前行为 | 性能含义 |
|---|---|---|
| GQM notification | 64-bit descriptor，包含 `connId`、ring `offset`、`length` | 每个 chunk 只需读取一次元数据；payload 区没有额外 header |
| Data ring | 每个方向独立，payload-only | 空间布局简单；cursor 负责正确性 |
| Write cursor | 多 writer 通过 `fetch_add` 预留空间 | 受原子写竞争和 GQM push 串行化影响 |
| Read cursor | 单 poller 单调推进 | 回收逻辑简单，不需要处理乱序 completion |
| Poller | 每轮尝试一次 `pop()`；空轮询超过阈值后固定 sleep | 忙时低开销，空闲后新请求可能遇到 sleep 尾延迟 |
| Dispatch | 查询 `connTable_`，再 `runInEventBaseThread()` | 生命周期安全，但每条消息有 lock 和 lambda 成本 |
| Buffering | 每 lane 一个 IOBuf pool，必要时 fallback allocation | 避免大部分 malloc/free，但每个 chunk 仍对应一个 IOBuf |
| Benchmark 支持 | `cpp2` 已输出 QPS、latency 和 SHM diagnostics | 基础可用，但需要更多 transport 内部指标 |

当前最明显的性能缺口有两个：

1. Poller 一次只处理一个 notification，未吸收现代 packet I/O 系统最核心的 bounded batching 思路。
2. 空轮询后的固定 `sleep_for(100us)` 会降低空闲 CPU，但对低频请求或 burst 后第一个请求可能形成 100 us 级尾延迟。

## 轮询策略综述

### Linux NAPI

NAPI 是 Linux 网络栈中经典的 interrupt/polling 混合模型。设备中断负责触发处理，随后内核以 bounded budget 的形式轮询设备队列。高负载时它通过轮询避免中断风暴，低负载时又可以回到中断或延迟中断机制。

Linux 还提供 busy polling 相关能力，例如 `SO_BUSY_POLL`、`net.core.busy_poll`、`net.core.busy_read`，以及 epoll-based busy polling。较新的 epoll busy polling 还提供 `busy_poll_usecs`、`busy_poll_budget`、`prefer_busy_poll` 等参数。

可借鉴经验：

- 每个 poll cycle 要有 budget。
- 区分低延迟 busy polling 和 IRQ mitigation。
- 根据负载在 active polling、deferred polling、interrupt-like mode 之间切换。
- 暴露 poll 时间、poll budget、busy-poll 偏好等调优参数。
- 任何 coalescing 或 sleep 窗口都是吞吐、CPU 与尾延迟之间的折中。

对共享内存传输的适配：

共享内存路径没有硬件中断兜底，但 NAPI 的状态机思想仍然成立。Poller 可以根据近期命中、empty poll 次数、队列积压等信号在 hot、warm、cold 状态间切换。 bounded budget 可以避免单个活跃 lane 长时间占用 CPU。

### DPDK Poll Mode Driver

DPDK PMD 是更激进的用户态轮询模型。应用绕过内核网络栈，在用户态直接轮询 NIC Rx/Tx descriptor。典型执行模型有两类：

- run-to-completion：一个 core 从 Rx ring 取包，处理，并放入 Tx ring。
- pipeline：一个或多个 core 负责收包，再通过 ring 交给其他 core 处理。

DPDK 的性能关键点是 burst API、prefetch、NUMA-local mempool、per-queue/per-core ownership，以及尽量避免多个 core 共享同一个队列。

可借鉴经验：

- 一次处理 burst，而不是一个包一个包处理。
- 队列最好由单一 core 拥有。
- buffer pool 尽量 per-core、NUMA-local。
- 指针数组和批处理元数据要考虑 cacheline。
- 批量 allocate/free buffer。
- 底层 queue primitive 不应隐藏策略，batch 策略应由上层 loop 控制。

对共享内存传输的适配：

当前 GQM `pop()` API 是标量接口。即使不改变接口，也可以在 poller 内部反复 pop，构造本地 batch。payload copy 前可以预取即将访问的 offset，也可以批量从 pool 取 buffer。lane 分配应尽量保持 queue/poller affinity。

### AF_XDP 与 netmap

AF_XDP 和 netmap 都通过 shared ring / shared buffer 降低 syscall 和 copy 成本。AF_XDP 中的 `need_wakeup` 思想用于避免用户态在不必要时持续 busy spin；netmap 则强调简单共享 buffer 模型和批量摊销成本。

可借鉴经验：

- shared ring 很适合高频数据交换。
- sleep/wakeup hint 可以减少无意义 spin。
- 快速 I/O 只是把瓶颈上移，应用层 poll loop 仍然需要精心设计。

对共享内存传输的适配：

这里没有 kernel producer 需要 `need_wakeup`，但可以引入等价的本地状态：如果 poller 观察到长期无 GQM 活动、无 cursor 推进，就进入低 duty mode；一旦看到 cursor 活动或 burst，就回到 hot mode。

### VPP Vector Packet Processing

VPP 将 Rx ring 中的 packet 组织成 vector，再让整个 vector 依次通过 packet processing graph 中的节点。它不是逐包完整跑完整 pipeline，而是一个节点处理一批 packet 后再进入下一个节点。

可借鉴经验：

- 处理 vector，而不是独立 message。
- batch size 必须有上限，避免破坏尾延迟和 cache locality。
- 按 next stage 聚合工作。

对共享内存传输的适配：

共享内存 poller 可以把 pop 到的 notification 按目标 `EventBase` 分组，然后每组提交一次 dispatch。这里不需要引入 VPP 的 graph 架构，只借鉴“vectorized input”和“grouped next-stage dispatch”。

### IX

IX 是面向低延迟和高吞吐的 protected dataplane OS。它使用专用 hardware thread 和网络队列，处理 bounded batch，并尽量避免 coherence traffic 和跨核同步。

可借鉴经验：

- 对微秒级 RPC，专用 poller core 是合理的性能换资源选择。
- bounded batching 可以在不无限拉高尾延迟的情况下提升吞吐。
- hot path 要减少跨核同步。
- 控制面策略和数据面处理应明确分离。

对共享内存传输的适配：

当前 transport 已经有明显的 control/data plane 分离。下一步应将 poller loop 的策略显式化：budget、idle policy、lane placement、公平性都应成为可观测、可调优的参数。

### ZygOS

ZygOS 针对微秒级 networked tasks，基于 IX 类 dataplane 思路，引入 work-conserving scheduler，缓解纯静态分区在负载不均和服务时间分布复杂时的尾延迟问题。

可借鉴经验：

- 静态 partitioning 很快，但在 skew 下可能不公平。
- work-conserving scheduling 可以减少“有 core 空闲，但另一个 core 排队”的问题。
- 尾延迟评估不能只看均匀 client 分布，必须包含高 fan-in 和 skew workload。

对共享内存传输的适配：

当前 lane 模型本质上是 partitioned。fast path 上应保持 partitioning，但 benchmark 必须覆盖 skew。如果 hot lane 饱和，再考虑更好的连接 hash、load-aware lane selection 或有限的跨 lane stealing。

### Shenango 与 Caladan

Shenango 和 Caladan 关注微秒级服务下的 CPU 效率和资源干扰控制。Shenango 使用 IOKernel 做 packet steering 和快速 core reallocation；Caladan 通过微秒级监控和 placement 策略缓解资源干扰。

可借鉴经验：

- 固定 spinning core 很快，但在低平均负载下 CPU 成本高。
- 负载和资源干扰的变化可能快于传统毫秒级 OS 调度。
- 轮询系统不能只测延迟，还必须测 CPU efficiency。
- Hyperthread、memory bandwidth、LLC 干扰必须纳入评估。

对共享内存传输的适配：

第一步不是引入新 scheduler，而是先测清楚 poller CPU、idle spin ratio、burst 下尾延迟。如果固定 poller core 成本过高，自适应 duty cycling 和可选 poller placement policy 是风险更低的路径。

### Snap

Snap 是 Google 的用户态 host networking 系统。它强调模块化、最小共享状态、动态 CPU resource scaling，并在生产环境中支持多种 networking service。

可借鉴经验：

- 共享状态应尽量少且边界清晰。
- 高性能用户态网络服务需要明确的 CPU 资源策略。
- 生产系统更重视可观测性和动态调度能力。

对共享内存传输的适配：

当前 `connTable_` shared mutex 是每条消息都会触达的少数共享状态之一。减少这条路径上的同步，符合 Snap 的 minimal sharing 思路。

### Metronome

Metronome 研究的是 DPDK 场景下的间歇式自适应轮询。它用 sleep/wake 模式替代持续轮询，目标是在给定平均延迟约束下，让 CPU 使用率随负载变化。

可借鉴经验：

- 持续 busy polling 并不是唯一选择。
- sleep/wake 参数应随流量负载自适应。
- CPU 使用率和 latency target 可以一起建模。

对共享内存传输的适配：

这是替代当前固定 `sleep_for(100us)` 的最直接参考。共享内存 poller 可以以目标延迟为约束，根据到达率、empty-poll streak、recent burst size 调整 sleep 时间。

## 可吸收优化映射

| 外部模式 | 来源系统 | 当前缺口 | BusyPoll 可吸收方式 | 预期收益 | 风险 |
|---|---|---|---|---|---|
| Poll budget | NAPI、DPDK | 标量 pop loop | 增加每轮 `maxPollBudget` 和 `maxPollTimeNs` | 限制 CPU 独占；支持 batching | budget 太大可能抬高尾延迟 |
| Burst receive | DPDK、VPP | 每次只处理一个 GQM entry | 一轮最多 pop `N` 个 notification 到本地数组 | 降低每消息固定开销 | batch 占用更多临时内存；可能影响公平性 |
| Grouped next-stage dispatch | VPP | 每个 chunk 一个 EventBase lambda | 按 EventBase 分组后一次提交多个 chunk | 降低 lock 和 lambda 压力 | 生命周期处理更复杂 |
| Adaptive idle policy | NAPI、Metronome | empty spin 后固定 100 us sleep | hot/warm/cold 状态机，配置 target latency | 稀疏流量 P99 更好，idle CPU 更低 | 参数需要调优 |
| Queue/core affinity | DPDK、IX | lane round-robin | 按 EventBase、core、hash 映射 conn 到 lane，避免 HT sibling 干扰 | 扩展性和 locality 更好 | hash 或 placement 不佳仍会 skew |
| Minimal shared state | IX、Snap | 每个 chunk 两次 `connTable_` shared-lock lookup | per-lane dense table 或 ConnEntry snapshot | 降低 lock contention | 必须保持生命周期安全 |
| Per-core pools | DPDK | pool per-lane 但有 mutex | bulk pool 操作，或可选 lock-free freelist | 降低 allocator 开销 | 每 lane 保留更多内存 |
| Work-conserving fallback | ZygOS | 静态 lane partition | 先 benchmark skew，再评估 load-aware lane selection 或有限 stealing | 改善 skew 下 tail | 跨 lane stealing 会增加同步 |
| CPU efficiency target | Shenango、Caladan、Metronome | 当前指标偏 transport latency | benchmark 增加 poller CPU、cycles、power | 量化 latency/CPU 取舍 | 需要稳定测试环境 |

## 推荐优化方向

### P0：有预算的 batch polling

将“一轮只处理一个 notification”的 poller 形态改为 bounded batch 策略。本文不做软件详细设计，只描述策略要求：

- 每次 poll iteration 最多 pop `maxBatch` 个 notification。
- 如果 GQM 提前变空，立即停止本轮 batch。
- 如果超过 `maxPollTimeNs`，也停止本轮 batch。
- 保留 `maxBatch = 1` 作为完全兼容的 baseline mode。
- benchmark 中评估 `maxBatch = 4, 8, 16, 32, 64`。

预期结果：

- `noop` 和小 `sum` workload 在高并发下收益最大。
- dispatch 平均延迟应下降，因为固定成本被摊销。
- 如果 batch 过大，P99 可能变差，因此需要同时限制 item 数和时间。

### P0：自适应 idle policy

将固定 empty-spin 后 `sleep_for(100us)` 改为显式状态模型：

| 状态 | 进入条件 | 轮询行为 | 退出条件 |
|---|---|---|---|
| Hot | 最近持续 pop 成功或队列有积压 | spin + pause，启用 batch | 多轮 empty |
| Warm | 间歇性到达 | 短 spin window，短 sleep 或 yield | 命中回 Hot；持续 empty 进入 Cold |
| Cold | 长时间 idle | 低 duty sleep，但 sleep 上限受 target latency 约束 | 任意成功 pop 或观察到 cursor 活动 |

关键是 cold sleep 不能是固定 100 us，而应从目标延迟预算推导。例如 transport 目标 P99 贡献是 20 us，则 cold sleep 不应是 100 us。

预期结果：

- 稀疏流量的 P99/P99.9 应改善。
- 相比 pure busy spin，idle CPU 应明显下降。
- burst recovery time 应变成可观测和可调优的指标。

### P1：Grouped EventBase dispatch

当前 poller 每个 chunk 提交一个 `runInEventBaseThread()` lambda。按 EventBase 分组可降低 scheduling 和锁开销。

| 方案 | 概念 | 优点 | 缺点 |
|---|---|---|---|
| Per-message lambda | 当前行为 | 简单、安全 | lambda 和 lock 开销高 |
| Per-EventBase vector | 一个 lambda 向同一 EventBase 投递多个 chunk | 摊销效果好 | 需要处理 batch lifetime |
| Per-connection chain | 同一 connId 的相邻 chunk 合并 | 降低 callback 次数 | 必须确保 frame parser 语义不变 |

保守的第一步是 per-EventBase grouping，不改变 payload 格式和连接语义。

### P1：Lane affinity 与拓扑感知

当前 multi-lane 方向是对的，但连接放置还需要测量和优化。

建议 benchmark 维度：

- `shm_lanes`: 1、2、4、8。
- `num_clients`: 1、2、4、8、16、32。
- `io_threads` 和 `cpu_threads`: 足以暴露跨核竞争。
- 记录 poller pinned CPU，以及它是否与 EventBase 或 CPU worker 共享物理 core / HT sibling。

候选策略：

- Round-robin baseline。
- 按 connection ID hash。
- EventBase-local lane selection。
- handshake 时基于 load 的 lane selection。

### P1：连接 dispatch 快路径

当前 dispatch 路径在调度前拿一次 shared lock，在 EventBase lambda 内再拿一次 shared lock。这保证了生命周期安全，但在高 message rate 下成本明显。

后续可评估：

- per-lane dense `connId -> ConnEntry` array。
- 带 EventBase-thread unregister 约束的 atomic pointer snapshot。
- generation counter 防止 stale entry。
- batch 内同 connId/group 只查一次。

这不应是第一项代码改动，除非 profiling 证明 lock contention 是主瓶颈。生命周期安全比过早去锁更重要。

## 收益预估

| 优化项 | 吞吐影响 | 延迟影响 | CPU 影响 | 置信度 | 理由 |
|---|---:|---:|---:|---|---|
| 仅 batch pop | +10% 到 +25% | 平均 dispatch 下降；P99 在有界 batch 下应基本中性 | cycles/msg 略降 | 中 | 减少重复 loop、pop、branch、timestamp、stats 开销 |
| Batch pop + grouped dispatch | +15% 到 +40% | dispatch avg 下降 10% 到 30%；P99 取决于 budget | EventBase scheduling 开销下降 | 中 | 摊销 shared lock 和 lambda |
| Adaptive idle | 饱和时基本中性 | 稀疏 P99/P99.9 可能显著改善 | idle CPU 下降 30% 到 90% | 中 | 当前固定 100 us sleep 很可能进入尾延迟 |
| Lane affinity | 并发场景 +10% 到 +30% | skew/topology 压力下 tail 降低 | cache/HT 干扰下降 | 中低 | 强依赖机器拓扑 |
| Dispatch table fast path | 高 pps 场景 +5% 到 +20% | lock-induced tail 降低 | lock/cacheline traffic 下降 | 中低 | 需要 profiling 证明 lock 占比 |
| Bulk IOBuf/prefetch | 小中 payload +5% 到 +15% | 主要改善平均延迟 | allocator/cache miss 成本下降 | 低 | 取决于 copy size 和 pool hit rate |
| Fairness/backpressure | 通常不提升峰值 | hot/cold 混合下 P99 更稳 | 可能略增 | 中 | 这是控制面收益，不是原始速度收益 |

## 量化指标

Benchmark 应区分应用层结果和 transport 内部机制。

| 类别 | 指标 | 定义 | 价值 |
|---|---|---|---|
| RPC 吞吐 | QPS | 每秒完成 RPC 数 | 核心容量指标 |
| RPC 延迟 | avg、P50、P90、P99、P99.9 | client 观测的 request latency | 反映用户可见影响 |
| Payload 吞吐 | bytes/s | 每秒传输 app bytes | upload/download 重要 |
| Poll 效率 | pop empty ratio | empty pops / total pop attempts | 衡量无效轮询 |
| Poll 生产率 | avg batch size | 每个非空 poll iteration 成功 pop 数 | 验证 batching |
| Poll delay | dispatch avg/P99/P99.9 | GQM pop 到 EventBase 执行 | 隔离 poller-to-IO-thread handoff |
| Write 开销 | write avg/P99 | `writeData()` wall time | 捕获 flow control 和 memcpy 成本 |
| 背压 | flow-control yields/timeouts | writer 等待 ring space 的次数 | 识别 ring pressure 和 peer lag |
| 队列压力 | GQM/ring occupancy | `writeCursor - readCursor`，若可用再加 GQM depth | 识别饱和点 |
| 同步成本 | locks per message | shared locks / successful pop | 跟踪 dispatch 开销 |
| 分配成本 | IOBuf allocs per message | pool alloc count / successful pop | 识别 pool miss 或 chunk 过碎 |
| CPU 成本 | cycles/msg、CPU%、context switches | `perf stat` 或 OS counter | 量化轮询成本 |
| 公平性 | per-client QPS 和 P99 | client 间分布 | 识别 hot connection 对 cold connection 的影响 |
| 稳定性 | interval variance | 各统计窗口 median 与波动 | 避免只优化峰值 QPS |

建议新增诊断 counter：

| Counter | 用途 |
|---|---|
| `pollLoopCount` | poll loop iteration 数 |
| `batchCount` | 非空 batch 数 |
| `batchItemsSum` | 每个 batch pop 到的 notification 总和 |
| `batchItemsMax` | 最大 batch size |
| `dispatchLambdaCount` | EventBase dispatch 调用次数 |
| `dispatchItemsSum` | dispatch 携带的 chunk 总数 |
| `readCursorLagBytes` | 周期采样 `writeCursor - readCursor` |
| `idleStateTransitions` | Hot/warm/cold 状态切换次数 |
| `idleSleepNsSum` | 总 sleep 时间 |
| `pollerCpuId` | poller 实际运行 CPU，启动时采样 |

## Benchmark 方案

建议继续使用现有 `fbthrift/thrift/perf/cpp2` harness，不另起一套大框架。该 harness 已支持 `noop`、`sum`、`download`、`upload`、client count、pipeline depth、warmup、CSV output 和 SHM diagnostics。

### 实验矩阵

| 实验 | Workload | 变量 | 主要指标 | 目的 |
|---|---|---|---|---|
| E1：稀疏延迟 | `noop` | QPS cap 或 burst interval；batch size；idle policy | P99/P99.9、poller CPU、empty ratio | 验证 adaptive idle |
| E2：饱和小 RPC | `noop` | clients、pipeline、batch budget | QPS、dispatch avg/P99、locks/msg | 验证 batch polling 和 grouped dispatch |
| E3：轻 CPU RPC | `sum` | clients、pipeline、lanes | QPS、RPC P99、poller CPU | 测更真实的小请求/响应 |
| E4：Payload scan | `upload`、`download` | chunk size: 64 B 到 64 KB | bytes/s、write avg、flow-control yields | 测 memcpy 和 buffer-pool 行为 |
| E5：Lane scaling | `noop`、`sum` | lanes: 1、2、4、8；client count | QPS scaling、per-lane batch、per-lane empty ratio | 找 lane 饱和点和拓扑限制 |
| E6：Fan-in 公平性 | hot/cold clients 混合 | 1 个 hot client，多 cold client | per-client P99/P99.9、公平性 ratio | 识别 hot connection starvation |
| E7：Burst recovery | `noop` | idle/hot 窗口交替 | idle 后首个响应延迟、ramp-up QPS | 验证 cold 状态唤醒恢复 |
| E8：Backpressure stress | `upload` | 小 ring 或高 payload 并发 | flow-control yields、timeouts、P99 | 验证 ring pressure 行为 |

### 建议参数网格

| 参数 | 取值 |
|---|---|
| `transport` | `shm`，以及 `rocket` baseline |
| `num_clients` | 1、2、4、8、16、32 |
| `max_outstanding_ops` | 1、10、100 |
| `shm_lanes` | 1、2、4、8 |
| `chunk_size` | 64、256、1024、4096、16384、65535 |
| `batch_budget` | 1、4、8、16、32、64 |
| `idle_target_us` | 5、10、20、50、100 |
| `warmup_sec` | 3 或 5 |
| `duration_sec` | 最少 30；尾延迟测试建议 60 |

### 结果记录

每次运行应保存：

- client 原始日志。
- server 原始日志。
- parse 后 summary CSV。
- 系统元信息：CPU 型号、kernel version、CPU governor、SMT on/off、进程 affinity、lane count、poller CPU ID。
- transport 配置：batch budget、idle policy、ring size、GQM depth。

Summary 表模板：

| Transport | Workload | Clients | Pipeline | Lanes | Batch | QPS Median | Avg us | P99 us | P99.9 us | Poller CPU% | Empty Ratio% | Avg Batch |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| shm | noop | 8 | 100 | 4 | 1 | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| shm | noop | 8 | 100 | 4 | 16 | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| rocket | noop | 8 | 100 | n/a | n/a | TBD | TBD | TBD | TBD | TBD | n/a | n/a |

Comparison 表模板：

| Scenario | Baseline | Candidate | QPS Delta | P99 Delta | CPU Delta | Verdict |
|---|---:|---:|---:|---:|---:|---|
| Saturated noop, 8c/100p | TBD | TBD | TBD | TBD | TBD | 若 QPS 提升且 P99 无明显回退，则接受 |
| Sparse noop | TBD | TBD | TBD | TBD | TBD | 若 P99/P99.9 和 CPU 同时改善，则接受 |
| Payload 4 KB upload | TBD | TBD | TBD | TBD | TBD | 若 bytes/s 提升且无 timeout，则接受 |

## 验收标准

优化是否接受，应看对应实验结果，而不是只看峰值吞吐。

| 优化项 | 验收条件 |
|---|---|
| Batch polling | 饱和小 RPC 至少 10% QPS 提升，且 P99 回退不超过 5% |
| Grouped dispatch | 每消息 dispatch lambda 数下降，RPC P99 无明显回退 |
| Adaptive idle | 稀疏负载 P99/P99.9 改善或持平，同时 idle CPU 明显下降 |
| Lane affinity | 多 lane QPS scaling 改善，per-lane imbalance 降低 |
| Dispatch fast path | 每消息 lock 数下降，生命周期安全测试保持干净 |
| Fairness | hot-client 压力下 cold-client P99 改善，且总 QPS 无大幅损失 |

## 风险与开放问题

| 风险 | 影响 | 缓解方式 |
|---|---|---|
| Batch 引入 head-of-line blocking | batch 太大会延迟后续连接 | 同时按 item count 和 time 限制 |
| Grouped dispatch 的生命周期安全 | 当前两次 lookup 模型避免 use-after-free | 保留 EventBase-thread unregister invariant |
| Lane skew | 静态 partition 可能让某个 poller 过载 | 先补 per-lane metrics，再改策略 |
| Idle policy 过拟合 | POSIX SHM 和真实 CXL device 行为可能不同 | POSIX 与 device-file 模式都跑 benchmark |
| 测量噪声 | 尾延迟受 CPU governor、SMT、日志、共租干扰影响 | pin 进程，降低日志，记录系统元信息 |
| Folly API 兼容性 | Folly 基础库不能破坏公开 API | 优化放在 config/default 后，保持默认兼容 |

## 推荐推进顺序

建议下一阶段按以下顺序推进：

1. 先用当前 scalar poller 建立干净 baseline。
2. 补充 benchmark 指标：batch size、dispatch call count、poller CPU、per-lane counters。
3. 单独评估 batch polling：`batch_budget = 1, 4, 8, 16, 32`。
4. 单独评估 adaptive idle policy。
5. 再考虑 grouped EventBase dispatch 和 connTable fast path。

这个顺序能降低正确性风险，也能避免过早优化错误组件。如果实验显示稀疏流量 P99 主要由固定 sleep 主导，则 adaptive idle 应优先；如果饱和 QPS 主要受 lock/lambda 数限制，则 grouped dispatch 应提前。

## 参考文献

| 主题 | 文献/资料 | 链接 |
|---|---|---|
| Linux hybrid interrupt/polling | Linux kernel NAPI documentation | https://docs.kernel.org/6.15/networking/napi.html |
| DPDK polling model | DPDK Poll Mode Driver documentation | https://doc.dpdk.org/guides-19.02/prog_guide/poll_mode_drv.html |
| AF_XDP shared rings and busy polling | eBPF AF_XDP documentation | https://docs.ebpf.io/linux/concepts/af_xdp/ |
| netmap shared-buffer packet I/O | Luigi Rizzo, "netmap: A Novel Framework for Fast Packet I/O", USENIX ATC 2012 | https://www.usenix.org/conference/atc12/netmap-novel-framework-fast-packet-io |
| VPP vectorized packet processing | FD.io VPP packet processing graph documentation | https://s3-docs-7day.fd.io/vex-yul-rot-jenkins-2/vpp-docs-verify-2206-ubuntu2004-x86_64/1/aboutvpp/extensible.html |
| IX dataplane OS | Belay et al., "The IX Operating System", TOCS 2016 / OSDI 2014 lineage | https://mast.stanford.edu/pubs/the_ix_operating_system/ |
| ZygOS work-conserving scheduler | Prekas, Kogias, Bugnion, "ZygOS: Achieving Low Tail Latency for Microsecond-scale Networked Tasks", SOSP 2017 | https://infoscience.epfl.ch/record/231395?v=pdf |
| Shenango CPU efficiency | Ousterhout et al., "Shenango: Achieving High CPU Efficiency for Latency-sensitive Datacenter Workloads", NSDI 2019 | https://www.usenix.org/conference/nsdi19/presentation/ousterhout |
| Caladan interference control | Fried et al., "Caladan: Mitigating Interference at Microsecond Timescales", OSDI 2020 | https://www.usenix.org/conference/osdi20/presentation/fried |
| Snap userspace host networking | Marty et al., "Snap: a Microkernel Approach to Host Networking", SOSP 2019 | https://research.google/pubs/snap-a-microkernel-approach-to-host-networking/ |
| Adaptive intermittent polling | Faltelli et al., "Metronome: adaptive and precise intermittent packet retrieval in DPDK" | https://arxiv.org/abs/2103.13263 |
