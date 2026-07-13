# Claude 工作入口

执行仓库任务时遵循 `AGENTS.md`。

如果任务涉及 CXL/SHM、busy-poll、AsyncTransport 性能或 paired `folly` / `fbthrift`
优化，必须在分析或修改前完整读取：

```text
../fbthrift/thrift/perf/cpp2/performance/AGENT_PROTOCOL.md
```

路径不存在时必须报告知识库不可用，不得假设历史实验已经排查完毕。
