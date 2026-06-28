# RDMA + PSRDADA Pipeline - 工作流程详解

本文档详细说明 RDMA + PSRDADA 数据接收管道的工作流程和架构设计。

## 📋 整体架构

管道由三个独立但协调的组件组成：

```
发送端 (RDMA) 
    ↓
┌─────────────────────────────────────────────────────┐
│  1. PSRDADA Ring Buffer (dada_db)                   │
│     - 共享内存环形缓冲区                              │
│     - 多进程访问（写入者+读取者）                      │
└─────────────────────────────────────────────────────┘
    ↓ 写入                           ↑ 读取
┌─────────────────────────┐   ┌─────────────────────────┐
│  2. RDMA 接收器         │   │  3. 数据写盘器          │
│  (Demo_psrdada_online)  │   │  (dada_dbdisk)          │
│  - 接收网络数据         │   │  - 读取ring buffer      │
│  - 写入ring buffer      │   │  - 写入.dada文件        │
└─────────────────────────┘   └─────────────────────────┘
```

## 🔄 数据流程

### 阶段1: 初始化

```bash
./run_demo.sh start
```

1. **创建Ring Buffer**
   ```bash
   dada_db -k 0xdada -b ${BLOCK_BYTES} -n ${NBUFS} -p
   ```
   - 计算block大小: `(PKT_HEADER + PKT_DATA) × PKT_PER_BLOCK`
   - 创建共享内存段
   - 不锁定（-p）,等待写入者连接

2. **启动数据写盘器**
   ```bash
   dada_dbdisk -k 0xdada -D ./data_out -o -z &
   ```
   - 作为读取者连接到ring buffer
   - 等待写入者开始写入数据
   - 后台运行，使用nohup防止收到SIGINT

3. **启动RDMA接收器**
   ```bash
   ./build/Demo_psrdada_online --smac ... --dmac ... --pkt_size 8256 --send_n 64
   ```
   - 作为写入者连接到ring buffer
   - 初始化RDMA资源
   - 注册ring buffer内存（如果连续）
   - 开始接收数据

### 阶段2: 数据接收

**关键参数计算：**
- `pkt_size` = PKT_HEADER + PKT_DATA = 64 + 8192 = **8256 字节**
- `batch_size` = pkt_size × send_n = 8256 × 64 = **528,384 字节**
- `block_size` = pkt_size × PKT_PER_BLOCK = 8256 × 16384 = **135,266,304 字节**
- `每block批次数` = 135,266,304 / 528,384 = **256 批次**

**接收循环：**

```
1. RDMA轮询完成队列 (ibv_poll_cq)
   ↓
2. 累积到一个批次 (send_n 个包)
   ↓
3. 检查当前block剩余空间
   ↓ 不足？
4. 请求新block (GetBuffPtr)
   ↓
5. 拷贝数据到ring buffer (memcpy)
   ↓
6. 递减剩余写入计数 (DecrementWriteCount)
   ↓
7. 检查block是否已满 (IsBlockFull)
   ↓ 已满？
8. 标记block为已写入 (MarkWritten)
   ↓
9. 重新轮询...
```

**智能空间管理：**
```cpp
// 每次写入前检查空间
if (block_bufsz < bytes_needed) {
    // 自动请求新block
    GetBuffPtr(block_bufsz);
}
```

这确保了：
- ✅ 永远不会因空间不足而crash
- ✅ Block大小不需要是批次大小的精确倍数
- ✅ 自动处理剩余空间

### 阶段3: 数据写盘

**dada_dbdisk 行为：**

```
1. 检测到ring buffer有新数据
   ↓
2. 读取完整的block
   ↓
3. 写入.dada文件 (带时间戳)
   ↓
4. 释放block给写入者重用
   ↓
5. 继续监听...
```

**文件输出：**
- 位置: `./data_out/`
- 格式: `2026-02-10-12:34:56.dada`
- 包含: ASCII header + 二进制数据

### 阶段4: 优雅关闭

**触发方式：**
- 接收器完成预定数据量
- 用户按 Ctrl+C
- 执行 `./run_demo.sh stop`

**关闭流程：**

```
1. 捕获信号 (SIGINT/SIGTERM)
   ↓
2. 停止RDMA接收线程
   ↓
3. 发送EOD标记到ring buffer
   ↓
4. 等待dada_dbdisk检测EOD并关闭文件
   ↓
5. 断开与ring buffer的连接
   ↓
6. 销毁ring buffer (run_demo.sh cleanup)
```

## 🔧 关键优化

### 1. 包大小对齐

**问题：** 之前包头被重复计算导致大小不对齐

**解决方案：**
```bash
# run_demo.sh: 传入完整包大小
PKT_SIZE=$(( PKT_HEADER + PKT_DATA ))  # 8256

# 代码: 直接使用，不再加包头
int pkt_len = pkt_size;  // 不是 pkt_size + PKT_HEAD_LEN
```

### 2. 智能Buffer管理

**之前问题：** 只在 `block_bufsz <= 0` 时请求新block，导致剩余16KB时crash

**现在方案：**
```cpp
// 提前检查是否有足够空间
long int bytes_needed = send_n * pkt_len;
if (block_bufsz <= 0 || block_bufsz < bytes_needed) {
    GetBuffPtr(block_bufsz);  // 请求新block
}
```

### 3. 双模式日志

**正常模式：** 简洁输出，只显示进度
```
[Progress] Blocks written: 10 | Ring buffer: 25.3% full (256/1024 MB)
```

**Debug模式：** 详细调试信息
```bash
./build/Demo_psrdada_online ... --debug
```

## 📊 性能监控

### Ring Buffer状态

```bash
# 查看当前状态
./run_demo.sh status

# 或手动查询
dada_dbmetric -k 0xdada
```

### 日志文件

```bash
# 接收器日志
tail -f ./data_out/logs/demo_psrdada_online.log

# 写盘器日志  
tail -f ./data_out/dada_dbdisk.log
```

### 输出文件

```bash
# 检查输出文件
ls -lh ./data_out/*.dada

# 验证文件大小
du -sh ./data_out/
```

## 🐛 故障排查

### 问题1: "Insufficient buffer space"

**原因：** Block大小不是批次大小的整数倍

**解决方案：** 
- 最新代码已自动处理
- 或调整 `PKT_PER_BLOCK` 使其满足：
  ```
  (PKT_HEADER + PKT_DATA) × PKT_PER_BLOCK % (pkt_size × send_n) == 0
  ```

### 问题2: Ring buffer已存在

**现象：**
```
dada_db: key 0xdada already exists
```

**解决方案：**
```bash
./run_demo.sh stop  # 清理旧ring buffer
```

### 问题3: 无输出文件

**检查：**
1. dada_dbdisk是否运行: `./run_demo.sh status`
2. Ring buffer是否有数据写入
3. 目录权限: `ls -ld ./data_out`

---

## 📚 配置参考
PKT_HEADER="8"                 # Bytes per packet header
PKT_DATA="4096"                # Bytes per packet data
PKT_PER_BLOCK="64"             # Number of packets per block
NBUFS="8"                      # Number of blocks in ring

# Output files
NBLOCKSAVE="1"                 # How many blocks per output file
DUMP_DIR="./data_out"          # Output directory
```

## Architecture

### Data Flow
```
UDP/RDMA Source (Sender)
        ↓
Demo_psrdada_online (RDMA Receiver)
        ↓
PSRDADA Ring Buffer (shared memory, key=0xdada)
        ↓
dada_dbdisk (Ring Consumer)
        ↓
.dada files in data_out/
```

### Process Lifecycle

1. **dada_db** creates the ring buffer in shared memory
   - Block size = (header + data) × pkts_per_block
   - Number of blocks = nbufs

2. **Demo_psrdada_online** (writer)
   - Initializes PSRDADA header from template
   - Receives RDMA packets
   - Writes data to ring buffer blocks

3. **dada_dbdisk** (reader)
   - Reads filled blocks from ring buffer
   - Writes 4KB PSRDADA headers + data to `.dada` files
   - Files are automatically managed per size limit

## File Structure

```
rdma_dada/
├── run_demo.sh              # Main orchestration script
├── cleanup.sh               # Emergency cleanup utility
├── build.sh                 # Build script
├── demo/
│   └── Demo_psrdada_online.cpp   # RDMA receiver executable
├── src/
│   └── psrdada_ringbuf.cpp       # Ring buffer interface
├── include/
│   ├── psrdada_ringbuf.h         # Ring buffer header
│   ├── RoCEv2Dada.h              # RDMA headers
│   └── ...
├── header/
│   ├── array_GZNU.header         # PSRDADA header template
│   └── paf_test.header
└── data_out/                # Default output directory (created at runtime)
    ├── demo.log             # Demo process log
    ├── dbdisk.log           # dada_dbdisk log
    └── *.dada               # Output data files
```

## Troubleshooting

### "dada_db not found in PATH"
Install PSRDADA:
```bash
sudo apt-get install psrdada libpsrdada-dev
```

### "Receiver exited immediately"
Check the log:
```bash
tail -f ./data_out/demo.log
```

### "Cannot connect to ring buffer"
The ring buffer may already exist from a previous failed run. Clean up:
```bash
./cleanup.sh
```

Then retry:
```bash
./run_demo.sh start
```

### Manual ring inspection
List all PSRDADA ring buffers:
```bash
dada_db -l
```

Remove a specific ring (e.g., key 0xdada):
```bash
dada_db -k dada -d
```

## Advanced Usage

### Run in foreground (see output directly)
Modify `run_demo.sh` or wrap the command:
```bash
./build/Demo_psrdada_online --smac 00:11:22:33:44:55 --dmac aa:bb:cc:dd:ee:ff \
  --sip 192.168.1.1 --dip 192.168.1.2 --sport 1234 --dport 5678 \
  --key 0xdada --device 0 --gpu 0 --cpu -1 \
  --pkt_size 4104 --send_n 64
```

### Custom buffer layout
Adjust ring parameters in `run_demo.sh`:
```bash
PKT_HEADER="16"           # Larger header
PKT_DATA="8192"           # Larger payload
PKT_PER_BLOCK="32"        # Fewer packets per block
NBUFS="16"                # More blocks = larger ring
NBLOCKSAVE="2"            # 2 blocks per output file
```

### Monitor ring buffer in real-time
```bash
# In one terminal, start pipeline
./run_demo.sh start

# In another, monitor
watch -n 1 'dada_db -l'
```

## Comparison with Old Workflow

| Aspect | Old | New |
|--------|-----|-----|
| Ring creation | Implicit in Demo | Explicit `dada_db` |
| Data write | Demo + ring | Demo (receiver only) |
| File output | Internal StartDbdisk() | Separate `dada_dbdisk` process |
| Process management | Manual | Coordinated by run_demo.sh |
| Cleanup | Manual/Ad-hoc | `./cleanup.sh` or `run_demo.sh stop` |

## See Also
- [PAF_pipeline](https://github.com/user/PAF_pipeline) - Reference implementation
- [PSRDADA Documentation](http://psrdada.sourceforge.net/)
- [README.md](README.md) - Project overview
