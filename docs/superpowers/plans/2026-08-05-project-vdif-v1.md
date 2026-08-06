# Project VDIF Profile v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the placeholder 64-byte packet profile with the fixed 32-byte Project VDIF v1 header and TFP payload contract.

**Architecture:** Keep the existing standalone `PacketFormatConfig` seam. Extend its declarative axis sources just enough for header-derived extents, VDIF-derived time, and Station-ID lookup; keep binary decoding outside this change.

**Tech Stack:** C++11, strict in-repo JSON parser, JSON Schema 2020-12, CMake/CTest, Python 3 CLI regression test.

## Global Constraints

- Header size is exactly 32 bytes/eight little-endian words.
- Payload encoding is `TWOS_COMPLEMENT`.
- `NCHAN` is UINT8 range 1-255 and is not restricted to powers of two.
- Payload order is `TFP`; unpacked order is `TFPA`.
- Existing unrelated dirty-worktree changes must not be modified.
- Every production behavior follows a witnessed RED then GREEN test run.

---

### Task 1: Fixed Project VDIF profile loading

**Files:**
- Modify: `config/packet_formats/frontend.example-v1.json`
- Modify: `tests/packet_format_config_test.cpp`
- Modify: `include/rdma_dada/config/packet_format_config.h`
- Modify: `src/config/packet_format_config.cpp`

**Interfaces:**
- Consumes: `LoadPacketFormatConfig(path, config, error)`.
- Produces: `PacketFormatConfig::sample_encoding` and support for `HEADER`, `DERIVED`, and `LOOKUP` axis expressions.

- [x] **Step 1: Write the failing success-profile test**

Change expected literals to:

```cpp
Expect(config.application_header_bytes == 32, "v1 header is 32 bytes");
Expect(config.sample_encoding == "TWOS_COMPLEMENT",
       "payload uses two's complement");
Expect(config.component_order == "IQ", "raw components are I then Q");
Expect(config.packed_order == std::vector<std::string>({"T", "F", "P"}),
       "packet payload order is TFP");
```

- [x] **Step 2: Run RED**

Run:

```bash
cmake --build build --target packet_format_config_test --parallel
./build/packet_format_config_test config/packet_formats/frontend.example-v1.json
```

Expected: failure because the current parser requires a 64-byte header and four packed axes.

- [x] **Step 3: Implement the minimal model/parser change**

Add:

```cpp
enum class PacketAxisValueSource {
    kConstant, kConfig, kHeader, kDerived, kLookup
};

struct PacketFormatConfig {
    // existing fields...
    std::string sample_encoding;
};
```

Parse `HEADER:name` for extents and `DERIVED:name` or
`LOOKUP:map_name:field_name` for origins. Validate header size exactly 32,
`sample_encoding=TWOS_COMPLEMENT`, packed axes exactly TFP, and output TFPA.

- [x] **Step 4: Run GREEN**

Run the Task 1 command and require `packet_format_config_test passed`.

### Task 2: Full eight-word field contract

**Files:**
- Modify: `config/packet_formats/frontend.example-v1.json`
- Modify: `tests/packet_format_config_test.cpp`
- Modify: `config/packet_formats/packet-format-v1.schema.json`

**Interfaces:**
- Consumes: generic `ApplicationHeaderField` descriptors.
- Produces: the exact Word 0-7 field table from the design spec.

- [x] **Step 1: Add a failing field-layout assertion**

Assert the known literals:

```cpp
Expect(config.header_fields.size() == 22, "all v1 fields are declared");
Expect(config.header_fields[0].name == "invalid_data", "Word 0 begins correctly");
Expect(config.header_fields[13].name == "edv", "Word 4 begins correctly");
Expect(config.header_fields.back().name == "word7_reserved", "Word 7 is reserved");
```

- [x] **Step 2: Run RED**

Run the packet-format unit test and require failure on field count/layout.

- [x] **Step 3: Replace the fixture and Schema**

Declare each field at offsets 0, 4, 8, 12, 16, 20, 24, and 28 using
little-endian `UINT32` storage and the bit ranges in the design spec. Make
`application_header_bytes` JSON Schema `const: 32`, add
`sample_encoding: {"const":"TWOS_COMPLEMENT"}`, allow three unique packed
axes, and add HEADER/DERIVED/LOOKUP expression patterns.

- [x] **Step 4: Run GREEN**

Run the unit test and JSON syntax checks.

### Task 3: Inspect output and strict failures

**Files:**
- Modify: `tools/packet_format_inspect.cpp`
- Modify: `tests/packet_format_inspect_test.py`

**Interfaces:**
- Consumes: validated `PacketFormatConfig`.
- Produces: machine-readable 32-byte/TFP/TWOS_COMPLEMENT summary.

- [x] **Step 1: Add failing CLI expectations**

Require:

```python
"APPLICATION_HEADER_BYTES=32"
"SAMPLE_ENCODING=TWOS_COMPLEMENT"
"PACKED_ORDER=TFP"
"OUTPUT_ORDER=TFPA"
```

Also write a temporary profile with 64 header bytes and require nonzero exit.

- [x] **Step 2: Run RED**

Run `ctest --test-dir build -R '^packet_format_inspect_test$' --output-on-failure`.

- [x] **Step 3: Emit the new fields**

Add `SAMPLE_ENCODING` to inspect output; existing join behavior emits TFP.

- [x] **Step 4: Run GREEN**

Run both packet-format CTests and require two passes.

### Task 4: Documentation and full verification

**Files:**
- Create: `doc/PROJECT_VDIF_PROFILE_V1.md`
- Modify: `config/packet_formats/README.md`
- Modify: `doc/ALGORITHM_MODULE_CONTRACTS.md`
- Modify: `doc/DEVELOPMENT_PLAN.md`
- Modify: `tests/README.md`

**Interfaces:**
- Produces: the durable FPGA/GPU header and payload contract.

- [x] **Step 1: Document the exact header and formulas**

Copy the approved layout and distinguish it from standard VDIF. Document
Station-ID mapping, TFP-to-TFPA scatter, arbitrary NCHAN, Two’s Complement, and
frame length validation.

- [x] **Step 2: Run focused verification**

```bash
ctest --test-dir build -R '^packet_format_(config|inspect)_test$' --output-on-failure
```

- [x] **Step 3: Run full portable verification**

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- [x] **Step 4: Run static and sanitizer checks**

```bash
c++ -std=c++11 -Iinclude -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
  src/config/packet_format_config.cpp tools/packet_format_inspect.cpp \
  tests/packet_format_config_test.cpp
c++ -std=c++11 -Iinclude -fsanitize=address,undefined \
  -fno-omit-frame-pointer src/config/json_value.cpp \
  src/config/packet_format_config.cpp tests/packet_format_config_test.cpp \
  -o /private/tmp/project_vdif_profile_test
/private/tmp/project_vdif_profile_test \
  config/packet_formats/frontend.example-v1.json
git diff --check
```

### Required integration found during verification

- [x] Change top-level `PipelineConfig` contract and example conversion fixture
  from a 64-byte placeholder to the fixed 32-byte Project VDIF header.
- [x] Require top-level raw `PAYLOAD_ORDER=TFP`.
- [x] Update raw record/block/rate expectations and the Linux-only DADA header
  round-trip expectations.
