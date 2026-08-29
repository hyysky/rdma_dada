# 项目模块状态

更新日期：2026-08-29
当前开发分支：`codex/pipeline-architecture`；本轮完成多 Station sender、生产几何
Power fixture、多 CUDA stream、compute-ring CUDA 注册和 EOD 驱动的 worker transfer
生命周期，并已通过目标服务器正式验收。

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
| `pipeline_worker` | 469-Station Full Power 与 coherency/Stokes 约 30 Gbps 正式重复门禁均已通过；Power GPU-only direct/1 与 staged/3 匹配门禁也已通过 | 保留 `SYNCHRONOUS_DIRECT/1`；新增 1–4 个有界 staged slots，每 slot 独占 output pinned staging、device buffers、non-blocking stream 和事件；compute ring 由 CUDA 直接注册，单 writer 按 block sequence 写 output ring；读取以 compute-ring EOD 收口 | 后续明确实验为 coherency GPU-only 与高于 30 Gbps 的饱和点，不重复 Full 基线 |
| 分阶段测试控制器 | full profile 集成及正式重复门禁已通过 | 支持 `receive`、`unpack`、`gpu`、`full`；full 继承 qths1 unpack profile，固定 CPU/NUMA、queue、ring/window、source port 和 1 秒 preparation；Full 汇总使用 sender aggregate payload rate | 保持通过配置为基线；只有实现或实验参数变化时才重跑相关拓扑 |
| 测试结果 Catalog | 已完成服务器验收 | 原子导入 compact suite；确定性 JSON/CSV；按拓扑/速率/结果/日期/profile 查询；显式证据提升；测试任务只回传开发任务 | 开发任务维护汇总表；“总结成文”在更新论文时按需查询 |
| `dada2rdma` | 待开发 | 已定义职责边界 | 完成整链验收后设计 processed packetization 与发送路径 |
| `pipelinectl` | 待开发 | 已定义为配置编译、ring/进程生命周期和监控入口，不包含算法 | registry 与整链契约稳定后实现 |

`dada_dbnull -s -z` 用于性能 drain；只有需要检查 `.dada` 内容时才使用
`dada_dbdisk`。二者是外部 PSRDADA consumer，不属于项目算法模块。

## 算法模块状态

| 模块 | 输入 → 输出 | 状态 | 备注 |
| --- | --- | --- | --- |
| `vdif_unpack` | raw VDIF/TFP → payload-only ATFP/CI8 | 功能与约 30 Gbps 性能已正式验收 | CPU 并行解析、单 writer 保序发布；1/2/4 worker-count 对照仍待执行 |
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
| RDMA placement 对照，30 Gbps/60 s | 已完成 | `NSGE=2` direct suite `rdma2dada-30Gbps-60s-20260829T054121Z` 四轮精确闭合；staged-copy suite `rdma2dada-30Gbps-60s-20260829T055947Z` 四轮均出现约 0.54% receiver deficit，作为受控性能失败边界保留，并由 `evidence-adjudications.json` 覆盖旧 runner 的 PASS 判定 |
| 30 Gbps receive + unpack，60 s | 正式通过 | 1 warm-up + 3 measured；固定 receiver CPU13、worker CPU14--19、sink CPU20、NUMA1、1 秒 preparation；sender/receiver/unpack/dbnull 精确闭合，missing/late/duplicate/header error 为 0 |
| Receiver 停止排空 | 已通过服务器验收 | 固定 1 秒 drain；四轮 drain 约 1.000 秒，期间 completion/repost 正常，`completions_after_stop=0` |
| 35 Gbps unpack-only，60 s | 未通过 | receiver 接收量约为计划的一半；尚未证明 unpack 是首个饱和阶段 |
| 完整 GPU pipeline 预算 | 已通过服务器验收 | 编译器报告双速率来源、20% deadline、逐级/合计传输和显存；RTX 3090 Release/CUDA 回归 3/3，30 Gbps 当前 deadline 11.184810 ms；不代表实时速率通过 |
| GPU-only 正确性 | 正式通过 | `gpu_pressure_writer` 原样发布 compiler header；1 Gbps direct/1 与 staged/3 流程闭合，30 Gbps Power 两模式也均完成 writer→GPU→output block/byte/EOD 闭合；Full 路径保持不变 |
| GPU-only 持续压力 | Power direct/1 与 staged/3 正式通过 | direct suite `gpu-30Gbps-60s-20260829T113850Z`、staged suite `gpu-30Gbps-60s-20260829T114602Z`；均为 60 s、1 warm-up + 3 measured 全 PASS/CLEANUP PASS，summary 明确使用 writer 实测速率约 30.36751 Gbps |
| 生产几何 GPU-only 压力 | Power 已通过，coherency 待执行 | Power `A=469,F=4,P=1,B=350` 已完成匹配 direct/1 与 staged/3；staged 实测 `max_inflight=3`，不是单 slot 退化。coherency/Stokes `A=469,F=2,P=2,B=350` 尚未执行 GPU-only 匹配测试 |
| 低速完整 GPU pipeline | 已通过服务器验收 | 0.1 Gbps，1 warm-up + 3 measured；双 Station sender、receiver、unpack、GPU、output header/EOD 和 cleanup 每轮精确闭合 |
| 30 Gbps 完整 GPU pipeline，60 s | 原型正式通过 | `A=2` 小几何，1 warm-up + 3 measured；sender、receiver、unpack、三 slot CUDA、output、dbnull/EOD 和 cleanup 每轮闭合；不能替代论文 `A=469` 主几何验收 |
| 469-Station full Power | 正式通过 | suite `full-30.2505Gbps-60s-20260829T033639Z`；`A=469,F=4,P=1,B=350`、`STAGED_PIPELINE/3`、Beamform→Power→K128 MEAN；60 s、1 warm-up + 3 measured 全部 PASS/CLEANUP PASS，实际 sender aggregate 30.248563937–30.248563939 Gbps，sender/receiver/unpack/GPU/output/EOD 精确闭合 |
| 469-Station full coherency/Stokes | 正式通过 | suite `full-30.2505Gbps-60s-20260829T075447Z`；`A=469,F=2,P=2,B=350`、`STAGED_PIPELINE/3`、Beamform→Stokes→K128 MEAN；60 s、1 warm-up + 3 measured 全部 PASS/CLEANUP PASS，实际 sender aggregate 30.248563934–30.248563938 Gbps，sender/receiver/unpack/GPU/output/EOD 精确闭合；输出 `AA/BB/AB_REAL/AB_IMAG`，reference 另行核对 I/Q/U/V 推导 |
| 约 500-Station 扩展实验 | 非当前门槛 | 当前论文生产主几何固定为 A=469；只有未来明确改变科学输入规模时才新增约 500-Station sender/权重/full 验收 |
| 低错误率/长时间连续观测 | 待执行 | 错误率目标 0.001%–0.1%；还需 Station 失败、资源恢复和稳态验证 |
| 论文第3章模块证据闭环 | 部分完成 | receive/unpack 正式基线已具备；仍需 GPU 模块 suite 收口、unpack 指标补齐及 1/2/4 worker 匹配比较 |

因此当前最准确的表述是：**30 Gbps receive-only、receive+unpack，以及生产几何
Full Power（`A=469,F=4,P=1,B=350`）与 Full coherency/Stokes
（`A=469,F=2,P=2,B=350`）pipeline 均已完成 60 秒、1 warm-up + 3 measured
的正式重复门禁。** 论文两种产品模式的完整链主验收均已有权威 compact 证据；
GPU-only Power 30 Gbps 匹配压力已完成；论文决定性的阶段 P50/P95、GPU/PCIe
利用率、饱和点及 coherency GPU-only 对照仍待补齐。
详细证据与边界见 [`VDIF_UNPACK_STATUS.md`](VDIF_UNPACK_STATUS.md)。

## 下一阶段顺序

1. 冻结并复用已通过的 469-Station Full Power 与 coherency/Stokes profile、source port、CPU/NUMA、preparation、ring/window 和 binary identity；不重复测试未变化的 ingest/unpack。
2. 保留已完成的 RDMA staged-copy 与 NSGE=2 direct placement 匹配对照及其证据裁决，不再为同一结论重跑。
3. 按论文缺口补 coherency GPU-only、阶段 P50/P95、利用率、headroom 和 first-saturated-stage；随后再推进 module registry。

具体可执行任务见
[`docs/superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md)。
论文第3章模块级证据与优化比较使用独立计划
[`docs/superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md`](superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md)；
该计划不把第4章完整链路、多 GPU/多节点或端到端 headroom 纳入第3章门禁。

## 当前限制

- 约 30 Gbps 正式重复证据覆盖 receive-only、receive+unpack，以及 469-Station
  `rdma2dada -> unpack -> pipeline_worker -> output -> dbnull` Full Power 与 Full
  coherency/Stokes 完整链。
- sender/controller 已支持两台物理 sender 按 235/234 Station 分片，并在完整链中验证
  Station ID→A 轴映射；Power 与 coherency/Stokes 的生产几何网络 fixture 均已正式验收。
- GPU worker 保留单 stream 同步 direct 基线，并新增有界多 stream staged 路径；worker
  在 transfer 生命周期注册整个 compute ring，H2D 直接读取 ring block，不再经过 pinned
  input staging。每个 slot 保留 pinned output，单 writer 等待逻辑 next sequence 完成后
  写 output ring，禁止 CUDA 完成乱序变成 ring 乱序。
- 控制器已能把 NIC/raw/receiver 留在 NUMA1，并从 unpack 开始将 compute/output/GPU 路径
  放到 NUMA0；该能力尚需服务器 A/B 验收，不能提前声称 H2D 已改善。
- 生产 Power 链为 `A=469,F=4,P=1,B=350`、Beamform → Power →
  Integrate(K=128, MEAN)，compute/output block 为 49,946,624/582,400 B；生产
  coherency/Stokes 链为 `A=469,F=2,P=2,B=350`，output block 为 1,164,800 B，
  输出 `AA/BB/AB_REAL/AB_IMAG`。早期单 stream/pageable-ring
  诊断的平均 service/H2D/算法/D2H 为 16.243/12.077/3.873/0.260 ms；随后通过 compute-ring
  CUDA 注册、三 slot staged pipeline 和 active elapsed/summary 口径修复，两种生产链均
  完成约 30 Gbps、60 秒正式重复门禁。旧诊断只作为优化前证据，不再代表当前实现状态。
- worker 只接受 block-scoped `ATFP/CI8`；第一版输出格式自动确定：Beamformed 为 CF32，
  Power/Stokes/Integration 为 F32。
- Power 与 Stokes 互斥；Stokes 只允许 `NPOL=2`；Beamformed 不直接积分。
- 增大 ring 只能吸收突发，不能修复稳态服务率不足。
- `DumpToDada()` 是旧实现，不作为 pipeline sink。

## 剩余测试工作

1. Power GPU-only block writer、严格 header 契约及 direct/1 对 staged/3 已收口；
   下一步只补生产 coherency/Stokes 匹配测试及更高压力饱和点。
2. 为 unpack 增加低开销 active service、CPU、queue/ring HWM 指标，执行 1/2/4 parser
   worker 受控比较；该比较称为 worker-count scaling，不称为串行到并行 speedup。
3. 用统一 Chapter 3 module suite 打包现有 CPU/CUDA 数值门禁的三次重复、身份和误差字段。
4. 在所有决定性指标稳定后，仅重跑受指标变更影响的 Full Power/coherency suite，补齐
   stage P50/P95、arrival interval、headroom、利用率和 first saturated stage；未变化的
   receive/unpack 功能基线直接复用。
