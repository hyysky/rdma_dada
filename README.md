# RDMA/PSRDADA Pipeline

该项目正在重构为独立的 PSRDADA ring-connected pipeline。当前可用部分是 RoCE v2 数据接收到 raw ring，以及可在 macOS 上开发和测试的配置、DADA header、pipeline core 和 beamform FP32 reference。Beamform CUDA FP32/TF32 backend 已加入代码，尚待目标服务器验证；通用 worker 仍在建设中。

## 当前数据契约

- 所有运行时参数由 JSON 配置输入；`NANT`/`NCHAN`/`NPOL`、`PKT_TSAMP`、`UTC_START` 和 ring/file 几何不写死在程序中。
- raw ring 中的每条 record 是 `64-byte application header + payload`。RoCE 接收缓冲区中额外的 Ethernet/IPv4/UDP 42 字节会在写 ring 前剔除。
- PSRDADA 的 header block 描述一次 transfer；数据 block 不会重复前缀 ASCII header。
- `.dada` 文件大小由 `blocks_per_file` 配置，因此是 ring data block 大小的整数倍。
- 当前 RDMA ingest 使用内部注册缓冲区，验证 CQ completion 后再拷贝到 raw ring。尚未启用 RDMA 直接写 ring 的优化路径。

完整的目标架构和 header 传播规则见 [doc/PIPELINE_ARCHITECTURE.md](doc/PIPELINE_ARCHITECTURE.md)。
算法模块的输入输出、`TFPA/TFPB/TFBS` 数据布局、独立积分模块和 worker 调用规则见
[doc/ALGORITHM_MODULE_CONTRACTS.md](doc/ALGORITHM_MODULE_CONTRACTS.md)。

## 分层与目录

```text
include/rdma_dada/config/       便携 JSON 和运行参数契约
include/rdma_dada/pipeline/     block、metadata、stage 和 DADA header 契约
include/rdma_dada/io/rdma/      RoCE/libibverbs 适配器接口
include/rdma_dada/io/psrdada/   PSRDADA ring/header 适配器接口
src/config|pipeline|io/         与公共接口镜像的底层实现
modules/                        独立算法，不持有 ring 和进程生命周期
apps/rdma2dada/                 已实现的 NIC→raw ring 组合入口
apps/pipeline_worker/           计划中的模块链工作进程
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

RTX 4090 的默认 CUDA architecture 是 `89`，可通过
`-DCMAKE_CUDA_ARCHITECTURES=89` 显式覆盖。CUDA 构建建议使用 CMake 3.24 或更新
版本；项目最低版本仍为 3.18。

## JSON 配置

示例文件是 [config/pipeline.example.json](config/pipeline.example.json)。开始前可检查派生的 record/block/ring/file 几何：

```bash
./build/pipeline_config_inspect config/pipeline.example.json
```

将旧的 shell 配置转为 JSON：

```bash
python3 scripts/convert_config_to_json.py old.conf config/pipeline.json
```

## 运行当前 demo

先修改 [scripts/run_demo.sh](scripts/run_demo.sh) 顶部的 MAC/IP/port/NIC 参数，以及 JSON 配置。当前 launcher 没有启动下游 worker，所以要求 `disk.enabled=true`，由 `dada_dbdisk` 作为 raw ring 的 reader。

```bash
bash scripts/run_demo.sh start
```

该脚本在前台持有本次运行的所有 PID 和 ring key。在同一终端按 `Ctrl+C` 会停止 receiver、等待 reader 处理 EOD，然后清理 ring。`stop` 和 `status` 子命令已禁用，因为脚本不保存跨进程的 PID 状态。

数据和日志默认写入 `data_out/`。当前可执行文件名为 `build/rdma2dada`。

## 当前限制

- Linux RDMA 构建、CQ 错误路径、NIC flow steering 和持续运行需要在目标服务器验证。
- `beamform` 已有 NPY 权重加载、host FP32 reference 和异步 CUDA FP32/TF32 backend；CUDA 路径尚待 RTX 4090 服务器编译、数值和性能验证。`power` / `stokes` 仍待实现。这些算法保持独立模块，并可由配置组合为一个两端连 ring 的 worker 进程。
- raw ring 的 payload order 目前可配置为 `UNKNOWN`；在前端格式确定前，不对其做推断。
- `DumpToDada()` 仍是旧实现，不应用作 pipeline sink；当前使用 PSRDADA 的 `dada_dbdisk`。
