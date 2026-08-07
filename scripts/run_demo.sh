#!/bin/bash
set -o pipefail
# RDMA + PSRDADA Ring Buffer Demo Launcher
# The process group is owned by this foreground invocation and is cleaned up
# when it exits. Cross-invocation stop/status commands are intentionally absent.
# Usage: bash scripts/run_demo.sh [start]

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
}

declare -a pids
declare -a keys

# ============= Configuration =============
# Network parameters
DMAC="10:70:fd:11:e2:e3"
DIP="10.17.16.11"
DPORT="17201"

# Hardware parameters
DEVICE="1"

# CUDA device ID. The current raw ingest keeps RDMA receive buffers in host
# memory; this value only selects a CUDA device in CUDA-enabled builds.
GPU="0"

# CPU thread affinity (bind_cpu_id):
#   -1 = No binding, OS schedules freely - default
#   >=0 = Bind to specific CPU core (e.g., 10) - reduces latency
CPU="-1"

# Receiver configuration
SEND_N=64
NSGE="4"

# Directories and files
DUMP_DIR="./data_out"
DUMP_HEADER="header/array_GZNU.header"
PIPELINE_CONFIG="${PIPELINE_CONFIG:-config/pipeline.example.json}"
LOG_DIR="${DUMP_DIR}/logs"
DEMO_LOG="${LOG_DIR}/rdma2dada.log"

# ============= Load and Validate Data Contract =============
KEY="0xdada"
ACTION="${1:-start}"

if [ "${ACTION}" != "start" ]; then
    echo_err "Unsupported action: ${ACTION}"
    echo "Usage: bash $0 [start]"
    echo "Stop the running foreground session with Ctrl+C."
    exit 2
fi

    CONFIG_INSPECT="./build/pipeline_config_inspect"
    if [ ! -x "${CONFIG_INSPECT}" ]; then
        echo "ERROR: ${CONFIG_INSPECT} is missing; run scripts/build.sh first" >&2
        exit 1
    fi

    if ! CONFIG_VALUES=$("${CONFIG_INSPECT}" "${PIPELINE_CONFIG}"); then
        echo "ERROR: invalid pipeline config: ${PIPELINE_CONFIG}" >&2
        exit 1
    fi

    while IFS='=' read -r config_key config_value; do
        case "${config_key}" in
            RAW_RECORD_BYTES) RAW_RECORD_BYTES="${config_value}" ;;
            RAW_BLOCK_BYTES) BLOCK_BYTES="${config_value}" ;;
            RAW_RING_BLOCKS) NBUFS="${config_value}" ;;
            RAW_RING_BYTES) RING_BYTES="${config_value}" ;;
            RAW_FILE_BYTES) FILE_BYTES="${config_value}" ;;
            DBDISK_ENABLED) DBDISK_ENABLED="${config_value}" ;;
            DIRECT_IO) DIRECT_IO="${config_value}" ;;
        esac
    done <<< "${CONFIG_VALUES}"

    if [ -z "${RAW_RECORD_BYTES}" ] || [ -z "${BLOCK_BYTES}" ] ||
       [ -z "${NBUFS}" ] || [ -z "${RING_BYTES}" ] ||
       [ -z "${FILE_BYTES}" ] || [ -z "${DBDISK_ENABLED}" ] ||
       [ -z "${DIRECT_IO}" ]; then
        echo "ERROR: incomplete output from ${CONFIG_INSPECT}" >&2
        exit 1
    fi

    if [ "${DBDISK_ENABLED}" -ne 1 ]; then
        echo_err "disk.enabled must be true for the current demo launcher."
        echo_err "No downstream pipeline worker is started yet, so disabling dada_dbdisk would leave the raw ring without a reader."
        exit 1
    fi

    echo_info "========================================"
    echo_info "RDMA + PSRDADA Ring Buffer Demo"
    echo_info "========================================"
    echo_info "Configuration:"
    echo_info "  Network: ANY source → ${DMAC}, ${DIP}:${DPORT}"
    echo_info "  Data contract: ${PIPELINE_CONFIG}"
    echo_info "  PSRDADA: ${RAW_RECORD_BYTES} bytes/record, ${NBUFS} blocks"
    echo_info "  Block size: ${BLOCK_BYTES} bytes, Ring: ${RING_BYTES} bytes"
    if [ "${DBDISK_ENABLED}" -eq 1 ]; then
        echo_info "  Disk sink: enabled, ${FILE_BYTES} bytes/file, ${DUMP_DIR}/"
    else
        echo_info "  Disk sink: disabled"
    fi
    echo_info "  Logs: ${LOG_DIR}/"
    echo_info "  NSGE per WR: ${NSGE}"
    echo_info "========================================"
    mkdir -p "${DUMP_DIR}"
    mkdir -p "${LOG_DIR}"

# ============= Function Definitions =============

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

trap cleanup EXIT

        # Step 1: Create ring buffer
        # -p asks PSRDADA to page the allocated blocks into memory.
        # rdma2dada acquires the writer lock after creation.
        dada_create_cmd=(dada_db -k "${KEY}" -b "${BLOCK_BYTES}" -n "${NBUFS}" -p)
        echo_info "Command: ${dada_create_cmd[*]}"
        "${dada_create_cmd[@]}"
        if [ $? -ne 0 ]; then
            echo_err "Failed to create ring buffer"
            exit 1
        fi
        keys+=("${KEY}")
        echo_info "Created ringbuffer (unlocked, ready for writer)"
        sleep 1
        
        # Step 2: Start dada_dbdisk, the current demo's only ring reader.
        if [ "${DBDISK_ENABLED}" -eq 1 ]; then
            # Use nohup to prevent receiving SIGINT so it can finish after EOD.
            dada_dbdisk_args=(-k "${KEY}" -D "${DUMP_DIR}" -z)
            if [ "${DIRECT_IO}" -eq 1 ]; then
                dada_dbdisk_args+=(-o)
            fi
            echo_info "Command: nohup dada_dbdisk ${dada_dbdisk_args[*]} &"
            nohup dada_dbdisk "${dada_dbdisk_args[@]}" \
                > "${DUMP_DIR}/dada_dbdisk.log" 2>&1 &
            DBDISK_PID=$!
            pids+=("${DBDISK_PID}")
            echo_info "Started dada_dbdisk (PID: ${DBDISK_PID}, protected from SIGINT)"

            echo_info "Waiting for dada_dbdisk to connect as reader..."
            sleep 3

            if ! kill -0 "${DBDISK_PID}" 2>/dev/null; then
                echo_err "ERROR: dada_dbdisk (PID ${DBDISK_PID}) exited prematurely!"
                echo_err "Check log: ${DUMP_DIR}/dada_dbdisk.log"
                cat "${DUMP_DIR}/dada_dbdisk.log"
                exit 1
            fi

            if grep -q "locked" "${DUMP_DIR}/dada_dbdisk.log" 2>/dev/null; then
                echo_info "✓ dada_dbdisk connected successfully"
            else
                echo_warn "WARNING: Cannot confirm dada_dbdisk connection. Check log if issues occur."
                echo_warn "Log file: ${DUMP_DIR}/dada_dbdisk.log"
            fi
        else
            echo_info "dada_dbdisk is disabled by configuration"
        fi

        # Step 3: Start receiver (run in foreground, but trap will handle cleanup)
        CMD=(./build/rdma2dada
            --dmac "${DMAC}" --dip "${DIP}" --dport "${DPORT}"
            --key "${KEY}" --device "${DEVICE}" --gpu "${GPU}" --cpu "${CPU}"
            --send_n "${SEND_N}" --nsge "${NSGE}"
            --config "${PIPELINE_CONFIG}" --dump-header "${DUMP_HEADER}")
    
        echo_info "Command: ${CMD[*]}"
        echo_info "========================================"
        echo_info "Logs: ${DEMO_LOG}"
        echo_info "========================================"
        
        # Add timestamp marker to log file
        echo "" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        echo "Run started at: $(date '+%Y-%m-%d %H:%M:%S')" >> "${DEMO_LOG}"
        echo "Command: ${CMD[*]}" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        
        # Run receiver in foreground with output to both terminal and log file
        # tee -a appends to log file, 2>&1 redirects stderr to stdout
        "${CMD[@]}" 2>&1 | tee -a "${DEMO_LOG}"
        DEMO_STATUS=${PIPESTATUS[0]}
        if [ "${DEMO_STATUS}" -ne 0 ]; then
            echo_err "rdma2dada exited with status ${DEMO_STATUS}"
            exit "${DEMO_STATUS}"
        fi
        
        # Add exit timestamp marker to log file
        echo "========================================" >> "${DEMO_LOG}"
        echo "Run ended at: $(date '+%Y-%m-%d %H:%M:%S')" >> "${DEMO_LOG}"
        echo "========================================" >> "${DEMO_LOG}"
        
        if [ "${DBDISK_ENABLED}" -eq 1 ]; then
            echo_info "Receiver exited, waiting for readers to finish..."
            # Give dada_dbdisk time to detect EOD, close files, and exit cleanly.
            sleep 5
        fi
        echo_info "Starting cleanup sequence..."

echo_info "Done."
