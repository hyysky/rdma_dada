# rdma2dada

Implemented ingest application and current Linux integration-test entry point.
It consumes the compiler-generated `resolved_observation.json`, validates its
identity and derived geometry, publishes the matching RAW DADA header, receives
fixed RoCE/UDP frames, strips the 42-byte network header and writes complete raw
records to the configured PSRDADA ring.

```bash
rdma2dada --plan artifacts/resolved_observation.json
```

Receiver device, destination MAC/IP/port, raw ring key, record size and block
geometry come only from the resolved plan. `--send_n`, `--nsge`, `--cpu` and
`--debug` remain runtime tuning options; no geometry-changing override exists.
`--preflight-only` validates the plan and receiver device without accessing the
ring.

The RAW_PACKET flow matches only destination MAC, destination IPv4 and
destination UDP port. Source MAC, IPv4 and UDP port are wildcarded so packets
from every configured FPGA/Station reach the same receive queue.

A successful receive completion with a length different from the configured
Ethernet-frame length is dropped, counted and immediately reposted; it is never
written to the raw ring and does not stop reception. CQ status errors, invalid
WR IDs, unexpected opcodes and repost failures remain fatal. Wrong-length logs
are emitted at power-of-two counts, and shutdown prints accepted and
wrong-length totals plus their ratio.

有限 transfer 停止时，接收线程先处理已经从 CQ 取出但不足 `send_n` 的 completion，
将这些完整 record 追加到当前 raw block；随后按实际有效字节发布最后一个 partial block，
再由主线程发送 PSRDADA EOD。发布字节数必须是 raw record size 的整数倍。空 transfer
不会预先获取或发布空 block。shutdown summary 同时给出 `accepted`、`published`、
full/partial block 和 `cq_tail_records`，正常结束必须满足 `accepted=published`。
