# Agent 工作入口

默认遵循仓库现有开发和测试约定。

当任务涉及 CXL、共享内存、busy-poll、AsyncTransport 性能，或者 paired
`folly` / `fbthrift` worktree 的跨仓库优化时，开始分析、提出优化或修改代码前，
必须完整读取 sibling `fbthrift` worktree 中的：

```text
../fbthrift/thrift/perf/cpp2/performance/AGENT_PROTOCOL.md
```

如果该路径不存在，必须明确报告跨仓库性能知识库不可用；不得声称已经检查过历史实验，
也不得在 `folly` 中创建 Frontier 或 EDR 副本。

命中已有 component、file、mechanism、metric 或 workload 时，必须继续读取协议所引用
的具体 EDR，并在修改前说明与历史实验相比改变了什么条件。
