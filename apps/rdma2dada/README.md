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
geometry come only from the resolved plan. Only `--recv-wr-num`,
`--poll-batch`, `--poll-cpu` and `--debug` are runtime
tuning options; no geometry-changing override exists.
`--preflight-only` validates the plan and receiver device without accessing the
ring.

The RAW_PACKET flow matches only destination MAC, destination IPv4 and
destination UDP port. Source MAC, IPv4 and UDP port are wildcarded so packets
from every configured FPGA/Station reach the same receive queue.

A single RAW_PACKET QP/CQ and receive thread use exactly two SGEs per WR. SGE0
receives the fixed 42-byte Ethernet/IPv4/UDP header into scratch memory; SGE1
receives one VDIF record directly into one of two outstanding PSRDADA blocks.
There is no packet-to-ring memcpy and no legacy copy/SPSC or multi-QP mode.

正确长度的 completion 保留对应 VDIF slot。错误长度会将该 slot 清零；连续 16 个
错误长度包或任何 CQ/WR/repost 错误会终止 transfer。有限 transfer 只发布连续完成的
完整 record 前缀，随后发送 PSRDADA EOD。正常无错误结束必须满足
`accepted=published`。

正常停止采用固定 1 秒 receiver drain：停止请求后继续轮询 CQ、发布 raw block 并
重投递 WR，达到单调时钟 deadline 后才提交 partial tail 和 EOD。drain 只记录
`drain_duration_ns`、`completions_after_stop` 和 `exit_reason` 三个汇总字段；不会在
稳态每包路径增加时间戳或日志。
