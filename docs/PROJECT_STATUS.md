# 项目模块状态

更新日期：2026-08-10
当前已验收基线：`872e734`（Observation/Resolved Plan、ATFP 转换和统一 GPU worker）

本文是项目当前实现状态的汇总入口。详细数据契约仍以 `doc/`、`config/` 和各模块
README 为准；历史设计和执行记录保存在 `docs/superpowers/`，不作为当前状态判断依据。

## 状态口径

| 状态 | 含义 |
| --- | --- |
| 已验收 | 功能、数值或接口已在目标 Linux/GPU 服务器按对应验收范围通过 |
| 性能验收中 | 功能正确，但持续实时数据流吞吐和运行余量尚未确认 |
| 已实现，待整链验收 | 模块级测试通过，尚未在目标进程/ring 拓扑中完成最终验收 |
| 暂缓 | 已有实现或修改，但当前阶段不继续开发或不纳入提交 |
| 待开发 | 只有目录、README、设计或计划，尚无可运行产品实现 |

“已验收”不自动代表满足实时观测速率。实时能力必须单独证明持续吞吐、ring 占用、
背压、丢包、CPU/NUMA/GPU 利用率和运行余量。

## 当前端到端数据流

```text
多 Station UDP/Project VDIF
  -> rdma2dada
  -> raw PSRDADA ring（32-byte header + TFP payload）
  -> vdif_unpack_worker
  -> compute PSRDADA ring（payload-only ATFP/CI8）
  -> pipeline_worker
       H2D -> ComplexConvert(ATFP/CI8 -> TFPA/CF32)
       -> Beamform -> [Power | Stokes] -> [TimeIntegrate] -> D2H
  -> output PSRDADA ring
  -> dada_dbnull / dada_dbdisk（测试或落盘）
  -> dada2rdma（待开发）
```

当前固定使用一个 compute ring 和一个 output ring。GPU 算法模块在同一
`pipeline_worker` 进程内组合，中间 GPU buffer 不设置 PSRDADA ring 边界。

## 配置、数据契约和基础设施

| 组件 | 当前状态 | 已具备能力 | 未完成事项 |
| --- | --- | --- | --- |
| Observation JSON | 已验收 | 单一用户配置入口；显式 conversion scale；ring、Station、packet、算法链和积分参数 | 后续按观测需求扩展更多输出格式和转换模式 |
| Resolved Observation Plan | 已验收 | 严格解析、checked geometry、CONFIG_ID/GEOMETRY_ID、单一跨进程运行契约 | 与未来 `pipelinectl` 的完整生命周期编排集成 |
| Observation compiler | 已验收 | 生成 resolved plan、ring plan、validation report、RAW/UNPACKED/CONVERTED/BEAMFORMED/output 4096-byte headers 和 SHA256 manifest | 无当前阻断项 |
| Project VDIF v1 | 已验收 | 固定 32-byte/8-word header；Station ID、首通道、NCHAN、NPOL；TFP/IQ/TWOS_COMPLEMENT payload | 实际 FPGA 到位后的兼容性回归 |
| DADA header codec | 已验收 | transfer header 传播、阶段 header 编译、标准 `HDR_SIZE=4096` 互操作 | 新增模块时补充对应 header transform |
| PSRDADA ring adapter | 已验收于现有入口 | header/data block、EOD、capacity 和 reader/writer 生命周期 | 持续观测和异常恢复需随整链继续验证 |

## 应用状态

| 应用 | 当前状态 | 已完成 | 下一步 |
| --- | --- | --- | --- |
| `fpga_sender_sim` | 已验收 | 多服务器分别模拟 Station；Project VDIF；source port；`sendmmsg`；定速发送和错误注入基础能力 | 配合 ATFP 整链完成目标 payload 速率阶梯和低错误率测试 |
| `rdma2dada` | 已验收，整链性能待确认 | 只匹配接收端 MAC/IP/port；CQ 校验/repost；partial raw block；accepted=published/EOD；Resolved Plan 入口 | 在新版 ATFP 整链中重新测量 sustained receive、ring occupancy 和 headroom |
| `vdif_unpack_worker` | 性能验收中 | 独立 raw→compute 进程；Station→A；时间对齐；缺失补零；payload-only block-scoped ATFP；partial/EOD；严格 plan/header/capacity 门禁 | 完成当前 1–40 Gbps 阶梯，定位首个饱和阶段；随后决定进一步优化 |
| `pipeline_worker` | 已验收，整链性能待确认 | 单 compute ring→单 output ring；Resolved Plan；H2D→转换→Beamform→Power/Stokes→积分→D2H；header/EOD/capacity 门禁 | 完成与 ingest/unpack 同时运行的吞吐、占用和跨 block overlap 评估 |
| `dada2rdma` | 待开发 | 已有目录和职责说明 | 定义 processed data packetization、header 更新、UDP/RDMA 输出和验收方式 |
| `pipelinectl` | 待开发 | 已有职责边界：不包含算法 | 实现配置编译、拓扑/reader count 校验、ring 创建、进程启动顺序、健康监控、停止和定向清理 |

`dada_dbdisk` 和 `dada_dbnull` 是外部 PSRDADA consumer，不属于本项目算法模块。
性能测试默认使用 `dada_dbnull -s -z`，只有需要检查落盘内容时使用 `dada_dbdisk`。

## 算法模块状态

| 模块 | 输入 → 输出 | 当前状态 | 备注 |
| --- | --- | --- | --- |
| `vdif_unpack` | raw Project VDIF/TFP → payload-only ATFP/CI8 | 功能已验收，性能验收中 | CPU 进程模块；当前重点是避免小粒度复制并验证持续吞吐 |
| `host_to_device` | host bytes → device bytes | 已验收 | 异步 CUDA copy，不拥有 buffer；由 worker stream 调用 |
| `complex_convert` | ATFP CI8/CI16 → TFPA CF32 | 已验收 | CPU reference + CUDA fused transpose/convert/scale；第一版 worker 输入为 CI8 |
| `beamform` | TFPA CF32 + FPAB complex weights → TFPB CF32 | 已验收 | CPU reference；CUDA FP32/TF32；NPY 权重；A→B |
| `power` | TFPB CF32 → TFPB F32 | 已验收 | 实部平方加虚部平方；CPU/CUDA |
| `stokes` | TFPB CF32, P=2 → TFBS F32 | 已验收 | `AA, BB, AB_REAL, AB_IMAG`；CPU/CUDA |
| `time_integrate` | TFPB/TFBS F32 → 缩短 T 的同布局 F32 | 已验收；优化暂缓 | SUM/MEAN；单 block 输入/输出；已有 CPU/CUDA 实现。当前未提交的 benchmark/优化不纳入基线 |
| `device_to_host` | device bytes → host bytes | 已验收 | 异步 CUDA copy，不拥有 buffer；由 worker stream 调用 |
| 通用 module registry | 配置模块列表 → 兼容链 | 待开发 | 指 GPU/CPU 算法模块的注册、构造、顺序和契约检查；当前 worker 使用固定合法链 |

算法模块的“已验收”主要指 reference/CUDA 数值、边界和 worker 组合测试通过；除当前
ATFP campaign 外，尚未为每个模块给出整链目标速率下的独立 service-time 预算。

## 测试和性能状态

| 验收范围 | 状态 | 结论 |
| --- | --- | --- |
| macOS portable 配置、header、CPU reference | 已通过 | 用于开发回归，不替代服务器验收 |
| RTX 3090 CUDA 单模块 correctness | 已通过 | H2D/D2H、ComplexConvert、Beamform、Power、Stokes、TimeIntegrate |
| GPU worker 数值组合 | 已通过 | Beamform-only、Beamform+Power、Beamform+Stokes，以及 Power/Stokes 后积分 |
| Resolved Plan + PSRDADA + CUDA worker integration | 已通过 | 三次 clean repetition；动态 ring key；header/geometry/EOD/cleanup 门禁 |
| ATFP unpack 功能与 ring integration | 已通过 | full/partial/双 transfer、零填充、ATFP 字节、header/EOD |
| 旧 TFPA unpack 速率路径 | 不满足需求，已停止 | Release 版本 1 Gbps 通过、2 Gbps 首点失败；该结论促成 ATFP 重设计 |
| 新 ATFP 完整 pipeline 速率阶梯 | 进行中 | “unpack优化”任务执行 1–40 Gbps payload 目标速率 campaign；完成前不声明实时能力 |
| 低错误率测试 | 待执行 | 目标错误率 0.001%–0.1%，在极限稳定速率附近验证统计与连续运行行为 |
| 长时间连续观测 | 待执行 | 验证多 transfer、Station 参与、EOD/重启策略、ring 稳态和无资源泄漏 |

## 后续开发顺序

1. 完成新 ATFP 整链速率 campaign，确定最高稳定 payload 速率、首个饱和阶段和安全余量。
2. 若性能门禁通过，提交并推送当前 unpack/controller 改动；若未通过，先针对证据中的
   饱和阶段优化并重复验收。
3. 开发通用 module registry，使算法组合从固定分支迁移为受契约约束的配置组合。
4. 开发 `pipelinectl`，统一管理 artifact、ring、进程、监控、失败中止和清理。
5. 运行完整持续观测数据流验收，包括正确包、低错误率、Station 启动失败和长时间运行。
6. 开发 `dada2rdma` 及后续输出链路。

## 当前明确限制

- 当前 GPU worker 每个 transfer 使用一条 non-blocking stream，但在提交每个 output block
  前同步；尚无双 buffer/event 的跨 block H2D/计算/D2H overlap。
- 当前 worker 只接受 block-scoped `ATFP/CI8` 输入，输出格式第一版由产品自动确定：
  Beamformed 为 CF32，Power/Stokes/Integration 为 F32。
- Power 与 Stokes 为互斥分支；Stokes 只允许 `NPOL=2`；Beamformed 不直接积分。
- ring 增大只能吸收突发，不能替代稳态处理速率不足的修复。
- `DumpToDada()` 是旧实现，不作为 pipeline sink。
