# RDMA/PSRDADA Pipeline

该项目正在重构为独立的 PSRDADA ring-connected 实时天线阵列处理 pipeline。当前已实现
统一 Observation/Resolved Plan、RoCE v2 ingest、Project VDIF TFP→ATFP 解包、GPU 上的
ATFP→TFPA 转换，以及 Beamform、Power/Stokes、Time Integration 组成的双 ring
`pipeline_worker`。功能、数值和 PSRDADA/CUDA 集成已经通过对应服务器验收；新 ATFP
整链的持续吞吐和运行余量仍在测试，不能仅凭 correctness 结果声明满足实时观测速率。

各应用、算法、配置、测试和后续工作的统一状态见
[docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md)。

## 当前数据契约

- 用户只维护 Observation JSON；编译后的 `resolved_observation.json` 是各进程共同的
  运行契约。编译器同时生成 RAW、UNPACKED、CONVERTED、BEAMFORMED 和最终产品 header
  （没有处理模块时只生成前两级）。`NANT`/`NCHAN`/`NPOL`、`PKT_TSAMP`、`UTC_START`
  和 ring/file 几何不写死。
- raw ring 中的每条 record 是 `32-byte Project VDIF v1 header + TFP payload`。RoCE 接收缓冲区中额外的 Ethernet/IPv4/UDP 42 字节会在写 ring 前剔除。
- PSRDADA 的 header block 描述一次 transfer；数据 block 不会重复前缀 ASCII header。
- 一个解包/计算 block 包含所有 `A` 个阵元的数据。总 UDP record 数必须是 `A` 的
  整数倍；`T=PKT_NSAMP×每阵元每block的UDP包数`。
- `.dada` 文件大小由 `blocks_per_file` 配置，因此是 ring data block 大小的整数倍。
- 当前 RDMA ingest 使用内部注册缓冲区，验证 CQ completion 后再拷贝到 raw ring。尚未启用 RDMA 直接写 ring 的优化路径。

完整的目标架构和 header 传播规则见 [doc/PIPELINE_ARCHITECTURE.md](doc/PIPELINE_ARCHITECTURE.md)。
算法模块的输入输出、`TFPA/TFPB/TFBS` 数据布局、独立积分模块和 worker 调用规则见
[doc/ALGORITHM_MODULE_CONTRACTS.md](doc/ALGORITHM_MODULE_CONTRACTS.md)。
后续开发顺序、各阶段验收标准、模块输入输出门禁和配置参数扩展规则见
[doc/DEVELOPMENT_PLAN.md](doc/DEVELOPMENT_PLAN.md)。

## 分层与目录

```text
include/rdma_dada/config/       便携 JSON 和运行参数契约
include/rdma_dada/pipeline/     block、metadata、stage 和 DADA header 契约
include/rdma_dada/io/rdma/      RoCE/libibverbs 适配器接口
include/rdma_dada/io/psrdada/   PSRDADA ring/header 适配器接口
src/config|pipeline|io/         与公共接口镜像的底层实现
modules/                        独立算法，不持有 ring 和进程生命周期
apps/rdma2dada/                 已实现的 NIC→raw ring 组合入口
apps/vdif_unpack_worker/        已实现的 raw ring→ATFP compute ring 解包进程
apps/pipeline_worker/           已实现的双 ring 模块链工作进程
apps/dada2rdma|pipelinectl/     计划中的输出与编排入口
tools/                          配置检查和 SSD 诊断工具
scripts/                        最上层用户启动、构建和配置转换
tests/                          macOS 单元测试与后续 Linux 集成测试
```

依赖只能向底层流动：`scripts → apps`；`apps` 负责组合 `io + pipeline + modules`；`modules` 只依赖 `pipeline/config` 契约。算法模块通过 `AlgorithmModule` 交换 metadata 和 block view，不直接调用 PSRDADA 或 libibverbs。

## macOS 开发

macOS 不编译 libibverbs/PSRDADA/CUDA 数据面，只构建可移植部分：

```bash
bash scripts/build.sh
ctest --test-dir build --output-on-failure
```

这个模式不需要 CUDA，也不要求在 macOS 安装 PSRDADA。

## Linux 服务器构建

服务器需要 libibverbs 开发文件与 PSRDADA。当前对照的 PSRDADA 源码版本是 `151198ea3ec4a3a237f51e6217a5a2d7ff39194e`。CUDA 不是 raw RDMA ingest 的必需依赖：

```bash
BUILD_RDMA_PIPELINE=ON USE_CUDA=OFF bash scripts/build.sh
ctest --test-dir build --output-on-failure
```

启用 CUDA 的算法模块后，再使用 `USE_CUDA=ON`。

在服务器上可以先关闭 RDMA/PSRDADA，只验证 CUDA 算法模块：

```bash
BUILD_RDMA_PIPELINE=OFF USE_CUDA=ON bash scripts/build.sh
ctest --test-dir build -L cuda --output-on-failure
```

RTX 3090 的默认 CUDA architecture 是 `86`，可通过
`-DCMAKE_CUDA_ARCHITECTURES=86` 显式覆盖。CUDA 构建建议使用 CMake 3.24 或更新
版本；项目最低版本仍为 3.18。

Linux 上先编译 Observation 配置，再由 worker 消费同一份 Resolved Plan：

```bash
./build/observation_config_compile config/observation.json run/observation
./build/pipeline_worker run/observation/resolved_observation.json
```

输入必须是 host compute ring 中的 `UNPACKED/ATFP/CI8` 数据；worker 在 GPU 内完成
ATFP→TFPA 与 CI8→CF32，再执行 Beamform 以及可选 Power/Stokes/Integration。
输出 ring block 大小由权重 NPY 的 B 维和算法链自动推导；准确尺寸关系见
[apps/pipeline_worker/README.md](apps/pipeline_worker/README.md)。

## JSON 配置

示例文件是 [config/pipeline.example.json](config/pipeline.example.json)。开始前可检查派生的 record/block/ring/file 几何：

```bash
./build/pipeline_config_inspect config/pipeline.example.json
```

Observation JSON、Resolved Plan、ring plan 和 DADA header artifact 的生成方式：

```bash
./build/observation_config_compile \
  config/observation.example.json run/observation
```

`config/pipeline_worker.example.json` 仅用于模块级兼容测试，不再作为应用入口。

前端固定 32-byte Project VDIF v1 header 和 TFP payload 轴布局使用独立
packet-format profile：

```bash
./build/packet_format_inspect \
  config/packet_formats/frontend.example-v1.json
```

wire 字段表见 [doc/PROJECT_VDIF_PROFILE_V1.md](doc/PROJECT_VDIF_PROFILE_V1.md)，配置与轴
表达式见 [config/packet_formats/README.md](config/packet_formats/README.md)。

将旧的 shell 配置转为 JSON：

```bash
python3 scripts/convert_config_to_json.py old.conf config/pipeline.json
```

## 运行当前 demo

先修改 [scripts/run_demo.sh](scripts/run_demo.sh) 顶部的接收端 MAC/IP/port、NIC 参数，
以及 JSON 配置。接收 flow 不限制发送端 MAC/IP/port。当前 launcher 没有启动下游
worker，所以要求 `disk.enabled=true`，由 `dada_dbdisk` 作为 raw ring 的 reader。

```bash
bash scripts/run_demo.sh start
```

该脚本在前台持有本次运行的所有 PID 和 ring key。在同一终端按 `Ctrl+C` 会停止 receiver、等待 reader 处理 EOD，然后清理 ring。`stop` 和 `status` 子命令已禁用，因为脚本不保存跨进程的 PID 状态。

数据和日志默认写入 `data_out/`。当前可执行文件名为 `build/rdma2dada`。

## 当前限制

- Linux RDMA 构建、CQ 错误路径、NIC flow steering 和有限 transfer 已有服务器验证；
  新 ATFP 完整整链的目标速率、持续运行和安全余量仍在验证。
- `beamform` 已有 NPY 权重加载、host FP32 reference 和异步 CUDA FP32/TF32
  backend；`power`、`stokes`、`time_integrate` 已有 host FP32 reference 和异步
  CUDA kernel。独立 CUDA correctness、GPU 数值组合链和 Resolved Plan 驱动的
  PSRDADA/CUDA ring integration 已在目标服务器通过；整链持续性能仍需继续验证。
- `host_to_device`、`device_to_host` 已实现为无 buffer 所有权的异步 CUDA 传输
  模块，并已接入 worker；当前 PSRDADA ring block 仍按普通 host 内存处理。
- 观测流程固定为解析重排后执行 Beamform，再选择 Power 或 Stokes，最后按需执行
  time integration。当前 worker 已实现 `beamform`、`beamform+power`、
  `beamform+stokes`，以及后两条链的可选积分；整数复数转 CF32 已作为固定前缀接入
  worker。VDIF 解包已改为 payload-only ATFP 输出并通过功能验收，通用模块 registry
  仍待实现。
- 当前 CUDA worker 使用单条 non-blocking stream，但在提交每个输出 ring block 前
  同步；双 buffer/event 的跨 block H2D/计算/D2H overlap 尚未实现。
- Project VDIF v1 已固定 32-byte header、TFP/IQ payload 和 Station-ID 聚合契约；
  binary decoder、packet-group 状态机、ATFP 聚合、partial/EOD 和缺失补零已通过功能
  验收；1–40 Gbps payload 目标速率 campaign 正在进行，低错误率测试尚未完成。
- `DumpToDada()` 仍是旧实现，不应用作 pipeline sink；当前使用 PSRDADA 的 `dada_dbdisk`。
