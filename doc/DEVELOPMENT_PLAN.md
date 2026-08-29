# Pipeline development plan

更新日期：2026-08-29

本文记录当前里程碑、进入条件和开发顺序。模块轴、数值及 header 契约以
[`ALGORITHM_MODULE_CONTRACTS.md`](ALGORITHM_MODULE_CONTRACTS.md) 为准；实时性能要求和
服务器验收规则以 [`../docs/agents/testing.md`](../docs/agents/testing.md) 为准。

## 当前基线

已经实现并完成对应服务器功能/数值验收：

- Observation JSON、Resolved Observation Plan、artifact compiler 和 stage DADA headers；
- 固定 32-byte Project VDIF v1 与 Station→A/TFP wire contract；
- NSGE=2 直接写 raw ring 的 `rdma2dada`；
- coordinator + parser worker pool + sole writer 的并行 ATFP unpack；
- fused ATFP/CI8→TFPA/CF32 conversion 与显式 scale；
- H2D/D2H、Beamform、Power、Stokes、Time Integration 的 CPU/CUDA 实现；
- 单 compute ring→单 output ring 的 Resolved-Plan-driven `pipeline_worker`；
- module/CUDA 数值、PSRDADA 生命周期、header/geometry/EOD 的重复集成测试；
- 四阶段版本化控制器，以及 0.1 Gbps full-stage 正确性验收；
- compute-ring CUDA 直接注册、三 slot staged pipeline、有序单 writer 和 active throughput
  指标；
- 生产几何 `A=469,F=4,P=1,B=350` Full Power、约 30 Gbps、60 秒、
  1 warm-up + 3 measured 正式门禁。

receive-only、receive+unpack 与 469-Station Full Power GPU pipeline 均已完成约 30 Gbps、
60 秒、1 warm-up + 3 measured 的正式重复门禁。权威 Power suite 为
`full-30.2505Gbps-60s-20260829T033639Z`。GPU-only 生产几何压力暂缓，当前结果不外推到
coherency/Stokes、多 GPU 或额外 headroom。

## 所有模块的输入输出门禁

任何新模块或行为修改必须同时满足：

1. 明确输入和输出的 `DATA_STAGE`、`ORDER`、`SAMPLE_FORMAT`、`MEMORY`、shape 和 backend；
2. 明确保留、修改和删除的 header 字段，不无说明地丢失 observation metadata；
3. 给出唯一 checked geometry：frame/block/ring/intermediate bytes，拒绝溢出或不整除；
4. 给出数值类型、scale、累计精度、误差和独立 CPU/手算 oracle；
5. 配置与 header 重复字段必须一致，否则在创建资源或发布 output header 前失败；
6. 覆盖错误 order/dtype/shape、容量不足、非法顺序、partial/EOD 和资源清理；
7. 记录目标 payload rate、packet rate、block deadline、运行时长和安全余量；
8. 在真实进程/ring 边界测量 service time、占用、等待和首个饱和阶段；isolated kernel
   correctness 或增大 ring 不能替代实时验收。

新增 JSON 参数时，必须同步 schema/parser、example/compiler、resolved plan、header
transform、检查工具和边界测试；可推导值不重复配置。

## Milestone 1：ingest/unpack 稳态门禁（已完成）

目标：把直接接收与并行 unpack 从单次探索结果提升为可重复的 30 Gbps 稳态证据。

- receive-only：30 Gbps，1 warm-up + 3 measured，每次 60 s；
- unpack-only：相同重复方式和持续时间；
- 记录 NIC delta、CQ/poll/repost、accepted/published、ring occupancy、线程 affinity、
  parse/copy/writer service time 和完整计数闭合；
- 若 receive-only 失败，先处理 receiver/NIC admission；不通过增加 unpack window 掩盖；
- 若 receive-only 通过而 unpack-only 失败，才根据 worker/writer service time 优化 unpack。

完成标准：每次 measured 都精确闭合、clean EOD/cleanup，无持续 ring full，且所有
machine-readable artifacts 可由版本化 runner 重现。

2026-08-26 验收结果：receive-only 与 receive+unpack 均以固定 source port、CPU/NUMA、
queue、ring/window 和 preparation profile 完成 1 warm-up + 3 measured。receive-only
结果根为 `/home/user/wy/task8c-drain-results/drain-receive-30Gbps-60s-20260826-r2`；
unpack 结果根为 `/home/user/wy/task8c-unpack-results/unpack-30Gbps-60s-20260826-r3`。
后续 GPU/full 测试继承该 profile；ingest/unpack 未变化时不从头重复此门禁。

## 论文第3章模块验证闭环（待执行）

论文第3章只覆盖模块级功能、数值、性能和匹配优化比较；完整
`rdma2dada -> unpack -> GPU worker -> output` 持续性能、多 GPU/多节点扩展和端到端
headroom 属于第4章，不作为第3章门禁。

第3章待完成事项：

1. 将配置/VDIF、receive、parallel unpack 和 GPU 算法/传输测试收口为三次连续 clean、
   带 SHA256 身份和紧凑 JSON 的模块 suite；
2. HF 恢复后先只读审计历史结果根目录，可验证则复用，缺失身份、重复次数或 manifest
   时按当前契约重跑，不从对话摘要重造证据；
3. 补充 unpack parse/copy/raw-block critical-path service time、进程 CPU/NUMA、ring pressure、
   writer wait/HWM 等结构化指标；
4. 复用已经通过的 30 Gbps receive/unpack 正式 suite；只有实现、profile 或硬件边界变化
   时才重跑，并明确记录 profile diff；
5. 在同一正式基线下仅改变 parse/copy worker 数量，比较 1/2/4 workers。该结果只称为
   worker-count scaling，不冒充 serial/reference speedup；
6. 论文总结只读取 suite 的 `preflight.json`、`summary.json` 和 `runs/*.json`，每个结论
   绑定精确路径；`evidence.log` 仅作原始行审计，失败才查看 `debug/`。

Stokes 的论文与工程门禁固定为 `AA`、`BB`、`AB_REAL`、`AB_IMAG` 四个相关产物；当前
不单独发布 I/Q/U/V 数组。数值 reference 需要额外核对
`I=AA+BB`、`Q=AA-BB`、`U=2*AB_REAL`、`V=-2*AB_IMAG`，但这不改变工程输出契约。

详细执行计划见
[`../docs/superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md`](../docs/superpowers/plans/2026-08-24-chapter3-module-validation-evidence.md)。

## Milestone 2：完整 GPU pipeline 基线验收（已完成）

目标：验证真实边界 `UDP -> raw -> unpack -> compute -> GPU worker -> output -> dbnull`。

执行前先从 Resolved Plan 计算：

- 每秒 raw/compute/output bytes 和 packet rate；
- raw/compute/output block arrival deadline；
- ATFP→CF32 扩张、beam count、Power/Stokes 和积分对输出几何的影响；
- 每 block H2D/kernel/D2H 预算、显存峰值和必要安全余量。

预算编译已实现：默认使用 Observation 速率，性能 runner 显式覆盖目标 payload 速率并
保留双速率来源；第一版使用 20% deadline/显存余量。现有小几何 30 Gbps 使用
Beamform → Power → Integrate(K=128, MEAN)，整链 deadline 为 11.184810 ms；每 block 的
时间采样由 6,553,600 降到 51,200，最终 output block 为 819,200 B，计划/建议空闲显存为
577,536,064/693,043,277 B。RTX 3090 fresh Release 构建、预算字段和相关 CUDA 回归已
通过。早期 30 Gbps 单次诊断的平均 service/H2D/算法/D2H 为
16.243/12.077/3.873/0.260 ms，暴露 pageable ring H2D 与逐 block 同步问题。当前实现已用
`dada_cuda_dbregister` 注册 compute ring，由三 slot staged pipeline 直接 H2D，并按 active
区间记录吞吐；生产几何 `A=469,F=4,P=1,B=350` Full Power 已在约 30 Gbps、60 秒、
1 warm-up + 3 measured 正式通过。worker 以 compute-ring EOD 结束逻辑 transfer，避免
精确整 block 的有限观测在 EOD 发布前因 `TRANSFER_SIZE` 字节上限误开 continuation。

第一步 NUMA 优化已完成控制器开发：`ingress_numa_node` 绑定 raw ring 和
`rdma2dada`，`processing_numa_node` 绑定 unpack、compute/output ring、GPU worker 和 sink。
目标服务器实验固定为 ingress=1、processing=0；旧 `numa_node` 继续作为同节点兼容参数，
禁止与分区参数混用。服务器验收必须与全 NUMA1 基线保持其余速率、几何、线程和二进制
身份一致，分别比较 receiver/unpack 闭合、H2D、算法、D2H、总 service 和清理结果。

GPU-only 生产几何压力当前暂缓。恢复时仍按以下边界执行：

1. 网络侧复用当前两台 sender 的 235/234 Station 分片和已通过的约 30 Gbps profile；
   不重新测试未变化的 receive/unpack 边界。
2. GPU 侧先完成 repository-owned、严格 header、完整-block 平滑 pacing 的输入 writer，
   再采用接近生产的 `A≈500,F=4,P=1,B≈350` ATFP/FPAB 几何；现有 `dada_junkdb`
   路径不作为正式验收入口。
3. GPU-only 必须使用匹配权重和真实输出几何，记录 H2D、转换、Beamform、可选产品、D2H、
   output wait、显存和逐 block 闭合；合成填充值只影响科学数值，不降低计算/传输负载。
4. GPU-only 结果称为“生产几何 GPU 压力”，与已经通过的 469-Station Full Power 完整链
   分开记录；前者用于隔离 GPU 资源边界，后者证明 Station-ID→A、时间对齐、unpack 和 GPU
   的联合闭合。

`A=2` 小几何 full 结果保留为原型证据；最终 Power 主验收已经升级为 469-Station
生产几何。多 stream 已实现为 1--4 个有界 staged slots；当前正式基线使用 staged/3、
compute-ring CUDA 直注册、有序 output writer 和 EOD 驱动的输入 transfer 生命周期。
accepted unpack profile、固定 source port、CPU/NUMA、1 秒 preparation、ring/window 均已
复用，未重新把 unchanged ingest/unpack 当新模块测试。

GPU-only 恢复后的任务：

1. 对生产几何 GPU-only 做小速率 known-data/geometry correctness 连续三次；
2. 以 dbnull drain output ring，执行 1/5/10/.../30/32 Gbps 的 warm-up+3 阶梯；
3. 比较 `SYNCHRONOUS_DIRECT/1` 与 `STAGED_PIPELINE/3`，按 H2D/GPU/D2H、slot/writer wait、
   max inflight 和顺序发布证据定位瓶颈；split-NUMA 仅作为独立命名实验；
4. 多 Station 输入就绪后，重复 `full` 阶段并核对全部 Station、block、header 和 EOD；
5. 只有 `full` 的全部 measured 通过，才能称该 rate 为完整 pipeline 稳定速率。

论文后续正式测试统一采用 physical Ethernet line rate 为主速率，并同时记录 UDP
datagram/payload 与 astronomical signal payload。最终主几何固定为
`A=469,F=4,P=1,B=350`、每 F 1 MHz、两台 sender 按 235/234 Stations 分片。Power 与
coherency 必须分别完成 GPU-only 和 full 验收。正式时长采用 30 秒还是 60 秒、以及是否
执行完整 `1/5/10/20/30/35/40` 阶梯仍待用户确认，确认前不启动新的正式 campaign。

当前 Power Full 输入由 `generate_gpu_pressure_fixture.py` 生成：
`STAGED_PIPELINE/3`、Beamform→Power→K128 MEAN Integration、T=13,312、积分后
T=104、output block=582,400 B。runner 保持既有 aggregate VDIF-record rate 语义；约
30 Gbps 主点允许采用几何导出的实际速率，不为精确 30.000 Gbps 增加新的速率接口。

完成标准：给出最高可重复 payload 速率、每级 service-time 预算和明确运行 headroom。

## Milestone 3：通用 module registry

目标：以 registry/factory 替换当前固定合法链，但不放宽观测约束。

- 配置表达有序算法列表；
- factory 构造模块并逐级调用 header/geometry contract；
- 自动检查 stage/order/dtype/memory/shape；
- 前缀固定为 convert→beamform；Power/Stokes 互斥；Integration 仅跟合法实数产品；
- 在创建 output ring 前计算全部中间和最终 buffer；
- 非法链报告模块名和不兼容字段。

完成标准：当前所有合法链由 registry 表达，错误链在资源创建前拒绝，并重复完整 GPU
correctness 回归。

## Milestone 4：pipelinectl

目标：用一个用户入口管理 artifact、ring 和进程生命周期；`pipelinectl` 不执行算法。

- 编译 Observation JSON 并核验 manifest；
- 校验 ring keys、block geometry、reader count 和进程拓扑；
- 按 consumer→worker→receiver 顺序启动并等待 readiness；
- 监控 Station、receiver、worker、sink 和 EOD；任一 mandatory Station/进程失败则停止整链；
- 提供 start/status/stop、定向 PID/key 清理和 machine-readable observation ledger；
- 不通过模糊进程名 kill，不接管 Git 或远端部署。

完成标准：单条命令能重复启动、监控和停止完整观测，失败路径不遗留进程、ring 或
capability。

## Milestone 5：持续观测和异常验收

- 多 transfer 与长时间运行；
- 0.001%–0.1% 低错误率、duplicate/late/bad packet 和 zero-fill 统计；
- mandatory Station 启动失败/提前退出时中止完整 transfer；
- header/EOD、下一次观测重新开始、资源恢复和无泄漏；
- 使用稳定速率并保留运行安全余量，不在极限点冒充生产配置。

## Milestone 6：输出链和后续优化

完成前述契约与整链基线后再：

- 设计并实现 `dada2rdma` processed packetization 与网络发送；
- 按完整链证据继续优化 compute-ring CUDA 注册、pinned output、CUDA event/multi-stream overlap；
- 再评估 GPUDirect 或其他进程边界；
- 对 Time Integration 和其他 kernel 做证据驱动优化。

所有优化必须保留相同输入输出 oracle，并用重复服务器基准证明收益。

## 当前执行入口

Milestone 1–2 的任务、文件、命令和验收条件见：

[`../docs/superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](../docs/superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md)

每个 milestone 的正式结果记录在 GitHub Issue；测试通过后由用户决定 commit 和 push。
