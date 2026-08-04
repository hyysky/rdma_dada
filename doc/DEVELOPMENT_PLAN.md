# Pipeline development plan

本文档记录 `pipeline-architecture` 阶段之后的实施顺序和验收标准。后续开发按里程碑
推进；除非观测需求发生变化，不跨过当前里程碑直接进入后续性能优化。

算法的轴顺序、数值定义和 header 字段仍以
[`ALGORITHM_MODULE_CONTRACTS.md`](ALGORITHM_MODULE_CONTRACTS.md) 为准；本文件负责
记录开发顺序和质量门禁。

## 当前基线

截至当前分支，已经具备：

- JSON 配置、DADA ASCII header、block view 和模块接口；
- Beamform、Power、Stokes 的 CPU reference backend；
- Beamform、Power、Stokes 的 CUDA backend 源码；
- 无 buffer 所有权的 H2D/D2H CUDA 传输模块；
- 双 host ring `pipeline_worker`，支持 Beamform、Beamform→Power、
  Beamform→Stokes 三条固定链；
- 10 项 macOS portable CTest，以及 CUDA 算法和 H2D→D2H round-trip 测试源码。

CUDA、PSRDADA、RDMA 数据面仍需要在目标 Linux/RTX 4090 服务器验证。

## 每个模块的输入输出门禁

任何新模块或已有模块修改，在合入前必须同时满足以下条件。不能只验证 kernel 数值而
忽略 header 或 block 几何。

1. **输入契约明确**：列出 `DATA_STAGE`、`ORDER`、`SAMPLE_FORMAT`、`MEMORY`、维度字段、
   必填观测字段和允许的 backend。
2. **输出契约明确**：列出修改、保留和删除的 header 字段，不能无说明地丢弃未知观测
   metadata。
3. **轴和 shape 明确**：使用 `T/F/P/A/B/S` 记号给出输入、输出 shape 和线性 order；
   模块必须检查相邻模块是否兼容。
4. **数值表示明确**：给出 component/sample 位宽、实数或复数、量化 scale、累计精度和
   输出 dtype，不能从指针类型隐式推断。
5. **几何公式唯一**：定义 input/output frame bytes、`RESOLUTION`、block bytes 和中间
   buffer bytes；所有乘法检查溢出，所有缩放检查整除。
6. **时间和速率一致**：修改采样数或积分长度时，同步更新 `TSAMP`、
   `BYTES_PER_SECOND`、`TRANSFER_SIZE`、`FILE_SIZE` 和 `OBS_OFFSET`；无法精确表示时
   拒绝配置。
7. **一 block 输入一 block 输出**：第一版模块不得静默丢弃、补齐或缓存跨 block 尾部；
   输出大小变化必须能在处理前由 header 和配置计算。
8. **配置/header 交叉校验**：header 表示上游实际数据，JSON 表示本进程配置；同一参数
   同时出现时必须一致，否则在发布 output header 前终止 transfer。
9. **独立数值 oracle**：先提供小尺寸 CPU reference 或手算结果，再验证 CUDA；不能只用
   同一实现生成 expected data。
10. **错误路径有测试**：至少覆盖缺字段、错误 order/dtype/shape、block 不整除、容量不足、
    backend/device/stream 不匹配和溢出。

## 配置参数扩展规则

后续可以按观测需求增加 JSON 参数，但必须完成以下同步修改：

1. 确认参数属于 input header、worker JSON，还是可由二者推导；可推导值不重复配置。
2. 在配置结构体、严格 JSON parser、示例 JSON 和配置检查工具中同时加入。
3. 定义单位、范围、默认策略、是否允许运行中改变，以及改变后影响的 header 字段和
   block 几何。
4. 添加合法值、缺失值、边界值、错误类型、未知字段和溢出测试。
5. 如果改变现有字段语义或必填性，提升 `schema_version`；只增加明确可选且有兼容默认值
   的字段时才保持当前版本。
6. 更新模块契约、应用 README 和服务器运行示例，避免代码、配置与文档出现三套定义。

## Milestone 1：目标服务器基线验证

目标：在开发新算法前，确认当前 CUDA、PSRDADA 和 RDMA 基线可靠。

- 使用 CUDA 12.8、RTX 4090 编译全部 CUDA targets；
- 运行 Beamform FP32/TF32、Power、Stokes、H2D→D2H 测试；
- 比较 CUDA 和 CPU reference 的数值与允许误差；
- 创建两个测试 ring，分别验证 CPU 和 CUDA `pipeline_worker`；
- 检查 input/output header、block size、sequence、EOD 和多 transfer 生命周期；
- 验证 `rdma2dada` 的 CQ completion、flow steering、丢包/error 和持续运行；
- 记录吞吐、单 block 延迟、显存占用和 host copy 开销。

完成标准：CUDA CTest 全部通过，双 ring worker 能稳定处理已知数据，RDMA ingest 没有
未处理 CQ 错误或 block 生命周期错误。

## Milestone 2：VDIF 解包和整数复数转换

目标：将当前 worker 的输入边界从 `CONVERTED/TFPA/CF32` 前移到 raw packet ring。

- 固化实际 payload order 和包头字段；
- 实现 64-byte application header 校验、序号检查、去头和 TFPA 重排；
- 定义丢包、乱序、重复包和跨 block 包组处理规则；
- 实现 `CI8/CI16/... → CF32` CUDA conversion，scale 从配置加载；
- 验证 `F×A×P×T`、UDP payload 和所有阵元分组关系；
- 为每种支持位宽提供 CPU oracle、CUDA 数值测试和 header transform 测试。

完成标准：raw 测试记录经过解包和转换后，与独立构造的 TFPA/CF32 expected block 完全
一致或满足定义的数值误差。

## Milestone 3：时间积分模块

目标：在 Power 或 Stokes 之后提供独立、可配置的 block-local integration。

- 支持 `sum` 和 `mean`；
- 定义 `INTEGRATION_LENGTH`、累计 dtype 和溢出策略；
- 输入一个 block，输出一个缩短 T 维的 block；
- 要求 input T 可被 integration length 整除；
- 正确更新 `TSAMP`、`BYTES_PER_SECOND`、`RESOLUTION`、block/file/offset 字段；
- 实现 CPU reference、CUDA backend 和边界测试。

完成标准：Power 和 Stokes 两条链均可追加积分，数值、shape、header 和输出 ring block
大小全部由测试覆盖。

## Milestone 4：配置驱动的通用模块链

目标：用模块 registry 替换当前三条硬编码链，同时保留观测流程约束。

- JSON 使用有序模块数组；
- Module factory 创建模块并逐级调用 `ConfigureHeader()`；
- 自动检查相邻模块的 stage/order/dtype/memory/shape；
- 在处理前计算所有中间和最终 buffer 尺寸；
- 固定前段顺序为 unpack/reorder→convert→beamform；
- Power 和 Stokes 继续作为互斥分支；Integration 只允许出现在合法产品之后；
- 对非法顺序给出包含模块名和不兼容字段的错误信息。

完成标准：配置文件可以表达当前合法链，错误链在创建 ring 或发布 output header 前被拒绝。

## Milestone 5：完整运行编排

目标：从一个配置文件启动和管理 ingest、worker、可选 disk sink 及所有 rings。

- 扩展 demo 以创建正确 block size 和 reader count 的多个 ring；
- 按拓扑启动 `rdma2dada`、一个或多个 worker 和 `dada_dbdisk`；
- 实现可靠的 signal、EOD、错误传播和清理；
- 开发 `pipelinectl` 的配置检查、启动和状态功能；
- 添加 synthetic input 的端到端集成测试。

完成标准：单条命令可以运行 NIC/raw ring→处理 ring→disk 的完整观测链，任一进程失败
时不会遗留错误 reader count 或锁住的 ring。

## Milestone 6：输出传输和性能优化

在正确性与端到端生命周期稳定之后再进行：

- 实现 `dada2rdma` 重新打包和网络发送；
- 使用 pinned bounce buffer 或谨慎评估 `cudaHostRegister()` PSRDADA block；
- 引入双 buffer、CUDA events 和多 stream，实现跨 block H2D/compute/D2H overlap；
- 评估 RDMA 直接写 ring、CUDA ring process boundary 和 GPUDirect 路径；
- 添加长时间稳定性、吞吐、延迟和背压基准。

完成标准：优化前后使用相同的输入输出正确性测试，并用服务器基准证明优化收益；不能以
改变数据契约或跳过生命周期检查换取吞吐。

## 执行记录

每个 milestone 开始时，在 GitHub Issue 中记录目标、服务器配置和验收命令；完成后记录
测试输出、性能数据、发现的问题及对应 commit。未通过的验收项不能标记为完成。
