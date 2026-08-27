# 项目模块状态

更新日期：2026-08-27
当前代码基线：`b3f8d64` 之后的未提交开发工作树（直接 raw-ring 接收、并行 ATFP unpack、统一 Observation/Resolved Plan 与 GPU worker）

本文是项目当前实现、验收边界和后续顺序的统一入口。数据契约以 `doc/`、`config/`
和各模块 README 为准；历史设计与执行记录保存在 `docs/superpowers/`。

## 状态口径

| 状态 | 含义 |
| --- | --- |
| 已验收 | 对应范围已在目标 Linux/RTX 3090 服务器通过功能、数值或接口验收 |
| 性能验收中 | 功能正确，但持续吞吐、重复性或运行余量尚未达到正式门禁 |
| 已实现，待整链验收 | 模块级测试通过，尚未在目标进程/ring 拓扑完成最终验收 |
| 暂缓 | 已有实现，但当前阶段不继续优化 |
| 待开发 | 只有职责、设计或目录，尚无完整产品实现 |

“已验收”不等于满足实时观测速率。实时能力还必须证明持续吞吐、ring 占用、背压、
丢包、CPU/NUMA/GPU 利用率和安全余量。

## 当前数据流与进程边界

```text
多 Station UDP/Project VDIF
  -> rdma2dada（1 QP/CQ/线程，NSGE=2，直接写 raw-ring record slot）
  -> raw PSRDADA ring（32-byte VDIF header + TFP payload）
  -> vdif_unpack_worker（协调线程 + parser worker pool + 单 writer）
  -> compute PSRDADA ring（payload-only、block-scoped ATFP/CI8）
  -> pipeline_worker
       H2D -> ComplexConvert(ATFP/CI8 -> TFPA/CF32)
       -> Beamform -> [Power | Stokes] -> [TimeIntegrate] -> D2H
  -> output PSRDADA ring
  -> dada_dbnull / dada_dbdisk
  -> dada2rdma（待开发）
```

当前固定为一个 compute ring 和一个 output ring。GPU 算法在同一个 `pipeline_worker`
进程中组合，中间 GPU buffer 不设置 PSRDADA ring 边界。

## 配置与数据契约

| 组件 | 状态 | 已完成 | 后续 |
| --- | --- | --- | --- |
| Observation JSON | 已验收 | 单一用户配置入口；显式 conversion scale；Station、packet、ring、算法链和积分参数 | 按观测需求扩展更多输入/输出格式 |
| Resolved Observation Plan | 已验收 | 严格解析、checked geometry、CONFIG_ID/GEOMETRY_ID、跨进程统一契约 | 接入未来 `pipelinectl` 生命周期编排 |
| Observation compiler | 已验收 | 生成 resolved/ring plan、validation report、各 stage 的 4096-byte DADA header 和 SHA256 manifest | 新 stage 随契约扩展 |
| Project VDIF v1 | 已验收 | 固定 32-byte header；Station ID、首通道、NCHAN、NPOL；TFP/IQ/TWOS_COMPLEMENT payload | FPGA 到位后做 wire 兼容性回归 |
| DADA header/ring adapter | 已验收于现有入口 | header 传播、HDR_SIZE 互操作、block/EOD/capacity 生命周期 | 长时间连续观测和异常恢复验收 |

## 应用状态

| 应用 | 状态 | 当前能力 | 下一步 |
| --- | --- | --- | --- |
| `fpga_sender_sim` | 已验收 | 多服务器模拟 Station；Project VDIF；source port；`sendmmsg`；定速及错误注入 | 用于重复速率和异常观测验收 |
| `rdma2dada` | 阶段性能按满足 | destination-only flow；1 QP/CQ/线程；NSGE=2 直接写 raw ring；用户决定暂以 30 Gbps payload 进入下一阶段 | 正式 warm-up+3 与少量 admission deficit 复核延期，不删除原始证据 |
| `vdif_unpack_worker` | 阶段性能按满足 | coordinator + 固定 parser worker pool + 单 compute writer；有界队列和 window lease/ACK；Station→A、补零、partial/EOD；用户决定暂以 30 Gbps payload 进入下一阶段 | 正式重复门禁延期；完整 GPU 链不得复用 unpack-only 结论 |
| `pipeline_worker` | 功能及低速整链已验收，整链性能待确认 | 单 compute ring→单 output ring；Resolved Plan；H2D→转换→Beamform→Power/Stokes→可选积分→D2H；编译期 GPU budget | 按预算执行完整数据流持续速率验收 |
| 分阶段测试控制器 | 分区 NUMA 开发完成，服务器待验收 | 同一 resolved plan 下支持 `receive`、`unpack`、`gpu`、`full`；`ingress_numa_node` 控制 raw ring/receiver，`processing_numa_node` 控制 unpack、compute/output ring、GPU worker 和 sink；保留单 `numa_node` 兼容路径 | 在 qths1 对当前全 NUMA1 基线与 ingress=1/processing=0 做严格匹配比较 |
| 测试结果 Catalog | 已完成服务器验收 | 原子导入 compact suite；确定性 JSON/CSV；按拓扑/速率/结果/日期/profile 查询；显式证据提升；测试任务只回传开发任务 | 开发任务维护汇总表；“总结成文”在更新论文时按需查询 |
| `dada2rdma` | 待开发 | 已定义职责边界 | 完成整链验收后设计 processed packetization 与发送路径 |
| `pipelinectl` | 待开发 | 已定义为配置编译、ring/进程生命周期和监控入口，不包含算法 | registry 与整链契约稳定后实现 |

`dada_dbnull -s -z` 用于性能 drain；只有需要检查 `.dada` 内容时才使用
`dada_dbdisk`。二者是外部 PSRDADA consumer，不属于项目算法模块。

## 算法模块状态

| 模块 | 输入 → 输出 | 状态 | 备注 |
| --- | --- | --- | --- |
| `vdif_unpack` | raw VDIF/TFP → payload-only ATFP/CI8 | 功能已验收，性能验收中 | CPU 并行解析、单 writer 保序发布 |
| `host_to_device` | host bytes → device bytes | 已验收 | 异步 CUDA copy，由 worker stream 调用 |
| `complex_convert` | ATFP CI8/CI16 → TFPA CF32 | 已验收 | CUDA fused transpose/convert/scale；worker 第一版输入 CI8 |
| `beamform` | TFPA CF32 + FPAB weights → TFPB CF32 | 已验收 | CPU oracle；CUDA FP32/TF32；NPY 权重；A→B |
| `power` | TFPB CF32 → TFPB F32 | 已验收 | 实部平方加虚部平方；CPU/CUDA |
| `stokes` | TFPB CF32, P=2 → TFBS F32 | 已验收 | AA、BB、AB_REAL、AB_IMAG；CPU/CUDA |
| `time_integrate` | TFPB/TFBS F32 → 缩短 T 的 F32 | 已验收；优化暂缓 | SUM/MEAN；block-local；性能优化待整链证据驱动 |
| `device_to_host` | device bytes → host bytes | 已验收 | 异步 CUDA copy，由 worker stream 调用 |
| 通用 module registry | 配置模块列表 → 兼容链 | 待开发 | 当前 worker 使用固定合法链；registry 在整链基线后开发 |

## 测试与性能边界

| 验收范围 | 状态 | 结论 |
| --- | --- | --- |
| macOS portable 与 CPU reference | 已通过 | 开发回归，不替代服务器验收 |
| RTX 3090 CUDA 单模块与组合数值 | 已通过 | Convert、Beamform、Power、Stokes、TimeIntegrate、H2D/D2H |
| Resolved Plan + PSRDADA + CUDA worker integration | 已通过 | 三次 clean repetition；header/geometry/EOD/cleanup 通过 |
| ATFP unpack 功能与 ring integration | 已通过 | full/partial/连续 transfer、补零、ATFP bytes、header/EOD |
| 旧串行/TFPA 路径 | 已淘汰 | 实时性不足，已由 ATFP + 并行 unpack + direct receive 替代 |
| 30 Gbps unpack-only，30/60 s | 单次探索通过 | sender、receiver、unpack、dbnull 在成功 run 中精确闭合；不含 GPU worker |
| 30 Gbps 重复性能门禁 | 延期 | 两个 repeat suite 分别出现 251/700 包的 receiver/NIC admission 缺口；用户决定当前阶段按满足推进，但未将其升级为正式稳定速率 |
| Receiver 停止排空 | 本地开发完成，服务器待验收 | 由 4096 次空 CQ 轮询改为固定 1 秒 drain；期间继续处理 completion、发布 raw block 和重投递 WR，仅保留三个退出汇总指标 |
| 35 Gbps unpack-only，60 s | 未通过 | receiver 接收量约为计划的一半；尚未证明 unpack 是首个饱和阶段 |
| 完整 GPU pipeline 预算 | 已通过服务器验收 | 编译器报告双速率来源、20% deadline、逐级/合计传输和显存；RTX 3090 Release/CUDA 回归 3/3，30 Gbps 当前 deadline 11.184810 ms；不代表实时速率通过 |
| GPU-only 正确性 | 旧单-block 路径已通过；新压力路径待验收 | 旧证据覆盖 compute ring→CUDA worker→output ring→dbnull 生命周期；新 `dada_junkdb` 路径按任意目标速率向上取整为整 block/秒，并要求逐 block 输入/输出计数闭合 |
| GPU-only 持续压力 | 本地开发完成，服务器待验收 | `dada_junkdb`→compute ring→GPU worker→output ring→dbnull；保存实际注入速率、worker service/output wait、CUDA H2D/算法/D2H 和进程账本。HF 恢复后按 passing profile 做 warm-up+3；未验收前无稳定速率结论 |
| 生产几何 GPU 压力 | 配置/验收规划中 | sender 暂不扩展；GPU-only 使用 `A≈500,F=4,P=1,B≈350` 的 ATFP header、block 和 FPAB 权重，约 30 Gbps 基线另测 `A=500` 的 32 Gbps 点。该测试先证明单 GPU 的真实矩阵、传输和显存能力，不宣称 500-Station 网络整链通过 |
| 低速完整 GPU pipeline | 已通过服务器验收 | 0.1 Gbps，1 warm-up + 3 measured；双 Station sender、receiver、unpack、GPU、output header/EOD 和 cleanup 每轮精确闭合 |
| 500-Station 完整 GPU pipeline | 待输入能力 | 必须由多 Station sender 或合法 raw-VDIF generator 提供约 500 个 Station，覆盖 Station-ID→A、unpack、GPU 和 output；不能用双 Station 结果替代 |
| 低错误率/长时间连续观测 | 待执行 | 错误率目标 0.001%–0.1%；还需 Station 失败、资源恢复和稳态验证 |
| 论文第3章模块证据闭环 | 规划完成，待执行 | 已审计可复用测试和缺口；需模块 suite 三次 clean、receive/unpack 正式基线、unpack 指标补齐及 1/2/4 worker 匹配比较。HF 恢复前不运行远程测试 |

因此当前最准确的表述是：**30 Gbps 是 receive/unpack 阶段用于后续开发的临时 payload
基线，并有 unpack-only 单次精确闭合证据；正式可重复门禁已延期，不等同于完整 GPU
pipeline 的稳定速率。**
详细证据与边界见 [`VDIF_UNPACK_STATUS.md`](VDIF_UNPACK_STATUS.md)。

## 下一阶段顺序

1. 生成并校验 `A≈500,F=4,P=1,B≈350` Observation、block geometry 和 FPAB 权重。
2. 先比较全 NUMA1 与 ingress=1/processing=0，确认 compute ring 本地化对 H2D 和 unpack 的净收益。
3. NUMA 对照通过后，再根据 H2D/GPU/D2H/output-wait 证据实现并比较多 stream/inflight block 模式。
4. GPU 可行后再决定开发多 Station sender，或先用合法 raw-VDIF generator 验证 unpack+GPU。
5. 具备约 500 Station 输入后执行 `full` 重复门禁；随后开发 registry、`pipelinectl` 和 `dada2rdma`。
6. 在正式发布 ingest/unpack 性能结论前，恢复延期的 30 Gbps receive/unpack 重复门禁。

具体可执行任务见
[`docs/superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md)。
论文第3章模块级证据与优化比较使用独立计划
[`docs/superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md`](superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md)；
该计划不把第4章完整链路、多 GPU/多节点或端到端 headroom 纳入第3章门禁。

## 当前限制

- 30 Gbps 证据只覆盖 `rdma2dada -> raw ring -> vdif_unpack_worker -> compute ring -> dbnull`。
- 当前 sender/controller 网络 fixture 只覆盖两个 Station；它能测试 aggregate payload 吞吐，
  不能验证约 500 个 Station 的 A 轴映射。GPU-only 可使用 A≈500 的合成 compute blocks，
  但这不是同一条完整链。
- GPU worker 每个 transfer 使用一条 non-blocking stream，但提交 output block 前仍同步；
  尚无跨 block 双 buffer/event overlap。
- 控制器已能把 NIC/raw/receiver 留在 NUMA1，并从 unpack 开始将 compute/output/GPU 路径
  放到 NUMA0；该能力尚需服务器 A/B 验收，不能提前声称 H2D 已改善。
- 当前 30 Gbps 小几何链为 Beamform → Power → Integrate(K=128, MEAN)：compute/output
  block 为 52,428,800/819,200 B，输入 `T=6,553,600` 可被 128 整除，输出 `T=51,200`；
  到达间隔 13.981013 ms，20% 余量后的整链 deadline 为 11.184810 ms；H2D+D2H 合计
  53,248,000 B/block，最低合计速率 4,760,742,472 B/s。1 Gbps full-chain 已精确闭合；
  30 Gbps 单次诊断平均 service 为 16.243 ms，其中 H2D/算法/D2H 为
  12.077/3.873/0.260 ms，output wait 可忽略。输出压缩有效，但当前单 stream 路径未达到
  30 Gbps 稳态要求；同时 receiver 仅发布计划包数的约 19.12%，完整链路性能未通过。
- worker 只接受 block-scoped `ATFP/CI8`；第一版输出格式自动确定：Beamformed 为 CF32，
  Power/Stokes/Integration 为 F32。
- Power 与 Stokes 互斥；Stokes 只允许 `NPOL=2`；Beamformed 不直接积分。
- 增大 ring 只能吸收突发，不能修复稳态服务率不足。
- `DumpToDada()` 是旧实现，不作为 pipeline sink。
