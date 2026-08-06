# vdif_unpack

Host-side unpack boundary that validates raw record geometry, removes the fixed
32-byte Project VDIF v1 packet header, aggregates Station IDs, and reorders
TFP payload samples into TFPA compute layout. The exact wire contract is documented
in [`doc/PROJECT_VDIF_PROFILE_V1.md`](../../doc/PROJECT_VDIF_PROFILE_V1.md) and
loaded from the strict packet-format profile described in
[`config/packet_formats/README.md`](../../config/packet_formats/README.md).

目前已实现：

- 不依赖 packed struct 的 32-byte little-endian header codec；
- 严格 `vdif_unpack` JSON、相对路径解析、Station ID→A 轴映射校验；
- payload-only 窗口、group 和 compute block 的 checked-size 推导；
- raw header 到 `UNPACKED/TFPA/CI8` compute header 的转换，未知观测字段原样保留；
- 丢包策略元数据固定为 `LOSS_POLICY=ZERO_FILL`；
- 有界跨 raw-block 重排引擎：在 `Configure` 时一次性分配 payload arena、slot 状态和
  Station→A 查找表，收包期间不为 group payload 单独申请内存；
- group key 使用 `(reference_epoch, seconds, frame, first_channel_id, NCHAN, NPOL)`，
  不引入额外 group 编号，也不要求每秒 group 数是整数；
- 每个 Station packet 的 TFP payload 直接 scatter 到 TFPA slot，32-byte packet header
  不进入窗口；完整 group 同样等待 `window_blocks` 稳定边界，防止后到的更早 key 被误判；
- 窗口超时、arena 满或 EOD 时对已观察但不完整的 group 保持缺失 Station slice 为零，
  并严格按 key 顺序输出。

统计项分别记录收到、接受、坏 header、VDIF invalid-data、未知 Station、重复、late、
窗口淘汰、完整/不完整 group 和缺失 Station 数。丢包比例的分母是
`expected_station_packets_for_observed_groups`；完全没有任何 Station packet 到达的 group
第一版无法从数据流中推断，因此不会进入分子或分母。

窗口的默认跨度是两个 raw block，但它不是第二个 ring，也不保存 packet header。将有序
group 交给通用 `GroupBlockWriter`：writer 延迟获取非 owning 输出 block，将每个 group
直接复制到当前 offset，写满时提交完整 block，并在 EOD 只提交非空的有效字节。具体
PSRDADA `Acquire/Commit` adapter 已由独立的 `vdif_unpack_worker` 实现；它在每个
transfer 发布转换后的 header 和 data EOD，支持部分末尾 block，并在连续模式下重新
锁定 raw ring 等待下一次 transfer。
