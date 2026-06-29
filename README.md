# RDMA + PSRDADA Standalone Module

基于 RoCE v2 的零拷贝网络数据接收模块，数据直接写入 PSRDADA 环形缓冲区并落盘。

## ✨ 特性一览

- **零拷贝 RDMA 接收**: RoCE v2 网卡直接 DMA 写入 ring buffer
- **PSRDADA 集成**: 数据自动写入 psrdada 环形缓冲区，支持 dada_dbdisk 后台写盘
- **智能内存注册**: 自动检测内存连续性，连续 → 单个 MR／非连续 → 分块注册
- **自动缓冲管理**: 空间不足时自动请求新 block，永不 crash
- **线程安全**: 互斥锁保护关键路径
- **Debug 模式**: `--debug` 参数开启详细诊断日志

> 详细文档：[QUICKSTART.md](QUICKSTART.md) · [WORKFLOW.md](WORKFLOW.md) · [BUGFIXES_AND_IMPROVEMENTS.md](BUGFIXES_AND_IMPROVEMENTS.md)

---

## 快速开始

### 1. 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install libpsrdada-dev libibverbs-dev librdmacm-dev
pkg-config --cflags --libs psrdada   # 验证
```

### 2. 编译

```bash
cd rdma_dada
bash build.sh
# 或手动：mkdir -p build && cd build && cmake .. && make
```

### 3. 配置并启动

编辑 `run_demo.sh` 中的网络参数（SMAC、DMAC、SIP、DIP 等），然后一键启动：

```bash
./run_demo.sh start     # 创建 ring buffer + 启动接收器 + 启动写盘器
./run_demo.sh status    # 查看运行状态
./run_demo.sh stop      # 停止
```

数据文件保存在 `./data_out/*.dada`。

也可手动分步操作：

```bash
# 创建 ring buffer（8GB）
dada_db -k 0xdada -b 8G

# 启动接收器
./build/Demo_psrdada_online \
  --smac a0:88:c2:6b:40:c6 --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 --dip 192.168.14.12 \
  --sport 61440 --dport 4144 \
  --pkt_size 8256 --send_n 64 --key 0xdada \
  --dump-dir ./data_out --dump-header header/array_GZNU.header

# 另一终端监控
watch -n 1 'dada_dbmetric -k 0xdada'
```

---

## 实用工具

| 脚本 | 用途 |
|------|------|
| `build.sh` | 一键编译 |
| `run_demo.sh` | 一键启动／停止（含 dada_dbdisk 写盘） |
| `check_psrdada_ring.sh` | 检查 ring buffer 内存连续性 |

---

## 项目结构

```
rdma_dada/
├── include/          # 头文件
├── src/              # 源码
├── demo/             # 演示程序
│   ├── Demo_psrdada_online.cpp
│   └── ssd_write_benchmark.cpp
├── header/           # PSRDADA header 模板
├── build.sh          # 编译脚本
└── run_demo.sh       # 启动脚本
```

## 文档资源

| 文档 | 描述 |
|------|------|
| [QUICKSTART.md](QUICKSTART.md) | ⭐ **快速开始** - 5分钟上手指南 |
| [WORKFLOW.md](WORKFLOW.md) | 📊 **工作流程** - 详细架构和数据流说明 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 🏗️ **系统架构** - 组件交互和内存管理详解 |
| [BUGFIXES_AND_IMPROVEMENTS.md](BUGFIXES_AND_IMPROVEMENTS.md) | 🐛 **v1.2.0改进** - Bug修复和功能增强 |
| [USAGE_EXAMPLE.md](USAGE_EXAMPLE.md) | 📖 使用示例和最佳实践 |
| [PSRDADA_ROCE_INTEGRATION.md](PSRDADA_ROCE_INTEGRATION.md) | 🔧 技术集成详解 |
| [NON_CONTIGUOUS_MEMORY_SOLUTION.md](NON_CONTIGUOUS_MEMORY_SOLUTION.md) | 💾 非连续内存解决方案 |

## 常见问题 (FAQ)

### Q1: "Insufficient buffer space" 错误

**A:** v1.2.0已修复此问题。如果使用旧版本，请升级并重新编译：
```bash
git pull  # 如果使用git
./build.sh
```

新版本会自动检测剩余空间并请求新block。

### Q2: 如何启用Debug模式？

**A:** 编辑 `run_demo.sh`，在启动命令中添加 `--debug`：
```bash
CMD="./build/Demo_psrdada_online ... --debug"
```

或直接运行：
```bash
./build/Demo_psrdada_online --smac ... --debug
```

### Q3: Block大小如何计算？

**A:** 
```
包大小 = PKT_HEADER + PKT_DATA (例如: 64 + 8192 = 8256)
单批次发送/接收大小 = 包大小 × SEND_N (例如: 8256 × 64 = 528,384)
Block大小 = 包大小 × PKT_PER_BLOCK（ = SEND_N × SEND_NUM_PER_BLOCK ） (例如: 8256 × 16384 = 135,266,304)
Block大小是单批次大小的整数倍！
```
**重要：** v1.2.0会自动处理非整数倍的情况，无需手动调整。

### Q4: 数据保存在哪里？

**A:** 默认保存在 `./data_out/` 目录，文件格式为 `*.dada`

查看输出文件：
```bash
ls -lh ./data_out/*.dada
```

### Q5: 如何查看实时进度？

**A:** 程序会自动每2秒打印进度：
```
[Progress] Blocks written: 10 | Ring buffer: 25.3% full (256/1024 MB)
```

或查看日志文件：
```bash
tail -f ./data_out/logs/demo_psrdada_online.log
```

### Q6: Ring buffer已存在怎么办？

**A:** 停止并清理：
```bash
./run_demo.sh stop
```

或者手动清理：
```bash
dada_db -k 0xdada -d
```

---

## 版本历史

### v1.2.0 (2026-02-10)
- 🐛 **修复critical bug**: 包大小重复计算导致block不对齐
- 🐛 **修复critical bug**: 缓冲区空间不足检查逻辑错误
- ✨ **新增**: Debug模式（`--debug`参数）
- ✨ **改进**: 简化日志输出，正常模式更清晰
- ✨ **改进**: 智能缓冲区管理，自动检测并请求新block
- 🔧 **重构**: 简化block写入逻辑
- 📖 **文档**: 新增详细的bug修复说明文档

详细改进请参考：[BUGFIXES_AND_IMPROVEMENTS.md](BUGFIXES_AND_IMPROVEMENTS.md)

### v1.1.0
- ✅ 智能内存注册，支持连续/非连续内存
- ✅ 分块RDMA注册支持
- ✅ 底层ipcbuf API集成
- ✅ 资源释放顺序修复
- ✅ 增强错误处理

### v1.0.0
- 🎉 初始版本发布
- 基础RDMA接收功能
- PSRDADA ring buffer集成

---

## 故障排查

### 内存不连续问题

```bash
# 检查ring
./check_psrdada_ring.sh 0xdada

# 如果不连续，增加SHMMAX
sudo sysctl -w kernel.shmmax=17179869184
```

详见：[NON_CONTIGUOUS_MEMORY_SOLUTION.md](NON_CONTIGUOUS_MEMORY_SOLUTION.md)

### PSRDADA 缓冲错误

```bash
# 问题: "Failed to connect to dada_hdu"
# 解决: 确保缓冲已创建
dada_db -k 0xdada -b $block_size -p -l 
```

### RDMA 设备错误

```bash
# 检查 IB 设备
ibv_devices
lspci | grep -i infiniband

# 检查网络配置
ifconfig
ethtool <interface>
```

### 编译问题

```bash
# 检查 psrdada 库
pkg-config --exists psrdada && echo "OK" || echo "psrdada not found"
pkg-config --cflags psrdada
pkg-config --libs psrdada
```

## 原始工程位置

此模块原始文件位置：
- `libsrc/udp_rdma/` - 原始 RDMA 模块
- `libsrc/udp_rdma/demo/Demo_psrdada_online.cpp` - 原始演示

