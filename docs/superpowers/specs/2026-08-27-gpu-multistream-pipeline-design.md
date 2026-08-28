# GPU Multi-Stream Block Pipeline Design

## Status and scope

This design adds a bounded, ordered CUDA block pipeline to `pipeline_worker`
without changing PSRDADA, ring block formats, algorithm data contracts, or the
wire protocol. It retains the current synchronous implementation as a strict
performance reference and adds a staged multi-stream implementation behind the
same worker execution seam.

The implementation covers one compute ring, one output ring, one GPU, and one
ordered output writer. Multi-GPU execution, GPUDirect RDMA and changes to
algorithm kernels are out of scope. The implemented design registers PSRDADA
compute-ring data blocks with CUDA for direct H2D; this is host-memory
registration, not GPUDirect RDMA.

## Goals

- Overlap H2D for block N+1, GPU algorithms for block N, and D2H for block N-1.
- Preserve compute-ring block order exactly in the output ring.
- Never let CUDA access a PSRDADA block after that block is released.
- Keep memory and work bounded by a configured number of in-flight blocks.
- Retain the current single-stream direct implementation for matched A/B tests.
- Preserve partial-block, EOD, strict-header, backpressure, and failure semantics.

## Configuration

Observation JSON gains a `processing.cuda_pipeline` object:

```json
{
  "processing": {
    "cuda_pipeline": {
      "mode": "STAGED_PIPELINE",
      "inflight_blocks": 3
    }
  }
}
```

`mode` has two legal values:

- `SYNCHRONOUS_DIRECT`: the existing path. One non-default stream copies from
  the open compute-ring block, runs the algorithm chain, copies directly into
  the open output-ring block, synchronizes, and then releases both blocks.
- `STAGED_PIPELINE`: the new bounded pipeline using pinned host staging and one
  CUDA stream per slot.

For backward compatibility, an omitted `cuda_pipeline` object resolves to
`SYNCHRONOUS_DIRECT` with one in-flight block. `inflight_blocks` is required for
`STAGED_PIPELINE`, must be between 1 and 4, and must be absent or equal to 1 for
`SYNCHRONOUS_DIRECT`. The resolved plan always records both final values so
configuration identity and result artifacts remain unambiguous.

`output_ring_blocks` remains derived from the existing ring plan in this phase;
multi-stream execution does not introduce a second way to specify ring geometry.

## Execution seam

`pipeline_worker` selects one of two adapters at a new execution seam:

- `SynchronousDirectExecutor` preserves the existing code path.
- `StagedPipelineExecutor` implements bounded asynchronous block execution.

Both adapters accept the same resolved geometry and algorithm chain and expose
the same small interface:

```text
PrepareTransfer(...)
SubmitBlock(sequence, ring_data, input_bytes)
Drain()
Abort(reason)
FinishTransfer()
```

The PSRDADA adapter in `apps/pipeline_worker/main.cpp` owns ring connection,
header publication, transfer lifecycle, and conversion of callback failures to
process exit status. CUDA slot allocation, stream/event management, ordered
completion, and execution metrics stay behind the execution seam.

The output seam exposes acquire, commit, and abort operations for one output
ring block. Direct mode acquires a ring block and performs D2H directly into
that block. Staged mode acquires the block only when its logical sequence is
next, copies from pinned output, then commits. This keeps direct mode free of an
extra host copy while preserving one ordered output writer.

## Staged slot resources

Each staged slot exclusively owns:

1. one non-blocking CUDA stream;
2. one input-ring lease backed by a transfer-lifetime CUDA registration;
3. device input storage;
4. device converted storage;
5. device scratch storage;
6. device output storage;
7. one pinned output staging buffer;
8. H2D, algorithm, D2H, and completion events;
9. logical sequence, actual input/output byte counts, state, and error data.

Beamforming weights and immutable geometry are shared. Beamforming already
keeps a cuBLAS handle per stream; other CUDA modules enqueue work on the stream
provided by the block execution context. Submission remains serialized on the
PSRDADA reader thread, so module configuration state is not mutated concurrently.

Pinned output buffers are private worker allocations, not PSRDADA shared
memory. The compute PSRDADA ring itself is registered once per transfer with
`dada_cuda_dbregister` and unregistered after all CUDA work is synchronized.
This supersedes the original pinned-input staging design and removes the
ring-to-staging CPU copy.

## Input-ring compatibility

PSRDADA may recycle a direct-read block after its lease is released. Therefore
the staged executor must retain each compute-ring block lease until that slot's
H2D event proves CUDA no longer references the ring pointer.

For each input block the staged adapter:

1. waits until a bounded slot is free;
2. enqueues H2D directly from the CUDA-registered compute-ring block;
3. records an H2D completion event before conversion, algorithms, D2H and the
   final completion event on the same slot stream;
4. releases the input lease only after H2D completion;
5. returns the consumed byte count to PSRDADA through the lease-aware adapter.

After the H2D event completes, no later CUDA operation references the ring
block, so PSRDADA may safely recycle it while conversion/algorithms/D2H continue
in that slot. If every slot is busy, the reader waits for a slot release; this
propagates bounded backpressure instead of allocating more memory or dropping
data.

## Ordered output writer

The staged D2H target is the slot's pinned output buffer. A single writer is the
only thread that calls output-ring `ipcio_open_block_write` and
`ipcio_close_block_write`.

Each submitted block receives a monotonically increasing logical sequence that
is independent of the wrapping PSRDADA physical block ID. CUDA streams may
complete out of order, but the writer waits for `next_publish_sequence` and
publishes only that slot. Later completed slots remain ready but unpublished.
After a successful write, the writer advances the sequence and releases the slot.

This deliberately permits head-of-line blocking to preserve the data contract.
No configuration may select completion-order publication.

## Partial blocks, EOD, and backpressure

Each slot records actual input bytes and derives actual output bytes with the
existing geometry planner. The staging buffers retain full configured capacity,
but the output writer commits only the derived actual output size.

At EOD the reader stops accepting new blocks, then `Drain()` waits for all
submitted slots and publishes them in sequence. Only after the last slot is
published may the worker finish modules, close the output transfer, and emit EOD.

If the output ring is full, the writer blocks in the normal PSRDADA write call.
Ready slots accumulate up to `inflight_blocks`; once all are occupied, input
submission blocks and compute-ring backpressure propagates upstream. Ring size
continues to provide burst tolerance only and does not hide insufficient steady
state service rate.

## Failure semantics

Synchronous launch errors are captured during submission. Asynchronous CUDA and
cuBLAS failures are captured when the ordered writer synchronizes the completion
event. A failure records the slot and logical sequence, stops new submission,
and prevents publication of that block and every later sequence. The worker then
synchronizes or cancels all owned streams, closes any open output block with zero
bytes, finishes or releases module resources, and returns a non-zero transfer
result.

Cleanup errors never overwrite the original failure classification. EOD is not
reported as successful after a failed block.

## Resource budget

The compiler reports separate direct and staged budgets. For staged execution:

- per-slot device bytes include input, converted, scratch, and output buffers;
- total slot device bytes are multiplied by `inflight_blocks`;
- weights are counted once;
- pinned host bytes are `(input_block_bytes + output_block_bytes) * slots`;
- the existing safety reserve is applied to the recommended free-device budget;
- CUDA runtime and library workspace remain explicitly excluded.

For the current Power+Integration geometry, three slots require approximately
1.73 GB of device buffers and 2,457,600 bytes of pinned output memory before
CUDA runtime and library workspace. Compute-ring bytes are registered host
memory and are reported separately from worker-owned pinned staging.

## Metrics

Machine-readable metrics add:

- execution mode and configured slot count;
- submitted, completed, and published blocks;
- maximum simultaneously occupied slots;
- completion reorder count;
- slot-wait and writer-wait totals and maxima;
- per-stage H2D, algorithm, and D2H event timings;
- submit-to-publish service timing;
- compute-ring registration blocks/bytes/time, zero input-staging bytes, and
  output staging/publish accounting;
- planned and actual pinned/device memory.

Metrics collection must avoid per-poll clocks. CUDA events supply device-stage
timing, and host clocks are sampled only at block lifecycle transitions.

## Test strategy

Portable tests cover schema parsing, resolved-plan serialization, mode rules,
budget arithmetic, slot-state transitions, strict publication order, bounded
backpressure, partial blocks, EOD drain, and error propagation.

CUDA tests compare both adapters against the existing numerical oracle for all
supported product chains. A deterministic delay fixture makes an earlier slot
finish after a later slot and verifies that output sequences remain ordered.

PSRDADA integration tests exercise compute ring to output ring with mode
`SYNCHRONOUS_DIRECT` and staged slot counts 1, 2, 3, and 4. Server performance
testing first preserves the current binary/configuration geometry and compares:

1. all-NUMA1 synchronous direct;
2. split-NUMA synchronous direct;
3. split-NUMA staged pipeline with 1, 2, 3, and 4 slots.

Functional acceptance requires three consecutive clean repetitions. Performance
acceptance requires a warm-up and three measured repetitions, exact block and
EOD accounting, clean resource teardown, and sustained service time below the
configured deadline with the required headroom.

## Files in scope

Primary implementation files:

- `include/rdma_dada/pipeline/gpu_block_pipeline.h` (new)
- `src/pipeline/gpu_block_pipeline.cpp` (new)
- `apps/pipeline_worker/main.cpp`
- Observation, resolved-plan, worker-config, and GPU-budget headers/sources
- `CMakeLists.txt`

Tests, controller/profile/catalog schemas, and relevant module/testing/status
documentation are updated with the new mode, slot count, metrics, and acceptance
contract. PSRDADA sources, unpack implementation, packet formats, CUDA algorithm
kernels, ring block formats, and weight files are not modified in this phase.
