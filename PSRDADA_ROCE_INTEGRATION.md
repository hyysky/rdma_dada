# PSRDADA与RoCE集成说明

## 概述

本文档说明如何将PSRDADA环形缓冲区与RoCE RDMA直接集成，实现零拷贝的高性能数据接收。

## 架构

```
发送端 --[RoCE/RDMA]--> PSRDADA Ring Buffer --> dada_dbdisk --> DADA文件
                             |
                          block级切换
                   (ipcbuf_get_next_write/
                    ipcbuf_mark_filled)
```

## 关键改进

### 1. 使用底层ipcbuf API

**改进前**：使用高层`ipcio_open_block_write`/`ipcio_update_block_write`
**改进后**：使用底层`ipcbuf_get_next_write`/`ipcbuf_mark_filled`

**优势**：
- 更精确的block级控制
- 与RDMA内存注册完美对齐
- 避免不必要的内存拷贝

### 2. API变化

#### GetWriteBuffer()
```cpp
// 获取下一个可写入的block
// RoCE可以直接将数据写入到这个地址
char* ptr = ringbuf->GetWriteBuffer(bytes);
```

内部实现：
- 调用`ipcbuf_get_next_write()`获取下一个空闲block
- 验证请求大小不超过block大小
- 返回可直接写入的内存地址

#### MarkWritten()
```cpp
// 标记block已写入完成
// bytes: 实际写入的字节数
ringbuf->MarkWritten(bytes);
```

内部实现：
- 调用`ipcbuf_mark_filled()`标记block已填充
- 自动切换到下一个block
- reader（如dada_dbdisk）可以读取此block

#### GetBlockSize()
```cpp
// 获取单个block的大小
// 用于配置RoCE参数：pkt_size * send_n 应等于 block_size
uint64_t block_size = ringbuf->GetBlockSize();
```

## 使用流程

### 1. 创建环形缓冲区

```cpp
PsrdadaRingBuf *ringbuf = new PsrdadaRingBuf();

// block_bytes = 数据包大小 * 单次接收数据包数目
uint64_t block_bytes = (PKT_HEAD_LEN + pkt_size) * send_n;
uint64_t nbufs = 8;  // 环形缓冲区block数量

ringbuf->Init(PSRDADA_KEY, block_bytes, nbufs, header_path);
```

### 2. 注册到RDMA

```cpp
// 将整个ring注册为RDMA内存区域
struct ibv_mr *ring_mr = ringbuf->RegisterWholeRing(
    pd, 
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
);

// 将MR设置到RoCE接收器
rdma_dada->SetDirectMr(ring_mr);
```

### 3. 接收数据循环

```cpp
// 在接收回调中：
char* GetBuffPtr(long int& buf_size) {
    uint64_t request_size = (PKT_HEAD_LEN + pkt_size) * send_n;
    
    // 等待空闲block
    while (ringbuf->GetFreeSpace() < request_size && !exit_flag) {
        usleep(1000);
    }
    
    // 获取可写入的block地址
    char *ptr = ringbuf->GetWriteBuffer(request_size);
    buf_size = request_size;
    return ptr;
}

int SendBuffPtr(void) {
    // 标记block已写入
    uint64_t written_size = (PKT_HEAD_LEN + pkt_size) * send_n;
    return ringbuf->MarkWritten(written_size);
}
```

### 4. 启动输出到文件

```cpp
// 启动dada_dbdisk作为后台进程，自动读取ring并写入文件
ringbuf->StartDbdisk("./data_out", "header/array_GZNU.header");
```

### 5. 清理

```cpp
ringbuf->StopDbdisk();
ringbuf->Cleanup();
delete ringbuf;
```

## 配置建议

### Block大小配置

确保block大小与RoCE配置对齐：

```
block_bytes = (PKT_HEAD_LEN + pkt_size) * send_n
```

例如：
- PKT_HEAD_LEN = 42 字节
- pkt_size = 6414 字节
- send_n = 64 包
- block_bytes = (42 + 6414) * 64 = 413,184 字节

### Ring缓冲区大小

```
total_size = block_bytes * nbufs
```

建议：
- `nbufs >= 8`：确保足够的缓冲空间
- 根据网络速度和磁盘写入速度调整
- 监控`GetFreeSpace()`避免溢出

## 性能优化

### 1. 零拷贝路径

```
网络 --> RDMA --> PSRDADA ring --> dada_dbdisk --> 磁盘
         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
              零拷贝区域
```

### 2. 避免阻塞

```cpp
// 检查是否有足够空间
if (ringbuf->GetFreeSpace() < required_size) {
    // 处理背压：暂停接收或丢包
}
```

### 3. 监控

```cpp
// 定期打印状态
uint64_t used = ringbuf->GetUsedSpace();
uint64_t free = ringbuf->GetFreeSpace();
uint64_t block_size = ringbuf->GetBlockSize();
printf("Used: %lu MB, Free: %lu MB, BlockSize: %lu KB\n",
       used/1024/1024, free/1024/1024, block_size/1024);
```

## 常见问题

### Q: GetWriteBuffer返回NULL？

**可能原因**：
1. ring已满 - 检查`GetFreeSpace()`
2. 请求大小超过block大小 - 检查`bytes <= GetBlockSize()`
3. dada_dbdisk未运行或读取太慢

**解决**：
- 等待空闲空间
- 增加nbufs
- 优化磁盘写入速度

### Q: 性能不如预期？

**检查点**：
1. block大小是否与RoCE配置对齐
2. nbufs是否足够大
3. dada_dbdisk是否及时读取
4. 磁盘IO是否成为瓶颈

### Q: 如何调试？

```cpp
// 打开PSRDADA日志
export DADA_DEBUG=1

// 监控ring状态
dada_dbmonitor -k 0xdada

// 检查共享内存
ipcs -m
```

## 参考

- PSRDADA文档: http://psrdada.sourceforge.net/
- InfiniBand Verbs API: https://linux.die.net/man/3/ibv_reg_mr
- Demo: `demo/Demo_psrdada_online.cpp`
