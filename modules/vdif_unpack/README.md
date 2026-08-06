# vdif_unpack

Planned host module that validates raw record geometry, removes the fixed
32-byte Project VDIF v1 packet header, aggregates Station IDs, and reorders TFP
payload samples into TFPA compute layout. The exact wire contract is documented
in [`doc/PROJECT_VDIF_PROFILE_V1.md`](../../doc/PROJECT_VDIF_PROFILE_V1.md) and
loaded from the strict packet-format profile described in
[`config/packet_formats/README.md`](../../config/packet_formats/README.md).

The profile parser and validator are implemented. Packet decoding, sequence
policy, loss handling and TFPA reorder are not implemented yet and require the
binary golden records and an explicit loss policy.
