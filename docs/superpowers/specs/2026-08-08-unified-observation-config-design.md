# Unified Observation Configuration Design

## Goal

The user maintains one observation JSON. A shared configuration compiler turns
that file into one validated, immutable observation plan, all required process
configuration, and the expected DADA header for every configured pipeline
stage. No process or test script independently supplies data geometry.

## Configuration ownership

### User-authored observation JSON

The observation JSON is the only user-authored runtime configuration. It owns:

- observation identity, UTC start and duration;
- ordered Station ID list, whose length defines `NANT` and whose order defines
  the antenna axis;
- first channel, `NCHAN`, `NPOL`, sample interval and packet samples;
- the selected fixed wire-profile identifier;
- groups per block, ring block counts, reorder-window block count and ring keys;
- disk enablement, blocks per file and direct-I/O policy;
- receiver NIC/MAC/IP/port parameters that select the input process;
- configured algorithm chain, weight file, weight scale, beam selection,
  compute precision, CUDA device and integration parameters.

The user does not enter payload, record, block, window, ring, file or transfer
byte counts.

### Fixed Project VDIF profile

The versioned Project VDIF profile is developer-owned protocol metadata. It
defines:

- the fixed 32-byte application-header bit fields and semantics;
- header endian and bit numbering;
- payload encoding, complex component order and packed `TFP` axes;
- supported component type, currently `CI8` with two's-complement encoding;
- field-width and profile-version constraints.

It does not contain observation-specific `NANT`, `NCHAN`, `NPOL`, packet sample
count or payload byte count. The observation JSON selects the profile by ID;
users do not edit the profile for each observation.

`packet-format-v1.schema.json` remains an independent developer schema because
it validates protocol-description files, not observations.

### Generated configuration

The following files become generated artifacts rather than independent user
inputs:

- normalized resolved observation plan consumed directly by all processes;
- raw and downstream DADA headers;
- ring creation plan;
- optional FPGA sender-simulator configuration for tests;
- validation report and SHA256 manifest.

Existing example files are retained during migration. They are not deleted
until every consumer uses generated configuration and the user separately
approves deletion.

## Canonical input fields

The first schema contains these sections:

```text
schema_version
observation
  observation_id
  utc_start
  duration_seconds
  station_ids[]
  first_channel_id
  nchan
  npol
  sample_interval_ps
metadata
  telescope
  bandwidth_hz
  center_frequency_hz
wire
  profile
  samples_per_packet
blocks
  groups_per_block
  raw_ring_blocks
  compute_ring_blocks
  window_blocks
rings
  raw_key
  compute_key
storage
  enabled
  blocks_per_file
  direct_io
receiver
  device
  destination_mac
  destination_ip
  destination_port
processing
  backend
  cuda_device
  modules[]
```

`wire.profile` is a path resolved relative to the observation JSON. The loaded
profile's `format_id` is its identity; the user does not repeat that ID in the
observation JSON.

`duration_seconds` is a decimal string, parsed exactly and converted to integer
picoseconds. Exponent notation and precision below one picosecond are rejected.
`utc_start` is an integer-second UTC boundary in schema version 1.

`station_ids` is the sole source for `NANT`. `nbeam` is derived from the
selected beam list or weight-file B dimension; any explicit expected value is
validation-only and cannot redefine the dimension.

`metadata.telescope`, `metadata.bandwidth_hz` and
`metadata.center_frequency_hz` are required observation metadata in schema
version 1. They are preserved in every DADA header as `TELESCOPE`,
`BANDWIDTH_HZ` and `CENTER_FREQUENCY_HZ`. They affect `CONFIG_ID` but do not
change block-byte geometry or `GEOMETRY_ID` in this schema.

## Derived geometry

Let:

```text
A = station_ids.size
F = nchan
P = npol
T_pkt = samples_per_packet
G = groups_per_block
H = wire-profile header bytes
C = complex sample bytes from the wire profile
```

The compiler derives:

```text
payload_bytes       = T_pkt * F * P * C
raw_record_bytes    = H + payload_bytes
group_period_ps     = T_pkt * sample_interval_ps
expected_groups     = duration_ps / group_period_ps
records_per_block   = G * A
raw_block_bytes     = G * A * raw_record_bytes
compute_block_bytes = G * A * payload_bytes
window_groups       = window_blocks * G
window_payload      = window_blocks * compute_block_bytes
raw_ring_bytes      = raw_ring_blocks * raw_block_bytes
compute_ring_bytes  = compute_ring_blocks * compute_block_bytes
```

The observation duration must be exactly divisible by `group_period_ps`.
There is no silent rounding. Window allocation also includes compiler-reported
validity bitmap, group-slot metadata and alignment overhead; users do not set a
second maximum-byte value.

File size is `blocks_per_file * stage_block_bytes`, so every generated `.dada`
file is an integer number of ring blocks.

## Header chain

The compiler builds the raw DADA header and applies the same production header
transforms used by the configured modules:

```text
RAW/TFP
  -> UNPACKED/ATFP
  -> CONVERTED/TFPA
  -> BEAMFORMED/TFPB
  -> POWER/TFPB or POLARIZATION_PRODUCTS/TFBS
  -> optional INTEGRATED output
```

The compiler does not duplicate module-specific header formulas. If the
configured next module rejects the preceding header, compilation fails. In
particular, the current ATFP output cannot be accepted by a TFPA-only converter
until the planned fused ATFP transpose/conversion path exists.

Power and Stokes are alternative products in one linear two-ring worker. A
simultaneous Power-and-Stokes output requires a future fan-out design and is not
part of this schema.

## Cross-process consistency

The compiler serializes a canonical `ResolvedObservationPlan` and calculates:

```text
CONFIG_ID   = SHA256(canonical resolved configuration and referenced contents)
GEOMETRY_ID = SHA256(canonical data-geometry subset)
```

Referenced contents include the selected wire profile and weight-file digest;
file-system paths alone are not sufficient identity.

Both identifiers are written into the resolved plan and the raw DADA header.
Every process reads that same resolved plan. Each downstream process verifies
the identifiers, required header fields, input block capacity and output block
capacity before processing data.

Geometry-affecting command-line overrides are forbidden. Command-line options
may change only non-contract behavior such as log verbosity. Ring creation
uses the generated ring plan, not shell arithmetic.

## Startup and failure behavior

The configuration compiler supports a preflight operation that performs all
parsing, derivation, profile validation, header-chain validation, weight-shape
validation and generated-artifact consistency checks without creating rings,
adding capabilities or starting processes.

Formal execution repeats the same validation internally immediately before
resource creation. Any mismatch fails closed and identifies the JSON path,
expected value, actual value and derivation that failed.

## Verification

Acceptance covers:

- exact input-schema and unknown-field rejection;
- checked integer arithmetic and duration divisibility;
- Project VDIF profile compatibility;
- byte-exact raw, ATFP and algorithm-stage geometry fixtures;
- byte-for-byte expected DADA headers for every supported module chain;
- identical `CONFIG_ID` and `GEOMETRY_ID` in all generated artifacts;
- rejection of modified, stale or mixed generated configurations;
- `--preflight-only` creating no ring, process or capability;
- three consecutive clean server executions before acceptance.

Performance tests remain separate from configuration correctness and follow
the repository's real-time acceptance rules.
