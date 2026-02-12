#!/bin/bash
# check_psrdada_ring.sh - 检查 PSRDADA ring buffer 内存连续性

KEY=${1:-dada}
VERBOSE=${2:-0}

echo "========================================"
echo "PSRDADA Ring Buffer 连续性检查"
echo "========================================"
echo "Ring Key: $KEY"
echo ""

# 1. 检查 ring 是否存在
echo "[1/4] 检查 ring 是否存在..."
SEGMENTS=$(ipcs -m | grep -i "$KEY" | wc -l)

if [ $SEGMENTS -eq 0 ]; then
    echo "✗ 错误：未找到 key=$KEY 的共享内存段"
    echo ""
    echo "解决方案："
    echo "  运行程序创建 ring，或手动创建："
    echo "  dada_db -k $KEY -b <block_size> -n <nbufs> -l"
    exit 1
else
    echo "✓ 找到 $SEGMENTS 个共享内存段"
fi

# 2. 检查段数量（连续性的关键指标）
echo ""
echo "[2/4] 检查内存连续性..."
if [ $SEGMENTS -eq 1 ]; then
    echo "✓ 优秀：只有 1 个共享内存段（内存连续）"
    CONTIGUOUS="YES"
elif [ $SEGMENTS -eq 2 ]; then
    echo "⚠ 警告：有 2 个共享内存段"
    echo "  可能是 header 和 data 分开，需要进一步检查"
    CONTIGUOUS="MAYBE"
else
    echo "✗ 错误：有 $SEGMENTS 个共享内存段（内存不连续）"
    CONTIGUOUS="NO"
fi

# 3. 显示详细信息
echo ""
echo "[3/4] 共享内存段详情:"
echo "-------------------------------------------------------------------------------"
ipcs -m | head -n 3
ipcs -m | grep -i "$KEY"
echo "-------------------------------------------------------------------------------"

# 提取信息
TOTAL_BYTES=0
while read -r line; do
    if [[ $line =~ $KEY ]]; then
        BYTES=$(echo $line | awk '{print $5}')
        TOTAL_BYTES=$((TOTAL_BYTES + BYTES))
    fi
done < <(ipcs -m | grep -i "$KEY")

echo "总共享内存大小: $TOTAL_BYTES bytes ($((TOTAL_BYTES / 1024)) KB, $((TOTAL_BYTES / 1024 / 1024)) MB)"

# 4. 检查系统限制
echo ""
echo "[4/4] 检查系统共享内存限制..."
SHMMAX=$(cat /proc/sys/kernel/shmmax)
SHMALL=$(cat /proc/sys/kernel/shmall)
SHMMNI=$(cat /proc/sys/kernel/shmmni)
PAGE_SIZE=$(getconf PAGE_SIZE)
SHMALL_BYTES=$((SHMALL * PAGE_SIZE))

echo "  SHMMAX (最大段大小):   $SHMMAX bytes ($((SHMMAX / 1024 / 1024)) MB)"
echo "  SHMALL (总内存):       $SHMALL_BYTES bytes ($((SHMALL_BYTES / 1024 / 1024)) MB)"
echo "  SHMMNI (最大段数):     $SHMMNI"

if [ $TOTAL_BYTES -gt $SHMMAX ]; then
    echo "✗ 警告：ring 大小 ($TOTAL_BYTES) > SHMMAX ($SHMMAX)"
    echo "  这可能导致内存分配到多个段（不连续）"
else
    echo "✓ ring 大小 ($TOTAL_BYTES) ≤ SHMMAX ($SHMMAX)"
fi

# 总结和建议
echo ""
echo "========================================"
echo "检查结果总结"
echo "========================================"
echo "内存连续性: $CONTIGUOUS"

if [ "$CONTIGUOUS" = "YES" ]; then
    echo "状态: ✓ 适合 RoCE RDMA 使用"
    echo ""
    echo "可以安全地注册整个 ring 到 RDMA。"
    exit 0
elif [ "$CONTIGUOUS" = "MAYBE" ]; then
    echo "状态: ⚠ 需要程序运行时验证"
    echo ""
    echo "建议："
    echo "1. 运行程序并查看 RegisterWholeRing() 的输出"
    echo "2. 如果失败，按下面的步骤重建 ring"
    exit 0
else
    echo "状态: ✗ 不适合 RoCE RDMA 使用"
    echo ""
    echo "修复步骤："
    echo "=========================================="
    echo ""
    echo "1. 停止所有使用此 ring 的进程："
    echo "   pkill -f dada_dbdisk"
    echo "   pkill -f Demo_psrdada"
    echo ""
    echo "2. 删除现有的 ring："
    echo "   dada_db -k $KEY -d"
    echo ""
    echo "3. 增加 SHMMAX（如果需要）："
    RECOMMENDED_SHMMAX=$((TOTAL_BYTES * 2))
    echo "   sudo sysctl -w kernel.shmmax=$RECOMMENDED_SHMMAX"
    echo ""
    echo "4. 重新创建 ring："
    echo "   让程序自动创建，或手动创建："
    echo "   dada_db -k $KEY -b <block_size> -n <nbufs> -l"
    echo ""
    echo "5. 重新运行此脚本验证："
    echo "   $0 $KEY"
    echo ""
    exit 1
fi
