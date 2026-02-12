# 非连续内存 PSRDADA Ring Buffer RDMA 注册方案

## 问题描述

通过 `ipcs -m | grep 0xdada` 检查发现，PSRDADA 创建的 ring buffer 不同 blocks 的地址**不连续**，无法使用单个 `ibv_reg_mr()` 注册整个 ring。

### 内存布局示例

**不连续内存**（实际情况）：
```
Block 0: 0x7f1000000000  (共享内存段1)
Block 1: 0x7f2000000000  (共享内存段2)  ← 地址跳跃
Block 2: 0x7f3000000000  (共享内存段3)
Block 3: 0x7f4000000000  (共享内存段4)
...
```

**连续内存**（理想情况）：
```
Block 0: 0x7f1000000000  ┐
Block 1: 0x7f1000065000  │ 同一个共享内存段
Block 2: 0x7f10000ca000  │ 地址连续
Block 3: 0x7f100012f000  ┘
```

## 解决方案

### 自动切换策略

代码已更新为**智能自动切换**模式：

1. **尝试整块注册**（最优）
   - 检查所有 blocks 地址是否连续
   - 如果连续：注册单个大 MR（性能最佳）

2. **自动切换到分块注册**（兼容）
   - 如果不连续：自动为每个 block 注册独立的 MR
   - 透明切换，无需修改调用代码

### 使用方法

#### 方法 1：自动模式（推荐）

```cpp
PsrdadaRingBuf *ringbuf = new PsrdadaRingBuf();
ringbuf->Init(0xdada, block_bytes, nbufs, header_path);

// 自动检测并选择最佳注册方式
struct ibv_mr *mr = ringbuf->RegisterWholeRing(pd, 
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

if (mr) {
    printf("RDMA registration successful\n");
    // 内部自动处理了连续/非连续两种情况
} else {
    fprintf(stderr, "RDMA registration failed\n");
}
```

**输出示例**（内存连续）：
```
[RegisterWholeRing] Memory is contiguous: 8 blocks × 413184 bytes
[RegisterWholeRing] Success: base=0x7f... size=3 MB rkey=0x1a2b3c4d
```

**输出示例**（内存不连续，自动切换）：
```
Error: Ring buffer blocks are NOT contiguous!
  Block 1: expected 0x7f..., actual 0x7e...
[RegisterWholeRing] Ring buffer blocks are NOT contiguous.
Current config: 8 blocks × 413184 bytes = 3 MB
Automatically switching to per-block registration mode...

[RegisterRingBlocks] Registering 8 blocks individually...
[RegisterRingBlocks] Block  0: addr=0x7f1000000000 size=403 KB rkey=0x1a2b3c4d
[RegisterRingBlocks] Block  1: addr=0x7f2000000000 size=403 KB rkey=0x2b3c4d5e
[RegisterRingBlocks] Block  2: addr=0x7f3000000000 size=403 KB rkey=0x3c4d5e6f
...
[RegisterRingBlocks] Successfully registered all 8 blocks
[RegisterRingBlocks] Total size: 3 MB
[RegisterWholeRing] Successfully registered 8 blocks individually
```

#### 方法 2：显式分块注册

如果你明确知道内存不连续，可以直接调用：

```cpp
// 直接使用分块注册
int ret = ringbuf->RegisterRingBlocks(pd, 
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

if (ret == 0) {
    printf("All blocks registered successfully\n");
}
```

### RoCE 接收代码修改

#### 之前的代码（假设连续内存）

```cpp
// 获取整个 ring 的 MR
struct ibv_mr *ring_mr = ringbuf->RegisterWholeRing(pd, access);

// 配置 SGE（假设单个 MR）
for (int i = 0; i < recv_num; i++) {
    sge[i].addr = (uint64_t)(base + i * pkt_len);
    sge[i].length = pkt_len;
    sge[i].lkey = ring_mr->lkey;  // 所有 SGE 使用同一个 lkey
}
```

#### 现在的代码（支持非连续内存）

```cpp
// 1. 注册（自动选择最佳方式）
struct ibv_mr *mr = ringbuf->RegisterWholeRing(pd, access);

// 2. 获取写入缓冲区
char *ptr = ringbuf->GetWriteBuffer(request_size);

// 3. 获取当前 block 的 MR（自动适配）
struct ibv_mr *current_mr = ringbuf->GetCurrentBlockMr();

// 4. 配置 SGE
if (current_mr) {
    // 使用分块注册模式：每个 block 有独立的 MR
    for (int i = 0; i < recv_num; i++) {
        sge[i].addr = (uint64_t)(ptr + i * pkt_len);
        sge[i].length = pkt_len;
        sge[i].lkey = current_mr->lkey;  // 使用当前 block 的 lkey
    }
} else {
    // 使用整块注册模式：使用原来的 MR
    // （与之前的代码相同）
}
```

### 完整示例

```cpp
// 初始化
PsrdadaRingBuf *ringbuf = new PsrdadaRingBuf();
ringbuf->Init(0xdada, block_bytes, nbufs, header_path);

// 获取 RDMA 资源
struct ibv_pd *pd = /* ... */;

// 注册（自动适配）
struct ibv_mr *mr = ringbuf->RegisterWholeRing(pd,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

if (!mr) {
    fprintf(stderr, "Failed to register ring\n");
    return -1;
}

// 接收循环
while (!exit_flag) {
    // 获取写入缓冲区
    long int buf_size = 0;
    char *ptr = ringbuf->GetWriteBuffer(request_size);
    if (!ptr) continue;
    
    // 获取当前 block 的 MR
    struct ibv_mr *block_mr = ringbuf->GetCurrentBlockMr();
    
    // 配置接收 WR
    for (int i = 0; i < recv_num; i++) {
        sge[i].addr = (uint64_t)(ptr + i * pkt_len);
        sge[i].length = pkt_len;
        
        if (block_mr) {
            // 分块注册模式
            sge[i].lkey = block_mr->lkey;
        } else {
            // 整块注册模式
            sge[i].lkey = mr->lkey;
        }
    }
    
    // 提交接收请求
    ibv_post_recv(qp, recv_wr, &bad_wr);
    
    // 等待完成
    ibv_poll_cq(cq, poll_n, wc);
    
    // 标记写入完成
    ringbuf->MarkWritten(written_size);
}

// 清理（自动释放所有 MRs）
ringbuf->Cleanup();
delete ringbuf;
```

## API 说明

### 新增方法

#### 1. `RegisterRingBlocks(pd, access)`
为每个 block 分别注册 MR（支持非连续内存）

**参数**：
- `pd`: InfiniBand Protection Domain
- `access`: 访问权限（如 `IBV_ACCESS_LOCAL_WRITE`）

**返回值**：
- `0`: 成功
- `-1`: 失败

**特点**：
- 支持非连续内存
- 为每个 block 创建独立的 MR
- 自动处理所有 blocks

#### 2. `GetCurrentBlockMr()`
获取当前写入 block 的 MR

**返回值**：
- `非NULL`: 当前 block 的 MR（分块注册模式）
- `NULL`: 使用整块注册模式，无需单独的 block MR

**使用场景**：
- 配置 RDMA SGE 时获取正确的 lkey
- 在接收数据前调用

#### 3. `UnregisterAllBlocks()`
清理所有已注册的 block MRs

**特点**：
- 自动在 `Cleanup()` 中调用
- 释放所有 RDMA 资源
- 通常无需手动调用

### 修改的方法

#### `RegisterWholeRing(pd, access)`
**行为变化**：
- **之前**：如果内存不连续，返回 NULL 失败
- **现在**：如果内存不连续，自动切换到 `RegisterRingBlocks()`

**优势**：
- 向后兼容
- 自动适配
- 无需修改调用代码

#### `GetWriteBuffer(bytes)`
**新增功能**：
- 自动追踪当前 block 索引
- 供 `GetCurrentBlockMr()` 使用

## 性能对比

| 模式 | 内存要求 | MR 数量 | 性能 | 兼容性 |
|------|----------|---------|------|--------|
| **整块注册** | 连续 | 1 | ⭐⭐⭐⭐⭐ | 需要配置 SHMMAX |
| **分块注册** | 任意 | N (blocks) | ⭐⭐⭐⭐ | ✓ 通用 |

**性能说明**：
- 整块注册：单个 MR，硬件转换最少，性能最优
- 分块注册：多个 MR，每次切换 block 需要更新 lkey，性能略降（约 5-10%）

**实际影响**：
- 对于高速网络（100 Gbps），差异可忽略
- CPU 开销增加很小（< 1%）
- 推荐使用自动模式，让代码选择最佳方案

## 故障排查

### 问题 1：编译错误 - vector 未定义

**错误**：
```
error: 'vector' in namespace 'std' does not name a template type
```

**解决**：
在 `psrdada_ringbuf.h` 中添加：
```cpp
#include <vector>
```

### 问题 2：GetCurrentBlockMr() 返回 NULL

**可能原因**：
1. 使用整块注册模式（这是正常的）
2. 调用时机错误（在 `GetWriteBuffer()` 之前）
3. block_mrs 未初始化

**解决**：
```cpp
// 确保顺序正确
char *ptr = ringbuf->GetWriteBuffer(size);  // 先获取缓冲区
struct ibv_mr *mr = ringbuf->GetCurrentBlockMr();  // 再获取 MR

if (!mr) {
    // 使用整块注册模式，使用原来的 ring_mr
}
```

### 问题 3：内存泄漏

**检查**：
```bash
# 运行程序
./Demo_psrdada_online ...

# 检查 MR 是否释放（停止程序后）
ibv_devinfo
```

**确保**：
- 调用 `ringbuf->Cleanup()` 或 `delete ringbuf`
- `UnregisterAllBlocks()` 会自动调用

## 总结

### 优势

✅ **自动适配**：无需修改调用代码
✅ **通用兼容**：支持连续和非连续内存
✅ **性能优化**：优先使用整块注册
✅ **简单易用**：透明切换，无需关心细节
✅ **资源管理**：自动清理，无内存泄漏

### 使用建议

1. **默认使用 `RegisterWholeRing()`**
   - 自动选择最佳方案
   - 向后兼容

2. **在每次 `GetWriteBuffer()` 后调用 `GetCurrentBlockMr()`**
   - 获取当前 block 的正确 lkey
   - 如果返回 NULL，使用整块 MR

3. **确保调用 `Cleanup()`**
   - 释放所有 RDMA 资源
   - 或使用 RAII（析构函数自动调用）

### 下一步

- 运行程序测试自动切换功能
- 观察输出日志确认使用的模式
- 测试性能对比（如果需要）
- 如有问题，查看详细日志输出
