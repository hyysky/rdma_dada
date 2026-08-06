# Project VDIF Profile v1

本文档是 FPGA 发送端与 GPU 接收 pipeline 之间的 Project VDIF v1 wire contract。它借用
VDIF 基础字段，但为本项目定义了私有扩展；不能假设任意标准 VDIF 工具都能直接解析。

## 固定 frame

```text
32-byte header（8 × UINT32 little-endian） | TFP complex payload
```

- header 固定 32 bytes，payload 固定从 byte 32 开始；v1 不支持 legacy 或扩展长度；
- bit 编号是在 little-endian word 解码后采用 LSB0；
- payload 使用 `TWOS_COMPLEMENT`，分量顺序 `IQ`；I 为实部，Q 为虚部；
- 支持 `CI8`（8+8 bit）和 `CI16`（16+16 bit）；
- 每包来自一个数值 Station ID，包含一段连续频率的全部 1 或 2 路偏振；
- payload 张量顺序固定 `T-F-P`。

## 8-word 字段表

| Word / byte | Bits | 字段 | v1 规则 |
| --- | --- | --- | --- |
| 0 / 0 | 31 | `invalid_data` | 0 有效，1 无效 |
| 0 / 0 | 30 | `legacy_mode` | 必须为 0 |
| 0 / 0 | 29–0 | `seconds_from_reference_epoch` | 相对 reference epoch 的秒数 |
| 1 / 4 | 31–30 | `word1_reserved` | 必须为 0 |
| 1 / 4 | 29–24 | `reference_epoch` | 从 2000-01-01 起的半年 epoch |
| 1 / 4 | 23–0 | `frame_number_within_second` | 每秒从 0 开始 |
| 2 / 8 | 31–29 | `vdif_version` | 必须为 0 |
| 2 / 8 | 28–24 | `channel_count_code` | 必须为 31，表示 project sentinel |
| 2 / 8 | 23–0 | `frame_length_units_8_bytes` | header+payload 总字节数除以 8 |
| 3 / 12 | 31 | `data_type` | 必须为 1，表示 complex |
| 3 / 12 | 30–26 | `component_bits_minus_one` | CI8 为 7，CI16 为 15 |
| 3 / 12 | 25–16 | `thread_id` | v1 必须为 0 |
| 3 / 12 | 15–0 | `station_id` | 数值天线标识 |
| 4 / 16 | 31–24 | `edv` | 必须为 `0xff`，project private |
| 4 / 16 | 23–16 | `profile_version` | 必须为 1 |
| 4 / 16 | 15–8 | `sample_encoding` | 必须为 1，即 `TWOS_COMPLEMENT` |
| 4 / 16 | 7–0 | `flags` | v1 必须为 0 |
| 5 / 20 | 31–16 | `first_channel_id` | 本 packet 的最小全局频率编号 |
| 5 / 20 | 15–8 | `nchan` | 连续通道数，1–255，不要求为 2 的幂 |
| 5 / 20 | 7–0 | `npol` | 必须为 1 或 2 |
| 6 / 24 | 31–0 | `nsamp_per_packet` | T 轴长度，必须大于 0 |
| 7 / 28 | 31–0 | `word7_reserved` | 必须为 0 |

`channel_count_code=31` 不表示 `2^31` 个 channel。Project VDIF 解析器必须忽略标准
VDIF 的该项推导，直接使用 Word 5 的 `nchan`。

## Payload 与 frame 几何

设单个复数 sample 的分量字节数为 `C`（CI8 时 C=1，CI16 时 C=2）：

```text
complex_sample_bytes = 2 * C
payload_bytes = NSAMP_PER_PACKET * NCHAN * NPOL * complex_sample_bytes
frame_bytes = 32 + payload_bytes
frame_bytes = frame_length_units_8_bytes * 8
```

上述计算必须检查整数溢出，`frame_bytes` 必须能被 8 整除，header 中的位宽、编码、
NPOL 和 frame length 必须与加载的 profile 及实际 UDP record 一致。由于 frame length
字段只有 24 bit，`frame_bytes / 8` 不能超过 `0xffffff`。

## 多天线聚合

同一组 packet 使用以下 key：

```text
(reference_epoch, seconds_from_reference_epoch,
 frame_number_within_second, first_channel_id, nchan, npol)
```

统一时间源保证不同 Station ID 对齐。每个完整 group 中，每个已配置 Station ID 必须恰好
出现一次。`antenna_map` 将数值 Station ID 映射为 Beamform 权重 `[F,P,A,B]` 的 A 索引：

```text
raw src[t,f,p] -> unpacked dst[t,f,p,antenna_map[station_id]]
```

未知 Station ID、重复 packet、geometry 不一致或 group 不完整均视为错误；零填充或跳过等
容错行为必须等后续丢包策略明确后才能启用。

## PSRDADA 边界

raw PSRDADA ring 的 data block 保留每个 record 的 32-byte header。`vdif_unpack` 校验、
聚合和去头后，计算 ring 只保存 TFPA payload；每个 transfer 的 PSRDADA ASCII header
则更新为 `ORDER=TFPA`、正确的 sample format、block geometry 和 byte rate。

机器可读 profile 位于
[`config/packet_formats/frontend.example-v1.json`](../config/packet_formats/frontend.example-v1.json)。
当前已实现 profile parser/validator 和 inspect 工具；binary decoder、packet-group 状态机
及 TFPA scatter 尚未实现。

当前顶层 pipeline 示例使用 `T=512, F=1, P=2, CI8`，所以每个天线 packet 的
payload 为 2048 bytes。该值属于观测几何，不是 wire profile 的全局常量；运行时必须
与 Word 5、Word 6 和 component width 一致。
