# DOUBLE_POLLER 设计审查报告

**审查日期**: 2026-05-18
**审查对象**: `DOUBLE_POLLER.md` — SHM Tight Inner Loop 重构：SPSC + NAPI 排空方案
**对应实现**: `folly/io/async/ShmPollerService.h/.cpp`（当前代码仍为旧路径，文档描述计划中的重构）

---

## 一、总体评价

**设计方向正确。** 两级 Poller + per-Lane SPSC + 条件 eventfd + NAPI 式自适应状态机是业界验证的高效模式，与 DPDK ring + eventfd、io_uring SQ/CQ 的设计哲学一致。核心不变量（Lane 1:1 IO 线程 1:1 Poller 线程）保证了 SPSC 无锁正确性。

**性能预期合理。** 从旧路径的 ~1-2μs/包（每包 `runInEventBaseThread` + `std::function` 堆分配 + `loopBody` 税）降到 ~0.05μs/包（SPSC push + 均摊），20-40× 改善在高负载场景下可信。

**确认的优秀设计点**：
1. SPSC per Lane（零 CAS 竞争，单写单读）
2. 条件 eventfd（仅 COLD→HOT 转换时 1 次 syscall，HOT 期间零 syscall）
3. Tight inner loop 批量排空（2048 包/800μs 双重配额均摊 loopBody 税）
4. Poller idle 三级退避（HOT→WARM→COLD 自适应 CPU 使用）
5. `atomic_thread_fence(acquire)` per batch（减少 per-packet fence 开销）

以下逐项分析发现的问题与优化建议，按优先级排列。

---

## 二、P0 — 正确性问题

### 2.1 ioThreadSleeping 竞态窗口

**位置**: 文档 2.2 节 Level 1 `flushEvbBatch()` + Level 2 IO 线程 COLD→HOT 转换

**问题描述**:

Poller 侧流程：
```
1. SPSC.push(data)                ← release 语义
2. if ioThreadSleeping.load(acquire):
3.   ioThreadSleeping.store(false, release)
4.   write(eventfd, 1)
```

IO 线程侧流程：
```
1. SPSC 排空（try_pop 返回空）
2. ioThreadSleeping.store(true, release)
3. 不重注册 LoopCallback → epoll_wait 阻塞
```

存在如下竞态窗口：

```
时间线          Poller 线程                      IO 线程
─────────────────────────────────────────────────────────────
  T1                                             SPSC 排空
  T2                                             ioThreadSleeping = true
  T3          SPSC.push(data)
  T4          ioThreadSleeping.load() → true
  T5                                             （还未进入 epoll_wait，
                                                  但已决定不再 spin）
  T6          ioThreadSleeping.store(false)
  T7          write(eventfd, 1)                  epoll_wait 唤醒 ✓
```

T7 能正常唤醒。但如果 Poller 在 T4 读到 **false**（IO 线程在 T1-T2 之间还没设置 true，但正在从 HOT 退出）：

```
时间线          Poller 线程                      IO 线程
─────────────────────────────────────────────────────────────
  T1                                             SPSC 排空
  T2          SPSC.push(data)
  T3          ioThreadSleeping.load() → false    （还在 HOT spin）
  T4          不写 eventfd
  T5                                             ioThreadSleeping = true
  T6                                             epoll_wait 阻塞
                                                 → SPSC 中有数据但无唤醒 ✗
```

**后果**: SPSC 中有数据，但 IO 线程在 COLD 状态，Poller 认为不需要写 eventfd → 数据滞留直到下一次 Poller push 触发唤醒。

**修复方案**: IO 线程设置 `sleeping=true` 后，再 double-check SPSC：

```cpp
// ShmDeliveryCallback::runLoopCallback() 末尾
ioThreadSleeping_.store(true, std::memory_order_release);
// Double-check: 如果 poller 在我们上次检查之后 push 了数据，中止 sleep
if (spsc_.front() != nullptr) {
    ioThreadSleeping_.store(false, std::memory_order_release);
    evb->runInLoop(this);
    return;
}
// 安全进入 epoll_wait
```

**原理**: `ioThreadSleeping.store(true, release)` 建立了一个 happens-before 关系。如果 Poller 的 `SPSC.push()` 发生在 IO 线程的 `store(true)` 之后，则 Poller 的 `ioThreadSleeping.load(acquire)` 一定能看到 `true` 并写 eventfd。double-check 覆盖的是 Poller push 发生在 IO 线程 `store(true)` 之前但 `load` 发生在 `store(true)` 之前的窗口。

### 2.2 readCursor 批量更新顺序

**位置**: 文档 3.1 节 Level 1 Poller Phase 末尾

**当前代码问题**: `ShmPollerService.cpp:349-350` 每个 GQM 包都更新 `readCursor`：
```cpp
localReadCursor += length;
ctx.readCursor->store(localReadCursor, std::memory_order_release);
```

文档设计改为批量更新（Phase 3 全部 memcpy 完成后统一 store），这是正确的。但如果实现时误将 `readCursor` 更新放到 per-descriptor 循环内，写入端可能看到已推进的 readCursor 但对应的数据区域尚未完成 memcpy → 数据损坏。

**确认**: 文档设计正确，Phase 3 全部 memcpy → Phase 末尾 `readCursor.store()`。实现时必须严格遵守此顺序。

---

## 三、P1 — 效率优化

### 3.1 Tight Loop 时间检查降频

**位置**: Level 2 IO 线程 `runLoopCallback()` 内循环条件 `now() < deadline`

**问题**: `std::chrono::steady_clock::now()` 在 Linux 上通过 vDSO 调用 `clock_gettime`，开销约 20-40ns。每轮迭代都调用时，2048 包满载产生 ~40-80μs 的纯计时开销，占 800μs 时间配额的 5-10%。

**优化**: 每 64 次迭代检查一次时间：

```cpp
constexpr uint32_t kTimeCheckInterval = 64;
uint32_t count = 0;
auto deadline = std::chrono::steady_clock::now() + kMaxDrainTime;

while (count < kMaxDrainCount) {
    // 仅在检查点判断时间
    if ((count & (kTimeCheckInterval - 1)) == 0 && count > 0) {
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    auto slot = spsc_.try_pop();
    if (!slot) break;
    slot->transport->onDataReceived(std::move(slot->data));
    count++;
}
// 循环结束后做最终时间检查（不影响下一轮 runInLoop 决策）
```

**收益**: 计时调用从 ~2048 次降到 ~32 次，节省 ~40μs/轮。

### 3.2 IOBufPool 无锁化

**位置**: `ShmPollerService.h:108-139`，当前 `std::mutex` + `std::vector<void*>`

**问题**: IOBufPool 的访问模式是 SPSC：
- **生产者（push）**: IO 线程 — Thrift parser 释放 IOBuf 时触发 `iobufPoolDeleter` → `pool.push()`
- **消费者（alloc）**: Poller 线程 — `pool.alloc()` 获取缓冲区用于 memcpy

mutex 在 poller 热路径上每批 32 个包产生 32 次 lock/unlock（~20-50ns/次 = ~640-1600ns/批）。

**优化**: 替换为基于 atomic ring buffer 的无锁实现：

```cpp
struct IOBufPool {
    static constexpr size_t kCapacity = 128;
    static constexpr size_t kMask = kCapacity - 1;
    static_assert((kCapacity & kMask) == 0, "capacity must be power of 2");

    alignas(64) std::atomic<uint32_t> head_{0}; // push 侧（IO 线程）
    alignas(64) std::atomic<uint32_t> tail_{0}; // alloc 侧（Poller 线程）
    std::array<std::atomic<void*>, kCapacity> slots_{};

    void* alloc() {
        auto t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            auto& slot = slots_[t & kMask];
            void* p = slot.load(std::memory_order_acquire);
            if (!p) {
                // 空：回退到 malloc
                return std::malloc(kBufSize);
            }
            if (slot.compare_exchange_weak(p, nullptr,
                    std::memory_order_acq_rel)) {
                tail_.store(t + 1, std::memory_order_release);
                return p;
            }
        }
    }

    void push(void* p) {
        auto h = head_.load(std::memory_order_relaxed);
        auto& slot = slots_[h & kMask];
        void* expected = nullptr;
        if (slot.compare_exchange_strong(expected, p,
                std::memory_order_release)) {
            head_.store(h + 1, std::memory_order_release);
        } else {
            std::free(p); // 池满，直接释放
        }
    }

    ~IOBufPool() {
        for (auto& s : slots_) {
            if (void* p = s.load(std::memory_order_relaxed)) {
                std::free(p);
            }
        }
    }
};
```

**收益**: 消除 poller 热路径 mutex，每批节省 ~640-1600ns。head/tail 分 cache-line 避免.false sharing。

### 3.3 per-Lane 连接表消除全局 shared_lock

**位置**: 当前代码 `ShmPollerService.cpp:363-373`，全局 `connMu_` + `connTable_`

**问题**: 文档设计中 Poller Phase 3 仍需查询 transport 指针。全局 `shared_mutex` 在多 Lane 场景下产生不必要的 cache-line bouncing：Lane 0 的 poller 读 connTable 时，Lane 3 的 register/unregister 写操作会使 Lane 0 的 shared_lock 缓存行失效。

由于 Lane 1:1 绑定 IO 线程，connId 不会跨 Lane，全局表是多余的。

**优化**: 将连接表下沉到 Lane 级别：

```cpp
struct LaneContext {
    // ...existing members...
    alignas(64) std::atomic<uint32_t> connTableVersion{0}; // 写时递增
    struct ConnSlot {
        BusyPollSharedMemoryTransport* transport{nullptr};
        // cache-line padding 避免相邻 slot false sharing
    } __attribute__((aligned(64)));
    std::unordered_map<uint16_t, ConnSlot> laneConnTable;
};
```

同步规则：
- **Poller 线程**（读）：直接读 `laneConnTable`，无需锁（poller 只读自己 Lane 的表）
- **IO 线程**（写）：register/unregister 在 IO 线程上执行（EventBase 串行化保证）
- **安全保证**：`onDataReceived()` 在 IO 线程上同步执行，unregister 也在 IO 线程上，EventBase 的单线程保证不存在 use-after-free

**收益**: 消除每包 `shared_lock` 开销（~10-30ns），消除多 Lane 间的 cache-line bouncing。

---

## 四、P2 — 稳定性 + 性能

### 4.1 SPSC 队列满：反压替代 panic

**位置**: 文档 2.2 节 Level 1 "if full → LOG(FATAL) panic"

**问题**: 4096 slot 缓冲在 100 万 QPS 下仅 ~4ms 余量。短暂的 IO 线程卡顿（内核调度延迟、handler 偶发慢路径）不应导致进程 crash。

**优化**: 两阶段反压策略：

```cpp
// Poller Phase 3 — flushEvbBatch 中
for (auto& item : evbBatch) {
    uint32_t backoffSpins = 0;
    while (!spsc.push(std::move(item))) {
        backoffSpins++;
        if (backoffSpins > kMaxSpscBackoffSpins) { // 例: 100000 (~100ms)
            LOG(FATAL) << "SPSC queue stuck for lane=" << laneIdx
                       << " spins=" << backoffSpins;
        }
        // 退避：给 IO 线程机会排空
        if (backoffSpins < kYieldThreshold) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
}
```

同时，Poller 在 SPSC 满时暂停从 GQM pop 新数据（不推进 readCursor），自然将反压传导到写入端。

**收益**: 吸收瞬态抖动（~100ms 级别），仅对真正的死锁/配置错误 crash。

### 4.2 SPSC Slot 缓存行对齐

**问题**: 文档未提及 SPSC 队列内部数据结构的 cache-line 对齐。如果 SPSC 的写指针和读指针共享 cache-line（典型实现中仅相隔 8 字节），Poller 写入和 IO 线程读取会产生 false sharing，每次指针更新导致 ~40ns 的 cache-line bounce。

**优化**: SPSC control block 强制 cache-line 分离：

```cpp
template <typename T, size_t Capacity>
class SpscQueue {
    static constexpr size_t kMask = Capacity - 1;
    static_assert((Capacity & kMask) == 0, "Capacity must be power of 2");

    alignas(64) std::atomic<uint32_t> writePos_{0};  // Poller 写
    char pad_[64 - sizeof(std::atomic<uint32_t>)];
    alignas(64) std::atomic<uint32_t> readPos_{0};   // IO 线程写
    char pad2_[64 - sizeof(std::atomic<uint32_t>)];

    alignas(64) std::array<T, Capacity> slots_{};
    // ...
};
```

如果 slot 本身较大（含指针 + IOBuf），考虑使用 slot index + 外部数组，避免 SPSC ring 内部 padding 浪费空间。

**收益**: 消除读写指针 false sharing，SPSC 吞吐提升 ~10-20%。

---

## 五、P3 — 调优建议

### 5.1 Poller Idle 退避阈值调优

**位置**: 文档 3.1 节 idle state machine "empty > 64 → WARM, empty > 512 → COLD"

**问题**: HOT→WARM 的 64 次空轮门槛偏低。假设 GQM pop 周期 ~100ns，64 次仅需 ~6.4μs 就会降级到 WARM。在 10K QPS burst 场景下（burst 间隔 ~100μs），poller 会在 HOT 和 WARM 之间频繁切换，每次切换改变 yield 策略可能引入微小的延迟毛刺。

**建议**: 提高阈值或使用自适应方案：

| 方案 | HOT→WARM | WARM→COLD | 适用场景 |
|------|----------|-----------|----------|
| 当前 | 64 | 512 | 超低延迟优先 |
| 保守 | 256 | 2048 | 通用 |
| 自适应 | `64 * (1 + recentBurstWindow/10)` | 自适应 | 混合负载 |

推荐先使用保守方案（256/2048），上线后根据实际 idle 分布直方图调优。

---

## 六、额外建议

### 6.1 SPSC 预取（Software Prefetch）

在 tight inner loop 中，`try_pop()` 读取下一个 slot 时可以预取后续 slot：

```cpp
while (count < kMaxDrainCount) {
    // 预取下一个 slot
    __builtin_prefetch(&spsc_.peekNext(), 0, 1);
    auto slot = spsc_.try_pop();
    if (!slot) break;
    slot->transport->onDataReceived(std::move(slot->data));
    count++;
}
```

在 ARM 平台上效果更明显（内存延迟更高），预期减少 ~10-20ns/slot 的 cache miss 延迟。

### 6.2 batch readCursor 更新与 writeData 通知

当前设计在 Poller Phase 末尾才更新 `readCursor`，这会导致 `writeData` 的 flow-control spin 在整批处理期间看到的 `readCursor` 是旧的（可能误判为空间不足）。可以考虑：

- 保持批量更新（正确性优先），但在 `writeData` 侧增加短暂的 optimistic spin（当前已有 spin+yield 机制）
- 或在 Phase 中间插入一次中间 readCursor 更新（每 16-32 个包），在延迟和反压响应性间取舍

这个 trade-off 需要实际基准测试验证。

---

## 七、优先级总览

| 优先级 | 问题 | 类型 | 预估收益 | 实现复杂度 |
|--------|------|------|----------|-----------|
| **P0** | 2.1 ioThreadSleeping 竞态窗口 | 正确性 | 消除数据滞留 | 低 |
| **P0** | 2.2 readCursor 批量更新顺序 | 正确性 | 防止数据损坏 | 低（文档已正确） |
| **P1** | 3.1 时间检查降频 | 效率 | ~40μs/轮 | 低 |
| **P1** | 3.2 IOBufPool 无锁化 | 效率 | ~0.6-1.6μs/批 | 中 |
| **P1** | 3.3 per-Lane connTable | 效率 | ~0.3-1μs/批 | 中 |
| **P2** | 4.1 SPSC 满反压替代 panic | 稳定性 | 生产可用性 | 中 |
| **P2** | 4.2 SPSC cache-line 对齐 | 性能 | ~10-20% SPSC 吞吐 | 低 |
| **P3** | 5.1 idle 退避阈值调优 | 调优 | 减少状态切换 | 低 |
| 参考 | 6.1 SPSC 预取 | 性能 | ~10-20ns/slot | 低 |
| 参考 | 6.2 batch readCursor 延迟 | trade-off | 需基准测试 | 中 |
