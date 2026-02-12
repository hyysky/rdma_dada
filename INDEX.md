# RDMA + PSRDADA 独立模块 - 文档索引

## 📖 阅读指南

根据您的需要，选择对应的文档：

### 🚀 我想快速开始
→ **[QUICKSTART.md](QUICKSTART.md)** (5分钟)
- 快速编译步骤
- 立即运行演示
- 常见问题解答

### 📚 我想深入了解
→ **[README.md](README.md)** (20分钟)
- 完整的功能说明
- 详细的编译步骤
- 系统架构说明
- 故障排查指南

### 📋 我想了解提取内容
→ **[EXTRACTION_SUMMARY.md](EXTRACTION_SUMMARY.md)** (10分钟)
- 提取了哪些文件
- 原始工程位置映射
- 模块统计信息
- 源文件清单

### ✅ 我想查看完成状态
→ **[COMPLETION_REPORT.md](COMPLETION_REPORT.md)** (5分钟)
- 提取完成情况
- 文件清单和大小
- 质量检查清单
- 后续计划

---

## 🎯 快速导航

| 我想... | 查看文档 |
|--------|---------|
| 快速编译和运行 | [QUICKSTART.md](QUICKSTART.md) |
| 了解模块功能 | [README.md](README.md) |
| 查看API文档 | `include/` 目录下的头文件 |
| 查看实现代码 | `src/` 目录下的源文件 |
| 查看演示程序 | [demo/Demo_psrdada_online.cpp](demo/Demo_psrdada_online.cpp) |
| 了解模块提取 | [EXTRACTION_SUMMARY.md](EXTRACTION_SUMMARY.md) |
| 查看完成报告 | [COMPLETION_REPORT.md](COMPLETION_REPORT.md) |

---

## 📁 文件结构概览

```
rdma_dada/                    RDMA + PSRDADA 独立模块
├── 📖 README.md             → 详细使用文档
├── 🚀 QUICKSTART.md         → 5分钟快速开始
├── 📋 EXTRACTION_SUMMARY.md → 提取说明文档
├── ✅ COMPLETION_REPORT.md  → 完成报告
├── 📑 INDEX.md              → 本文档
├── 🔨 build.sh              → 编译脚本 (bash build.sh)
├── ⚙️ CMakeLists.txt         → CMake 配置
│
├── 📂 include/              头文件目录
│   ├── ZjlabRoCEv2.h       RDMA 类定义
│   ├── ibv_utils.h         IB 工具接口
│   ├── pkt_gen.h           UDP 数据包工具
│   └── psrdada_ringbuf.h   环形缓冲接口
│
├── 📂 src/                  源代码目录
│   ├── ZjlabRoCEv2.cpp     RDMA 核心实现
│   ├── ibv_utils.cpp       IB 工具实现
│   ├── pkt_gen.cpp         数据包生成实现
│   └── psrdada_ringbuf.cpp 环形缓冲实现
│
└── 📂 demo/                 演示程序
    └── Demo_psrdada_online.cpp  完整演示程序
```

---

## ⚡ 5分钟快速开始

```bash
# 1. 编译
cd rdma_dada && bash build.sh

# 2. 创建 PSRDADA 环形缓冲
sudo dada_db -k 0xdada -b 8G -p 4

# 3. 运行演示
./build/Demo_psrdada_online \
  -d 0 \
  --smac a0:88:c2:6b:40:c6 \
  --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 \
  --dip 192.168.14.12 \
  --sport 61440 \
  --dport 4144

# 4. 监控 (另一个终端)
watch -n 1 'dada_dbmetric -k 0xdada'
```

更多细节: [QUICKSTART.md](QUICKSTART.md)

---

## 📊 模块信息

| 项目 | 值 |
|------|-----|
| **模块名称** | RDMA + PSRDADA |
| **版本** | v1.0.0 |
| **提取日期** | 2026年2月6日 |
| **文件总数** | 15 个 |
| **代码行数** | ~3900 行 |
| **总大小** | 71.54 KB |
| **编程语言** | C++ (C++11) |
| **编译系统** | CMake 3.5+ |

---

## 🔧 主要功能

- ✅ **RDMA 网络接收** - RoCE v2 高速网络通信
- ✅ **PSRDADA 环形缓冲** - 共享内存管理
- ✅ **零拷贝优化** - GPU/主机内存直接访问
- ✅ **批量处理** - 高效数据批量传输
- ✅ **性能监控** - 实时带宽和数据统计
- ✅ **CPU 亲和性** - 线程绑定优化

---

## 📦 依赖库

### 必需
- `libpsrdada-dev` - PSRDADA 环形缓冲库
- `libibverbs-dev` - InfiniBand 驱动
- `librdmacm-dev` - RDMA 通信库
- `pthreads` - 线程库

### 可选
- `libcuda` - NVIDIA CUDA (GPU 支持)

### 安装 (Ubuntu/Debian)
```bash
sudo apt-get install libpsrdada-dev libibverbs-dev librdmacm-dev
```

---

## 🎓 学习路径

### 初级用户 👶
1. 阅读 [QUICKSTART.md](QUICKSTART.md)
2. 运行演示程序
3. 查看 `--help` 选项

### 中级用户 👨‍💻
1. 阅读 [README.md](README.md)
2. 学习 `include/*.h` 中的 API
3. 研究 `demo/Demo_psrdada_online.cpp` 的实现

### 高级用户 👨‍🔬
1. 研究 `src/` 中的实现细节
2. 参考 [EXTRACTION_SUMMARY.md](EXTRACTION_SUMMARY.md)
3. 根据需要修改和扩展

---

## 🔗 相关资源

### 文档
- [RDMA/RoCE 介绍](README.md#功能概述)
- [PSRDADA 文档](README.md#-psrdada-环形缓冲)
- [性能优化](README.md#性能优化)

### 工具
- `dada_db` - PSRDADA 缓冲创建
- `dada_dbmetric` - 缓冲监控
- `ibv_devices` - IB 设备列表

### 命令
```bash
# 查看帮助
./build/Demo_psrdada_online --help

# 创建缓冲
dada_db -k 0xdada -b 8G -p 4

# 监控缓冲
dada_dbmetric -k 0xdada

# 检查 IB 设备
ibv_devices
```

---

## ❓ 常见问题

### Q: 如何编译？
A: `cd rdma_dada && bash build.sh`

### Q: 如何运行？
A: `./build/Demo_psrdada_online` (需要正确的参数)

### Q: 如何监控？
A: `dada_dbmetric -k 0xdada`

### Q: 如何排查问题？
A: 查看 [README.md](README.md#故障排查) 或 [QUICKSTART.md](QUICKSTART.md#-故障排查)

更多问题: [README.md](README.md#故障排查)

---

## 📞 获取帮助

1. **查看文档**
   - [README.md](README.md) - 详细说明
   - [QUICKSTART.md](QUICKSTART.md) - 快速开始
   - 头文件中的注释

2. **查看示例**
   - [demo/Demo_psrdada_online.cpp](demo/Demo_psrdada_online.cpp)
   - [QUICKSTART.md](QUICKSTART.md#常见用例)

3. **故障排查**
   - [README.md 故障排查](README.md#故障排查)
   - [QUICKSTART.md 故障排查](QUICKSTART.md#-故障排查)

---

## ✅ 完成清单

- [x] 源文件全部提取
- [x] 头文件全部提取
- [x] 构建配置完成
- [x] 文档编写完整
- [x] 演示程序包含
- [x] 快速开始指南
- [x] 质量检查通过

---

## 📜 许可证

Copyright (C) 2024-2026 by ZheJiang Lab. All rights reserved.

---

## 🚀 现在就开始！

选择您的路径：

- **想立即尝试?** → [QUICKSTART.md](QUICKSTART.md)
- **想完全了解?** → [README.md](README.md)
- **想查看细节?** → [EXTRACTION_SUMMARY.md](EXTRACTION_SUMMARY.md)
- **想看代码?** → `src/` 和 `include/` 目录

---

**祝您使用愉快！** 🎉
