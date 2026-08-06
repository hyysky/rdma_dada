# vdif_unpack_worker

`vdif_unpack_worker` 是独立的 CPU/PSRDADA 进程，连接 raw ring 和 compute ring：

```text
raw ring (Project VDIF records, TFP) -> vdif_unpack_worker
    -> compute ring (payload only, TFPA)
```

启动前必须用配置计算出的精确 block size 创建两个 ring。进程先连接并锁定 raw ring
reader，收到每个 transfer 的 ASCII header 后才锁定 compute ring writer；它校验 raw
header 与 JSON 几何一致，发布更新后的 compute header，然后重排每个 data block。

```bash
vdif_unpack_worker config/vdif_unpack.example.json
```

`runtime.run_once=false` 用于连续观测：一个 transfer EOD 后进程解锁两个 HDU，再次锁定
raw reader 等待下一份 header。`true` 用于有限功能测试。未知 Station、重复包、VDIF
invalid-data 包和迟到包只计数并丢弃；缺失 Station 在 window 到期或 EOD 时补零。每个
transfer 结束会记录完整/不完整 group、坏包以及缺失 Station 比例，并向 compute data
ring 发布 EOD。

当前第一版按已确认的 `PKT_NBIT=16` 处理 `CI8`（8-bit 实部 + 8-bit 虚部）。CI16 wire
codec 已存在，但在观测位宽与对应 ring/header 几何最终确定前，本 worker 不接受 CI16
配置。
