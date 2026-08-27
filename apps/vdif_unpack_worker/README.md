# vdif_unpack_worker

`vdif_unpack_worker` 是独立的 CPU/PSRDADA 进程，连接 raw ring 和 compute ring：

```text
raw ring (Project VDIF records, TFP)
  -> coordinator + parser worker pool + sole writer
  -> compute ring (payload-only, block-scoped ATFP)
```

启动前用 `observation_config_compile` 生成统一产物，并按 `ring_plan.json` 的精确 block
size 创建两个 ring。进程只读取同一份 `resolved_observation.json`；每个 transfer 开始时
校验 `CONFIG_ID`、`GEOMETRY_ID`、header 数据几何和 ring capacity，一致后才发布 compute
header。

```bash
vdif_unpack_worker \
  --plan artifacts/resolved_observation.json \
  --thread-cpus 14,15,16,17,18,19
```

CPU 列表顺序固定为 `COORDINATOR,WORKER...,WRITER`，至少包含三个互不相同的 CPU；
worker 数量由中间 CPU 数量确定。CPU 14–19 是当前 qths1 NUMA1 测试 profile，不是产品
默认值。应用会设置并核对每个 pthread 的实际 affinity。

## 并行处理模型

1. coordinator 持有一个 raw block，把 records 划分为连续区间；
2. parser workers 并行解码为预分配的 16-byte、无指针 descriptor；
3. coordinator 按原始 record 顺序更新 Station watermark、解决 duplicate 并预留 window slot；
4. workers 并行把已接受的完整 TFP payload 复制到互不重叠的天线平面；
5. coordinator 将完成的 window range 作为 immutable lease 放入有界 ready queue；
6. sole writer 是唯一调用 compute-ring Acquire/Commit 的线程，并按 ordinal 顺序发布；
7. writer 返回 ACK 后，coordinator 才能复用对应 window range。

因此 raw ring 始终只有一个 reader，compute ring 始终只有一个 writer，不引入额外
PSRDADA ring。所有 descriptor、queue 和 lease storage 在 transfer 准备阶段分配； measured
per-record path 不做 heap allocation，也不打印逐包日志。

## 数据和失败语义

raw record 是 `32-byte Project VDIF header + TFP payload`；packet header 不进入私有 window
或 compute ring。输出 block 是 payload-only、block-scoped `[A,T,F,P]`。同一天线的一整个
TFP payload 只复制一次；partial EOD block 只提交有效字节。

未知 Station、重复包、invalid-data、迟到包和 observation 边界外包计数后丢弃；缺失
Station 和完全缺失 group 在 missing-wait watermark 或 EOD 时补零。任一 parser、
coordinator、writer、ring 或 affinity 错误都会终止完整 transfer；EOD 时先结束 parse/copy，
finalize partial range，等待所有 writer ACK，再发布 compute EOD 并 join threads。

`processing.run_once=false` 在一个 transfer EOD 后重新锁定两个 HDU，等待下一份 header；
`true` 用于有限功能或验收测试。当前第一版 worker 接受 `PKT_NBIT=16` 对应的 CI8
（8-bit 实部 + 8-bit 虚部）；CI16 codec 已存在，但尚未开放为 worker 输入模式。

当前架构、性能证据和正式验收缺口见
[`../../docs/VDIF_UNPACK_STATUS.md`](../../docs/VDIF_UNPACK_STATUS.md)。
