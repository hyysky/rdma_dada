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

RTX 4090 CUDA 算法测试构建：

```bash
cmake -S . -B build-cuda \
  -DBUILD_TESTING=ON \
  -DBUILD_RDMA_PIPELINE=OFF \
  -DBUILD_SSD_BENCHMARK=OFF \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-cuda --parallel
ctest --test-dir build-cuda -L cuda --output-on-failure
```

包含 PSRDADA/RDMA adapter 测试时使用：

```bash
cmake -S . -B build-linux \
  -DBUILD_TESTING=ON \
  -DBUILD_RDMA_PIPELINE=ON \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

最后一种构建要求 Linux、PSRDADA、libibverbs 和 CUDA Toolkit 均已安装。

## 测试总表

| CTest 名称 | 来源 | 功能 | 构建条件 |
| --- | --- | --- | --- |
| `pipeline_config_test` | `pipeline_config_test.cpp` | 解析严格 JSON 配置，校验 record/block/file/rate 几何、溢出和 DADA header 派生值 | `BUILD_TESTING=ON` |
| `pipeline_config_inspect_test` | `pipeline_config_inspect` 工具 | 确认示例 JSON 能通过用户侧配置检查工具 | `BUILD_TESTING=ON` |
| `pipeline_config_rejects_legacy_test` | `pipeline_config_inspect` 工具 | 确认运行时拒绝旧 `.conf`；CTest 将非零退出视为成功 | `BUILD_TESTING=ON` |
| `config_conversion_test` | `config_conversion_test.py` | 将旧 `.conf` 转为 JSON，与示例比较，并确认未知字段、缺字段和非整数被拒绝 | 找到 Python 3 |
| `pipeline_core_test` | `pipeline_core_test.cpp` | 验证 `AlgorithmModule` 的 header 传播、block view、sequence 和 host execution context | `BUILD_TESTING=ON` |
| `beamform_module_test` | `beamform_module_test.cpp` | 验证 CPU reference、NPY int8/int16 权重、scale、TFPA→TFPB、shape 和容量错误路径 | `BUILD_TESTING=ON` |
| `beamform_cuda_fp32_test` | `beamform_cuda_test.cpp` | 在 CUDA stream 上验证异步 FP32 batched complex GEMM，以及 T/F batch stride | `USE_CUDA=ON` |
| `beamform_cuda_tf32_test` | `beamform_cuda_test.cpp` | 使用相同已知结果验证 TF32/Tensor Core 配置路径 | `USE_CUDA=ON`，GPU CC ≥ 8.0 |
| `dada_header_roundtrip_test` | `dada_header_roundtrip_test.cpp` | 通过 PSRDADA ASCII header 完成序列化/反序列化，验证未知字段保留及版本拒绝 | `BUILD_RDMA_PIPELINE=ON` |

## 单项调用

### `pipeline_config_test`

```bash
./build/pipeline_config_test config/pipeline.example.json
ctest --test-dir build -R '^pipeline_config_test$' --output-on-failure
```

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

```bash
./build-cuda/beamform_cuda_test \
  build-cuda/manual_beamform_fp32_weights.npy FP32

./build-cuda/beamform_cuda_test \
  build-cuda/manual_beamform_tf32_weights.npy TF32

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

和 CPU 测试一样，传入的 `.npy` 路径只用于临时 fixture，测试结束时会删除。

### `dada_header_roundtrip_test`

```bash
./build-linux/dada_header_roundtrip_test config/pipeline.example.json
ctest --test-dir build-linux \
  -R '^dada_header_roundtrip_test$' --output-on-failure
```

该测试链接 PSRDADA header API，因此不会出现在 macOS/portable build 中。

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
