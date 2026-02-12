# RDMA + PSRDADA 模块 - 快速开始指南

## 🚀 快速开始 (5分钟)

### 1️⃣ 编译

```bash
cd rdma_dada
./build.sh
```

**编译输出**: `rdma_dada/build/Demo_psrdada_online`

### 2️⃣ 配置参数 (可选)

编辑 `run_demo.sh` 调整网络参数：

```bash
# 网络参数
SMAC="02:a2:02:00:02:fa"      # 源MAC地址
DMAC="10:70:fd:11:e2:e3"      # 目标MAC地址
SIP="10.17.16.60"              # 源IP地址
DIP="10.17.16.11"              # 目标IP地址
SPORT="60000"                  # 源端口
DPORT="17201"                  # 目标端口

# PSRDADA配置
PKT_HEADER=64                  # 包头大小(字节)
PKT_DATA="8192"               # 数据大小(字节)
SEND_N=64                     # 批处理大小
PKT_PER_BLOCK=16384           # 每个block的包数
NBUFS="8"                     # ring buffer块数
```

### 3️⃣ 启动接收器

使用一键启动脚本（**推荐**）：

```bash
./run_demo.sh start
```

脚本会自动：
- ✅ 创建 PSRDADA ring buffer (key=0xdada)
- ✅ 启动 dada_dbdisk 写盘进程
- ✅ 启动 RDMA 接收器
- ✅ 等待数据并自动保存到 `./data_out/`

### 4️⃣ 停止接收器

按 `Ctrl+C` 优雅退出，或在另一个终端执行：

```bash
./run_demo.sh stop
```

### 5️⃣ 查看状态

```bash
./run_demo.sh status
```

---

## 📊 输出说明

### 正常模式（默认）

简洁的输出，只显示关键信息：

```
[Main] Connecting to PSRDADA ring buffer (key=0xdada)...
[Main] ✓ psrdada ring buffer initialized
[RDMA] Using normal receive mode (with copy to ring buffer)
[Progress] Blocks written: 10 | Ring buffer: 25.3% full (256/1024 MB)
[Progress] Blocks written: 20 | Ring buffer: 50.1% full (512/1024 MB)
```

### Debug模式

需要详细调试信息时，编辑 `run_demo.sh`，在启动命令中添加 `--debug`：

```bash
CMD="./build/Demo_psrdada_online \
    --smac ${SMAC} --dmac ${DMAC} \
    --sip ${SIP} --dip ${DIP} --sport ${SPORT} --dport ${DPORT} \
    --key ${KEY} --device ${DEVICE} --gpu ${GPU} --cpu ${CPU} \
    --pkt_size ${PKT_SIZE} --send_n ${SEND_N} \
    --file-bytes ${FILE_BYTES} \
    --debug"
```

Debug输出包含：
- 详细的buffer获取和释放过程
- 每次写入计数的变化  
- memcpy操作的详细信息
- 轮询统计
- 所有内部状态变化

---

## 🛠️ 手动运行（高级）

我们提供了一个简单启动脚本 `run_demo.sh`，放在项目根目录，用于快速启动 demo 并在后台运行 `dada_dbdisk` 将数据写入磁盘。脚本顶部包含可编辑的变量：网络参数、`PKT_HEADER`、`PKT_DATA`、`PKT_PER_BLOCK`、`NBUFS`（每个 block 的包数与环大小），以及 `DUMP_DIR` 和 `DUMP_HEADER`。

脚本行为概览：
- 计算每个 block 的字节数： `(PKT_HEADER + PKT_DATA) * PKT_PER_BLOCK`，以及总环大小 `BLOCK_BYTES * NBUFS`。
- 若系统中存在 `dada_db`，脚本会尝试创建 PSRDADA 环形缓冲（key 使用 `--key` 指定的值）；若不存在则打印手动创建命令供参考。
- 启动 `Demo_psrdada_online` 并传递 `--dump-dir` 与 `--dump-header`（默认 `./data_out` 和 `header/header_GZNU.header`），随后调用 `dada_dbdisk -k <key> -D <out_dir>` 在后台写盘。

运行示例（修改脚本顶部变量后运行）：
```bash
cd rdma_dada
./run_demo.sh
```

或直接用 demo 的命令行选项启动并控制写盘：
```bash
./build/Demo_psrdada_online --smac a0:88:c2:6b:40:c6 --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 --dip 192.168.14.12 --sport 61440 --dport 4144 \
  --key 0xdada --dump-dir ./data_out --dump-header header/header_GZNU.header
```


### 4️⃣ 监控数据

另开一个终端：
```bash
watch -n 1 'dada_dbmetric -k 0xdada'
```

---

## 📋 常见选项

| 选项 | 说明 | 示例 |
|------|------|------|
| `-d` | IB 设备号 | 0 (mlx5_0) |
| `--smac` | 源 MAC 地址 | `a0:88:c2:6b:40:c6` |
| `--dmac` | 目标 MAC 地址 | `c4:70:bd:01:43:c8` |
| `--sip` | 源 IP | `192.168.14.13` |
| `--dip` | 目标 IP | `192.168.14.12` |
| `--sport` | 源端口 | `61440` |
| `--dport` | 目标端口 | `4144` |
| `--gpu` | GPU 号 | 0 (默认) |
| `--pkt_size` | 数据包大小 | 6414 (默认) |
| `--send_n` | 批量大小 | 64 (默认) |
| `--key` | PSRDADA KEY | `0xdada` (默认) |
| `--help` | 显示帮助 | |

---

## 🔧 系统要求

- **OS**: Linux (Ubuntu 20.04+ 推荐)
- **硬件**: InfiniBand/RoCE 网卡
- **库**: 
  - `libpsrdada-dev`
  - `libibverbs-dev`
  - `librdmacm-dev`
  - `pthreads`

### 检查环境

```bash
# 检查 IB 设备
ibv_devices

# 检查 psrdada 库
pkg-config --exists psrdada && echo "✓ psrdada found" || echo "✗ psrdada missing"

# 检查网络接口
ifconfig | grep -i ib
```

---

## 📂 文件结构

```
rdma_dada/
├── build/               # 编译输出目录
│   └── Demo_psrdada_online  # ← 可执行文件在这里
├── include/             # 头文件
├── src/                 # 源代码
├── demo/                # 演示程序
├── CMakeLists.txt       # CMake 配置
├── build.sh             # 编译脚本
├── README.md            # 详细文档
└── EXTRACTION_SUMMARY.md # 模块提取说明
```

---

## 🐛 故障排查

### 问题: "Failed to connect to dada_hdu"

**原因**: PSRDADA 环形缓冲未创建
```bash
# 解决方案
sudo dada_db -k 0xdada -b 8G -p 4
```

### 问题: "Failed to open IB device"

**原因**: IB 设备不存在或 -d 参数错误
```bash
# 检查可用设备
ibv_devices

# 使用正确的设备号 (0 表示 mlx5_0)
./Demo_psrdada_online -d 0 ...
```

### 问题: 编译错误 "undefined reference to psrdada"

**原因**: psrdada 库未安装
```bash
# 安装依赖
sudo apt-get install libpsrdada-dev

# 验证
pkg-config --cflags --libs psrdada
```

### 问题: "Permission denied"

**原因**: 没有权限访问 PSRDADA 共享内存
```bash
# 解决方案
sudo chmod 666 /dev/shm/dada*
```

---

## ⏱️ 性能监控

实时监控接收性能：

```bash
# 打开接收器后，在另一个终端运行：
watch -n 1 'dada_dbmetric -k 0xdada'

# 输出示例：
# header_state          : filled
# blocks_in_use         : 3
# bytes_written         : 10485760 (10 MB)
# bytes_read            : 10485760
# bytes_available       : 8373657600
```

---

## 📊 性能参数说明

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| `pkt_size` | 6414 | 网络数据包数据部分大小 |
| `send_n` | 64 | 批量处理包数 (越大越有效率，但延迟更高) |
| `--gpu` | 0-7 | 目标 GPU 号 |
| `-d` (device) | 0-3 | IB 设备号 |

---

## 💡 常见用例

### 用例 1: 本机测试

```bash
# 终端 1: 运行接收器
./Demo_psrdada_online -d 0 \
  --smac $(ip link show | grep -A1 infiniband | tail -1 | awk '{print $2}') \
  --dmac $(ip link show | grep -A1 infiniband | tail -1 | awk '{print $2}') \
  --sip 192.168.14.13 \
  --dip 192.168.14.13 \
  --sport 61440 \
  --dport 4144

# 终端 2: 监控缓冲
watch -n 1 'dada_dbmetric -k 0xdada'
```

### 用例 2: 高性能配置

```bash
./Demo_psrdada_online -d 0 \
  --smac a0:88:c2:6b:40:c6 \
  --dmac c4:70:bd:01:43:c8 \
  --sip 192.168.14.13 \
  --dip 192.168.14.12 \
  --sport 61440 \
  --dport 4144 \
  --gpu 0 \
  --pkt_size 6414 \
  --send_n 128 \
  --cpu 0        # 绑定到 CPU 0
```

### 用例 3: GPU 直接写入

```bash
./Demo_psrdada_online -d 0 \
  ... \
  --gpu 0        # GPU 内存直接接收
```

---

## 📚 更多信息

- 详细文档: 查看 [README.md](README.md)
- 完整说明: 查看 [EXTRACTION_SUMMARY.md](EXTRACTION_SUMMARY.md)
- API 文档: 查看 header 文件注释

---

## ✅ 验证安装

运行演示程序和查看帮助：

```bash
./build/Demo_psrdada_online --help
```

应该看到类似输出：
```
Usage:
    ./Demo_psrdada_online [options]
Options:
    -d, NIC device number (default: 0)
    --smac, source MAC address (required)
    ...
```

---

**准备好了？开始使用吧！** 🎉
