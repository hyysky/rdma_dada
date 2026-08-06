# VDIF Unpack and Sender Simulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a functional Project VDIF v1 UDP simulator and a streaming raw-ring-to-TFPA compute-ring unpack path with bounded cross-block reordering, zero-fill loss handling, statistics and header propagation.

**Architecture:** Portable C++ libraries own the fixed 32-byte codec, strict JSON configuration, header transformation, bounded reassembly arena and non-owning compute-block writer. Thin POSIX and PSRDADA applications compose those libraries; Linux-only RDMA changes widen flow steering to destination-only matching and make wrong-length frames recoverable.

**Tech Stack:** C++11, CMake/CTest, POSIX UDP sockets, PSRDADA, libibverbs; no CUDA in this feature.

## Global Constraints

- Project VDIF v1 is exactly eight little-endian 32-bit words followed by CI8 or little-endian CI16 TFP payload.
- One unpack worker handles one fixed `first_channel_id`, `NCHAN`, `NPOL`, `NSAMP` and sample format per transfer.
- `antenna_map` contains exactly `NANT` unique numeric Station IDs and defines the output A order.
- `window_blocks` is at least 2; default 2 supports reordering across one raw-ring block boundary.
- Window storage is payload-only and equals `window_blocks * records_per_raw_block * packet_payload_bytes`.
- Missing Station slices are zero-filled when an observed group leaves the window; unknown, duplicate, invalid and late packets are counted and discarded without stopping the transfer.
- The compute ring is TFPA CI8/CI16 with no packet headers, and each committed byte count is divisible by one group byte count.
- Destination MAC, destination IPv4 and destination UDP port are matched; all source address fields are wildcarded.
- The simulator is functional, not line-rate, and rejects fragmented or path-MTU-oversized datagrams.
- Portable tests must run on macOS without PSRDADA, libibverbs or CUDA.
- Existing unrelated dirty-worktree files must not be staged or rewritten.

---

### Task 1: Fixed Project VDIF binary codec

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/project_vdif_v1.h`
- Create: `modules/vdif_unpack/project_vdif_v1.cpp`
- Create: `tests/project_vdif_v1_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: the fixed field contract in `doc/PROJECT_VDIF_PROFILE_V1.md`.
- Produces:

```cpp
struct ProjectVdifHeader {
    bool invalid_data;
    std::uint32_t seconds_from_reference_epoch;
    std::uint8_t reference_epoch;
    std::uint32_t frame_number_within_second;
    std::uint16_t station_id;
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;
    std::uint32_t nsamp_per_packet;
    std::uint8_t component_bits;
    std::uint32_t frame_length_units_8_bytes;
};

struct ProjectVdifGeometry {
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;
    std::uint32_t nsamp_per_packet;
    std::uint8_t component_bits;
    std::uint64_t payload_bytes;
};

bool DecodeProjectVdifV1(const std::uint8_t* bytes, std::uint64_t size,
                         ProjectVdifHeader* header, std::string* error);
bool EncodeProjectVdifV1(const ProjectVdifHeader& header,
                         std::uint8_t* bytes, std::uint64_t size,
                         std::string* error);
bool ValidateProjectVdifV1(const ProjectVdifHeader& header,
                           const ProjectVdifGeometry& geometry,
                           std::uint64_t actual_record_bytes,
                           std::string* error);
```

- [ ] **Step 1: Write the failing golden-vector test**

Construct eight known little-endian words, decode them, assert every public
field, encode the struct again and require byte-for-byte equality. Add separate
CI8 and CI16 geometry cases plus failures for short header, reserved bits,
wrong project sentinel, wrong EDV/version/encoding, wrong frame length and
overflowing geometry.

- [ ] **Step 2: Register the test and verify RED**

Run:

```bash
cmake -S . -B build -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build --target project_vdif_v1_test --parallel
```

Expected: build failure because `project_vdif_v1.h` and its functions do not
exist.

- [ ] **Step 3: Implement the minimal codec**

Use explicit little-endian word loads/stores and masks. Do not use packed C
struct overlays. Validate all fixed Project VDIF constants and checked formulas:

```text
complex_sample_bytes = 2 * component_bits / 8
payload_bytes = nsamp * nchan * npol * complex_sample_bytes
record_bytes = 32 + payload_bytes
record_bytes = frame_length_units_8_bytes * 8
```

- [ ] **Step 4: Verify GREEN and portable regression**

Run:

```bash
cmake --build build --target project_vdif_v1_test --parallel
ctest --test-dir build -R '^project_vdif_v1_test$' --output-on-failure
ctest --test-dir build --output-on-failure
```

Expected: codec test and all existing portable tests pass.

- [ ] **Step 5: Commit Task 1**

```bash
git add CMakeLists.txt tests/README.md tests/project_vdif_v1_test.cpp \
  include/rdma_dada/modules/vdif_unpack/project_vdif_v1.h \
  modules/vdif_unpack/project_vdif_v1.cpp
git commit -m "feat: add Project VDIF binary codec"
```

### Task 2: Strict unpack configuration and header transformation

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_config.h`
- Create: `modules/vdif_unpack/vdif_unpack_config.cpp`
- Create: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_header.h`
- Create: `modules/vdif_unpack/vdif_unpack_header.cpp`
- Create: `config/vdif_unpack.example.json`
- Create: `tests/vdif_unpack_config_test.cpp`
- Create: `tests/vdif_unpack_header_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `config/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: `PipelineConfig`, `PipelineLayout`, `PacketFormatConfig` and
  `pipeline::Metadata`.
- Produces:

```cpp
struct VdifUnpackConfig {
    std::uint32_t input_key;
    std::uint32_t output_key;
    std::string pipeline_config_path;
    std::string packet_format_path;
    std::uint16_t first_channel_id;
    std::vector<std::uint16_t> antenna_map;
    std::uint32_t window_blocks;
    std::uint64_t max_window_bytes;
    std::string output_memory;
};

struct VdifUnpackLayout {
    std::uint64_t raw_record_bytes;
    std::uint64_t records_per_raw_block;
    std::uint64_t group_bytes;
    std::uint64_t window_capacity_groups;
    std::uint64_t window_bytes;
    std::uint64_t compute_block_bytes;
};

bool LoadVdifUnpackConfig(const std::string& path,
                          VdifUnpackConfig* config, std::string* error);
bool ComputeVdifUnpackLayout(const VdifUnpackConfig& config,
                             const PipelineConfig& pipeline,
                             const PacketFormatConfig& packet,
                             VdifUnpackLayout* layout,
                             std::string* error);
bool BuildVdifUnpackOutputHeader(const pipeline::Metadata& input,
                                 const VdifUnpackConfig& config,
                                 const PipelineConfig& pipeline,
                                 const PipelineLayout& pipeline_layout,
                                 const VdifUnpackLayout& unpack_layout,
                                 pipeline::Metadata* output,
                                 std::string* error);
```

- [ ] **Step 1: Write failing strict-config tests**

Load `config/vdif_unpack.example.json` and assert hexadecimal ring keys,
`first_channel_id`, ordered Station IDs, `window_blocks=2`, the payload-only
window formula and compute-block divisibility. Mutate one case at a time to
reject duplicate Station IDs, wrong map length, `window_blocks<2`, unknown JSON
keys, allocation above `max_window_bytes`, packet/profile mismatch and overflow.

- [ ] **Step 2: Write failing header-transform tests**

Create a RAW metadata object containing required geometry and an unknown
`TELESCOPE` field. Require preservation of `TELESCOPE`, UTC and packet
provenance and exact updates to `DATA_STAGE=UNPACKED`, `ORDER=TFPA`,
`RECORD_HEADER_BYTES=0`, `RECORD_BYTES`, `RESOLUTION`, `MEMORY`,
`SAMPLE_FORMAT`, `FILE_SIZE`, `TRANSFER_SIZE=0`, loss policy and window blocks.
Require geometry conflicts to fail before output metadata is published.

- [ ] **Step 3: Verify RED**

Run:

```bash
cmake --build build --target vdif_unpack_config_test vdif_unpack_header_test --parallel
```

Expected: build failure because the config and header APIs do not exist.

- [ ] **Step 4: Implement strict parsing, checked layout and header transform**

Use the existing JSON parser and reject unknown fields. Resolve referenced
pipeline/profile paths relative to the unpack JSON file. Copy input metadata
before applying output fields so unknown observation values survive. Reject
CI8/CI16, F/P/A, record-size and block-size conflicts.

- [ ] **Step 5: Verify GREEN and regressions**

```bash
cmake --build build --target vdif_unpack_config_test vdif_unpack_header_test --parallel
ctest --test-dir build -R '^vdif_unpack_(config|header)_test$' --output-on-failure
ctest --test-dir build --output-on-failure
```

- [ ] **Step 6: Commit Task 2**

```bash
git add CMakeLists.txt config/README.md config/vdif_unpack.example.json \
  tests/README.md tests/vdif_unpack_config_test.cpp \
  tests/vdif_unpack_header_test.cpp include/rdma_dada/modules/vdif_unpack \
  modules/vdif_unpack/vdif_unpack_config.cpp \
  modules/vdif_unpack/vdif_unpack_header.cpp
git commit -m "feat: define VDIF unpack configuration and header transform"
```

### Task 3: Bounded cross-block reassembly engine

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h`
- Create: `modules/vdif_unpack/vdif_unpack_engine.cpp`
- Create: `tests/vdif_unpack_engine_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `modules/vdif_unpack/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: Task 1 codec and Task 2 config/layout.
- Produces:

```cpp
struct VdifGroupKey {
    std::uint8_t reference_epoch;
    std::uint32_t seconds_from_reference_epoch;
    std::uint32_t frame_number_within_second;
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;
    bool operator<(const VdifGroupKey& other) const;
};

struct VdifUnpackStatistics {
    std::uint64_t received_records;
    std::uint64_t accepted_packets;
    std::uint64_t invalid_header_packets;
    std::uint64_t invalid_data_packets;
    std::uint64_t unknown_station_packets;
    std::uint64_t duplicate_packets;
    std::uint64_t late_packets;
    std::uint64_t window_evictions;
    std::uint64_t completed_groups;
    std::uint64_t incomplete_groups;
    std::uint64_t missing_station_packets;
    std::uint64_t expected_station_packets_for_observed_groups;
};

typedef std::function<bool(const VdifGroupKey&, const std::uint8_t*,
                           std::uint64_t, std::string*)> VdifGroupEmitter;

class VdifUnpackEngine {
public:
    bool Configure(const VdifUnpackConfig& config,
                   const PipelineConfig& pipeline,
                   const VdifUnpackLayout& layout,
                   std::string* error);
    bool ConsumeRawBlock(const std::uint8_t* data, std::uint64_t size,
                         std::uint64_t raw_block_sequence,
                         const VdifGroupEmitter& emit,
                         std::string* error);
    bool Finish(const VdifGroupEmitter& emit, std::string* error);
    const VdifUnpackStatistics& statistics() const;
};
```

- [ ] **Step 1: Write the failing in-order and arbitrary-Station-order tests**

Generate independent CI8 records for two groups and four Station IDs. Feed
Station order `103,101,104,102` and verify emitted bytes use exact TFPA order
defined by `antenna_map=[101,102,103,104]`.

- [ ] **Step 2: Verify RED**

```bash
cmake --build build --target vdif_unpack_engine_test --parallel
```

Expected: missing engine API.

- [ ] **Step 3: Implement fixed arena, key map and scatter**

Allocate one payload arena in `Configure`; use slot indices and a free list so
packet ingestion performs no group-payload allocation. Zero a slot before use,
map Station ID to A once, and scatter complete complex samples into TFPA.

- [ ] **Step 4: Verify the basic test passes**

```bash
ctest --test-dir build -R '^vdif_unpack_engine_test$' --output-on-failure
```

- [ ] **Step 5: Add failing cross-block and loss-policy tests**

Cover a group split across raw blocks N/N+1, one missing Station zero-filled
after the horizon, EOD flush, duplicate/unknown/invalid/late packets, an old new
key arriving when the arena is full, oldest-slot eviction, partial raw blocks
containing whole records and rejection of a trailing partial record.

- [ ] **Step 6: Implement window advancement and counters**

After each block, emit only the stable ordered prefix using
`current-first_seen >= window_blocks-1`. If full, reject a newly observed key
older than the active oldest as late; otherwise finalize the active oldest with
zero fill and reuse its slot. Update expected/missing counters only when an
observed group is emitted.

- [ ] **Step 7: Verify GREEN and regressions**

```bash
cmake --build build --target vdif_unpack_engine_test --parallel
ctest --test-dir build -R '^vdif_unpack_engine_test$' --output-on-failure
ctest --test-dir build --output-on-failure
```

- [ ] **Step 8: Commit Task 3**

```bash
git add CMakeLists.txt modules/vdif_unpack/README.md tests/README.md \
  tests/vdif_unpack_engine_test.cpp \
  include/rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h \
  modules/vdif_unpack/vdif_unpack_engine.cpp
git commit -m "feat: add bounded VDIF reassembly engine"
```

### Task 4: Non-owning compute block writer

**Files:**
- Create: `include/rdma_dada/pipeline/group_block_writer.h`
- Create: `src/pipeline/group_block_writer.cpp`
- Create: `tests/group_block_writer_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: finalized group bytes from `VdifUnpackEngine`.
- Produces:

```cpp
class WritableBlockSink {
public:
    virtual ~WritableBlockSink() {}
    virtual bool Acquire(std::uint8_t** data, std::uint64_t* capacity,
                         std::string* error) = 0;
    virtual bool Commit(std::uint64_t bytes, std::string* error) = 0;
};

class GroupBlockWriter {
public:
    bool Configure(std::uint64_t group_bytes,
                   std::uint64_t block_capacity,
                   WritableBlockSink* sink,
                   std::string* error);
    bool Append(const std::uint8_t* group, std::uint64_t bytes,
                std::string* error);
    bool Finish(std::string* error);
};
```

- [ ] **Step 1: Write the failing mock-sink test**

Use four-byte groups and a twelve-byte block. Append five groups and assert one
full twelve-byte commit followed by one eight-byte EOD commit, exact order, no
extra empty commit and rejection of wrong group size/capacity.

- [ ] **Step 2: Verify RED**

```bash
cmake --build build --target group_block_writer_test --parallel
```

- [ ] **Step 3: Implement the non-owning writer**

Acquire a block lazily, copy each group directly to the current write offset,
commit exactly at capacity, and commit a non-empty partial block in `Finish`.

- [ ] **Step 4: Verify GREEN and regressions**

```bash
ctest --test-dir build -R '^group_block_writer_test$' --output-on-failure
ctest --test-dir build --output-on-failure
```

- [ ] **Step 5: Commit Task 4**

```bash
git add CMakeLists.txt tests/README.md tests/group_block_writer_test.cpp \
  include/rdma_dada/pipeline/group_block_writer.h \
  src/pipeline/group_block_writer.cpp
git commit -m "feat: add compute group block writer"
```

### Task 5: Deterministic UDP FPGA simulator

**Files:**
- Create: `include/rdma_dada/simulation/vdif_sender_sim.h`
- Create: `src/simulation/vdif_sender_sim.cpp`
- Create: `apps/fpga_sender_sim/main.cpp`
- Create: `apps/fpga_sender_sim/README.md`
- Create: `config/fpga_sender_sim.example.json`
- Create: `tests/vdif_sender_sim_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `apps/README.md`
- Modify: `config/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: Task 1 encoder and existing JSON parser.
- Produces:

```cpp
struct VdifSenderSimConfig {
    std::string destination_ip;
    std::uint16_t destination_port;
    std::uint32_t path_mtu;
    std::uint16_t station_id;
    ProjectVdifGeometry geometry;
    std::uint8_t reference_epoch;
    std::uint32_t start_seconds;
    std::uint64_t sample_interval_ps;
    std::uint64_t group_count;
    std::string mode;
    std::string start_utc;
    std::vector<std::uint64_t> drop_groups;
    std::vector<std::uint64_t> duplicate_groups;
    std::vector<std::uint64_t> invalid_header_groups;
};

bool LoadVdifSenderSimConfig(const std::string& path,
                             VdifSenderSimConfig* config,
                             std::string* error);
bool BuildVdifSenderRecord(const VdifSenderSimConfig& config,
                           std::uint64_t group_index,
                           std::vector<std::uint8_t>* record,
                           std::string* error);
```

- [ ] **Step 1: Write failing deterministic-record tests**

Require two Station configurations with the same group index to produce equal
time keys but different Station IDs and deterministic CI8/CI16 payload values.
Verify integer-picosecond second rollover, frame reset, exact record length,
path-MTU rejection, duplicate list validation and invalid-header injection.

- [ ] **Step 2: Verify RED**

```bash
cmake --build build --target vdif_sender_sim_test --parallel
```

- [ ] **Step 3: Implement strict config, time accumulator and record builder**

Parse the sample interval text into integer picoseconds without binary floating
rounding. Derive second and per-second frame ordinal deterministically from the
zero-based group index. Generate expected samples from a documented bounded
function of Station, group, t, f and p.

- [ ] **Step 4: Implement the UDP application**

Use a connected UDP socket, enable do-not-fragment where the platform exposes
it, wait for the common future UTC in realtime mode, apply configured fault
lists and require each `send` to transmit the complete record.

- [ ] **Step 5: Verify GREEN and loopback behavior**

```bash
cmake --build build --target vdif_sender_sim_test fpga_sender_sim --parallel
ctest --test-dir build -R '^vdif_sender_sim_test$' --output-on-failure
ctest --test-dir build --output-on-failure
```

- [ ] **Step 6: Commit Task 5**

```bash
git add CMakeLists.txt apps/README.md apps/fpga_sender_sim \
  config/README.md config/fpga_sender_sim.example.json tests/README.md \
  tests/vdif_sender_sim_test.cpp include/rdma_dada/simulation/vdif_sender_sim.h \
  src/simulation/vdif_sender_sim.cpp
git commit -m "feat: add functional VDIF UDP sender simulator"
```

### Task 6: PSRDADA VDIF unpack worker

**Files:**
- Create: `apps/vdif_unpack_worker/main.cpp`
- Create: `apps/vdif_unpack_worker/README.md`
- Create: `tests/vdif_unpack_worker_integration.sh`
- Modify: `CMakeLists.txt`
- Modify: `apps/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: Tasks 2-4 and PSRDADA HDUs selected by `input_key`/`output_key`.
- Produces: executable `vdif_unpack_worker CONFIG`.

- [ ] **Step 1: Add a failing Linux integration test harness**

The script creates raw and compute rings with exact configured block sizes,
writes a finite synthetic raw transfer containing a cross-block group, runs the
worker, reads the compute ring, and compares header plus TFPA bytes against a
separately generated expected file. It skips with CTest code 77 when PSRDADA
tools are unavailable.

- [ ] **Step 2: Verify RED on Linux configuration**

```bash
cmake -S . -B build-linux -DBUILD_RDMA_PIPELINE=ON -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build-linux --target vdif_unpack_worker --parallel
```

Expected: missing target.

- [ ] **Step 3: Implement the two-HDU worker**

Follow the existing `pipeline_worker` transfer lifecycle: connect/lock output,
read and parse the raw ASCII header, publish transformed output header, feed
each actual input block byte count into `VdifUnpackEngine`, append emitted
groups through a PSRDADA `WritableBlockSink`, flush at EOD, emit statistics,
propagate EOD, unlock and disconnect in reverse order.

- [ ] **Step 4: Verify full and partial output blocks**

Run the integration script once with a full final compute block and once with a
partial final block. Require exact header fields, byte counts, ordering and EOD.

- [ ] **Step 5: Commit Task 6**

```bash
git add CMakeLists.txt apps/README.md apps/vdif_unpack_worker \
  tests/README.md tests/vdif_unpack_worker_integration.sh
git commit -m "feat: add PSRDADA VDIF unpack worker"
```

### Task 7: Destination-only RDMA receive and recoverable wrong lengths

**Files:**
- Modify: `src/io/rdma/verbs_context.cpp`
- Modify: `src/io/rdma/receiver.cpp`
- Modify: `include/rdma_dada/io/rdma/receiver.h`
- Modify: `apps/rdma2dada/main.cpp`
- Create: `tests/rdma_receiver_integration.sh`
- Modify: `CMakeLists.txt`
- Modify: `apps/rdma2dada/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: existing RAW_PACKET QP and receive callbacks.
- Produces: destination-only steering and a receive loop that drops/reposts
  wrong-length successful receive completions without exiting.

- [ ] **Step 1: Write the failing Linux integration cases**

Send correct-length UDP frames from two source IP/port pairs to the same
destination and require both in the raw ring. Send one wrong-length frame
between valid frames and require it absent while later valid frames still
arrive.

- [ ] **Step 2: Verify current behavior fails**

Run the integration script and capture either the second source being filtered
or the receive thread exiting after the wrong-length completion.

- [ ] **Step 3: Wildcard all source flow fields**

Keep exact masks and values only for destination MAC, destination IPv4 and
destination UDP port. Set all source masks to zero and stop requiring source
CLI parameters in `rdma2dada`.

- [ ] **Step 4: Make wrong length recoverable**

Separate fatal completion validation from length disposition. For a successful
receive with valid opcode/WR ID but wrong `byte_len`, increment the 64-bit
counter, rate-limit its log, immediately repost that WR and continue. Do not add
it to the valid completion batch and do not write it to the raw ring.

- [ ] **Step 5: Verify GREEN and sustained reception**

Run the integration script, then a longer mixed-source burst. Require the
receiver to remain alive, valid record count to match, and wrong-length summary
to be non-zero.

- [ ] **Step 6: Commit Task 7**

```bash
git add CMakeLists.txt apps/rdma2dada/main.cpp apps/rdma2dada/README.md \
  include/rdma_dada/io/rdma/receiver.h src/io/rdma/receiver.cpp \
  src/io/rdma/verbs_context.cpp tests/README.md \
  tests/rdma_receiver_integration.sh
git commit -m "fix: accept multiple VDIF senders at RDMA ingest"
```

### Task 8: End-to-end functional test and documentation

**Files:**
- Create: `scripts/run_vdif_functional_demo.sh`
- Modify: `README.md`
- Modify: `doc/DEVELOPMENT_PLAN.md`
- Modify: `doc/PIPELINE_ARCHITECTURE.md`
- Modify: `doc/ALGORITHM_MODULE_CONTRACTS.md`
- Modify: `modules/vdif_unpack/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: all preceding applications and libraries.
- Produces: repeatable multi-sender functional commands and documented
  acceptance output.

- [ ] **Step 1: Write the orchestration script in validation-first form**

Validate binaries, JSON files, MTU, ring keys and block geometry before creating
rings. Start `rdma2dada`, `vdif_unpack_worker`, compute reader and one simulator
per Station ID; install signal cleanup before starting child processes.

- [ ] **Step 2: Run the portable suite**

```bash
cmake -S . -B build -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Expected: all portable tests pass.

- [ ] **Step 3: Run the Linux functional topology**

Run one in-order case and one injected loss/duplicate/reorder case. Require
exact TFPA output, zero-filled missing Station slice, non-zero matching counters,
valid compute header and clean EOD.

- [ ] **Step 4: Update durable documentation**

Record the final commands, JSON fields, memory formulas, known inability to
detect an entirely absent group, destination-only filter, MTU requirement and
statistics interpretation. Mark only the completed Milestone 2 items complete.

- [ ] **Step 5: Final verification**

```bash
git diff --check
ctest --test-dir build --output-on-failure
```

On Linux also run:

```bash
ctest --test-dir build-linux --output-on-failure
```

- [ ] **Step 6: Present tested changes for user-controlled Git publication**

Report all local commits and test evidence. Do not push to GitHub unless the
user explicitly requests it.
