# Tests

本目录包含 pipeline 配置、header、模块契约及 CUDA 算法的回归测试。通常应通过
CTest 调用；直接运行测试程序主要用于定位单项失败。

## 构建测试

macOS 或不带 CUDA/PSRDADA 的便携测试构建：

```bash
cmake -S . -B build \
  -DBUILD_TESTING=ON \
  -DBUILD_RDMA_PIPELINE=OFF \
  -DUSE_CUDA=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

RTX 3090 CUDA 算法测试构建：

```bash
cmake -S . -B build-cuda \
  -DBUILD_TESTING=ON \
  -DBUILD_RDMA_PIPELINE=OFF \
  -DBUILD_SSD_BENCHMARK=OFF \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-cuda --parallel
ctest --test-dir build-cuda -L cuda --output-on-failure
```

包含 PSRDADA/RDMA adapter 测试时使用：

```bash
cmake -S . -B build-linux \
  -DBUILD_TESTING=ON \
  -DBUILD_RDMA_PIPELINE=ON \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

最后一种构建要求 Linux、PSRDADA、libibverbs 和 CUDA Toolkit 均已安装。

## 测试总表

| CTest 名称 | 来源 | 功能 | 构建条件 |
| --- | --- | --- | --- |
| `project_vdif_v1_test` | `project_vdif_v1_test.cpp` | 对固定 32-byte Project VDIF v1 header 做 little-endian golden decode/encode，并校验 CI8/CI16 record 几何和保留字段错误路径 | `BUILD_TESTING=ON` |
| `vdif_unpack_config_test` | `vdif_unpack_config_test.cpp` | 校验 ring key、Station ID→A 映射、两 block payload-only 窗口几何、profile 冲突、内存上限与溢出错误路径 | `BUILD_TESTING=ON` |
| `vdif_unpack_header_test` | `vdif_unpack_header_test.cpp` | 校验 RAW→UNPACKED header 转换、未知字段保留、TFPA/无包头几何、零填充策略及输入冲突不发布输出 | `BUILD_TESTING=ON` |
| `vdif_unpack_engine_test` | `vdif_unpack_engine_test.cpp` | 校验任意 Station 顺序和跨 block 的 TFP→TFPA 重排、完整 group 稳定等待、超时/EOD 零填充、arena 淘汰、坏包/重复/late 统计及 partial-block 契约 | `BUILD_TESTING=ON` |
| `group_block_writer_test` | `group_block_writer_test.cpp` | 用内存 block sink 验证有序 group 直接填充满 block、EOD 精确提交部分 block、空传输不提交，以及 group/sink 容量错误不发布数据 | `BUILD_TESTING=ON` |
| `vdif_sender_sim_test` | `vdif_sender_sim_test.cpp` | 校验严格 sender JSON、整数皮秒跨秒/frame 重置、双 Station 同时间 key、CI8/CI16 确定性 payload、MTU 和 invalid-header 注入 | `BUILD_TESTING=ON` |
| `fpga_sender_sim_loopback_test` | `fpga_sender_sim_loopback_test.py` | 在 127.0.0.1 临时 UDP 端口验证真实发送的 Station ID、drop、byte-identical duplicate、invalid Word 7 和完整 datagram 长度 | 找到 Python 3 |
| `pipeline_config_test` | `pipeline_config_test.cpp` | 解析严格 JSON 配置，校验 record/block/file/rate 几何、溢出和 DADA header 派生值 | `BUILD_TESTING=ON` |
| `packet_format_config_test` | `packet_format_config_test.cpp` | 加载固定 32-byte/8-word Project VDIF profile，逐字段校验 bit layout、TFP→TFPA axis、HEADER/DERIVED/LOOKUP 引用和 payload 几何 | `BUILD_TESTING=ON` |
| `packet_format_inspect_test` | `packet_format_inspect_test.py` | 检查 profile inspect 的 32-byte、TWOS_COMPLEMENT、IQ 和 axis 输出，并确认未知字段、旧 signed 字段和 64-byte header 被拒绝 | 找到 Python 3 |
| `pipeline_config_inspect_test` | `pipeline_config_inspect` 工具 | 确认示例 JSON 能通过用户侧配置检查工具 | `BUILD_TESTING=ON` |
| `pipeline_worker_config_inspect_test` | `pipeline_worker_config_inspect` 工具 | 从 F/A/P、UDP 分组、product 和积分参数计算 input/product/scratch/output block bytes | `BUILD_TESTING=ON` |
| `pipeline_config_rejects_legacy_test` | `pipeline_config_inspect` 工具 | 确认运行时拒绝旧 `.conf`；CTest 将非零退出视为成功 | `BUILD_TESTING=ON` |
| `config_conversion_test` | `config_conversion_test.py` | 将旧 `.conf` 转为 JSON，与示例比较，并确认未知字段、缺字段和非整数被拒绝 | 找到 Python 3 |
| `pipeline_core_test` | `pipeline_core_test.cpp` | 验证 `AlgorithmModule` 的 header 传播、block view、sequence 和 host execution context | `BUILD_TESTING=ON` |
| `beamform_module_test` | `beamform_module_test.cpp` | 验证 CPU reference、NPY int8/int16 权重、scale、TFPA→TFPB、shape 和容量错误路径 | `BUILD_TESTING=ON` |
| `beamform_test_weights_generator_test` | `beamform_test_weights_generator_test.py` | 验证 CUDA Beamform 测试权重生成器输出的 NPY 版本、shape、dtype、order、scale 和已知系数 | 找到 Python 3 |
| `beamform_cuda_fp32_test` | `beamform_cuda_test.cpp` | 在 CUDA stream 上验证异步 FP32 batched complex GEMM，以及 T/F batch stride | `USE_CUDA=ON` |
| `beamform_cuda_tf32_test` | `beamform_cuda_test.cpp` | 使用相同已知结果验证 TF32/Tensor Core 配置路径 | `USE_CUDA=ON`，GPU CC ≥ 8.0 |
| `complex_convert_module_test` | `complex_convert_module_test.cpp` | 验证 CPU reference 的 CI8/CI16→CF32、scale、header/byte 几何、signed 和非重叠约束 | `BUILD_TESTING=ON` |
| `complex_convert_cuda_test` | `complex_convert_cuda_test.cpp` | 在 non-blocking stream 上验证 CI8/CI16 边界值、scale、size 和 sequence | `USE_CUDA=ON` |
| `power_module_test` | `power_module_test.cpp` | 验证 CPU reference 的 `TFPB/CF32→TFPB/F32`、header/byte rate、两帧数值结果和错误路径 | `BUILD_TESTING=ON` |
| `power_cuda_test` | `power_cuda_test.cpp` | 在 worker 风格 non-blocking stream 上验证异步 Power kernel 与 CPU 已知结果一致 | `USE_CUDA=ON` |
| `stokes_module_test` | `stokes_module_test.cpp` | 验证 CPU reference 的 `TFPB/CF32→TFBS/F32`、四个相关产物、双偏振约束、header 和错误路径 | `BUILD_TESTING=ON` |
| `stokes_cuda_test` | `stokes_cuda_test.cpp` | 在 non-blocking stream 上验证异步 Stokes kernel 与 CPU 已知结果一致 | `USE_CUDA=ON` |
| `time_integrate_module_test` | `time_integrate_module_test.cpp` | 验证 TFPB/TFBS 的 SUM/MEAN、累计积分 header、block 缩短、容量/整除/重叠等错误路径 | `BUILD_TESTING=ON` |
| `time_integrate_cuda_test` | `time_integrate_cuda_test.cpp` | 在 worker 风格 non-blocking stream 上验证异步时间积分与已知结果一致 | `USE_CUDA=ON` |
| `transfer_cuda_roundtrip_test` | `transfer_cuda_roundtrip_test.cpp` | 将确定性字节块依次通过 H2D 和 D2H，检查 header、size、sequence 与最终逐字节结果 | `USE_CUDA=ON` |
| `pipeline_worker_cuda_chain_test` | `pipeline_worker_cuda_chain_test.cpp` | 在一条 non-blocking stream 上验证 H2D→Beamform→Power→TimeIntegrate→D2H、scratch/output 几何、header、sequence 和手算结果 | `USE_CUDA=ON` |
| `pipeline_worker_core_test` | `pipeline_worker_core_test.cpp` | 验证 worker JSON/ring key、ASCII header、block/scratch 规划，以及 Power/Stokes 后积分的 header 和手算数值 | `BUILD_TESTING=ON` |
| `dada_header_roundtrip_test` | `dada_header_roundtrip_test.cpp` | 通过 PSRDADA ASCII header 完成 RAW/TFP/32-byte 与 COMPUTE/TFPA/无包头双阶段 round-trip，验证未知字段保留及版本拒绝 | `BUILD_RDMA_PIPELINE=ON` |
| `vdif_unpack_worker_integration_test` | `vdif_unpack_worker_integration.sh` | 创建精确 raw/compute rings，经 `dada_diskdb` 输入跨 block Project VDIF transfer，并用 `dada_dbdisk` 验证 full/partial compute block、TFPA 字节、完整 header、EOD 及同一 worker 的连续双 transfer | `BUILD_RDMA_PIPELINE=ON` 且具备 PSRDADA CLI；缺少时 skip 77 |

## 单项调用

### `pipeline_config_test`

```bash
./build/pipeline_config_test config/pipeline.example.json
ctest --test-dir build -R '^pipeline_config_test$' --output-on-failure
```

### Packet format profile 测试

```bash
./build/project_vdif_v1_test

./build/packet_format_config_test \
  config/packet_formats/frontend.example-v1.json

./build/packet_format_inspect \
  config/packet_formats/frontend.example-v1.json

ctest --test-dir build \
  -R '^packet_format_(config|inspect)_test$' --output-on-failure
```

该 profile 是 Project VDIF v1 的机器可读 wire contract；测试中的 `payload_bytes` 是一组
CI8/NCHAN=3 几何示例，实际观测仍须与 packet header 逐包交叉校验。

### VDIF unpack 配置与 header 测试

```bash
./build/vdif_unpack_config_test config/vdif_unpack.example.json
./build/vdif_unpack_header_test
./build/vdif_unpack_engine_test
ctest --test-dir build \
  -R '^vdif_unpack_(config|header|engine)_test$' --output-on-failure
```

前者加载 unpack、pipeline 和 packet-format 三份配置并检查 payload-only 窗口；后者验证
compute header 更新前会先完整检查 raw header 几何，失败时不会覆盖调用者已有 metadata。
engine 测试使用独立生成的 40-byte 小型 Project VDIF record，以手工已知字节验证 TFPA
位置和缺失 Station 的零区间，不依赖未来 UDP/RDMA adapter。

Linux 上的真实双 ring 测试：

```bash
tests/vdif_unpack_worker_integration.sh \
  build-linux/vdif_unpack_worker "$(pwd)"
ctest --test-dir build-linux \
  -R '^vdif_unpack_worker_integration_test$' --output-on-failure
```

该脚本的测试 sample 间隔固定为 `1 us`，分别验证末尾为完整 compute block、部分
compute block，以及 `runtime.run_once=false` 时同一 worker 的两个连续 transfer；缺少
`dada_db`、`dada_diskdb`、`dada_dbdisk` 等工具时返回 CTest skip code 77。

### Compute group block writer 测试

```bash
./build/group_block_writer_test
ctest --test-dir build -R '^group_block_writer_test$' --output-on-failure
```

该测试使用真实内存作为非 owning sink 边界，验证五个 4-byte group 依序形成一个
12-byte 满 block 和一个 8-byte EOD block。未来 PSRDADA adapter 只需实现相同的
`Acquire/Commit` 接口，writer 本身不申请或持有 ring block 存储。

### FPGA sender simulator 测试

```bash
./build/vdif_sender_sim_test config/fpga_sender_sim.example.json
python3 tests/fpga_sender_sim_loopback_test.py \
  build/fpga_sender_sim config/fpga_sender_sim.example.json
ctest --test-dir build \
  -R '^(vdif_sender_sim|fpga_sender_sim_loopback)_test$' \
  --output-on-failure
```

loopback 测试使用操作系统分配的临时端口，不依赖固定端口或外部网络；若执行环境禁止
创建本地 socket，需要为该测试开放 `127.0.0.1` 回环权限。

### 配置检查工具测试

合法 JSON 应返回 0：

```bash
./build/pipeline_config_inspect config/pipeline.example.json
```

旧 `.conf` 应返回非 0：

```bash
./build/pipeline_config_inspect config/pipeline.example.conf
```

对应的 CTest 调用为：

```bash
ctest --test-dir build \
  -R '^(pipeline_config_inspect_test|pipeline_config_rejects_legacy_test)$' \
  --output-on-failure
```

`pipeline_config_rejects_legacy_test` 设置了 `WILL_FAIL=TRUE`，所以直接运行工具时的
非零返回值是预期结果。

worker block 几何检查：

```bash
./build/pipeline_worker_config_inspect \
  config/pipeline_worker.example.json

ctest --test-dir build \
  -R '^pipeline_worker_config_inspect_test$' --output-on-failure
```

### `config_conversion_test`

```bash
python3 tests/config_conversion_test.py \
  scripts/convert_config_to_json.py \
  ./build/pipeline_config_inspect \
  config/pipeline.example.conf \
  config/pipeline.example.json

ctest --test-dir build -R '^config_conversion_test$' --output-on-failure
```

### `pipeline_core_test`

```bash
./build/pipeline_core_test
ctest --test-dir build -R '^pipeline_core_test$' --output-on-failure
```

### `beamform_module_test`

```bash
./build/beamform_module_test build/manual_beamform_weights.npy
ctest --test-dir build -R '^beamform_module_test$' --output-on-failure
```

参数是测试临时权重文件路径。测试会在该路径创建 `.npy` 并在结束时删除它，因此
不能传入真实权重文件。

### CUDA Beamform 测试

仓库提供一个持久化的已知权重文件：

```text
tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy
ORDER=FPAB2
shape=[F=2,P=1,A=2,B=2,real_imag=2]
dtype=int8
WEIGHTS_SCALE=0.5
```

需要重新生成时执行：

```bash
python3 scripts/generate_beamform_test_weights.py \
  tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy
```

```bash
./build-cuda/beamform_cuda_test \
  tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy FP32

./build-cuda/beamform_cuda_test \
  tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy TF32

ctest --test-dir build-cuda -L cuda --output-on-failure
```

也可以分别运行：

```bash
ctest --test-dir build-cuda \
  -R '^beamform_cuda_fp32_test$' --output-on-failure

ctest --test-dir build-cuda \
  -R '^beamform_cuda_tf32_test$' --output-on-failure
```

CUDA 测试创建一个 worker-owned non-blocking stream，将输入异步拷入显存，调用
Beamform，再在同一 stream 上异步拷回结果。测试覆盖 `T=2、F=2、P=1、A=2、B=2`
的已知复数结果。没有可用 CUDA device 时返回 77，CTest 将其标记为 skipped。

如果传入路径已经存在，CUDA 测试只读取并保留该文件；如果路径不存在，则创建同样的
临时 fixture，并在测试结束时删除。

### Complex conversion 测试

CPU reference：

```bash
./build/complex_convert_module_test
ctest --test-dir build \
  -R '^complex_convert_module_test$' --output-on-failure
```

CUDA：

```bash
./build-cuda/complex_convert_cuda_test
ctest --test-dir build-cuda \
  -R '^complex_convert_cuda_test$' --output-on-failure
```

两者使用独立写死的 CI8/CI16 输入与 CF32 expected 值，覆盖有符号边界、显式 scale、
frame 扩展和 sequence。CUDA 测试通过同一条 non-blocking stream 完成 H2D、转换和
D2H，仅在读取结果前同步；没有 CUDA device 时返回 77。

### Power 测试

CPU reference：

```bash
./build/power_module_test
ctest --test-dir build -R '^power_module_test$' --output-on-failure
```

CUDA：

```bash
./build-cuda/power_cuda_test
ctest --test-dir build-cuda -R '^power_cuda_test$' --output-on-failure
```

两个测试使用相同的 `T=2、F=2、P=2、B=2` 已知输入。CUDA 测试通过同一条
non-blocking stream 完成 H2D、Power kernel 和 D2H；没有可用 CUDA device 时返回
77，由 CTest 标记为 skipped。

### Stokes 测试

CPU reference：

```bash
./build/stokes_module_test
ctest --test-dir build -R '^stokes_module_test$' --output-on-failure
```

CUDA：

```bash
./build-cuda/stokes_cuda_test
ctest --test-dir build-cuda -R '^stokes_cuda_test$' --output-on-failure
```

两个测试使用相同的 `T=2、F=2、P=2、B=2` 输入，逐项检查
`AA、BB、AB_REAL、AB_IMAG`。CUDA 测试使用 worker 风格 non-blocking stream；
没有可用 CUDA device 时返回 77，由 CTest 标记为 skipped。

### H2D/D2H round-trip 测试

```bash
./build-cuda/transfer_cuda_roundtrip_test
ctest --test-dir build-cuda \
  -R '^transfer_cuda_roundtrip_test$' --output-on-failure
```

测试建立一个 512-byte、8 个 `RESOLUTION` frame 的确定性输入，经同一条
non-blocking stream 依次调用 `HostToDeviceModule` 和 `DeviceToHostModule`，只在链末
同步一次。除逐字节比较外，还检查两级 header 的科学字段不变、`MEMORY` 正确变化，
以及 block `size/sequence` 保持一致。没有 CUDA device 时返回 77。

### CUDA worker 组合链测试

```bash
./build-cuda/pipeline_worker_cuda_chain_test \
  tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy

ctest --test-dir build-cuda \
  -R '^pipeline_worker_cuda_chain_test$' --output-on-failure
```

该测试不连接 PSRDADA，直接按 worker 的真实调用顺序执行：

```text
Host -> H2D -> Beamform -> Power -> TimeIntegrate(K=2,MEAN) -> D2H -> Host
```

输入为 `T=2,F=2,P=1,A=2`，输出为 `T=1,F=2,P=1,B=2`，手算结果固定为
`[17.5,66,175,9]`。测试检查 96-byte scratch、16-byte output、最终 header 和
sequence，并且只在完整链末尾同步一次 stream。没有可用 CUDA device 时返回 77。

### Time integration 测试

CPU reference：

```bash
./build/time_integrate_module_test
ctest --test-dir build \
  -R '^time_integrate_module_test$' --output-on-failure
```

CUDA：

```bash
./build-cuda/time_integrate_cuda_test
ctest --test-dir build-cuda \
  -R '^time_integrate_cuda_test$' --output-on-failure
```

CPU 测试覆盖 `TFPB/MEAN`、`TFBS/SUM`、二次积分累计长度、header 字节率和时间更新，
以及缺字段、错误 frame、`T % K != 0`、输出过小、输入输出重叠和 backend 不匹配。
CUDA 测试在 worker-owned non-blocking stream 上执行 H2D、积分和 D2H，只在读取结果前
同步；没有可用 CUDA device 时返回 77。

### `dada_header_roundtrip_test`

```bash
./build-linux/dada_header_roundtrip_test config/pipeline.example.json
ctest --test-dir build-linux \
  -R '^dada_header_roundtrip_test$' --output-on-failure
```

该测试链接 PSRDADA header API，因此不会出现在 macOS/portable build 中。

### `pipeline_worker_core_test`

```bash
./build/pipeline_worker_core_test \
  config/pipeline_worker.example.json \
  build/manual_pipeline_worker_weights.npy

ctest --test-dir build \
  -R '^pipeline_worker_core_test$' --output-on-failure
```

第二个参数是测试临时 NPY 权重路径，测试结束时会删除，不能指向真实权重文件。
该测试不连接 PSRDADA，因此可在 macOS 检查 JSON/header/block/module-chain 逻辑。
当前覆盖 Beamform、Power、Stokes，以及 Power/MEAN 和 Stokes/SUM 两条积分组合链。

## 常用过滤方式

```bash
# 列出当前 build 注册的测试
ctest --test-dir build -N

# 运行所有 Beamform 测试
ctest --test-dir build-cuda -R 'beamform' --output-on-failure

# 只运行带 cuda label 的测试
ctest --test-dir build-cuda -L cuda --output-on-failure

# 显示更完整的测试命令和输出
ctest --test-dir build -V
```
