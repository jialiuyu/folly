# SHM Reply Head-of-Line Blocking Mitigation Plan

## Context

The current shared-memory transport path reuses fbthrift/Rocket's normal
`AsyncTransport` response model. This keeps socket and SHM behavior compatible,
but it also means ThreadManager-dispatched RPC replies must return to the IO
EventBase before the response can be written to SHM.

For very small benchmark RPCs such as `download()`, this creates avoidable
head-of-line blocking:

```text
SHM poller
  -> IO EventBase request delivery and Rocket dispatch
  -> ThreadManager worker handler
  -> HandlerCallback reply completion
  -> ReplyQueue
  -> IO EventBase drains ReplyQueue
  -> Rocket send path
  -> BusyPollSharedMemoryTransport::writeChain()
  -> ShmPollerService::writeData()
```

The SHM data plane itself does not require fd readiness or epoll-style write
ownership. The bottleneck exists because the current Rocket connection and
response-channel control plane are IO EventBase-affine.

## Current Evidence

The performance benchmark service currently has two different execution models:

- `sum`, `noop`, and `onewayNoop` are annotated with
  `@cpp.ProcessInEbThreadUnsafe`.
- `download`, `upload`, and `streamDownload` are not annotated and therefore use
  ThreadManager dispatch.

Generated code reflects this distinction:

- EventBase methods execute inline through `async_eb_*`.
- ThreadManager methods execute through `processInThread()` and `async_tm_*`.
- A worker-thread reply that is not already running on the IO EventBase is
  queued through `HandlerCallbackBase::putMessageInReplyQueue()`.

In shared SHM mode, received bytes are delivered by `ShmPollerService` via
`EventBase::runInEventBaseThread()`, and response writes eventually reach
`BusyPollSharedMemoryTransport::writeInternal()`, which delegates to
`ShmPollerService::writeData()`.

Therefore, a ThreadManager-dispatched SHM RPC still depends on the IO EventBase
for the final response write.

## Problem Statement

For small RPCs, ThreadManager dispatch and ReplyQueue handoff dominate the
actual handler work. With one IO thread and one SHM lane, the same IO EventBase
is responsible for:

- inbound SHM delivery callbacks,
- Rocket frame parsing and dispatch,
- ReplyQueue draining,
- Rocket response framing and write batching,
- outbound SHM writes.

If the IO EventBase is busy, worker replies accumulate behind the ReplyQueue
drain point. This explains low steady-state QPS and very long tail latency for
`download()` compared with EventBase-only RPCs such as `sum`.

## Goals

1. Restore benchmark `download()` performance for the trivial in-memory response
   case.
2. Preserve socket transport behavior.
3. Preserve legacy SHM behavior unless explicitly opted in.
4. Provide a path toward a general worker-thread SHM reply optimization without
   breaking Rocket ordering, lifetime, or backpressure semantics.

## Non-Goals

- Do not change public folly APIs for the quick fix.
- Do not make all RPC handlers run on the IO EventBase.
- Do not bypass Rocket framing or response metadata.
- Do not support socket worker-direct writes; this is SHM-specific.

## Option A: Execute Trivial SHM Benchmark RPCs on the IO EventBase

### Summary

Annotate `download()` with `@cpp.ProcessInEbThreadUnsafe` and implement the
corresponding `async_eb_download()` handler. This moves the trivial response
handler onto the IO EventBase, matching the current `sum()` execution model.

### Expected Path

```text
SHM poller
  -> IO EventBase request delivery and Rocket dispatch
  -> async_eb_download()
  -> HandlerCallback::result()
  -> Rocket send path
  -> BusyPollSharedMemoryTransport::writeChain()
  -> ShmPollerService::writeData()
```

This removes the worker hop and ReplyQueue handoff.

### Implementation Shape

In `fbthrift/thrift/perf/cpp2/if/StreamApi.thrift`:

```thrift
@cpp.ProcessInEbThreadUnsafe
ApiBase.Chunk2 download();
```

In the benchmark handler, add an EventBase handler implementation:

```cpp
void async_eb_download(
    apache::thrift::HandlerCallbackPtr<std::unique_ptr<Chunk2>> callback)
    override {
  stats_->add(kUpload_);
  callback->result(std::make_unique<Chunk2>(chunk_));
}
```

The exact return object construction should follow the generated signature and
local code style after regeneration.

### Benefits

- Minimal change.
- Directly validates the root-cause hypothesis.
- Removes ThreadManager scheduling and ReplyQueue handoff for `download()`.
- Keeps socket and general worker-dispatched RPC behavior unchanged.

### Risks

- `ProcessInEbThreadUnsafe` disables queue timeout and some overload protection.
- The handler must remain fast, non-blocking, and lock-light.
- This is not a general solution for real worker-thread RPCs.

### Acceptance Criteria

- Generated code routes `download()` through `async_eb_download()`.
- `download()` no longer calls `processInThread()` in generated code.
- `download()` QPS approaches the same order of magnitude as `sum()`.
- P99/P99.9 no longer show second-level tail latency under the benchmark setup.

## Option B: Add a SHM Worker-Direct Reply Fast Path

### Summary

Add a SHM-specific reply path that allows worker-thread completions to submit
serialized Rocket response frames to SHM without first waiting for the IO
EventBase to drain ReplyQueue.

This is the general fix, but it touches stricter invariants than Option A.

### Key Invariants

Any direct worker reply path must preserve:

- per-connection byte ordering,
- Rocket stream response ordering,
- write callback and error semantics,
- connection close and unregister lifetime safety,
- backpressure behavior when the SHM ring is full,
- compatibility with non-SHM transports.

### Proposed Architecture

Introduce a SHM-only reply writer owned by the shared-mode Rocket connection:

```text
worker thread
  -> serialize response payload
  -> Rocket frame bytes
  -> ShmDirectReplyWriter
  -> ShmPollerService::writeData()
```

The writer should be exposed only when the underlying transport is shared-mode
`BusyPollSharedMemoryTransport`.

A transport capability check is preferable to special-casing generic transports:

```cpp
bool supportsThreadSafeShmDirectReply() const;
```

The default implementation is false. Shared-mode SHM can opt in once ordering
and lifetime rules are satisfied.

### Phase B1: Experimental Direct Writer

Use a per-connection mutex to protect complete response-frame writes:

```text
worker
  -> build full Rocket frame IOBuf chain
  -> lock connection SHM write mutex
  -> write all chunks through ShmPollerService::writeData()
  -> unlock
```

Properties:

- Easy to reason about.
- Prevents frame interleaving between workers.
- Keeps direct-write scope narrow.

Limitations:

- A worker may block or spin under SHM flow control.
- Mutex contention can reduce gains for many simultaneous responses.
- Error handling and close races must be carefully guarded.

### Phase B2: Per-Connection SHM Reply Queue

Replace worker-side direct spinning with a per-connection MPSC queue and a
non-IO writer/drainer:

```text
worker
  -> build full Rocket frame IOBuf chain
  -> enqueue into connection SHM reply queue

SHM reply drainer
  -> dequeue in order
  -> ShmPollerService::writeData()
```

Properties:

- Preserves response order without holding worker threads in SHM flow control.
- Makes backpressure explicit.
- Avoids depending on the IO EventBase for reply draining.

Limitations:

- More infrastructure.
- Needs shutdown coordination with connection close.
- Needs metrics and bounded queue policy.

### Fallback Rules

The direct path should fall back to the existing ReplyQueue path when:

- the transport is not shared-mode SHM,
- the response carries FDs or socket-only features,
- the connection is closing,
- direct writer backpressure exceeds a bounded threshold,
- ordering state is uncertain,
- any feature flag disables the optimization.

### Required Metrics

Add counters for:

- direct reply attempts,
- direct reply successes,
- direct reply fallbacks,
- direct reply write failures,
- SHM write flow-control yields,
- direct reply queue depth,
- ReplyQueue enqueue/drain counts,
- IO EventBase notification queue size if available.

## Recommended Rollout

### Step 1: Implement Option A

Use the EventBase-handler approach for `download()` only. This is the fastest
way to validate the diagnosis and recover benchmark performance for trivial
payload responses.

### Step 2: Add Diagnostics

Before implementing Option B, add enough observability to prove where time is
spent:

- count worker replies that enter ReplyQueue,
- measure ReplyQueue wait time before IO EventBase drain,
- measure SHM write latency and flow-control yields,
- measure poller-to-EventBase dispatch latency.

### Step 3: Prototype Option B Behind a Feature Flag

Implement worker-direct SHM reply for simple request-response payloads only.
Keep socket, legacy SHM, streaming, sink, bidi, FD-carrying responses, and
complex error paths on the existing response path.

### Step 4: Expand Only After Invariant Tests Pass

Do not generalize the direct path until it passes ordering, close-race,
backpressure, multi-client, and mixed-RPC tests.

## Test Plan

### Static Verification

- Confirm generated `download()` code uses `async_eb_download()`.
- Confirm generated `download()` setup no longer invokes `processInThread()`.
- Confirm `sum()` and `download()` have matching executor metadata.

### Benchmark Verification

Run the same benchmark configuration before and after Option A:

```text
--transport=shm
--download_weight=1
--chunk_size=1024
--max_outstanding_ops=100
--io_threads=1
--shm_lanes=1
```

Expected:

- large QPS increase versus current worker-dispatched `download()`,
- no alternating server-side 0/100 QPS pattern,
- P99 and P99.9 no longer at second-level latency.

### Regression Verification

- Socket transport benchmark still works.
- Shared SHM benchmark still works for `sum`, `noop`, `upload`, and
  `streamDownload`.
- Legacy SHM fallback path is unchanged.
- Multi-client tests preserve response correctness.

### Option B Specific Tests

- Multiple workers replying on the same connection preserve frame order.
- Connection close during pending direct replies does not use freed state.
- SHM ring full condition does not spin forever on CPU workers.
- Direct path falls back correctly when disabled or unsupported.

## Open Questions

1. Should direct SHM replies be implemented below Rocket as an
   `AsyncTransport` capability, or above Rocket as a Rocket connection feature?
2. Should Phase B1 allow worker threads to block on SHM flow control, or should
   it immediately enqueue to a drainer?
3. What is the acceptable fallback threshold before returning to the existing
   IO EventBase ReplyQueue path?
4. Which Rocket response variants are safe for the first direct-path prototype:
   simple request-response only, or also exceptions?

## Recommendation

Implement Option A first for the benchmark `download()` RPC. It is the smallest
change and directly addresses the current performance cliff.

Treat Option B as a separate transport/Rocket design effort. SHM can support a
worker-direct data path, but the implementation must explicitly preserve
Rocket's connection ordering, lifetime, and backpressure semantics.
