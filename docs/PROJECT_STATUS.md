# 项目模块状态

更新日期：2026-08-28
当前提交基线：`598d11f`；工作树另含尚未提交的多 CUDA stream、compute-ring CUDA
注册、测试 profile 复用和配套测试/文档修改。

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
| `rdma2dada` | 30 Gbps 正式重复门禁已通过 | destination-only flow；1 QP/CQ/线程；NSGE=2 直接写 raw ring；固定 1 秒 drain | 保留 35 Gbps 未通过边界，后续只在接收实现变化时重跑正式基线 |
| `vdif_unpack_worker` | 30 Gbps 正式重复门禁已通过 | coordinator + 固定 parser worker pool + 单 compute writer；有界队列和 window lease/ACK；Station→A、补零、partial/EOD | 完整 GPU 链复用已验收 profile，不重复把 unchanged ingest/unpack 当新模块测试 |
| `pipeline_worker` | 双 Station 小几何 30 Gbps 正式重复门禁已通过 | 保留 `SYNCHRONOUS_DIRECT/1`；新增 1–4 个有界 staged slots，每 slot 独占 output pinned staging、device buffers、non-blocking stream 和事件；compute ring 由 CUDA 直接注册，单 writer 按 block sequence 写 output ring | 后续只在明确实验中做 direct/staged 或 slot-count 匹配对照；GPU-only 暂缓 |
| 分阶段测试控制器 | full profile 集成及正式重复门禁已通过 | 支持 `receive`、`unpack`、`gpu`、`full`；full 继承 qths1 unpack profile，固定 CPU/NUMA、queue、ring/window、source port 和 1 秒 preparation；Full 汇总使用 sender aggregate payload rate | 保持通过配置为基线；只有实现或实验参数变化时才重跑相关拓扑 |
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
| 30 Gbps receive-only，60 s | 正式通过 | 1 warm-up + 3 measured；每轮 accepted=published=54,505,814，CQ/repost/长度错误为 0，固定端口、EOD、cleanup 通过 |
| 30 Gbps receive + unpack，60 s | 正式通过 | 1 warm-up + 3 measured；固定 receiver CPU13、worker CPU14--19、sink CPU20、NUMA1、1 秒 preparation；sender/receiver/unpack/dbnull 精确闭合，missing/late/duplicate/header error 为 0 |
| Receiver 停止排空 | 已通过服务器验收 | 固定 1 秒 drain；四轮 drain 约 1.000 秒，期间 completion/repost 正常，`completions_after_stop=0` |
| 35 Gbps unpack-only，60 s | 未通过 | receiver 接收量约为计划的一半；尚未证明 unpack 是首个饱和阶段 |
| 完整 GPU pipeline 预算 | 已通过服务器验收 | 编译器报告双速率来源、20% deadline、逐级/合计传输和显存；RTX 3090 Release/CUDA 回归 3/3，30 Gbps 当前 deadline 11.184810 ms；不代表实时速率通过 |
| GPU-only 正确性 | 旧单-block 路径已通过；新压力路径暂缓 | 旧证据只覆盖 compute ring→CUDA worker→output ring→dbnull 生命周期；`dada_junkdb`/版本化压力 writer 不作为当前验收入口 |
| GPU-only 持续压力 | 暂缓 | 严格 header、平滑完整-block pacing 和 production geometry writer 尚未收口；恢复前不宣称 ready 或稳定速率 |
| 生产几何 GPU 压力 | 暂缓 | `A≈500,F=4,P=1,B≈350` 仍是未来单 GPU 资源实验，不属于当前双 Station full-chain 基线 |
| 低速完整 GPU pipeline | 已通过服务器验收 | 0.1 Gbps，1 warm-up + 3 measured；双 Station sender、receiver、unpack、GPU、output header/EOD 和 cleanup 每轮精确闭合 |
| 30 Gbps 完整 GPU pipeline，60 s | 正式通过 | 双 Station 小几何，1 warm-up + 3 measured；sender、receiver、unpack、三 slot CUDA、output、dbnull/EOD 和 cleanup 每轮闭合；aggregate payload median=29.999992290 Gbps |
| 500-Station 完整 GPU pipeline | 待输入能力 | 必须由多 Station sender 或合法 raw-VDIF generator 提供约 500 个 Station，覆盖 Station-ID→A、unpack、GPU 和 output；不能用双 Station 结果替代 |
| 低错误率/长时间连续观测 | 待执行 | 错误率目标 0.001%–0.1%；还需 Station 失败、资源恢复和稳态验证 |
| 论文第3章模块证据闭环 | 部分完成 | receive/unpack 正式基线已具备；仍需 GPU 模块 suite 收口、unpack 指标补齐及 1/2/4 worker 匹配比较 |

因此当前最准确的表述是：**30 Gbps receive-only、receive+unpack，以及双 Station
小几何完整 GPU pipeline 均已完成 60 秒、1 warm-up + 3 measured 的正式重复门禁。**
该结论不外推到约 500 Station 生产矩阵、GPU-only 压力、多 GPU 或额外 headroom。
详细证据与边界见 [`VDIF_UNPACK_STATUS.md`](VDIF_UNPACK_STATUS.md)。

## 下一阶段顺序

1. 冻结并复用当前通过的 full profile、source port、CPU/NUMA、preparation、ring/window 和 binary identity；不重复测试未变化的 ingest/unpack。
2. GPU-only 压力暂缓；恢复时先完成严格 header、完整-block pacing 的版本化输入 writer，再做 direct/staged 与 slot-count 匹配实验。
3. 下一项产品开发按既定顺序进入 module registry；之后实现 `pipelinectl`，再开展后续整链和 `dada2rdma`。

具体可执行任务见
[`docs/superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md)。
论文第3章模块级证据与优化比较使用独立计划
[`docs/superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md`](superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md)；
该计划不把第4章完整链路、多 GPU/多节点或端到端 headroom 纳入第3章门禁。

## 当前限制

- 30 Gbps 正式重复证据覆盖 receive-only、receive+unpack，以及双 Station 小几何
  `rdma2dada -> unpack -> pipeline_worker -> output -> dbnull` 完整链。
- 当前 sender/controller 网络 fixture 只覆盖两个 Station；它能测试 aggregate payload 吞吐，
  不能验证约 500 个 Station 的 A 轴映射。GPU-only 可使用 A≈500 的合成 compute blocks，
  但这不是同一条完整链。
- GPU worker 保留单 stream 同步 direct 基线，并新增有界多 stream staged 路径；worker
  在 transfer 生命周期注册整个 compute ring，H2D 直接读取 ring block，不再经过 pinned
  input staging。每个 slot 保留 pinned output，单 writer 等待逻辑 next sequence 完成后
  写 output ring，禁止 CUDA 完成乱序变成 ring 乱序。
- 控制器已能把 NIC/raw/receiver 留在 NUMA1，并从 unpack 开始将 compute/output/GPU 路径
  放到 NUMA0；该能力尚需服务器 A/B 验收，不能提前声称 H2D 已改善。
- 当前 30 Gbps 小几何链为 Beamform → Power → Integrate(K=128, MEAN)：compute/output
  block 为 52,428,800/819,200 B，输入 `T=6,553,600` 可被 128 整除，输出 `T=51,200`；
  到达间隔 13.981013 ms，20% 余量后的整链 deadline 为 11.184810 ms；H2D+D2H 合计
  53,248,000 B/block，最低合计速率 4,760,742,472 B/s。早期单 stream/pageable-ring
  诊断的平均 service/H2D/算法/D2H 为 16.243/12.077/3.873/0.260 ms；随后通过 compute-ring
  CUDA 注册、三 slot staged pipeline 和 active elapsed/summary 口径修复，完成 30 Gbps、
  60 秒正式重复门禁。旧诊断只作为优化前证据，不再代表当前实现状态。
- worker 只接受 block-scoped `ATFP/CI8`；第一版输出格式自动确定：Beamformed 为 CF32，
  Power/Stokes/Integration 为 F32。
- Power 与 Stokes 互斥；Stokes 只允许 `NPOL=2`；Beamformed 不直接积分。
- 增大 ring 只能吸收突发，不能修复稳态服务率不足。
- `DumpToDada()` 是旧实现，不作为 pipeline sink。
