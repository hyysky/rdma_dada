# vdif_unpack_worker

`vdif_unpack_worker` 是独立的 CPU/PSRDADA 进程，连接 raw ring 和 compute ring：

```text
raw ring (Project VDIF records, TFP) -> vdif_unpack_worker
    -> compute ring (payload only, block-scoped ATFP)
```

启动前先用 `observation_config_compile` 生成统一产物，再按 `ring_plan.json` 的精确
block size 创建两个 ring。进程只读取同一份 `resolved_observation.json`；收到每个
transfer 的 ASCII header 后校验 `CONFIG_ID`、`GEOMETRY_ID`、数据几何及两个 ring 的
实际 block capacity，全部一致后才发布 compute header 和处理数据。

```bash
vdif_unpack_worker --plan artifacts/resolved_observation.json
```

`processing.run_once=false` 用于连续观测：一个 transfer EOD 后进程解锁两个 HDU，再次锁定
raw reader 等待下一份 header。`true` 用于有限功能测试。未知 Station、重复包、VDIF
invalid-data 包、迟到包和 observation 边界外的包只计数并丢弃；缺失 Station 以及完全
缺失的 group 在 group-distance watermark 或 EOD 时补零。`EXPECTED_GROUPS` 是控制程序
传入的 exclusive stop boundary。每个 transfer 结束会记录完整/不完整/完全缺失 group、
连续 payload copy 次数、输出 block 以及缺失 Station 比例，并向 compute data ring 发布
EOD。

worker 使用固定环形 `[A,circular_group,T,F,P]` 私有窗口。同一天线的一整个 TFP
payload 只复制一次；输出时每个 compute block 只 Acquire 一次、按 A 平面填充并 Commit
一次，partial EOD block 只提交实际 `[A,G,T,F,P]` 字节。

当前第一版按已确认的 `PKT_NBIT=16` 处理 `CI8`（8-bit 实部 + 8-bit 虚部）。CI16 wire
codec 已存在，但在观测位宽与对应 ring/header 几何最终确定前，本 worker 不接受 CI16
配置。
