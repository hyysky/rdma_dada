# Independent ring-connected pipeline

## Processing model

Every process edge is one PSRDADA HDU. An HDU carries one
header block for a transfer and a sequence of data blocks. The pipeline does
not prepend a copy of the ASCII header to every data block.

For each observation, a stage performs the following transaction:

1. Acquire and parse the input header block.
2. Copy all upstream metadata so unknown observation fields are preserved.
3. Apply stage configuration and algorithm-derived changes to construct the
   output header.
4. Publish the output header before publishing output data.
5. Read one input data block, process it, and commit one or more output blocks.
6. Preserve block sequence/offset metadata and propagate end-of-data.
7. Release both HDUs and return a final stage status.

The output header is therefore a function of both inputs:

```text
output_header = stage(input_header, configuration, algorithm_output_geometry)
output_data   = algorithm(input_data, configuration)
```

## Runtime topology

Ring names A-D in the original diagram are examples, not fixed pipeline roles.
The configured topology consists of ring edges, optional sinks, and worker
processes. Each worker has exactly one input ring and one output ring. Process
boundaries are configurable, but the observation-mode algorithm order is not.

```text
NIC
  -> RDMA2DADA
  -> raw ring
  -> worker process [VDIF unpack]
  -> unpacked ring
       |-> optional dada_dbdisk sink
       `-> worker process [H2D, convert, beamform,
                           power OR stokes, optional time_integrate, D2H]
           -> processed ring
           -> optional DADA2RDMA / dada_dbdisk / downstream worker
```

The topology is configuration-driven. Each process boundary is backpressured
by its input and output rings. Algorithm boundaries do not imply process or ring
boundaries. Beamforming, Power and Stokes remain independent modules even when
they execute in one process and exchange device buffers directly. Power and
Stokes are sibling consumers of beamformed complex voltage, not consecutive
stages. Configuration may split compatible stages into separate workers by
inserting a ring; producing both branches requires separate readers/workers in
the first version.

```text
input ring
   -> StageRunner
      -> module 0
      -> module 1
      -> ...
      -> module N
   -> output ring
```

Configuration selects which optional branch and integration stage are enabled,
while the worker enforces the fixed observation order. During startup, each
module validates the previous module's output header, memory location, datatype,
dimensions and block geometry. An invalid composition fails before the output
header or any output data is published.

## Ring contracts

| Ring | Memory | Producer | Consumer(s) | Data meaning |
| --- | --- | --- | --- | --- |
| A | host | RDMA2DADA | VDIF unpack | 32-byte Project VDIF header plus TFP payload records |
| B | host, CUDA-registered by the GPU worker | VDIF unpack | configured worker and optional `dada_dbdisk` | unpacked/reordered samples without packet headers |
| C | CUDA device | configured worker | configured GPU worker | optional process boundary in GPU memory |
| D | host/pinned host | configured worker | DADA2RDMA or optional storage | example processed product ring |

A ring becomes a fan-out edge only when configuration enables multiple
consumers. Its `dada_db -r` value equals the number of enabled independent
readers. For example, ring B uses `-r 2` only when both a processing worker and
`dada_dbdisk` are enabled; without disk output it normally uses `-r 1`.

Ring C is a future optional process boundary using PSRDADA's CUDA-backed data
blocks (`dada_db -g GPU_ID`). PSRDADA requires persistence mode (`-w`) for a GPU
ring, must be compiled with CUDA, and all producer/consumer processes must
select a compatible CUDA device. Its header block remains host-accessible
PSRDADA metadata; only data blocks live on the CUDA device. The current
`pipeline_worker` connects two host rings and performs H2D/D2H inside the
process. At startup it registers every compute-ring data block with CUDA, so
H2D reads directly from the PSRDADA block without a separate pinned input
staging copy. It does not yet accept a CUDA ring endpoint.

## Header propagation across A-D

Every ring has its own header block. A worker reads the upstream header once per
transfer. It then passes an in-memory metadata object through every configured
module in order. Each module combines the current header with its own parameters
and algorithm-derived output geometry. Only the final header is published to
the worker's output ring, before the first output data block.

```text
ring input header H0
  -> module 0 header transform -> H1
  -> module 1 header transform -> H2
  -> ...
  -> module N header transform -> Hout
  -> ring output header Hout
```

- A header: observation metadata plus raw packet geometry, network source and
  raw ordering.
- B header: preserves observation fields; removes record-header bytes and
  updates `DATA_STAGE`, `ORDER`, `RECORD_BYTES`, `RESOLUTION`, block/file size
  and byte rate for unpacked data.
- Device-ring header, when configured: preserves scientific geometry and adds
  GPU/device placement, matrix/batch layout and process lineage.
- Processed header: preserves observation identity and time reference; updates beam,
  polarization/product, integration, output datatype/order, `RECORD_BYTES`,
  `RESOLUTION`, `TSAMP` and byte-rate fields.

Header update failures abort the transfer before any output data is published.
Data-block failures mark the stage failed and propagate EOD without advertising
a complete output transfer.

## Ownership boundaries

- `pipeline/core`: dependency-free metadata, block, stage, status and state
  machine abstractions; testable on macOS.
- `io/psrdada`: HDU reader/writer, header codec, block leases, EOD and ring
  validation; Linux with PSRDADA.
- `io/rdma`: RoCE ingest and memory registration; Linux with libibverbs.
- `modules`: algorithm implementations. CPU reference implementations remain
  portable; CUDA implementations are enabled only on Linux servers.
- `apps`: thin executable entry points, including a generic worker that composes
  a configured module chain with two ring endpoints.
- `config`: topology, transport geometry and per-stage algorithm parameters.
- `tests`: unit tests, mock-ring tests and Linux integration tests.

## Reference-code decisions

The DSA beamformer demonstrates useful block-level CUDA pipelining and CUDA
event tracking. Its practice of immediately clearing the input header and its
lack of an output ring are not adopted.

The phase-field telescope demonstrates a useful configuration-driven module
graph. Its raw-pointer signal bus and detached-thread ownership model are
replaced with PSRDADA ring ownership, explicit block leases, backpressure and
EOD propagation.

PSRDADA remains the authoritative implementation reference for header/data
block lifecycle and transfer semantics.

## Current GPU worker

The implemented `pipeline_worker` binds its compute and output HDUs from the
Resolved Observation Plan. It accepts `UNPACKED/ATFP/CI8` host-ring input and
supports fixed, compatibility-checked chains:

```text
H2D + ATFP CI8→TFPA CF32 conversion/transpose
  → Beamform
  → Power OR coherency products (AA, BB, AB_REAL, AB_IMAG)
  → optional TimeIntegrate (SUM or MEAN)
  → D2H
```

The output header is published only after input/output metadata, module,
weight, frame, transfer and full-block geometry validation. Integration length
must divide the block time-sample count. A finite input EOD terminates the
transfer without treating the next transfer's `OBS_OFFSET` as another input
header for the completed observation.

`SYNCHRONOUS_DIRECT` retains the single-slot reference execution path.
`STAGED_PIPELINE` supports a configured bounded number of in-flight slots; the
accepted production profiles use three. Each slot owns a non-blocking CUDA
stream, device input/converted/scratch/output buffers, pinned output staging,
CUDA completion events, byte accounting and a block sequence number. The
entire compute ring is registered with CUDA through PSRDADA at startup, so H2D
copies directly from the acquired ring block and input staging-copy bytes are
zero. Completed slots may become ready out of order, but one ordered publisher
commits them to the output ring by block sequence, preserving PSRDADA order and
backpressure semantics.

The Resolved Plan computes every stage and ring data-block geometry before data
flow starts. Actual HDU capacities and strict input metadata are checked
against that plan. Production A=469 Power and coherency-product Full pipelines
have both completed the nominal 30 Gb/s, 60 s, warm-up plus three-measured-run
acceptance campaign. GPU-only saturation and a strict same-build
`SYNCHRONOUS_DIRECT/1` versus `STAGED_PIPELINE/3` performance ablation remain
separate optional characterization work; they are not prerequisites for the
already completed Full acceptance.
