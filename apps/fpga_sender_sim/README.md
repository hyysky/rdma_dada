# fpga_sender_sim

`fpga_sender_sim` 是 Project VDIF v1 的功能测试发送端。一个进程和一份 JSON 只模拟
一个 Station ID；多台服务器使用相同的 destination、packet 和 time 参数，仅修改
`station.station_id`，即可模拟多个阵元向同一接收端发送数据。程序不绑定或限制发送端
IP、MAC 和端口，由操作系统选择源地址。

```bash
cmake --build build --target fpga_sender_sim --parallel
./build/fpga_sender_sim config/fpga_sender_sim.example.json
```

示例采用 CI8、TFP/IQ payload、`512 samples/packet` 和 `1 us/sample`，因此每个 packet
覆盖 `512 us`。32-byte header 加 4096-byte payload 形成 4128-byte UDP datagram，适合
配置的 9000-byte IPv4 path MTU。

## 时间与模式

- `BURST`：按 group index 尽快发送，用于第一版功能和丢包重排测试；
- `REALTIME`：要求所有发送端配置相同、且仍在未来的 `start_utc`，程序等待该 UTC，并按
  `group_index × nsamp_per_packet × sample_interval_ps` 的绝对时间发送；
- `sample_interval_ps` 使用十进制字符串，避免浮点数造成不同服务器的时间 key 不一致；
- `frame_number_within_second` 是当前秒内 packet 起始时间的零基 ordinal，不要求每秒
  group 数为整数。

## 确定性 payload

TFP 中每个复数 sample 按 IQ 分量写入。分量 bit pattern 为：

```text
u = (station_id + 7*group + 3*t + 5*f + 11*p + component)
    mod 2^component_bits
component: I=0, Q=1
```

CI8 写一个 byte；CI16 按 little-endian 写两个 byte。该值按 two's-complement 解释，因此
可以根据 Station、group、T/F/P 在接收端独立计算期望结果。

## 故障注入

- `drop_groups`：不发送指定 group；
- `duplicate_groups`：指定 group 连续发送两次完全相同的 datagram；
- `invalid_header_groups`：将 Project VDIF Word 7 保留字段置为非零。

三个列表必须严格递增、索引小于 `group_count`，且彼此不能重叠。每次 `send` 都必须返回
完整 record 长度，否则进程以失败退出。
