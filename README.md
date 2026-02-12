# RDMA + PSRDADA Standalone Module

提取的 RDMA 与 psrdada 环形缓冲集成的独立模块。

## ✨ 最新特性 (v1.2.0)

- ✅ **智能缓冲区管理**: 自动检测并请求新的ring buffer块，避免空间不足
- ✅ **精确包大小计算**: 修复包头重复计算问题，确保block大小完美对齐
- ✅ **Debug模式**: 可选的详细日志输出，方便问题诊断
- ✅ **简洁输出**: 正常模式下输出清晰简洁，仅显示关键进度信息
- ✅ **增强错误检查**: 全面的参数验证和错误提示
- ✅ **智能内存注册**: 自动检测并适配连续/非连续内存布局
- ✅ **分块RDMA注册**: 支持非连续PSRDADA ring buffer
- ✅ **自动数据写盘**: 内置dada_dbdisk后台支持

## 功能概述

- **高速RDMA接收**: 基于 RoCE v2 的零拷贝网络数据接收
- **PSRDADA集成**: 将RDMA数据直接写入psrdada环形缓冲区
- **智能缓冲管理**: 自动检测剩余空间并请求新block
- **灵活输出模式**: 支持正常模式和debug模式
- **数据持久化**: 自动将ring buffer数据写入磁盘文件

## 目录结构

```
rdma_dada/
├── CMakeLists.txt           # 编译配置
├── include/                 # 头文件
│   ├── RoCEv2Dada.h        # RDMA 类定义（重命名）
│   ├── ibv_utils.h         # InfiniBand 工具函数
│   ├── pkt_gen.h           # 数据包生成工具
│   └── psrdada_ringbuf.h   # PSRDADA 环形缓冲适配器（增强）
├── src/                     # 源代码
│   ├── RoCEv2Dada.cpp      # RDMA 实现（BUG修复）
│   ├── ibv_utils.cpp       # InfiniBand 工具实现（资源释放修复）
│   ├── pkt_gen.cpp         # 数据包生成实现
│   └── psrdada_ringbuf.cpp # PSRDADA 适配器实现（非连续内存支持）
├── demo/                    # 演示程序
│   └── Demo_psrdada_online.cpp # RDMA + PSRDADA 集成演示
├── header/                  # PSRDADA header 模板
│   └── array_GZNU.header   # 示例header文件
├── build.sh                 # 快速编译脚本
├── run_demo.sh             # 快速启动脚本（一键运行）
├── check_psrdada_ring.sh   # Ring buffer检查工具
└── README.md               # 本文件

### 文档目录
├── QUICKSTART.md                  # ⭐ 5分钟快速开始
├── WORKFLOW.md                    # 📊 工作流程详解
├── BUGFIXES_AND_IMPROVEMENTS.md  # 🐛 v1.2.0 Bug修复说明
├── USAGE_EXAMPLE.md              # 📖 使用示例
├── PSRDADA_ROCE_INTEGRATION.md   # 🔧 集成技术细节
└── NON_CONTIGUOUS_MEMORY_SOLUTION.md  # 💾 非连续内存方案
```

## 快速开始

### 推荐方式：一键启动

```bash
# 1. 编译
./build.sh

# 2. 配置网络参数（编辑 run_demo.sh）
vim run_demo.sh  # 修改 SMAC, DMAC, SIP, DIP 等

# 3. 启动（自动创建ring buffer + 启动接收器 + 启动写盘器）
./run_demo.sh start

# 4. 查看状态
./run_demo.sh status

# 5. 停止（Ctrl+C 或执行）
./run_demo.sh stop
```

**输出：** 数据文件保存在 `./data_out/*.dada`

详细说明请参考 [QUICKSTART.md](QUICKSTART.md)

---

## 编译和运行

详细说明请查看：
- **快速开始**: [QUICKSTART.md](QUICKSTART.md) - 5分钟上手指南
- **工作流程**: [WORKFLOW.md](WORKFLOW.md) - 详细的架构和流程说明
- **Bug修复**: [BUGFIXES_AND_IMPROVEMENTS.md](BUGFIXES_AND_IMPROVEMENTS.md) - v1.2.0改进说明

---

### 系统依赖

- **PSRDADA**: `libpsrdada-dev`
- **InfiniBand**: `libibverbs-dev`, `librdmacm-dev`
- **CUDA** (可选): 用于 GPU 直接内存访问

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install libpsrdada-dev libibverbs-dev librdmacm-dev

# 验证 psrdada 库
pkg-config --cflags --libs psrdada
```

## 编译方法

### 方法一：使用编译脚本

```bash
cd rdma_dada
bash build.sh
```

### 方法二：使用 CMake

```bash
cd rdma_dada
mkdir -p build
cd build
cmake ..
make
```

## 运行演示

### 先决条件

1. **创建 PSRDADA 环形缓冲**:
```bash
# 创建 key 为 0xdada 的环形缓冲，大小为 8GB
dada_db -k 0xdada -b 8G 
```

2. **验证缓冲创建**:
```bash
ipcs -m | grep dada
dada_dbmetric -k 0xdada
```

### 启动接收器

```bash
./build/Demo_psrdada_online \
  -d 0 \
  --smac a0:88:c2:6b:40:c6 \
  --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 \
  --dip 192.168.14.12 \
  --sport 61440 \
  --dport 4144 \
  --gpu 0 \
  --pkt_size 6414 \
  --send_n 64 \
  --key 0xdada
```

### 监控缓冲

在另一个终端：
```bash
watch -n 1 'dada_dbmetric -k 0xdada'

## 使用脚本快速启动并写盘

我们提供 `run_demo.sh`（项目根目录），用于快速启动 `Demo_psrdada_online` 并在后台启动 `dada_dbdisk` 将 PSRDADA 环形缓冲写入磁盘。

脚本顶部包含可编辑变量：
- `SMAC`, `DMAC`, `SIP`, `DIP`, `SPORT`, `DPORT` — 网络参数
- `KEY` — PSRDADA key（默认 `0xdada`）
- `PKT_HEADER`, `PKT_DATA` — 每包头/数据字节数（用于计算 block 大小）
- `PKT_PER_BLOCK` — 每个 block 包含的包数
- `NBUFS` — 环中 block 数目
- `DUMP_DIR` — dada_dbdisk 输出目录（默认 `./data_out`）
- `DUMP_HEADER` — header 模板文件路径（默认 `header/header_GZNU.header`）

脚本行为：
- 计算 `BLOCK_BYTES = (PKT_HEADER + PKT_DATA) * PKT_PER_BLOCK`，以及总环大小 `BLOCK_BYTES * NBUFS`。
- 如果系统存在 `dada_db`，脚本会尝试以给定参数创建 PSRDADA 环形缓冲；否则会打印建议的 `dada_db` 命令供手动执行。
- 启动 `Demo_psrdada_online`，并传递 `--dump-dir` 与 `--dump-header`，随后在后台运行 `dada_dbdisk -k <key> -D <out_dir>` 写盘。

使用示例：
```bash
cd rdma_dada
./run_demo.sh
```

或者直接使用 demo 的命令行选项：
```bash
./build/Demo_psrdada_online --smac a0:88:c2:6b:40:c6 --dmac c4:70:bd:01:43:c8 \
    --sip 192.168.14.13 --dip 192.168.14.12 --sport 61440 --dport 4144 \
    --key 0xdada --dump-dir ./data_out --dump-header header/header_GZNU.header
```

```

## 主要组件

### PsrdadaRingBuf 类（增强版）

环形缓冲适配器，封装 psrdada HDU/IPC 操作，支持非连续内存：

```cpp
class PsrdadaRingBuf {
    // 基础操作
    int Init(uint32_t key, uint64_t block_bytes, uint64_t nbufs, 
             const char *header_template_path);
    char* GetWriteBuffer(uint64_t bytes);  // 获取写缓冲（底层ipcbuf API）
    int MarkWritten(uint64_t bytes);       // 标记已写入（ipcbuf_mark_filled）
    uint64_t GetFreeSpace();               // 获取可用空间
    uint64_t GetUsedSpace();               // 获取已用空间
    uint64_t GetBlockSize();               // 获取单个block大小
    
    // RDMA注册（智能自动切换）
    struct ibv_mr* RegisterWholeRing(struct ibv_pd *pd, int access);
    // 自动检测内存布局：
    //   - 连续内存 → 单个MR注册（最优性能）
    //   - 非连续内存 → 自动切换到分块注册
    
    // 分块注册（新增，支持非连续内存）
    int RegisterRingBlocks(struct ibv_pd *pd, int access);
    struct ibv_mr* GetCurrentBlockMr();    // 获取当前block的MR
    void UnregisterAllBlocks();            // 清理所有MRs
    
    // 数据输出
    int StartDbdisk(const char *out_dir, const char *header_template_path);
    int StopDbdisk();
};
```

### RoCEv2Dada 类

RDMA 网络接收/发送实现，修复了多个严重BUG：

```cpp
class RoCEv2Dada {
    struct RdmaParam {
        uint8_t device_id;   // IB 设备号
        uint8_t gpu_id;      // GPU 号
        uint32_t pkt_size;   // 包数据大小
        uint32_t send_n;     // 批量大小
        int bind_cpu_id;     // CPU绑定
        bool SendOrRecv;     // 发送/接收模式
        int DirectToRing;    // 直接写入ring
        char SAddr[64];      // 源 IP
        char DAddr[64];      // 目标 IP
        char SMacAddr[64];   // 源 MAC
        char DMacAddr[64];   // 目标 MAC
        char src_port[64];   // 源端口
        char dst_port[64];   // 目标端口
        
        // 回调函数
        DataSend DataSendBuff;   // 数据发送完成回调
        GetBuff GetBuffPtr;       // 获取缓冲区回调
    };
    
    int Start();                    // 启动接收线程
    void * GetIbvRes() const;       // 获取 IB 资源指针
    int SetDirectMr(struct ibv_mr *mr);  // 设置直接写入MR
};
```

## 工作流程

```
1. 初始化阶段
   ├─ 创建 PSRDADA 环形缓冲对象
   ├─ 通过dada_db创建或连接共享内存ring
   ├─ 初始化 psrdada 环形缓冲 (ipcbuf API)
   ├─ 创建 RDMA 接收对象并初始化网络
   ├─ 智能注册内存：
   │  ├─ 检查内存是否连续
   │  ├─ 连续 → 单个MR注册（最优）
   │  └─ 非连续 → 自动分块注册每个block
   └─ 可选：启动dada_dbdisk后台写盘

2. 接收阶段（零拷贝）
   ├─ 调用GetWriteBuffer获取下一个可写block
   │  └─ 内部调用ipcbuf_get_next_write
   ├─ 获取当前block的MR（分块模式）
   │  └─ GetCurrentBlockMr() 返回正确的lkey
   ├─ 配置RDMA接收SGE（使用正确的lkey）
   ├─ RoCE网卡直接DMA写入ring buffer
   ├─ 轮询完成队列 (CQ)
   ├─ 调用MarkWritten标记block已填充
   │  └─ 内部调用ipcbuf_mark_filled
   └─ 自动切换到下一个block

3. 消费阶段 (dada_dbdisk 或其他)
   ├─ dada_dbdisk持续读取ring buffer
   ├─ 自动写入.dada文件到指定目录
   ├─ 环形缓冲自动循环利用
   └─ 支持多个reader并发读取
```

## 非连续内存支持

### 问题
PSRDADA的ring buffer可能由多个不连续的共享内存段组成，传统方法无法用单个RDMA MR注册。

### 解决方案
代码自动检测并适配：

**内存连续时**：
```
[RegisterWholeRing] Memory is contiguous: 8 blocks × 413184 bytes
[RegisterWholeRing] Success: base=0x7f... size=3 MB rkey=0x1a2b3c4d
```

**内存不连续时（自动切换）**：
```
Error: Ring buffer blocks are NOT contiguous!
Automatically switching to per-block registration mode...
[RegisterRingBlocks] Registering 8 blocks individually...
[RegisterRingBlocks] Block  0: addr=0x7f1000 size=403 KB rkey=0x1a2b3c4d
[RegisterRingBlocks] Block  1: addr=0x7f2000 size=403 KB rkey=0x2b3c4d5e
...
[RegisterRingBlocks] Successfully registered all 8 blocks
```

### 使用方法
无需修改代码，自动适配：
```cpp
// 自动检测并选择最佳注册方式
struct ibv_mr *mr = ringbuf->RegisterWholeRing(pd, access);

// 接收数据
char *ptr = ringbuf->GetWriteBuffer(size);
struct ibv_mr *block_mr = ringbuf->GetCurrentBlockMr();
// 使用正确的lkey配置SGE
sge[i].lkey = block_mr ? block_mr->lkey : mr->lkey;
```

详见：[NON_CONTIGUOUS_MEMORY_SOLUTION.md](NON_CONTIGUOUS_MEMORY_SOLUTION.md)

## 性能优化

- **零拷贝**: RDMA 直接写入 GPU/主机内存
- **智能内存注册**: 自动选择最优注册方式
  - 连续内存：单个MR（最优性能）
  - 非连续内存：分块注册（性能损失<5%）
- **底层ipcbuf API**: 精确的block级控制
- **批量处理**: 一次处理多个数据包，减少系统调用
- **CPU 亲和性**: 线程绑定到指定 CPU 核心
- **环形缓冲**: psrdada 高效的共享内存管理
- **后台写盘**: dada_dbdisk异步写入，不阻塞接收

## 已修复的严重BUG

### 1. 未初始化的时间戳变量
- **问题**: `ts_start`在第一次使用前未初始化
- **影响**: 第一次带宽计算使用垃圾值
- **修复**: 在循环开始前初始化`clock_gettime(&ts_start)`

### 2. 格式字符串类型错误
- **问题**: 使用`%x`打印指针，`%d`打印`long int`
- **影响**: 64位系统上可能段错误
- **修复**: 改用`%p`和`%ld`正确格式

### 3. RDMA资源释放顺序错误
- **问题**: 先释放PD，再释放依赖它的QP和MR
- **影响**: 访问已释放内存导致崩溃
- **修复**: 正确顺序 QP → MR → CQ → PD

### 4. 未检查返回值
- **问题**: 销毁函数未检查返回值
- **影响**: 资源泄漏无法检测
- **修复**: 添加完整的返回值检查和错误日志

### 5. 缓冲区溢出风险
- **问题**: 使用`strcpy`而非`strncpy`
- **影响**: 可能导致缓冲区溢出
- **修复**: 使用安全的字符串操作函数

## 实用工具脚本

### check_psrdada_ring.sh
检查ring buffer内存连续性：
```bash
./check_psrdada_ring.sh 0xdada
```

输出：
- 共享内存段数量
- 内存连续性状态
- 系统SHMMAX配置
- 修复建议

### setup_psrdada_ring.sh
自动配置ring buffer：
```bash
./setup_psrdada_ring.sh --force
./setup_psrdada_ring.sh -p 6414 -n 64 -b 8
```

功能：
- 自动检查和设置SHMMAX
- 创建ring buffer
- 验证内存连续性
- 提供详细诊断信息

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
批次大小 = 包大小 × SEND_N (例如: 8256 × 64 = 528,384)
Block大小 = 包大小 × PKT_PER_BLOCK (例如: 8256 × 16384 = 135,266,304)
```

**重要：** v1.2.0会自动处理非整数倍的情况，无需手动调整。

### Q4: 数据保存在哪里？

**A:** 默认保存在 `./data_out/` 目录，文件名格式为 `YYYY-MM-DD-HH:MM:SS.dada`

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

# 重新创建ring
dada_db -k 0xdada -d
./setup_psrdada_ring.sh --force
```

详见：[NON_CONTIGUOUS_MEMORY_SOLUTION.md](NON_CONTIGUOUS_MEMORY_SOLUTION.md)

### PSRDADA 缓冲错误

```bash
# 问题: "Failed to connect to dada_hdu"
# 解决: 确保缓冲已创建
dada_db -k 0xdada -b 8G -p 4

# 问题: 权限错误
# 解决: 设置共享内存权限
sudo chmod 666 /dev/shm/dada*
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

## 许可证

Copyright (C) 2024-2026 by ZheJiang Lab. All rights reserved.

## 更新历史

- **v1.1.0** (2026年2月): 
  - ✅ 非连续内存支持（自动分块注册）
  - ✅ 修复4个严重BUG（资源释放、未初始化变量等）
  - ✅ 底层ipcbuf API集成
  - ✅ 智能自动切换注册模式
  - ✅ 增强错误处理和日志
  - ✅ 新增实用脚本和完整文档
  
- **v1.0.0** (2026年2月): 初版，支持 RDMA + PSRDADA 集成
