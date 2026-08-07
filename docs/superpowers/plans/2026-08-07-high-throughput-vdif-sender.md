# High-throughput VDIF Sender Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an explicit-source, equal-rate, UDP-only paced sender path to `fpga_sender_sim` while preserving the existing deterministic schema-v1 behavior.

**Architecture:** Keep Project VDIF config/record logic portable in `rdma_vdif_sender_sim`. Add a portable fixed-point rate pacer and reusable packet pool, then let the application use Linux `sendmmsg()` or the existing single-message UDP fallback. One process remains one Station; HF divides one aggregate target equally and writes the same per-Station target into every schema-v2 config.

**Tech Stack:** C++11, strict repository JSON parser, Project VDIF v1 codec, IPv4 UDP sockets, Linux `sendmmsg()`, `CLOCK_MONOTONIC`, CMake/CTest, Python loopback integration tests.

## Global Constraints

- UDP is the only sender transport; do not implement or evaluate libibverbs sender code.
- Station identity comes only from the Project VDIF `Station ID`; source IP/port are transport diagnostics.
- Every Station sends at the same configured payload bit rate; no weights or per-Station override exist.
- The receive destination remains one IP/port; current server tests use `174.0.1.111:1000`.
- Preserve schema-v1 `BURST`/`REALTIME`, deterministic payload, and fault-list behavior.
- Schema-v2 `PACED` uses a common future `start_utc`, explicit source endpoint, reusable packet batches, and `REPEAT_TEMPLATE` or `DETERMINISTIC` payload.
- Tests must be written and observed failing before production code is added.
- Do not stage, commit, or push automatically. After local completion, hand off exact server tests to `GPU服务器代码测试`.
- Preserve unrelated worktree changes in `AGENTS.md`, time-integration files, benchmark files, and mixed `CMakeLists.txt` hunks.

---

### Task 1: Schema-v2 sender configuration

**Files:**
- Modify: `include/rdma_dada/simulation/vdif_sender_sim.h`
- Modify: `src/simulation/vdif_sender_sim.cpp`
- Modify: `tests/vdif_sender_sim_test.cpp`
- Create: `config/fpga_sender_sim.paced.example.json`

**Interfaces:**
- Consumes: schema-v1 `VdifSenderSimConfig` and strict JSON helpers already in `vdif_sender_sim.cpp`.
- Produces these additional fields in `VdifSenderSimConfig`:

```cpp
std::uint32_t schema_version;
std::string source_ip;
std::uint16_t source_port;
std::uint64_t target_payload_bits_per_second;
std::uint32_t batch_packets;
std::string payload_mode;
```

- Schema v1 sets `schema_version=1`, leaves source empty/zero, sets target rate and batch to zero, and sets `payload_mode="DETERMINISTIC"`.
- Schema v2 requires root objects `source` and `transmit`, `time.mode="PACED"`, non-empty future `start_utc`, `target_gbps>0`, `batch_packets=1..64`, and payload mode `DETERMINISTIC` or `REPEAT_TEMPLATE`.

- [ ] **Step 1: Add failing schema-v2 parsing tests**

Add a second config path argument to `vdif_sender_sim_test` and assertions equivalent to:

```cpp
Expect(config.schema_version == 2, "paced example uses schema v2");
Expect(config.source_ip == "127.0.0.1" && config.source_port == 41001,
       "schema v2 parses explicit source endpoint");
Expect(config.target_payload_bits_per_second == UINT64_C(10000000),
       "0.01 Gbps converts exactly to integer payload bps");
Expect(config.batch_packets == 16 && config.payload_mode == "REPEAT_TEMPLATE",
       "paced batch configuration parses");
```

Generate invalid temporary variants and require rejection for missing source,
source port zero, unknown source/transmit field, `time.mode` other than
`PACED`, zero/NaN/negative target, batch zero/65, and unknown payload mode.

- [ ] **Step 2: Run RED**

Run:

```bash
cmake --build build --target vdif_sender_sim_test --parallel
./build/vdif_sender_sim_test \
  config/fpga_sender_sim.example.json \
  config/fpga_sender_sim.paced.example.json
```

Expected: compile failure because the six new config fields do not exist, or runtime failure because schema version 2 is rejected.

- [ ] **Step 3: Implement strict dual-schema parsing**

Keep one validation function but branch the exact root-key set on
`schema_version`. Parse `target_gbps` from the JSON numeric token into integer
payload bits/s without repeated floating-point arithmetic. Reject values that
cannot be represented as a positive `uint64_t` bits/s.

- [ ] **Step 4: Run GREEN and schema-v1 regression**

Run the command from Step 2. Expected: all schema-v1 and schema-v2 assertions pass.

- [ ] **Step 5: Checkpoint**

Run `git diff --check` and inspect only the four Task-1 files before continuing.

### Task 2: Fixed-point payload-rate pacing

**Files:**
- Create: `include/rdma_dada/simulation/vdif_sender_rate.h`
- Create: `src/simulation/vdif_sender_rate.cpp`
- Create: `tests/vdif_sender_rate_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct SenderRatePlan {
    std::uint64_t target_payload_bits_per_second;
    std::uint64_t start_monotonic_ns;
};

bool ComputePayloadDeadlineNs(const SenderRatePlan& plan,
                              std::uint64_t cumulative_payload_bytes,
                              std::uint64_t* deadline_ns,
                              std::string* error);

bool ComputeEqualStationPayloadRate(std::uint64_t aggregate_bits_per_second,
                                    std::uint32_t station_count,
                                    std::uint64_t* station_bits_per_second,
                                    std::uint64_t* represented_aggregate,
                                    std::string* error);
```

The deadline is `start + floor(bytes*8*1e9/bps)` using checked wide integer arithmetic. Equal division uses integer bps and reports `station_rate*station_count`; it never assigns unequal remainders.

- [ ] **Step 1: Add failing rate tests**

Test exact 10-Gbps deadlines, non-divisible equal allocation, zero rate/count,
`uint64_t` overflow, monotonically increasing deadlines, and an absolute
start near `uint64_t` maximum.

- [ ] **Step 2: Run RED**

```bash
cmake -S . -B build -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build --target vdif_sender_rate_test --parallel
```

Expected: missing source/header/target failure.

- [ ] **Step 3: Implement minimum checked arithmetic**

Use compiler-supported unsigned 128-bit intermediates under GCC/Clang, with a compile-time failure on unsupported compilers rather than silently using floating point.

- [ ] **Step 4: Run GREEN**

```bash
./build/vdif_sender_rate_test
ctest --test-dir build -R '^vdif_sender_(rate|sim)_test$' --output-on-failure
```

Expected: both sender tests pass.

### Task 3: Reusable VDIF packet batch

**Files:**
- Create: `include/rdma_dada/simulation/vdif_sender_batch.h`
- Create: `src/simulation/vdif_sender_batch.cpp`
- Create: `tests/vdif_sender_batch_test.cpp`
- Modify: `src/simulation/vdif_sender_sim.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct VdifPacketView {
    const std::uint8_t* data;
    std::size_t bytes;
    std::uint64_t group_index;
};

class VdifSenderBatch {
public:
    bool Initialize(const VdifSenderSimConfig& config, std::string* error);
    bool Prepare(std::uint64_t first_group,
                 std::uint32_t packet_count,
                 std::string* error);
    std::uint32_t capacity() const;
    std::uint32_t size() const;
    const VdifPacketView& packet(std::uint32_t index) const;
};
```

`Initialize()` allocates all storage once. `Prepare()` rejects ranges outside
`group_count`, never changes packet data addresses, updates unique VDIF headers,
and either fills deterministic payloads or copies a Station-specific repeat
template.

- [ ] **Step 1: Add failing packet-batch tests**

Require exact batch capacity, stable data pointers across two `Prepare()` calls,
group indices and decoded time keys advancing, constant Station ID, different
Station templates, deterministic mode matching `BuildVdifSenderRecord()`, and
repeat mode retaining identical payload across groups.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target vdif_sender_batch_test --parallel
```

Expected: missing batch target/API.

- [ ] **Step 3: Implement header/payload writing into caller-owned storage**

Extract an internal record writer from `BuildVdifSenderRecord()` so both the
existing vector API and the fixed batch use the same Project VDIF encoder.
Allocate `batch_packets * record_bytes` in `Initialize()` and keep views bound
to stable offsets.

- [ ] **Step 4: Run GREEN and record regression**

```bash
./build/vdif_sender_batch_test
ctest --test-dir build -R '^vdif_sender_(batch|sim|rate)_test$' --output-on-failure
```

### Task 4: UDP runtime, source binding, `sendmmsg()`, and JSON statistics

**Files:**
- Create: `include/rdma_dada/simulation/udp_vdif_sender.h`
- Create: `src/simulation/udp_vdif_sender.cpp`
- Create: `tests/udp_vdif_sender_test.cpp`
- Modify: `apps/fpga_sender_sim/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct VdifSenderStats {
    std::uint64_t scheduled_packets;
    std::uint64_t sent_packets;
    std::uint64_t retried_packets;
    std::uint64_t failed_packets;
    std::uint64_t payload_bytes;
    std::uint64_t elapsed_ns;
    std::uint64_t batches;
    std::uint64_t short_batches;
    std::uint64_t overrun_batches;
};

bool RunUdpVdifSender(const VdifSenderSimConfig& config,
                      VdifSenderStats* stats,
                      std::string* error);
std::string FormatVdifSenderStatsJson(const VdifSenderSimConfig& config,
                                      const VdifSenderStats& stats);
```

Linux uses `sendmmsg()` for schema-v2 PACED mode. Schema v1 and non-Linux use
the existing connected UDP `send()` behavior. Version 2 binds before connect,
sets a large `SO_SNDBUF`, waits for common `start_utc`, and paces by cumulative
payload bytes. A short `sendmmsg()` result retries only the unsent suffix.

- [ ] **Step 1: Add failing stats and source-binding tests**

Test stable JSON keys and exact integer counts in C++. Extend the Python
loopback test to reserve a source port, launch schema v2, assert the observed
source port, decode every VDIF header, and parse the final JSON line.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target udp_vdif_sender_test fpga_sender_sim --parallel
./build/udp_vdif_sender_test
python3 tests/fpga_sender_sim_loopback_test.py \
  build/fpga_sender_sim \
  config/fpga_sender_sim.example.json \
  config/fpga_sender_sim.paced.example.json
```

Expected: missing runtime target/API and loopback CLI/config failure.

- [ ] **Step 3: Implement portable runtime and Linux batch adapter**

Move socket lifecycle, start-time parsing, pacing, sending, and statistics out
of `main.cpp`. Keep `main.cpp` responsible only for loading config, invoking the
runtime, printing the final JSON, and selecting exit status.

- [ ] **Step 4: Run GREEN**

Run the commands from Step 2. On macOS the v2 loopback uses the UDP single-send
fallback but must still bind the explicit source port and obey the same pacing
and statistics contract.

### Task 5: Linux-specific batch verification

**Files:**
- Create: `tests/fpga_sender_sim_linux_batch_test.py`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: schema-v2 sender and final JSON statistics.
- Produces: a Linux-only CTest that proves the `sendmmsg()` code path sent a
  multi-packet batch from the configured source port.

- [ ] **Step 1: Add the Linux test before exposing batch-path evidence**

The test launches a local UDP receiver, sends at least 64 records with
`batch_packets=16`, checks all records and source ports, and requires final JSON
`batches>=4`, `sent_packets=64`, `failed_packets=0`.

- [ ] **Step 2: Run RED on Linux or verify target registration locally**

macOS configure must register the test as skipped/not built. Linux initially
fails because runtime statistics do not yet identify the batch backend.

- [ ] **Step 3: Add explicit `backend="SENDMMSG"` statistics**

Add the backend name to the final JSON and require it in the Linux test. The
portable fallback reports `SEND`.

- [ ] **Step 4: Run GREEN on Linux through the server handoff**

The development task does not access the server. Include this test in Task 7's
handoff and require `backend=SENDMMSG`.

### Task 6: Documentation and complete local verification

**Files:**
- Modify: `apps/fpga_sender_sim/README.md`
- Modify: `config/README.md`
- Modify: `tests/README.md`
- Modify: `doc/DEVELOPMENT_PLAN.md`
- Modify: `README.md`

**Interfaces:**
- Documents payload-rate semantics, equal Station allocation, source-port
  semantics, schema compatibility, UDP-only scope, statistics, and server
  acceptance commands.

- [ ] **Step 1: Update documentation after behavior is green**

Include exact schema-v2 invocation and formulas:

```text
station_target_bps = floor(aggregate_target_bps / station_count)
represented_aggregate_bps = station_target_bps * station_count
payload_gbps = payload_bytes * 8 / elapsed_ns
```

- [ ] **Step 2: Run format and script checks**

```bash
git diff --check
python3 -m py_compile \
  tests/fpga_sender_sim_loopback_test.py \
  tests/fpga_sender_sim_linux_batch_test.py
```

- [ ] **Step 3: Run the complete portable build/test suite**

```bash
cmake -S . -B build -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Expected: every portable test passes; the Linux-only batch test is absent or
skipped on macOS.

### Task 7: GPU-server handoff and acceptance

**Files:** No product-code modification in this task.

**Interfaces:**
- Consumes: completed Task 8B worktree.
- Produces: explicit server PASS/FAIL evidence returned by `GPU服务器代码测试`.

- [ ] **Step 1: Notify the existing test task**

Include the branch, affected files, schema-v2 fixture, and these commands:

```bash
cmake -S . -B build-linux \
  -DBUILD_RDMA_PIPELINE=ON -DUSE_CUDA=OFF -DBUILD_TESTING=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux \
  -R '^(vdif_sender_|udp_vdif_sender|fpga_sender_sim)' \
  --output-on-failure
```

- [ ] **Step 2: Repeat the two-Station low-rate chain**

Require Station 101/102, distinct configured source ports, fixed destination
port 1000, accepted record counts, exact TFPA, compute header, and clean EOD.

- [ ] **Step 3: Run an initial all-valid paced smoke test**

Run a sufficiently long low-rate point first and require each sender's actual
payload rate within 2% of target, `backend=SENDMMSG`, zero failed packets, and
reconciled receiver counts. This is a smoke test, not the later 1–38 Gbps sweep.

- [ ] **Step 4: Report result and preserve Git authority**

After explicit server PASS, remind the user the Task 8B changes are ready for a
Git commit. Do not commit or push until the user decides.
