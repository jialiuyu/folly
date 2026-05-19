# AI Infra / LLM Inference Infra 2026 秋招冲刺计划

> 时间：2026/05/20 - 2026/09/15（17 周）
> 目标：面向社招投递 AI Infra、LLM Inference、Agent Infra、ML Systems、AI Platform 岗位
> 时间预算：工作日 <= 2.5h/天，周末可全天；建议每周 22-26h
> 主线：LLM inference infra 项目落地 + 既有 RPC/Redis/SHM/NCCL 经验重新包装

---

## 0. 总体判断

这条路线不把过去的 RPC、Redis、netpoll、SHM transport 经验丢掉，而是把它们重新翻译成 AI infra 面试官关心的能力：

| 既有经验 | AI infra 里的表达 |
|---|---|
| fbthrift / Kitex / RPC benchmark | 高并发请求接入、服务框架、连接管理、tail latency、benchmark |
| folly SHM transport / busy polling | 低延迟 IPC、零拷贝、热路径优化、竞态修复、P99 延迟分析 |
| Redis / HPC-Redis | 在线缓存、向量检索、内存管理、批处理、低延迟数据路径 |
| netpoll / epoll | 事件循环、backpressure、连接生命周期、高性能网络 I/O |
| NCCL | GPU 集群通信、collective、分布式推理/训练通信基础 |
| RAG study | Agent/RAG workload 理解，把底层 infra 和真实 AI 负载接起来 |

最终简历叙事：

> 我过去做高性能网络、RPC、缓存和共享内存通信。现在把这些能力迁移到 LLM inference infra，重点解决高并发推理服务中的请求调度、KV cache 复用、tail latency、可观测性和底层性能问题。

---

## 1. 岗位定位

### 主投岗位

- LLM Inference Infrastructure Engineer
- AI Infrastructure Engineer
- ML Systems Engineer
- AI Platform Engineer
- Agent Infrastructure Engineer
- High Performance Serving Engineer

### 次投岗位

- GPU Computing / CUDA Systems Engineer
- Distributed Training Infrastructure Engineer
- Storage / Cache / Vector Database Infrastructure Engineer
- RPC / High Performance Networking Engineer with AI workload exposure

### 暂不主投

- 纯基模算法研究
- 纯 prompt engineering / agent 应用搭建
- 只做业务层 API 编排的 AI 应用岗

原因：4-5 个月窗口内，最稳的路线是把已有系统能力迁移到 LLM serving，而不是从零证明自己是模型算法或训练框架专家。

---

## 2. 三个交付项目

### 项目 1：llm-inference-gateway-lab（主项目）

**一句话**：面向 RAG/Agent 混合负载的 OpenAI-compatible LLM inference gateway，后端接 vLLM/SGLang replica，支持 benchmark、prefix-cache-aware routing、限流熔断和可观测性。

**为什么不是玩具**：

- 解决真实 inference serving 问题：TTFT、TPOT、P95/P99、queueing、KV cache reuse。
- 能与 agent/RAG workload 结合，不是单纯 chat demo。
- 用 Go 写 gateway，能复用 netpoll/RPC 经验。
- 后端用 vLLM/SGLang，贴近真实生产栈。

**核心功能**：

- `/v1/chat/completions` OpenAI-compatible proxy
- backend health check
- latency-aware / queue-aware routing
- prefix-cache-aware routing
- timeout / retry / circuit breaker
- Prometheus metrics
- structured logs + request id
- RAG / agent workload generator
- benchmark report

**简历目标描述**：

> Built an OpenAI-compatible LLM inference gateway for RAG/agent workloads, with backend health checks, request-level metrics, retry/circuit breaker, and prefix-cache-aware routing across vLLM replicas.

---

### 项目 2：systems-performance-casebook（既有项目包装）

**一句话**：把已有 SHM transport、Redis-HPC、netpoll、NCCL 阅读成果整理成系统性能案例集，每个案例都有问题、设计、bug、优化和验证。

**为什么重要**：

- 这是你区别于普通 AI 应用开发者的核心证据。
- 面试官会更相信“做过底层系统的人能做好 inference infra”。
- 不需要重写大量代码，重点是整理成可讲述、可追问的工程故事。

**案例候选**：

1. fbthrift / folly shared-memory transport
   - connId 路由语义
   - SHM handshake 兼容性
   - GQM 初始化竞态
   - hot-path lock 优化
   - E2E latency / P99 benchmark

2. HPC-Redis vector engine
   - vector engine abstraction
   - batch processor
   - SVE / scalar fallback
   - three-layer cache
   - Redis 事件循环与低侵入式扩展

3. netpoll
   - epoll/kqueue
   - zero-copy buffer
   - connection lifecycle
   - backpressure

4. NCCL
   - Ring / Tree all-reduce
   - channel / proxy thread
   - topology and transport choices

**简历目标描述**：

> Implemented and analyzed low-latency RPC/shared-memory transport paths, including connection routing, handshake compatibility, race fixes, hot-path locking, and E2E P99 latency instrumentation.

---

### 项目 3：gpu-comm-kernel-bench（底层加分项）

**一句话**：小而硬的 GPU/通信性能实验，覆盖 CUDA/Triton kernel、NCCL collective benchmark 和 profiler 分析。

**范围控制**：

- 不做完整 FlashAttention。
- 不做完整 Megatron/DeepSpeed 训练框架。
- 不强依赖 RDMA/IB 环境。
- 只做能实测、能解释、能写进简历的最小闭环。

**核心内容**：

- CUDA/Triton reduction、softmax 或 layernorm kernel
- PyTorch baseline correctness check
- Nsight / profiler 分析
- NCCL all-reduce / all-gather benchmark
- Ring vs Tree / channel / topology 笔记

**简历目标描述**：

> Built a CUDA/Triton kernel benchmark suite for reduction/layernorm/softmax with correctness checks, PyTorch baselines, and profiler-based bottleneck analysis.

---

## 3. 每周节奏

### 工作日安排

| 天数 | 内容 | 时间 |
|---|---|---|
| 3 天 | 主项目编码 | 2-2.5h/天 |
| 1 天 | 文档 / paper / 源码笔记 | 2h |
| 1 天 | 面试基础 / 系统设计 / 算法 | 2h |

### 周末安排

| 时间 | 内容 |
|---|---|
| 周六 6-8h | 主项目大块开发、调试、benchmark |
| 周日 5-7h | 报告、README、复盘、简历沉淀、下周计划 |

### 每周必须产出

每周至少产出一个可见 artifact：

- 一个可运行脚本
- 一个模块
- 一张 benchmark 表
- 一篇技术笔记
- 一个 README section
- 一个面试问题答案

禁止只“学习”不沉淀。

---

## 4. Phase 1：LLM Serving 基础与 Benchmark（W1-W3，05/20 - 06/09）

> 目标：跑通 vLLM/SGLang serving，建立 TTFT/TPOT/P95/P99 benchmark 基线。

### W1（05/20 - 05/26）：环境与 serving 基线

**Week Goal**：跑通一个本地或云端 vLLM backend，并能用脚本请求。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 05/20 | 项目初始化 | 创建 `llm-inference-gateway-lab` 仓库结构 | repo skeleton |
| Thu 05/21 | vLLM 跑通 | 启动一个 OpenAI-compatible vLLM server | 启动脚本 |
| Fri 05/22 | 请求脚本 | Python 请求 `/v1/chat/completions` | smoke test |
| Sat 05/23 | Benchmark v0 | 实现并发请求压测，记录 latency | `bench.py` |
| Sun 05/24 | 指标定义 | 写清 TTFT、TPOT、E2E latency、QPS | metrics note |
| Mon 05/25 | 结果落盘 | CSV/JSON 输出 benchmark 结果 | result schema |
| Tue 05/26 | 周复盘 | README 写明如何复现 | W1 report |

### W2（05/27 - 06/02）：Benchmark matrix

**Week Goal**：不同并发、prompt length、output length 下有结构化结果。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 05/27 | 并发矩阵 | concurrency: 1/4/16/64 | matrix config |
| Thu 05/28 | prompt 矩阵 | short/medium/long prompt | workload file |
| Fri 05/29 | output 矩阵 | short/long generation | workload file |
| Sat 05/30 | 自动化 | 一键跑完整 benchmark matrix | runner |
| Sun 05/31 | 可视化 | 生成 latency / throughput 图 | report chart |
| Mon 06/01 | 分析 | 写 baseline 性能分析 | benchmark note |
| Tue 06/02 | 复盘 | 形成 W2 report | W2 report |

### W3（06/03 - 06/09）：KV cache / prefix cache 理解

**Week Goal**：理解 prefill/decode、KV cache、prefix caching 对 TTFT/TPOT 的影响。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 06/03 | prefill/decode | 写概念笔记和请求生命周期图 | serving note |
| Thu 06/04 | prefix workload | 构造相同 system prompt / tool schema 负载 | workload |
| Fri 06/05 | cache 对比 | 比较重复 prefix 与随机 prompt 的 TTFT | data |
| Sat 06/06 | 报告 | 完成 benchmark report v0.1 | report |
| Sun 06/07 | 源码阅读 | 阅读 vLLM scheduler / cache 相关文档或代码 | notes |
| Mon 06/08 | 面试沉淀 | 准备 5 个 serving 高频问答 | Q&A |
| Tue 06/09 | Phase 1 复盘 | 检查是否能讲清 TTFT/TPOT/KV cache | phase report |

**Phase 1 通过标准**：

- 能一键启动 backend 并运行 benchmark。
- 能解释 TTFT、TPOT、throughput、P95/P99。
- 能解释 KV cache 为什么影响显存和 routing。
- README 有可复现实验步骤。

---

## 5. Phase 2：Inference Gateway 主体开发（W4-W7，06/10 - 07/07）

> 目标：用 Go 实现 OpenAI-compatible gateway，把 RPC/netpoll 经验迁移到 LLM serving 接入层。

### W4（06/10 - 06/16）：Gateway MVP

**Week Goal**：请求能从 gateway 转发到 vLLM backend。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 06/10 | API 设计 | 定义 `/v1/chat/completions` proxy 行为 | design note |
| Thu 06/11 | Go skeleton | HTTP server、config、backend struct | code |
| Fri 06/12 | 转发逻辑 | 请求转发到 backend，保留 request id | code |
| Sat 06/13 | 测试 | 单元测试 + smoke test | tests |
| Sun 06/14 | Docker | docker compose 启动 gateway + backend | compose |
| Mon 06/15 | README | 写本地启动方式 | README |
| Tue 06/16 | 复盘 | MVP 性能和限制记录 | W4 report |

### W5（06/17 - 06/23）：Routing / health check

**Week Goal**：支持多个 backend，并具备基本健康检查和路由。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 06/17 | health check | 定时探测 backend 状态 | code |
| Thu 06/18 | round-robin | 实现最基础负载均衡 | code |
| Fri 06/19 | latency-aware | 维护 backend latency EWMA | code |
| Sat 06/20 | queue-aware | 维护 inflight / queue length | code |
| Sun 06/21 | benchmark | 对比 direct vs gateway overhead | data |
| Mon 06/22 | 文档 | 写 routing design doc | doc |
| Tue 06/23 | 复盘 | 准备 routing 面试问答 | Q&A |

### W6（06/24 - 06/30）：Timeout / retry / circuit breaker

**Week Goal**：补齐生产服务必备的稳定性机制。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 06/24 | timeout | request-level timeout | code |
| Thu 06/25 | retry | retry policy with idempotency note | code |
| Fri 06/26 | circuit breaker | backend failure window + open/half-open | code |
| Sat 06/27 | chaos test | 模拟 backend 挂掉 / 慢请求 | test |
| Sun 06/28 | backpressure | inflight 超限返回 429/503 | code |
| Mon 06/29 | 文档 | 稳定性设计文档 | doc |
| Tue 06/30 | 复盘 | W6 report | report |

### W7（07/01 - 07/07）：Metrics / tracing / observability

**Week Goal**：把 gateway 做成可观测服务。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 07/01 | Prometheus | 暴露 request count / latency histogram | code |
| Thu 07/02 | backend metrics | backend inflight / health / errors | code |
| Fri 07/03 | structured log | request id、backend id、latency | code |
| Sat 07/04 | Grafana | dashboard 或 markdown 图表 | dashboard |
| Sun 07/05 | benchmark | gateway under load | data |
| Mon 07/06 | README | 架构图 + metrics 示例 | README |
| Tue 07/07 | Phase 2 复盘 | Gateway v0.5 可展示 | phase report |

**Phase 2 通过标准**：

- Gateway 能稳定转发到多个 backend。
- 有 health check、routing、timeout、retry、backpressure。
- 有 Prometheus metrics。
- 能量化 gateway overhead。

---

## 6. Phase 3：Agent/RAG Workload 与 Prefix-aware Routing（W8-W10，07/08 - 07/28）

> 目标：把项目从“推理代理”升级为“面向 agent/RAG 真实负载的 inference infra”。

### W8（07/08 - 07/14）：RAG workload

**Week Goal**：复用现有 RAG 学习项目思想，构造长上下文检索负载。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 07/08 | RAG 数据 | 准备小型文档集和问题集 | dataset |
| Thu 07/09 | workload generator | 生成 retrieval + prompt 请求 | code |
| Fri 07/10 | long context | 控制 context length 分布 | workload |
| Sat 07/11 | benchmark | RAG workload 压测 | data |
| Sun 07/12 | 分析 | RAG 对 TTFT/P99 的影响 | report |
| Mon 07/13 | 文档 | RAG workload 设计 | doc |
| Tue 07/14 | 复盘 | W8 report | report |

### W9（07/15 - 07/21）：Agent workload

**Week Goal**：构造带 tool schema、多轮对话、重复 prefix 的 agent workload。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 07/15 | tool schema | 固定 system prompt + tool definitions | workload |
| Thu 07/16 | multi-turn | 多轮请求链路模拟 | code |
| Fri 07/17 | mixed workload | chat + RAG + agent 混合流量 | code |
| Sat 07/18 | 压测 | 比较不同负载下 tail latency | data |
| Sun 07/19 | 分析 | agent workload 特征总结 | report |
| Mon 07/20 | 文档 | workload taxonomy | doc |
| Tue 07/21 | 复盘 | W9 report | report |

### W10（07/22 - 07/28）：Prefix-cache-aware routing

**Week Goal**：实现并验证 prefix-aware routing。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 07/22 | prefix key | 从 system prompt/tool schema 计算 routing key | code |
| Thu 07/23 | sticky routing | 相同 prefix 尽量路由到同 backend | code |
| Fri 07/24 | fallback | backend 不健康时回退到 latency-aware | code |
| Sat 07/25 | 对比实验 | round-robin vs prefix-aware | data |
| Sun 07/26 | 报告 | cache hit / TTFT / P99 对比 | report |
| Mon 07/27 | README | 项目主 README v1.0 | README |
| Tue 07/28 | Phase 3 复盘 | 主项目可写入简历 | phase report |

**Phase 3 通过标准**：

- 有 RAG、agent、mixed 三类 workload。
- 有 round-robin 与 prefix-aware routing 对比。
- 能用数据解释 routing 对 TTFT/P99 的影响。
- 主项目 README 有架构图和 benchmark 图。

---

## 7. Phase 4：底层系统背书与 GPU/通信加分项（W11-W13，07/29 - 08/18）

> 目标：整理旧项目资产，同时补 GPU/通信最小闭环。

### W11（07/29 - 08/04）：Shared-memory RPC case study

**Week Goal**：把 folly/fbthrift SHM transport 整理成可面试讲述的系统案例。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 07/29 | 梳理提交 | 整理 SHM transport 相关 commits | commit map |
| Thu 07/30 | 问题定义 | 写 connId、handshake、GQM race 问题描述 | case note |
| Fri 07/31 | 性能视角 | 整理 P99 latency / hot-path lock 优化 | case note |
| Sat 08/01 | 案例文档 | 完成 `shm-rpc-case-study.md` | doc |
| Sun 08/02 | 口述练习 | 5min / 10min 两版讲述稿 | script |
| Mon 08/03 | 面试问答 | 准备 race、memory ordering、zero-copy 问答 | Q&A |
| Tue 08/04 | 复盘 | W11 report | report |

### W12（08/05 - 08/11）：Redis / cache / vector path case study

**Week Goal**：把 Redis-HPC 与向量检索/RAG storage 联系起来。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 08/05 | Redis 事件循环 | 梳理 Redis request path | notes |
| Thu 08/06 | vector engine | 整理 vector engine / batch processor | notes |
| Fri 08/07 | cache hierarchy | 整理 three-layer cache 和 SVE fallback | notes |
| Sat 08/08 | 案例文档 | 完成 `hpc-redis-vector-case-study.md` | doc |
| Sun 08/09 | 关联 RAG | 写 embedding store / retrieval infra 迁移点 | doc |
| Mon 08/10 | 口述练习 | 5min 项目讲述 | script |
| Tue 08/11 | 复盘 | W12 report | report |

### W13（08/12 - 08/18）：CUDA/Triton + NCCL 最小闭环

**Week Goal**：补足 GPU/通信基础，但不抢主线。

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 08/12 | kernel 选择 | 选择 reduction + layernorm 或 softmax | design |
| Thu 08/13 | correctness | 与 PyTorch baseline 做 allclose | tests |
| Fri 08/14 | profiler | Nsight / torch profiler 分析瓶颈 | report |
| Sat 08/15 | NCCL bench | all-reduce / all-gather benchmark | data |
| Sun 08/16 | NCCL 笔记 | Ring/Tree/channel/proxy thread 笔记 | notes |
| Mon 08/17 | README | gpu-comm-kernel-bench README | README |
| Tue 08/18 | Phase 4 复盘 | 底层加分项可讲述 | phase report |

**Phase 4 通过标准**：

- 旧项目至少 2 个 case study 可面试讲。
- CUDA/Triton 或 NCCL 有一个真实可运行实验。
- 能把 RPC/Redis/SHM 连接到 LLM inference infra。

---

## 8. Phase 5：简历、投递、面试冲刺（W14-W17，08/19 - 09/15）

> 目标：冻结项目，形成投递材料，边投边修。

### W14（08/19 - 08/25）：简历与 GitHub

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 08/19 | 简历 v1 | 写 AI infra 定位版简历 | resume |
| Thu 08/20 | 主项目 README | 完善 gateway README、架构图、benchmark 图 | README |
| Fri 08/21 | case study README | 整理 systems casebook 入口 | README |
| Sat 08/22 | GitHub profile | pin 主项目和 case study | profile |
| Sun 08/23 | 技术博客 | 写一篇 inference gateway 设计文章 | blog |
| Mon 08/24 | 简历 v2 | 修改措辞，压缩到高信号 | resume |
| Tue 08/25 | 复盘 | 确定投递版本 | W14 report |

### W15（08/26 - 09/01）：系统设计与项目深挖

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 08/26 | 设计题 1 | 设计 LLM inference gateway | design answer |
| Thu 08/27 | 设计题 2 | 设计多租户 GPU inference platform | design answer |
| Fri 08/28 | 设计题 3 | 设计 RAG/agent serving system | design answer |
| Sat 08/29 | mock interview | 45min 项目深挖模拟 | feedback |
| Sun 08/30 | 修补 | 根据反馈补文档或代码 | patch |
| Mon 08/31 | 八股 | C++/Go 并发、OS、网络、Redis | notes |
| Tue 09/01 | 复盘 | W15 report | report |

### W16（09/02 - 09/08）：第一批投递

| 日期 | Daily Goal | 具体任务 | 产出 |
|---|---|---|---|
| Wed 09/02 | 公司清单 | 目标公司/JD/内推人整理 | list |
| Thu 09/03 | 投递 1 | 投递 5-8 家主匹配岗位 | record |
| Fri 09/04 | JD 对齐 | 针对 JD 微调简历关键词 | resume variants |
| Sat 09/05 | mock interview | 第二次模拟面试 | feedback |
| Sun 09/06 | 算法 | 高频题 10 道 + 错题复盘 | record |
| Mon 09/07 | 项目回顾 | 复述三个项目，每个 5min | script |
| Tue 09/08 | 复盘 | 投递反馈与策略调整 | report |

### W17（09/09 - 09/15）：面试实战与调整

这周不安排固定开发任务。每天做三件事：

1. 面试前：按 JD 选择主项目、SHM case、Redis case 中最匹配的讲述顺序。
2. 面试后：记录问题、追问点、答得不好的知识点。
3. 当天补洞：把被问倒的问题补进 README、Q&A 或下一轮讲述稿。

---

## 9. 简历项目写法

### 项目 1：LLM Inference Gateway for Agent/RAG Workloads

可写 bullet：

- Built an OpenAI-compatible LLM inference gateway for RAG/agent workloads, supporting backend health checks, latency-aware routing, timeout/retry/circuit breaker, backpressure, and Prometheus metrics.
- Designed a benchmark suite to measure TTFT, TPOT, throughput, GPU memory pressure, and P95/P99 latency under mixed chat/RAG/agent workloads.
- Implemented prefix-cache-aware routing based on system prompts and tool schemas, and compared it against round-robin routing under repeated-prefix traffic.

如果有实测数据，投递前只保留已验证的具体数字；没有稳定数据就删除这一条：

- Reduced P95 latency / improved TTFT / increased prefix-cache hit rate under synthetic agent workloads, measured against a round-robin routing baseline.

### 项目 2：Low-latency Shared-memory RPC Transport

可写 bullet：

- Implemented and analyzed a shared-memory RPC transport path in folly/fbthrift, covering connection id routing, SHM handshake compatibility, GQM initialization race fixes, and hot-path lock optimization.
- Added E2E latency and P99 benchmark instrumentation for RPC workloads, and used diagnostic probes to debug routing, memory ordering, and poller lifecycle issues.

### 项目 3：HPC-Redis Vector Engine / Cache Path

可写 bullet：

- Extended Redis with a vector-engine abstraction and batch-processing path for high-performance retrieval experiments, integrating cache hierarchy, SIMD/SVE fallback, and low-intrusion Redis configuration hooks.
- Analyzed Redis event-loop constraints, memory layout, batching behavior, and cache-layer trade-offs for online vector retrieval workloads.

### 项目 4：GPU / NCCL Performance Bench

可写 bullet：

- Built a CUDA/Triton microbenchmark suite for reduction/layernorm/softmax kernels with PyTorch correctness checks and profiler-based bottleneck analysis.
- Studied NCCL collective communication paths, including Ring/Tree all-reduce, channels, proxy threads, and topology-related tuning.

---

## 10. 面试问题清单

### LLM inference

1. TTFT 和 TPOT 分别受什么影响？
2. prefill 和 decode 为什么行为不同？
3. KV cache 为什么会成为显存瓶颈？
4. continuous batching 如何影响吞吐和 latency？
5. prefix cache 在 agent workload 中为什么重要？
6. 多 backend inference gateway 如何路由？
7. 后端慢、挂、排队过长时怎么处理？
8. 如何设计 Prometheus metrics？
9. 如何定位 P99 tail latency？
10. RAG 和普通 chat workload 的 serving 特征有什么区别？

### 系统设计

1. 设计一个 LLM inference gateway。
2. 设计一个多租户 GPU inference platform。
3. 设计一个支持 agent tool calling 的 serving system。
4. 设计一个 embedding retrieval + generation 的 RAG infra。
5. 设计一个低延迟 RPC 服务框架。

### 底层系统

1. epoll/kqueue 事件循环如何管理连接生命周期？
2. backpressure 在 RPC/gateway 中怎么做？
3. 共享内存 transport 如何处理连接路由和生命周期？
4. busy polling 的收益和代价是什么？
5. CAS / memory ordering / shared_mutex 分别适合什么场景？
6. Redis 单线程事件循环为什么能高性能？
7. Redis 加批处理会破坏哪些语义？
8. Ring all-reduce 的步骤和通信量是什么？
9. NCCL proxy thread 为什么存在？
10. CUDA kernel 怎么判断 memory-bound 还是 compute-bound？

---

## 11. 降级策略

如果时间不够，按优先级保留：

1. 保留主项目 gateway + benchmark。
2. 保留 SHM RPC case study。
3. 保留 Redis vector/cache case study。
4. CUDA/Triton 只做一个 kernel。
5. NCCL 只做笔记和一个 benchmark。
6. 不做复杂 Web UI。
7. 不做完整 K8s operator。
8. 不做 Megatron/DeepSpeed 源码深挖。

最小可投递版本：

- Gateway 能跑。
- Benchmark 有数据。
- README 有架构图和结果。
- 旧项目有 2 个 case study。
- 简历能讲清楚“从底层系统迁移到 LLM inference infra”。

---

## 12. 每周复盘模板

```text
Week:
本周目标：
完成情况：
本周可展示产出：
本周最重要的技术收获：
本周最卡的问题：
是否影响主线：
下周必须完成的 3 件事：
需要降级或删除的任务：
可写进简历的新证据：
```

---

## 13. 最终验收标准

到 2026/09/15 前，至少满足：

- 主项目可以一键启动并跑 benchmark。
- 主项目 README 有架构图、实验步骤、结果表、关键设计取舍。
- 有一篇 inference gateway 或 agent workload serving 技术博客。
- 有两篇系统性能 case study：SHM RPC、Redis vector/cache。
- 有一份 AI infra 定位简历。
- 能 5 分钟讲主项目，10 分钟讲底层系统 case，30 分钟做系统设计延展。

最终人设：

> 有底层系统经验的 LLM inference infra 工程师，能从 RPC、缓存、共享内存、网络 I/O 和 GPU 通信视角优化 agent/RAG 推理服务。
