# Project VDIF Profile v1 Design

## Scope

This change defines and validates the packet-format profile consumed by the
future `vdif_unpack` module. It updates the JSON profile, portable C++ parser,
inspect tool, tests, and documentation. It does not yet decode binary packet
headers or reorder payload data.

## Fixed frame contract

- The application header is exactly 32 bytes: eight little-endian 32-bit words.
- The format is VDIF-derived, not strictly VDIF compliant.
- Payload samples use two's-complement complex integers.
- `NCHAN` is an arbitrary integer from 1 through 255; it need not be a power of
  two.
- Each packet belongs to one numeric Station ID and contains every configured
  polarization for one contiguous frequency segment.
- Payload tensor order is `TFP`; each tensor element stores adjacent `I,Q`
  components. `I` maps to the real component and `Q` maps to the imaginary
  component when the unpacked computation ring adopts the existing `RI`
  convention.
- Unpacked output order is `TFPA`; Station ID is resolved through the
  observation `antenna_map`, whose indices must match the Beamform weight A
  dimension.

## Header layout

All bit numbers refer to the decoded little-endian 32-bit word.

| Word | Bits | Field | v1 rule |
| --- | --- | --- | --- |
| 0 | 31 | `invalid_data` | 0 valid, 1 invalid |
| 0 | 30 | `legacy_mode` | must be 0 |
| 0 | 29-0 | `seconds_from_reference_epoch` | VDIF second count |
| 1 | 31-30 | `word1_reserved` | must be 0 |
| 1 | 29-24 | `reference_epoch` | six-month epoch from 2000-01-01 |
| 1 | 23-0 | `frame_number_within_second` | starts at zero |
| 2 | 31-29 | `vdif_version` | must be 0 |
| 2 | 28-24 | `channel_count_code` | must be 31; project NCHAN follows in Word 5 |
| 2 | 23-0 | `frame_length_units_8_bytes` | header plus payload, in eight-byte units |
| 3 | 31 | `data_type` | must be 1 for complex |
| 3 | 30-26 | `component_bits_minus_one` | v1 resolves to 8 or 16 bits |
| 3 | 25-16 | `thread_id` | must be 0 in v1 |
| 3 | 15-0 | `station_id` | numeric antenna identifier |
| 4 | 31-24 | `edv` | private project value `0xff` |
| 4 | 23-16 | `profile_version` | must be 1 |
| 4 | 15-8 | `sample_encoding` | must be 1 (`TWOS_COMPLEMENT`) |
| 4 | 7-0 | `flags` | must be 0 |
| 5 | 31-16 | `first_channel_id` | lowest channel number in the packet |
| 5 | 15-8 | `nchan` | 1 through 255 |
| 5 | 7-0 | `npol` | 1 or 2 |
| 6 | 31-0 | `nsamp_per_packet` | positive T extent |
| 7 | 31-0 | `word7_reserved` | must be 0 |

`channel_count_code=31` is a project sentinel. Project software must never
interpret it as standard VDIF `2^31` channels. The authoritative count is
Word 5 `nchan`.

## Payload and geometry

The payload is byte-aligned `CI8` or `CI16`:

```text
shape = [NSAMP_PER_PACKET, NCHAN, NPOL]
order = TFP
component order = IQ
encoding = TWOS_COMPLEMENT
```

For component width `C` bytes (`C=1` for CI8 and `C=2` for CI16):

```text
sample_bytes  = 2 * C
payload_bytes = NSAMP_PER_PACKET * NCHAN * NPOL * sample_bytes
frame_bytes   = 32 + payload_bytes
frame_bytes   = frame_length_units_8_bytes * 8
```

All arithmetic is overflow checked. `frame_bytes` must be divisible by eight.

## Aggregation

The packet group key is:

```text
(reference_epoch, seconds_from_reference_epoch,
 frame_number_within_second, first_channel_id, nchan, npol)
```

Each configured Station ID must occur exactly once in a complete group. The
future unpacker scatters:

```text
src[t,f,p] -> dst[t,f,p,antenna_map[station_id]]
```

Unknown Station IDs, duplicate packets, inconsistent geometry, and incomplete
groups are errors until an explicit loss policy is added.

## Configuration model

The packet-format JSON profile describes the fixed field locations and payload
axis sources:

```text
T extent = HEADER:nsamp_per_packet
F extent = HEADER:nchan
P extent = HEADER:npol
A extent = CONST:1

T origin = DERIVED:vdif_frame_time
F origin = HEADER:first_channel_id
P origin = CONST:0
A origin = LOOKUP:antenna_map:station_id
```

The parser accepts exactly 32 header bytes for this schema version. Header
extent expressions may reference declared header fields. `DERIVED` and
`LOOKUP` origins are declarative contracts for the future unpacker.

## Test contract

Portable tests cover successful loading, all field offsets and widths,
two's-complement encoding, arbitrary non-power-of-two NCHAN declaration,
TFP-to-TFPA axes, fixed 32-byte header rejection, header-reference validation,
bit overlap, and inspect output. Existing portable tests remain green.
