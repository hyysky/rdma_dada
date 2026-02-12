#!/bin/bash
# RDMA + PSRDADA Ring Buffer Demo Launcher
# Follows PAF_pipeline pattern: dada_db setup → Demo_psrdada_online → dada_dbdisk
# Usage: ./run_demo.sh

# Color output helper
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() { echo -e "${GREEN}[INFO]${NC} $@"; }
echo_warn() { echo -e "${YELLOW}[WARN]${NC} $@"; }
echo_err() { echo -e "${RED}[ERROR]${NC} $@"; }

# Flag to prevent cleanup from running multiple times
CLEANUP_DONE=0

trap ctrl_c INT

function ctrl_c() {
    echo_info "** Trapped CTRL-C, initiating shutdown..."
    # Don't call cleanup here - let EXIT trap handle it
    # Just set flag to exit the monitoring loop
    g_should_exit=1
}

declare -a pids
declare -a keys

# ============= Configuration =============
# Network parameters
SMAC="02:a2:02:00:02:fa"
DMAC="10:70:fd:11:e2:e3"
SIP="10.17.16.60"
DIP="10.17.16.11"
SPORT="60000"
DPORT="17201"

# Hardware parameters
DEVICE="1"

# GPU memory mode (RdmaDirectGpu):
#   =0：Host memory (malloc) - default, best compatibility
#   >0：GPU memory (cudaMalloc) - GPUDirect RDMA, needs CUDA
#   <0：Pinned host memory (cudaMallocHost) - DMA-friendly
GPU="0"

# CPU thread affinity (bind_cpu_id):
#   -1 = No binding, OS schedules freely - default
#   >=0 = Bind to specific CPU core (e.g., 10) - reduces latency
CPU="-1"

# Receiver configuration
SEND_N=64
NSGE="4"
PKT_PER_BLOCK=16384
# PSRDADA ring parameters
PKT_HEADER=64
PKT_DATA="8192"
NBUFS="8"

# Output file configuration (blocks per .dada file)
NBLOCKSAVE="10"

# Directories and files
DUMP_DIR="./data_out"
DUMP_HEADER="header/array_GZNU.header"
LOG_DIR="${DUMP_DIR}/logs"
DEMO_LOG="${LOG_DIR}/demo_psrdada_online.log"

# ============= Compute Derived Params =============
BLOCK_BYTES=$(( (PKT_HEADER + PKT_DATA) * PKT_PER_BLOCK ))
RING_BYTES=$(( BLOCK_BYTES * NBUFS ))
FILE_BYTES=$(( NBLOCKSAVE * BLOCK_BYTES ))
PKT_SIZE=$(( PKT_HEADER + PKT_DATA ))

# Convert hex key to decimal for dada_db
KEY="0xdada"

echo_info "========================================"
echo_info "RDMA + PSRDADA Ring Buffer Demo"
echo_info "========================================"
echo_info "Configuration:"
echo_info "  Network: ${SMAC} → ${DMAC}, ${SIP}:${SPORT} → ${DIP}:${DPORT}"
echo_info "  PSRDADA: PKT=(${PKT_HEADER}+${PKT_DATA})B, ${PKT_PER_BLOCK} pkt/block, ${NBUFS} blocks"
echo_info "  Block size: ${BLOCK_BYTES} bytes, Ring: ${RING_BYTES} bytes"
echo_info "  Output: ${FILE_BYTES} bytes/file, ${DUMP_DIR}/"
echo_info "  Logs: ${LOG_DIR}/"
echo_info "  NSGE per WR: ${NSGE}"
echo_info "========================================"
mkdir -p "${DUMP_DIR}"
mkdir -p "${LOG_DIR}"

# ============= Function Definitions =============

function status() {
    echo_info "Status of running processes:"
    
    if [ -f "${PID_FILE}" ]; then
        while read pid; do
            if [ -n "$pid" ] && kill -0 $pid 2>/dev/null; then
                cmd=$(ps -p $pid -o cmd= 2>/dev/null || echo "unknown")
                echo_info "  PID ${pid}: ${cmd}"
            fi
        done < "${PID_FILE}"
    else
        echo_warn "  No PID file found"
    fi
    
    # Check ring buffer
    if command -v dada_db >/dev/null 2>&1; then
        echo_info "PSRDADA ring buffers:"
        dada_db -l 2>/dev/null || echo_warn "  (dada_db -l not supported)"
    fi
}

function cleanup {
    # Prevent running cleanup multiple times
    if [ $CLEANUP_DONE -eq 1 ]; then
        echo_info "Cleanup already done, skipping..."
        return
    fi
    CLEANUP_DONE=1
    
    echo_info "Starting graceful shutdown..."
    
    # Step 1: Send SIGTERM to all background processes
    if [ ${#pids[@]} -gt 0 ]
    then
        echo_info "Background processes to terminate: ${pids[@]}"
        for pid in "${pids[@]}"
        do
            if kill -0 $pid 2>/dev/null; then
                local cmd=$(ps -p $pid -o comm= 2>/dev/null || echo "unknown")
                echo_info "  Terminating PID $pid ($cmd) gracefully..."
                kill -TERM $pid 2>/dev/null || true
            else
                echo_info "  PID $pid already exited"
            fi
        done
        
        # Step 2: Wait for processes to finish cleanup (with timeout)
        echo_info "Waiting for background processes to complete cleanup (max 15 seconds)..."
        local wait_time=0
        local all_done=false
        while [ $wait_time -lt 15 ]; do
            all_done=true
            local still_running=""
            for pid in "${pids[@]}"; do
                if kill -0 $pid 2>/dev/null; then
                    all_done=false
                    local cmd=$(ps -p $pid -o comm= 2>/dev/null || echo "unknown")
                    still_running="$still_running $pid($cmd)"
                fi
            done
            
            if $all_done; then
                echo_info "✓ All background processes terminated gracefully"
                break
            fi
            
            [ $wait_time -eq 0 ] && echo_info "  Still waiting for:$still_running"
            sleep 1
            wait_time=$((wait_time + 1))
        done
        
        # Step 3: Force kill any remaining processes
        if ! $all_done; then
            echo_warn "Some processes did not exit in time, forcing termination..."
            for pid in "${pids[@]}"; do
                if kill -0 $pid 2>/dev/null; then
                    local cmd=$(ps -p $pid -o comm= 2>/dev/null || echo "unknown")
                    echo_warn "  Force killing PID $pid ($cmd)"
                    kill -9 $pid 2>/dev/null || true
                fi
            done
            sleep 1
        fi
        
        # CRITICAL: Extra wait to ensure all file descriptors and IPC resources are released
        echo_info "Waiting extra 3 seconds for IPC cleanup to complete..."
        sleep 3
    else
        echo_info "No background processes to terminate"
    fi
    
    # Step 4: Destroy ring buffers (AFTER all processes have exited)
    if [ ${#keys[@]} -gt 0 ]
    then
        echo_info "Destroying ring buffers: ${keys[@]}"
        for key in "${keys[@]}"
        do
            echo_info "  Removing ring buffer $key..."
            # Use timeout to prevent hanging if dada_db -d gets stuck
            if timeout 5 dada_db -k $key -d > /tmp/dada_db_destroy_${key}.log 2>&1; then
                echo_info "  ✓ Ring buffer $key destroyed"
            else
                local exit_code=$?
                if [ $exit_code -eq 124 ]; then
                    echo_warn "  Ring buffer $key destruction timed out after 5 seconds"
                    echo_warn "  You may need to manually clean up: ipcrm -M $key"
                else
                    echo_warn "  Ring buffer $key destruction had issues (exit code: $exit_code)"
                fi
            fi
        done
        echo_info "✓ Ring buffer cleanup complete"
    else
        echo_info "No ring buffers to destroy"
    fi
    
    echo_info "✓ Shutdown complete"
}

# ============= Main Flow =============

ACTION="${1:-start}"

trap cleanup EXIT

case "${ACTION}" in
    start)
        # Step 1: Create ring buffer
        # Note: Use -p to create only, no -w (writer lock) or -l (keep locked)
        # The Demo_psrdada_online will lock it as writer
        dada_create_cmd="dada_db -k ${KEY} -b ${BLOCK_BYTES} -n ${NBUFS} -p"
        echo_info "Command: ${dada_create_cmd}"
        eval "${dada_create_cmd}"
        if [ $? -ne 0 ]; then
            echo_err "Failed to create ring buffer"
            exit 1
        fi
        keys+=(`echo $KEY `)
        echo_info "Created ringbuffer (unlocked, ready for writer)"
        sleep 1
        
        # Step 2: Start dada_dbdisk (data reader/writer)
        # Use nohup to prevent receiving SIGINT so it can finish reading after EOD
        dada_dbdisk_cmd="nohup dada_dbdisk -k ${KEY} -D ${DUMP_DIR} -o -z > ${DUMP_DIR}/dada_dbdisk.log 2>&1"
        echo_info "Command: ${dada_dbdisk_cmd} &"
        eval "${dada_dbdisk_cmd} &"
        DBDISK_PID=$!
        pids+=($DBDISK_PID)
        echo_info "Started dada_dbdisk (PID: ${DBDISK_PID}, protected from SIGINT)"
        
        # Wait for dada_dbdisk to connect as reader
        echo_info "Waiting for dada_dbdisk to connect as reader..."
        sleep 3
        
        # Check if dada_dbdisk is still running
        if ! kill -0 ${DBDISK_PID} 2>/dev/null; then
            echo_err "ERROR: dada_dbdisk (PID ${DBDISK_PID}) exited prematurely!"
            echo_err "Check log: ${DUMP_DIR}/dada_dbdisk.log"
            cat "${DUMP_DIR}/dada_dbdisk.log"
            exit 1
        fi
        
        # Check log for connection success
        if grep -q "locked" "${DUMP_DIR}/dada_dbdisk.log" 2>/dev/null; then
            echo_info "✓ dada_dbdisk connected successfully"
        else
            echo_warn "WARNING: Cannot confirm dada_dbdisk connection. Check log if issues occur."
            echo_warn "Log file: ${DUMP_DIR}/dada_dbdisk.log"
        fi

        # Step 3: Start receiver (run in foreground, but trap will handle cleanup)
        CMD="./build/Demo_psrdada_online \
        --smac ${SMAC} --dmac ${DMAC} \
        --sip ${SIP} --dip ${DIP} --sport ${SPORT} --dport ${DPORT} \
        --key ${KEY} --device ${DEVICE} --gpu ${GPU} --cpu ${CPU} \
        --pkt_size ${PKT_SIZE} --send_n ${SEND_N} \
        --nsge ${NSGE} \
        --file-bytes ${FILE_BYTES}"
    
        echo_info "Command: ${CMD}"
        echo_info "========================================"
        echo_info "Logs: ${DEMO_LOG}"
        echo_info "========================================"
        
        # Add timestamp marker to log file
        echo "" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        echo "Run started at: $(date '+%Y-%m-%d %H:%M:%S')" >> "${DEMO_LOG}"
        echo "Command: ${CMD}" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        
        # Run receiver in foreground with output to both terminal and log file
        # tee -a appends to log file, 2>&1 redirects stderr to stdout
        $CMD 2>&1 | tee -a "${DEMO_LOG}"
        
        # Add exit timestamp marker to log file
        echo "========================================" >> "${DEMO_LOG}"
        echo "Run ended at: $(date '+%Y-%m-%d %H:%M:%S')" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        
        echo_info "Receiver exited, waiting for readers to finish..."
        # Give dada_dbdisk time to detect EOD, close files, and exit cleanly
        sleep 5
        echo_info "Starting cleanup sequence..."
        ;;
    
    stop)
        cleanup
        ;;
    
    status)
        status
        ;;
    
    *)
        echo "Usage: $0 {start|stop|status}"
        echo ""
        echo "  start  : Create ring buffer → start receiver → start dada_dbdisk"
        echo "  stop   : Terminate all processes and clean up ring buffer"
        echo "  status : Show running processes and ring buffer status"
        exit 1
        ;;
esac

echo_info "Done."
