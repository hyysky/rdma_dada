# PSRDADA RoCE 集成使用示例

## 快速开始

### 1. 编译工程

```bash
cd rdma_dada
./build.sh
```

### 2. 配置参数

关键参数必须对齐：
```
block_bytes = (PKT_HEAD_LEN + pkt_size) * send_n
```

示例配置：
- 数据包大小：6414 字节
- 单次接收：64 包
- Block大小：(42 + 6414) × 64 = 413,184 字节 ≈ 403 KB
- Ring块数：8
- 总Ring大小：403 × 8 ≈ 3.2 MB

### 3. 运行示例

```bash
./build/Demo_psrdada_online \
    --device 0 \
    --smac 00:11:22:33:44:55 \
    --dmac 66:77:88:99:aa:bb \
    --sip 192.168.1.100 \
    --dip 192.168.1.101 \
    --sport 12345 \
    --dport 54321 \
    --pkt_size 6414 \
    --send_n 64 \
    --key 0xdada \
    --nbufs 8 \
    --dump-dir ./data_out \
    --dump-header header/array_GZNU.header
```

### 4. 监控运行状态

终端输出示例：
```
[Main] Initializing psrdada ring buffer with key 0xdada...
  Block size: 413184 bytes (403 KB)
  Number of blocks: 8
  Total ring size: 3 MB
[Main] Verified block size: 413184 bytes
[Demo] Registered ring MR: addr=0x7f1234567000 rkey=0x1a2b3c4d
Started dada_dbdisk (pid=12345) writing HDU key 0xdada to ./data_out

[RingBuf] Blocks: 123 | Used: 1 MB (33.3%) | Free: 2 MB | Block: 403 KB
[RingBuf] Blocks: 246 | Used: 2 MB (66.7%) | Free: 1 MB | Block: 403 KB
```

### 5. 检查输出文件

```bash
ls -lh ./data_out/
# 应该看到 .dada 文件
```

## API使用流程

### 初始化

```cpp
PsrdadaRingBuf *ringbuf = new PsrdadaRingBuf();

// 计算block大小：必须与RoCE配置对齐
uint64_t block_bytes = (PKT_HEAD_LEN + pkt_size) * send_n;
uint64_t nbufs = 8;

// 初始化ring
ringbuf->Init(0xdada, block_bytes, nbufs, "header/array_GZNU.header");

// 验证配置
uint64_t actual_size = ringbuf->GetBlockSize();
assert(actual_size == block_bytes);
```

### 注册到RDMA

```cpp
// 将整个ring注册为RDMA MR
struct ibv_mr *mr = ringbuf->RegisterWholeRing(
    pd, 
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
);

// 设置到RoCE接收器
rdma_dada->SetDirectMr(mr);
```

### 数据接收循环

```cpp
// 回调函数：获取写入缓冲区
char* GetBuffPtr(long int& buf_size) {
    uint64_t request_size = (PKT_HEAD_LEN + pkt_size) * send_n;
    
    // 等待空闲空间
    while (ringbuf->GetFreeSpace() < request_size) {
        usleep(1000);
    }
    
    // 获取可写block（底层调用 ipcbuf_get_next_write）
    char *ptr = ringbuf->GetWriteBuffer(request_size);
    if (!ptr) return NULL;
    
    buf_size = request_size;
    return ptr;  // RoCE将直接写入此地址
}

// 回调函数：标记数据已写入
int SendBuffPtr(void) {
    uint64_t written_size = (PKT_HEAD_LEN + pkt_size) * send_n;
    
    // 标记block已填充（底层调用 ipcbuf_mark_filled）
    return ringbuf->MarkWritten(written_size);
}
```

### 启动输出

```cpp
// 启动dada_dbdisk后台进程，自动读取ring并写入文件
ringbuf->StartDbdisk("./data_out", "header/array_GZNU.header");
```

### 清理

```cpp
// 停止dada_dbdisk
ringbuf->StopDbdisk();

// 清理ring资源
ringbuf->Cleanup();
delete ringbuf;
```

## 调试技巧

### 1. 检查PSRDADA状态

```bash
# 查看共享内存段
ipcs -m | grep dada

# 监控ring状态
watch -n 1 'dada_dbmonitor -k 0xdada'
```

### 2. 查看进程状态

```bash
# 查看dada_dbdisk进程
ps aux | grep dada_dbdisk

# 监控CPU使用
top -p $(pgrep dada_dbdisk)
```

### 3. 测试ring性能

```bash
# 写入测试
dada_dbnull -k 0xdada -z  # 只消费不处理

# 监控带宽
while true; do
    dada_dbmonitor -k 0xdada
    sleep 1
done
```

### 4. 常见错误排查

#### 错误：GetWriteBuffer返回NULL

可能原因：
- Ring已满：检查`GetFreeSpace()`
- Block大小不匹配：检查`GetBlockSize()`
- dada_dbdisk未运行或太慢

解决方案：
```bash
# 增加ring大小
--nbufs 16  # 从8改为16

# 检查dada_dbdisk是否运行
ps aux | grep dada_dbdisk

# 检查磁盘IO
iostat -x 1
```

#### 错误：Failed to create PSRDADA ring

可能原因：
- 已存在同key的ring
- 权限不足
- 共享内存不足

解决方案：
```bash
# 清理旧的ring
dada_db -k 0xdada -d

# 检查共享内存限制
cat /proc/sys/kernel/shmmax
cat /proc/sys/kernel/shmall

# 增加共享内存限制（需要root）
echo 17179869184 > /proc/sys/kernel/shmmax
```

#### 性能不足

检查点：
1. Block大小是否对齐（应该 = pkt_size × send_n）
2. Ring是否足够大（至少8个blocks）
3. 磁盘写入速度（SSD推荐）
4. CPU绑定（使用--cpu参数）

优化示例：
```bash
# 增加ring大小
--nbufs 16

# CPU绑定
--cpu 4

# 使用快速磁盘
--dump-dir /mnt/nvme/data_out
```

## 环境要求

### 软件依赖

```bash
# PSRDADA库
sudo apt-get install psrdada

# InfiniBand/RoCE驱动
sudo apt-get install ibverbs-utils rdma-core

# 可选：CUDA（如果使用GPU）
# 参考NVIDIA官方安装指南
```

### 验证环境

```bash
# 检查RDMA设备
ibv_devices

# 检查PSRDADA工具
which dada_db dada_dbdisk

# 检查共享内存
ipcs -l
```

## 性能参考

### 典型配置

- 网络：100 Gbps RoCE v2
- 数据包：6414 字节
- 批次：64 包/批
- Block：403 KB
- Ring：8 blocks (3.2 MB)

### 预期性能

- 吞吐量：> 80 Gbps
- 延迟：< 10 µs
- CPU占用：< 20% (单核)

### 瓶颈分析

| 组件 | 带宽 | 延迟 |
|------|------|------|
| RoCE网络 | 100 Gbps | < 5 µs |
| PSRDADA Ring | > 200 Gbps | < 1 µs |
| 磁盘写入(SSD) | 3-7 GB/s | 100 µs |
| 磁盘写入(HDD) | 200 MB/s | 10 ms |

**结论**：磁盘通常是瓶颈，建议使用NVME SSD。

## 更多信息

- 详细集成说明：[PSRDADA_ROCE_INTEGRATION.md](PSRDADA_ROCE_INTEGRATION.md)
- 工程文档：[README.md](README.md)
- PSRDADA官方文档：http://psrdada.sourceforge.net/
