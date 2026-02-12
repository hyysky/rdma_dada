# RDMA + PSRDADA 独立模块提取完成

## 概述

已成功将 RDMA 结合 psrdada 的数据包接收和存储部分从主工程中独立出来，放置在 `rdma_dada` 目录下。该模块完全自包含，可单独编译和运行。

## 提取内容

### 源文件和头文件

#### 核心库文件

| 文件 | 来源 | 描述 |
|------|------|------|
| `src/ZjlabRoCEv2.cpp` | `libsrc/udp_rdma/src/` | RDMA 网络通信核心实现 |
| `src/ibv_utils.cpp` | `libsrc/udp_rdma/src/` | InfiniBand 工具函数 |
| `src/pkt_gen.cpp` | `libsrc/udp_rdma/src/` | UDP 数据包生成工具 |
| `src/psrdada_ringbuf.cpp` | `libsrc/udp_rdma/src/` | PSRDADA 环形缓冲适配器 |

#### 头文件

| 文件 | 功能 |
|------|------|
| `include/ZjlabRoCEv2.h` | RDMA 类定义和参数结构 |
| `include/ibv_utils.h` | InfiniBand 资源管理接口 |
| `include/pkt_gen.h` | UDP 数据包生成接口 |
| `include/psrdada_ringbuf.h` | 环形缓冲访问接口 |

### 演示程序

| 文件 | 功能 |
|------|------|
| `demo/Demo_psrdada_online.cpp` | 完整的 RDMA 接收 + PSRDADA 存储演示 |

### 构建文件

| 文件 | 用途 |
|------|------|
| `CMakeLists.txt` | CMake 构建配置 |
| `build.sh` | 快速编译脚本 |

## 目录结构

```
rdma_dada/
├── CMakeLists.txt              # CMake 构建配置
├── build.sh                    # 编译脚本
├── README.md                   # 模块文档
├── include/                    # 头文件目录
│   ├── ZjlabRoCEv2.h          # RDMA 类
│   ├── ibv_utils.h            # IB 工具
│   ├── pkt_gen.h              # 数据包生成
│   └── psrdada_ringbuf.h      # 环形缓冲
├── src/                        # 源文件目录
│   ├── ZjlabRoCEv2.cpp        # RDMA 实现
│   ├── ibv_utils.cpp          # IB 工具实现
│   ├── pkt_gen.cpp            # 数据包生成实现
│   └── psrdada_ringbuf.cpp    # 环形缓冲实现
└── demo/                       # 演示程序
    └── Demo_psrdada_online.cpp # 集成演示
```

## 主要类和接口

### PsrdadaRingBuf 类

封装 psrdada HDU 和 IPCIO 的 C++ 适配器：

```cpp
class PsrdadaRingBuf {
    int Init(uint32_t key);                  // 初始化环形缓冲
    char* GetWriteBuffer(uint64_t bytes);   // 获取可写缓冲区
    int MarkWritten(uint64_t bytes);        // 标记已写入数据
    int StartBlock() / int StopBlock();      // 观测块边界
    uint64_t GetFreeSpace();                // 获取可用空间
    uint64_t GetUsedSpace();                // 获取已用空间
    struct ibv_mr* RegisterWholeRing(...);  // 为 RDMA 注册内存
};
```

### ZjlabRdma 类

RDMA 网络接收/发送实现：

```cpp
class ZjlabRdma {
    struct RdmaParam {
        uint8_t device_id;        // IB 设备号（如 0 表示 mlx5_0）
        uint8_t gpu_id;           // GPU 设备号
        uint32_t pkt_size;        // 数据包大小
        uint32_t send_n;          // 批量大小
        char SAddr[64];           // 源 IP
        char DAddr[64];           // 目标 IP
        char SMacAddr[64];        // 源 MAC
        char DMacAddr[64];        // 目标 MAC
    };
    
    int Start();                  // 启动接收线程
    void * GetIbvRes() const;    // 获取 IB 资源指针
};
```

## 功能特性

### ✅ 完整实现的功能

- **RDMA 网络接收**: 基于 RoCE v2 的高速网络接收
- **PSRDADA 环形缓冲**: 共享内存环形缓冲管理
- **数据写入**: 网络数据直接写入 psrdada 环形缓冲
- **零拷贝优化**: RDMA 直接写入 GPU/主机内存
- **批量处理**: 支持批量数据处理优化
- **CPU 亲和性**: 线程绑定到指定 CPU 核心
- **性能监控**: 实时带宽和数据计数统计

### 📝 工作流程

1. **初始化阶段**
   - 创建 PSRDADA 环形缓冲对象
   - 初始化 psrdada 库（连接已创建的共享内存）
   - 创建 RDMA 接收器并配置网络参数
   - 注册 psrdada 环形缓冲为 RDMA 可写内存

2. **接收阶段**
   - RDMA 接收网络数据包
   - 轮询完成队列
   - 批量拷贝数据到环形缓冲
   - 通知 psrdada 已写入新数据

3. **消费阶段**
   - 外部进程（如 dada_client）读取环形缓冲
   - 处理/存储数据
   - 环形缓冲自动循环利用

## 编译和运行

### 快速编译

```bash
cd rdma_dada
bash build.sh
```

### 编译输出

- 可执行文件: `rdma_dada/build/Demo_psrdada_online`

### 运行演示

```bash
# 第一步：创建 PSRDADA 环形缓冲
dada_db -k 0xdada -b 8G -p 4

# 第二步：启动 RDMA 接收器
./build/Demo_psrdada_online \
  -d 0 \
  --smac a0:88:c2:6b:40:c6 \
  --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 \
  --dip 192.168.14.12 \
  --sport 61440 \
  --dport 4144 \
  --gpu 0 \
  --pkt_size 6414

# 第三步：在另一个终端监控缓冲
watch -n 1 'dada_dbmetric -k 0xdada'
```

## 依赖库

### 必需库

- **PSRDADA**: `libpsrdada-dev`
- **InfiniBand**: `libibverbs-dev`
- **pthreads**: 线程库

### 可选库

- **CUDA**: 用于 GPU 直接内存访问

### 安装依赖 (Ubuntu/Debian)

```bash
sudo apt-get install libpsrdada-dev libibverbs-dev librdmacm-dev
```

## 源工程位置

此模块的原始文件位置：

| 原始位置 | 提取位置 |
|---------|---------|
| `libsrc/udp_rdma/` | `rdma_dada/` |
| `libsrc/udp_rdma/demo/Demo_psrdada_online.cpp` | `rdma_dada/demo/` |
| `libsrc/udp_rdma/src/` | `rdma_dada/src/` |
| `libsrc/udp_rdma/include/` | `rdma_dada/include/` |

## 独立性说明

✅ **完全独立**：
- 不依赖主工程的其他模块
- 可单独编译和运行
- 包含所有必需的头文件和源代码

✅ **自包含**：
- 所有依赖明确列出（PSRDADA, libverbs）
- CMake 配置完整
- 编译脚本简化

✅ **可直接集成**：
- 可作为独立库被其他项目使用
- 提供清晰的 API 接口
- 包含演示代码

## 文件清单

### 头文件 (4 个)
- `include/ZjlabRoCEv2.h`
- `include/ibv_utils.h`
- `include/pkt_gen.h`
- `include/psrdada_ringbuf.h`

### 源文件 (4 个)
- `src/ZjlabRoCEv2.cpp`
- `src/ibv_utils.cpp`
- `src/pkt_gen.cpp`
- `src/psrdada_ringbuf.cpp`

### 演示文件 (1 个)
- `demo/Demo_psrdada_online.cpp`

### 构建文件 (2 个)
- `CMakeLists.txt`
- `build.sh`

### 文档文件 (2 个)
- `README.md`
- `EXTRACTION_SUMMARY.md` (本文件)

## 总代码行数统计

| 文件类型 | 文件数 | 代码行数 |
|---------|--------|---------|
| 头文件 (.h) | 4 | ~300 行 |
| 源文件 (.cpp) | 5 | ~1500 行 |
| 构建文件 | 2 | ~50 行 |
| 文档 | 2 | ~400 行 |
| **总计** | **13** | **~2250 行** |

## 后续使用建议

1. **作为库使用**
   ```cpp
   #include "ZjlabRoCEv2.h"
   #include "psrdada_ringbuf.h"
   
   // 创建 RDMA 和 PSRDADA 对象
   PsrdadaRingBuf ringbuf;
   ZjlabRdma rdma(param);
   ```

2. **集成到其他项目**
   - 复制 `include/` 和 `src/` 到目标项目
   - 在 CMakeLists.txt 中链接库
   - 按 README.md 配置环境

3. **扩展功能**
   - 添加数据压缩/处理
   - 集成数据导出模块
   - 实现自定义控制协议

## 故障排查

详见 [README.md](README.md) 的故障排查部分。

## 版本信息

- **模块版本**: v1.0.0
- **提取日期**: 2026年2月6日
- **基于工程**: phase-field-telescope
- **原始版本**: ZjlabRdma v0.0.3, PsrdadaRingBuf v1.0.0

## 许可证

Copyright (C) 2024-2026 by ZheJiang Lab. All rights reserved.

---

**提取完成** ✅

该模块已完全准备好单独编译和使用。详见 `README.md` 获取完整的使用说明。
