# rdma2dada

Implemented ingest application and current Linux integration-test entry point.
It loads the JSON data contract, publishes a RAW DADA header, receives fixed
RoCE/UDP frames, strips the 42-byte network header and writes complete raw
records to one PSRDADA ring. It contains composition and lifecycle code only;
transport and ring behavior live under `src/io/`.

The RAW_PACKET flow matches only destination MAC, destination IPv4 and
destination UDP port. Source MAC, IPv4 and UDP port are wildcarded so packets
from every configured FPGA/Station reach the same receive queue. The legacy
`--smac`, `--sip` and `--sport` options remain accepted for launcher
compatibility but do not participate in receive filtering.

A successful receive completion with a length different from the configured
Ethernet-frame length is dropped, counted and immediately reposted; it is never
written to the raw ring and does not stop reception. CQ status errors, invalid
WR IDs, unexpected opcodes and repost failures remain fatal. Wrong-length logs
are emitted at power-of-two counts, and shutdown prints accepted and
wrong-length totals plus their ratio.
