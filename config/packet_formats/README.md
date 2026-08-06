# Packet format profiles

本目录保存前端 UDP record 的静态格式。当前
[`frontend.example-v1.json`](frontend.example-v1.json) 是 Project VDIF Profile v1
的机器可读定义，包头固定为 32 bytes（8 个 little-endian `UINT32` word），不是标准
VDIF 的完全兼容实现。完整协议见
[`doc/PROJECT_VDIF_PROFILE_V1.md`](../../doc/PROJECT_VDIF_PROFILE_V1.md)。

版本 1 的 JSON Schema 是
[`packet-format-v1.schema.json`](packet-format-v1.schema.json)。C++ 运行时还会执行字段
bit 重叠、header 引用、payload sample 对齐和常量 shape 几何等交叉校验：

```bash
build/packet_format_inspect \
  config/packet_formats/frontend.example-v1.json
```

## Record 与 application header

一个 raw record 是：

```text
32-byte Project VDIF header | payload_bytes bytes
```

32-byte header 在每个 UDP record 内重复出现；它不同于每个 PSRDADA transfer 只发布
一次的 ASCII header block。v1 不支持 16-byte legacy header、可变 header 或额外 extension
word，payload 总是从 byte 32 开始。

每个 `application_header.fields[]` 条目包含：

- `offset_bytes`：相对 32-byte header 起点的零基 byte offset；
- `type`：读取字段所用的整数 storage 类型；v1 均为 `UINT32`；
- `endian`：storage word 的 wire byte order；v1 固定 `LITTLE`；
- `bit_offset`、`bit_width`：word 解码后按 LSB0 计数的 bit 范围；
- 符号性由 `INT8/16/32/64` 或 `UINT8/16/32/64` 决定，不另设 `signed`；
- `scale`：物理量为 `decoded_integer * scale`；
- `semantic`、`unit`：字段的业务含义和单位。

字段可以共享一个 word，但实际 bit 不能重叠，storage 和 bit 范围也不能越过 byte 31。

## Payload 布局

Project VDIF v1 payload 是有符号二进制补码复整数：

```json
{
  "sample_format": "CI8",
  "sample_encoding": "TWOS_COMPLEMENT",
  "component_order": "IQ",
  "endian": "LITTLE",
  "packed_order": ["T", "F", "P"],
  "output_order": ["T", "F", "P", "A"]
}
```

`I` 对应复数实部，`Q` 对应复数虚部。单个 packet 只来自一个 Station ID，因此 raw
payload 没有 A 轴；`vdif_unpack` 按 `antenna_map[station_id]` 聚合所有天线后生成 TFPA。

表达式语法支持以下来源：

```text
extent = CONST:<positive integer>
       | CONFIG:<config.path>
       | HEADER:<header field>

origin = CONST:<non-negative integer>
       | CONFIG:<config.path>
       | HEADER:<header field>
       | DERIVED:<derivation name>
       | LOOKUP:<map name>:<header field>
```

当前 profile 的轴定义为：

```text
T extent = HEADER:nsamp_per_packet, origin = DERIVED:vdif_frame_time
F extent = HEADER:nchan,             origin = HEADER:first_channel_id
P extent = HEADER:npol,              origin = CONST:0
A extent = CONST:1,                  origin = LOOKUP:antenna_map:station_id
```

上述四组映射是 Project VDIF v1 固定契约，不能在 schema v1 profile 中交换或替换。

`NCHAN` 是 Word 5 中的 UINT8 直接值，范围 1–255，不要求是 2 的幂。Word 2 的
`channel_count_code=31` 只是 project sentinel，不能按标准 VDIF 解释成 `2^31`。

profile loader 会验证 `32 + payload_bytes` 的溢出和 8-byte 对齐。由于 T/F/P extent
来自 header，未来的 `vdif_unpack` 还必须按每个 packet 的实际值交叉验证：

```text
payload_bytes = NSAMP_PER_PACKET * NCHAN * NPOL * complex_sample_bytes
frame_bytes = 32 + payload_bytes
frame_bytes = frame_length_units_8_bytes * 8
```

## 后续解包开发仍需要的材料

1. 至少 2–4 个 FPGA 生成的完整 binary record；
2. 每个 record 的 22 个字段 expected 值；
3. 若干 IQ sample 的 expected `[t,f,p]` 坐标和值；
4. 一组包含全部 Station ID 的同步 packet group 和对应 `antenna_map`；
5. 丢包、重复、乱序和 invalid-data 的期望处理策略。

这些 fixture 将作为 `vdif_unpack` CPU oracle、TFP→TFPA 聚合和后续 CUDA 测试的独立
依据。
