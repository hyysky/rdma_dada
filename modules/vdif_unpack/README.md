# vdif_unpack

Host-side unpack boundary that validates raw record geometry, removes the fixed
32-byte Project VDIF v1 packet header, aggregates Station IDs, and reorders
TFP payload samples into block-scoped ATFP compute layout. The exact wire contract is documented
in [`doc/PROJECT_VDIF_PROFILE_V1.md`](../../doc/PROJECT_VDIF_PROFILE_V1.md) and
loaded from the strict packet-format profile described in
[`config/packet_formats/README.md`](../../config/packet_formats/README.md).

目前已实现：

- 不依赖 packed struct 的 32-byte little-endian header codec；
- 严格 `vdif_unpack` JSON、相对路径解析、Station ID→A 轴映射校验；
- payload-only 窗口、group 和 compute block 的 checked-size 推导；
- raw header 到 `UNPACKED/ATFP/CI8` compute header 的转换，未知观测字段原样保留；
- `GROUP_PERIOD_PS` 与固定 observation 起点共同定义严格 group ordinal，
  `EXPECTED_GROUPS` 是 transfer 的排他停止边界；
- 丢包策略元数据固定为 `LOSS_POLICY=ZERO_FILL`；
- 有界跨 raw-block 重排引擎：在 `Configure` 时一次性分配 payload arena、slot 状态和
  Station→A 查找表，收包期间不为 group payload 单独申请内存；
- 固定环形 slot 以严格 ordinal 作 ownership tag，并用 65536 项 Station→A 直接查找表；
- 每个 Station packet 的完整 TFP payload 只执行一次连续复制，写入天线平面
  `[A,circular_group,T,F,P]`，32-byte packet header 不进入窗口；
- watermark 使用所有 Station 已到达 ordinal 的最小值；只有每个 Station 都越过重排
  容限后，窗口压力才会按 ordinal 输出并对确认缺失的 Station 平面补零。单个领先
  Station 不能淘汰落后 Station 的仍可能到达数据；某个 Station 整体停流则失败而非
  静默推进；
- 每个 raw block 先一次解析全部 header 并更新多 Station watermark，再按原记录顺序
  放置 payload；描述符缓冲在配置时预留，不为每个 block 重复分配；
- 每个 block 仅获取一个 compute ring writable block，按天线连续复制一次（环形回绕时
  最多两段），最终只提交一次，实际字节布局为 block-scoped `[A,T,F,P]`。

统计项分别记录收到、接受、坏 header、VDIF invalid-data、未知 Station、重复、late、
越界、large-gap 推进、完整/不完整/完全缺失 group、payload copy 次数与输出 block；
诊断项还按 Station 记录 observed/accepted/late/highest ordinal，并记录最大 Station
ordinal 偏斜、large-gap 推进组数、单 Station/混合 raw block、单 block 最大 Station
记录数和最长连续 Station run。
丢包比例分母固定为 `EXPECTED_GROUPS*NANT`，因此完全缺失的 group 也纳入统计。

CPU-only 性能与正确性验收使用独立的 `SyntheticVdifSource` 生成合法 Project VDIF
record。它固定支持 `INTERLEAVED`、`STATION_BURST_425` 和
`STATION_BURST_2112` 三种到达顺序；每条 record 的 Station、时间戳和 TFP payload
均可确定性重放。手工 byte-level golden 负责验证 wire bytes，生产 codec 只作为附加
round-trip 检查，不作为 expected 数据来源。

窗口的默认跨度是两个 compute block，但它不是第二个 ring，也不保存 packet header。
ATFP engine 将有序 block view 交给 `AtfpBlockWriter`；writer 不跨 callback 持有输出
block，并在 EOD 对最后一个 partial block 只提交有效字节。具体
PSRDADA `Acquire/Commit` adapter 已由独立的 `vdif_unpack_worker` 实现；它在每个
transfer 发布转换后的 header 和 data EOD，支持部分末尾 block，并在连续模式下重新
锁定 raw ring 等待下一次 transfer。
