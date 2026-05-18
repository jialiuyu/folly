SHM Tight Inner Loop 重构：SPSC + NAPI 排空方案（v2）
===
> v2 更新：根据设计审查（参见 `DOUBLE_POLLER_REVIEW.md`）修复 P0 正确性问题、整合 P1/P2 优化建议。
一、修改文件清单
#
文件
改动量
改动性质
1
folly/folly/io/async/ShmPollerService.h
+130 行
重构：新增 SpscQueue/ShmWakeupHandler/ShmDeliveryCallback；Lane 结构扩展；IOBufPool 无锁化
2
folly/folly/io/async/ShmPollerService.cpp
+100 行
重构：poller→SPSC push + 条件 eventfd；新增 5 个方法实现
3
fbthrift/thrift/lib/cpp2/server/Cpp2Worker.h
+4 行
新增 assignedLane_ + getter/setter
4
fbthrift/thrift/lib/cpp2/server/Cpp2Worker.cpp
+8 行
allocateConnIdForLane(assignedLane_) + lane assignment callback
5
fbthrift/thrift/lib/cpp2/server/ThriftServer.h
+10 行
新增 ShmLaneAssignmentCallback typedef + 成员
6
fbthrift/thrift/perf/cpp2/server/Server.cpp
+14 行
SHM 模式强制 io_threads == shm_lanes；attachLaneEvb
7
fbthrift/thrift/perf/cpp2/util/Util.h
+5 行
newShmClient/newClient 增加 laneIdx 参数
8
fbthrift/thrift/perf/cpp2/client/Client.cpp
+8 行
每线程 attachLaneEvb；newClient 传递 laneIdx
--------------------------------------------------------------------------------
二、当前完整数据流图
2.1 Write Path（发请求）
[应用线程] handler->sum(req)
    │
    ▼
writeChain() → writeInternal()
    │
    ▼
ShmPollerService::writeData(peerConnId, data, len)
    │  connId % numLanes → laneIdx
    ▼
writeCursor.fetch_add(chunkLen)
    │  等待 readCursor 腾出空间（反压）
    ▼
memcpy → ringBase[offset]              ← CXL SHM 写入
    │
    ▼
atomic_thread_fence(release)
    │
    ▼
gqm->push({connId, offset, length})    ← GQM 硬件队列
2.2 Read Path（收响应）—— 两级 Poller
╔══════════════════════════════════════════════════════════════════════╗
║  Level 1: POLLER 线程 (dedicated core, 每 Lane 1 个)                ║
║                                                                      ║
║  pollerLoop() [while(running)]                                       ║
║    │                                                                 ║
║    ├─ Phase 1: batch gqm->pop() × ≤32                               ║
║    │   无数据 → idle state machine (HOT→WARM→COLD → yield)           ║
║    │                                                                 ║
║    ├─ Phase 2: atomic_thread_fence(acquire)     ← 1 次 per batch    ║
║    │                                                                 ║
║    ├─ Phase 3: per descriptor:                                       ║
║    │   memcpy ringBase → IOBuf（无锁 pool，优先池化，heap 兜底）      ║
║    │   evbBatch.push_back({transport, chunk})                         ║
║    │                                                                 ║
║    ├─ readCursor.store(localReadCursor, release)  ← 批量更新（v2）   ║
║    │   注意：必须在所有 memcpy 完成后才更新                            ║
║    │   pushCv_.notify_all()  ← 解除 writeData 反压                   ║
║    │                                                                 ║
║    └─ flushEvbBatch():                                               ║
║        for each item:                                                ║
║          retry push to SPSC（反压而非 panic）     ← v2 变更           ║
║        if ioThreadSleeping.load(acquire):                            ║
║          ioThreadSleeping.store(false, release)                      ║
║          write(eventfd, 1)                     ← 唯一 syscall        ║
║        else: 无 syscall (IO 线程在 spin)                             ║
╚══════════════════════════════════════════════════════════════════════╝
                         │
                    SPSC Queue (per Lane, 4096 slot, cache-line 对齐)
                         │
                         ▼
╔══════════════════════════════════════════════════════════════════════╗
║  Level 2: IO 线程 (EventBase, 容器化友好)                            ║
║                                                                      ║
║  ┌── COLD 状态 ────────────────────────────────────────────────┐     ║
║  │  epoll_wait(阻塞, timeout=最近 Timer)                       │     ║
║  │  CPU 占用 0%                                               │     ║
║  │  ioThreadSleeping = true                                    │     ║
║  └──────────────┬─────────────────────────────────────────────┘     ║
║                 │ eventfd 可读                                       ║
║                 ▼                                                    ║
║  ┌── HOT ENTRY: ShmWakeupHandler::handlerReady() ──────────────┐    ║
║  │  drain eventfd                                              │     ║
║  │  deliveryCallback->activate()                               │     ║
║  │    → evb->runInLoop(this)  // 注册到 loopCallbacks_         │     ║
║  └──────────────┬──────────────────────────────────────────────┘    ║
║                 │                                                    ║
║                 ▼                                                    ║
║  ┌── TIGHT INNER LOOP: ShmDeliveryCallback::runLoopCallback() ─┐   ║
║  │                                                             │     ║
║  │  count = 0, deadline = now() + 800μs                        │     ║
║  │  while (count < 2048) {                                     │     ║
║  │    if ((count & 63) == 0 && now() >= deadline) break ← v2  │     ║
║  │    __builtin_prefetch(peekNext())                   ← v2    │     ║
║  │    slot = SPSC.try_pop()                                    │     ║
║  │    if (!slot) break  // 队列排空                             │     ║
║  │    slot.transport->onDataReceived(std::move(slot.data))     │     ║
║  │      → readCallback_->readBufferAvailable()                 │     ║
║  │      → Rocket Parser → handleFrame()                        │     ║
║  │      → ThriftRocketServerHandler                            │     ║
║  │      → Cpp2Worker::dispatch → AsyncProcessor                │     ║
║  │      → Handler::sum()                                       │     ║
║  │    count++                                                  │     ║
║  │  }                                                          │     ║
║  │                                                             │     ║
║  │  if (SPSC 非空):                                            │     ║
║  │    evb->runInLoop(this)  // 重注册 → 下轮继续排空           │     ║
║  │  else:                                                      │     ║
║  │    不重注册 → loopCallbacks_ 变空                            │     ║
║  │    ioThreadSleeping.store(true, release)                     │     ║
║  │    // v2: double-check 防竞态                               │     ║
║  │    if (SPSC.front() != nullptr) {   ← v2 竞态修复           │     ║
║  │      ioThreadSleeping.store(false, release)                  │     ║
║  │      evb->runInLoop(this)  // 回到 tight loop               │     ║
║  │    } else:                                                  │     ║
║  │      下轮 epoll_wait 阻塞 → 回到 COLD                      │     ║
║  └─────────────────────────────────────────────────────────────┘   ║
╚══════════════════════════════════════════════════════════════════════╝
--------------------------------------------------------------------------------
三、两级 Poller 规则
3.1 Level 1: Poller 线程（GQM → SPSC）
属性
规则
备注
线程数
每 Lane 1 个（shm_lanes 个）
readCtx_.lanes[i]
绑核
dedicated physical core（--shm_core_base 起始）
pthread_setaffinity_np
轮询对象
GQM 硬件队列（gqm->pop()）
不可 miss
批处理
每轮 pop ≤ 32 个描述符
kBatchPopMax = 32
内存操作
memcpy ring → IOBuf（无锁 pool 优先，heap 兜底）
IoBufPool 128 slot, 无锁 CAS
投递方式
SPSC.push()（cache-line 对齐）
零 CAS 竞争（per-lane 单写单读）
唤醒 IO
仅在 ioThreadSleeping == true 时 write(eventfd)
否则零 syscall
flush 间隔
累计 ≥ 32 个包或 evb 切换时
kFlushInterval = 32
Idle 策略
HYBRID 三级退避（阈值可配）
可配 SPIN
反压
readCursor 推进 + pushCv_ 通知
writeData 等待线程解除
队列满
SPSC.push() 失败 → 重试 + yield（反压传导到 GQM）
超过阈值才 LOG(FATAL)
Poller Idle State Machine（v2 调优阈值）：
                    有数据到达
        ┌──────────────────────────┐
        │                          ▼
    ┌───────┐   empty > 256   ┌───────┐   empty > 2048  ┌───────┐
    │  HOT  │ ──────────────→ │ WARM  │ ─────────────→ │ COLD  │
    │ yield │ ←────────────── │yield×4│ ←───────────── │yield×16│
    └───────┘   有数据到达     └───────┘   有数据到达    └───────┘
        │                          │                          │
        └────→ 零开销持续 pop ←────┴────→ 中等开销 pop ←──────┘
                                            │
                                     SPIN 模式跳过所有退避
3.2 Level 2: IO 线程（SPSC → Thrift 处理）
属性
规则
备注
线程数
io_threads 个（SHM 模式强制 == shm_lanes）
1:1 Lane 绑定
绑核
排除 poller 核后的 CPU 集合
无 per-thread 严格绑核
状态机
COLD（epoll_wait 阻塞）⇄ HOT（LoopCallback spin drain）
NAPI 式自适应
COLD→HOT
Poller write(eventfd) → ShmWakeupHandler::handlerReady()
1 次 syscall
HOT 持续
ShmDeliveryCallback::runLoopCallback() 紧凑内循环排空 SPSC
零 syscall
计数配额
≤ 2048 包/轮
kMaxDrainCount = 2048
时间配额
≤ 800μs/轮
kMaxDrainTime = 800us，防止 Timer 饿死
时间检查
每 64 次迭代检查一次（v2）
减少 clock_gettime 开销
SPSC 预取
每次 pop 前预取下一 slot（v2）
__builtin_prefetch，减少 cache miss
HOT→COLD
SPSC 排空后不重注册 LoopCallback → ioThreadSleeping = true → double-check SPSC
v2 竞态修复
Timer 保证
配额耗尽时 runInLoop 重注册 → loopCallbacks_ 非空 → epoll_wait(0) 非阻塞 → Timer 正常 tick
双重配额保护
容器化
低负载 IO 线程 sleeping → CPU 占用 0%
可与其他容器共享核心
IO 线程状态机（v2 含竞态修复）：
                          Poller 写 eventfd
    ┌───────────┐     (仅 ioThreadSleeping==true)    ┌───────────────┐
    │   COLD    │ ────────────────────────────────→  │  HOT ENTRY    │
    │ epoll_wait│                                    │ handlerReady()│
    │ 阻塞      │  ←───────────────────────────────  │ drain eventfd │
    │ CPU 0%    │     SPSC 排空                      │ runInLoop()   │
    └───────────┘                                    └───────┬───────┘
         ↑                                                  │
         │                                                  ▼
         │                                          ┌──────────────┐
         │                                          │ TIGHT LOOP   │
         │                                          │ runLoopCb()  │
         │                                          │ drain SPSC   │
         │                                          │ ≤2048/800μs  │
         │                                          │              │
         │     SPSC 排空                             │ SPSC 非空    │
         │ ┌────────────────────────────────────┐   │              │
         │ │ ioThreadSleeping = true             │   │ runInLoop()  │
         │ │ double-check SPSC (v2 防竞态)      │   │ 重注册       │
         │ │ if 非空 → 回到 TIGHT LOOP          │   │ → Timer tick │
         │ │ else → 下轮 epoll_wait 阻塞        │   │ → 继续 drain │
         └─┤                                      │   └──────────────┘
           └────────────────────────────────────┘
--------------------------------------------------------------------------------
四、关键不变量
Lane[i] ←──1:1──→ IO 线程[i]       （SPSC 单写单读，零 CAS 竞争）
Lane[i] ←──1:1──→ Poller 线程[i]   （每 Lane 1 个 GQM poller）
connId  ←──%N───→ Lane[connId % numLanes]

保证方式:
  allocateConnIdForLane(laneIdx) = laneIdx + numLanes × counter++
  → connId % numLanes == laneIdx（数学恒成立）

配置约束:
  io_threads 必须是 shm_lanes 的整数倍
  推荐: io_threads == shm_lanes（1:1 映射）
--------------------------------------------------------------------------------
五、开销对比
5.1 旧路径 vs 新路径（高负载）
旧: Poller → runInEventBaseThread(lambda) → 每批 eventfd write + read
    → 每包穿越完整 loopBody() → epoll_wait(0) → queue execute → callback
    开销: ~1-2μs/包（eventfd ×2 + std::function 堆分配 + loopBody 税）

新: Poller → SPSC.push() → 条件 eventfd（通常不触发）
    → IO 线程 tight loop 排空 2048 包 → EventBase loop 税均摊
    开销: ~0.05μs/包（均摊后 loopBody 税趋近于零）
5.2 各场景开销分析
场景
旧路径（每包）
新路径（每包均摊）
改善
持续 10 万 QPS
1-2μs
~0.05μs
20-40×
间歇突发 1000 包
1-2μs
~0.05μs（1 次 eventfd 均摊）
20-40×
持续 10 QPS
1-2μs
1-2μs（无均摊优势）
1×
空载
0
0（epoll_wait 睡眠）
容器化友好
5.3 消除的开销清单
被消除项
旧路径开销
说明
write(eventfd) 高负载时
~300ns/投递
仅 COLD→HOT 转换时 1 次
read(eventfd) drainFd
~200ns/投递
同上
std::function 堆分配
~50-100ns + malloc 锁
DeliverySlot struct 直传
AtomicNotificationQueue::push CAS
~20-50ns
SPSC 纯 atomic load/store
loopBody() 循环税
~500ns-2μs/包
均摊到 2048 包 → ~0
IOBufPool mutex
~20-50ns/alloc
无锁 CAS atomic ring buffer
shared_lock(connMu_)
~10-30ns/包
per-Lane connTable，无锁
跨线程唤醒延迟
~1-10μs
HOT 期间无唤醒
clock_gettime (now())
~20-40ns/次 × 2048
每 64 次检查一次，~32 次/轮
--------------------------------------------------------------------------------
六、配置示例
典型 96 核 ARM，shm_lanes=4
Server:
  ./server --shm --shm_lanes=4 --shm_core_base=2 --io_threads=4
           --unix_socket_path=/tmp/thrift_shm_benchmark

  核心分配:
    Core 2,3,4,5  → Poller 线程 ×4（dedicated，busy-spin）
    Core 6,7,8,9  → IO 线程 ×4（1:1 绑定 Lane 0-3）
    Core 0,1,10-95 → CPU 线程池（ThriftServer ThreadManager）

  isolcpus 内核参数（推荐）:
    isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5

Client:
  ./client --transport=shm --shm_lanes=4 --shm_core_base=2
           --num_clients=4 --num_connections_per_thread=10
           --host=<server_ip> --port=7777 --sum_weight=1
--------------------------------------------------------------------------------
七、SPSC 队列反压策略（v2 替代 panic）
SPSC 容量 4096 slot。当 push() 返回 false 时不再立即 panic，而是：
1. Poller 暂停从 GQM pop 新数据（不推进 readCursor）
2. yield / 短暂 sleep 等待 IO 线程排空
3. 超过 kMaxSpscBackoffSpins（~100ms 级别）仍未恢复 → LOG(FATAL)

反压传导链:
  SPSC 满 → Poller 停止 pop GQM → readCursor 不推进
    → writeData 的 flow-control 检测到 ring 满 → 写入端 spin/yield
    → 端到端反压自然形成

设计意图：
  瞬态抖动（内核调度延迟、handler 偶发慢路径）不应导致进程 crash。
  4096 slot × 平均包大小 ~1KB = 4MB 缓冲，在 100 万 QPS 下仅 ~4ms 余量，
  需要反压机制吸收抖动。真正的结构性问题（消费端卡死、配置错误）
  通过超时阈值检测并暴露。
--------------------------------------------------------------------------------
八、v1→v2 变更摘要
变更
类型
详细说明
ioThreadSleeping double-check
P0 正确性
IO 线程设置 sleeping=true 后再检查 SPSC，消除竞态窗口
readCursor 批量更新顺序
P0 正确性
确认 Phase 3 所有 memcpy 完成后才 store readCursor
时间检查降频
P1 效率
now() 从每轮调用改为每 64 次迭代检查一次
IOBufPool 无锁化
P1 效率
mutex + vector 替换为 atomic ring buffer（SPSC 模式）
per-Lane connTable
P1 效率
全局 shared_mutex 下沉为 lane 级别，消除跨 Lane bouncing
SPSC 满反压
P2 稳定性
LOG(FATAL) 替换为重试 + 反压传导 + 超时 panic
SPSC cache-line 对齐
P2 性能
读写指针分 cache-line，slot 考虑对齐
idle 退避阈值调优
P3 调优
HOT→WARM: 64→256, WARM→COLD: 512→2048
SPSC 预取
参考
tight loop 中 __builtin_prefetch 预取下一 slot
