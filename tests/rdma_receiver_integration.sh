#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 RDMA2DADA SOURCE_DIR" >&2
    exit 2
fi

receiver=$1
source_dir=$2

for command_name in dada_db dada_dbdisk grep python3 seq ssh tail timeout; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "SKIP: ${command_name} is unavailable"
        exit 77
    fi
done

required_environment=(
    RDMA_TEST_DEVICE
    RDMA_TEST_DMAC
    RDMA_TEST_DIP
    RDMA_TEST_DPORT
    RDMA_TEST_SENDER_A
    RDMA_TEST_SENDER_A_IP
    RDMA_TEST_SENDER_B
    RDMA_TEST_SENDER_B_IP
    RDMA_TEST_REMOTE_SOURCE_DIR
    RDMA_TEST_SSH_KNOWN_HOSTS
)
for variable_name in "${required_environment[@]}"; do
    if [[ -z "${!variable_name:-}" ]]; then
        echo "SKIP: ${variable_name} is not configured"
        exit 77
    fi
done
if [[ ! -f "${RDMA_TEST_SSH_KNOWN_HOSTS}" ]]; then
    echo "RDMA_TEST_SSH_KNOWN_HOSTS is not a readable file" >&2
    exit 1
fi

if [[ ! -x "${receiver}" ]]; then
    echo "rdma2dada is not executable: ${receiver}" >&2
    exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/rdma-receiver.XXXXXX")
ring_key=${RDMA_TEST_RING_KEY:-00ee}
ring_pid=
reader_pid=
receiver_pid=

cleanup() {
    set +e
    for child_pid in "${receiver_pid}" "${reader_pid}"; do
        if [[ -n "${child_pid}" ]] && kill -0 "${child_pid}" 2>/dev/null; then
            kill -TERM "${child_pid}" 2>/dev/null || true
            wait "${child_pid}" 2>/dev/null || true
        fi
    done
    if [[ -n "${ring_pid}" ]]; then
        if kill -0 "${ring_pid}" 2>/dev/null; then
            kill -INT "${ring_pid}" 2>/dev/null || true
        fi
        wait "${ring_pid}" 2>/dev/null || true
    fi
    dada_db -d -k "${ring_key}" >/dev/null 2>&1 || true
    rm -rf "${test_root}"
}
trap cleanup EXIT

mkdir -p "${test_root}/output"
python3 - "${test_root}/pipeline.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
document = {
    "schema_version": 1,
    "observation": {
        "nant": 2,
        "nchan": 1,
        "npol": 2,
        "payload_order": "TFP",
        "utc_start": "2026-08-07-00:00:00",
    },
    "packet": {
        "header_bytes": 32,
        "payload_bytes": 1024,
        "samples": 256,
        "nbit": 16,
        "sample_interval_us": 1.0,
    },
    "ring_buffers": {
        "records_per_block": 8,
        "raw_blocks": 2,
        "compute_blocks": 2,
    },
    "disk": {
        "enabled": True,
        "blocks_per_file": 1,
        "direct_io": False,
    },
}
path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
PY

dada_db -k "${ring_key}" -b 8448 -a 4096 -n 2 -r 1 -p -w -l \
    >"${test_root}/dada_db.log" 2>&1 &
ring_pid=$!
sleep 1
if ! kill -0 "${ring_pid}" 2>/dev/null; then
    echo "failed to create persistent raw test ring" >&2
    exit 1
fi

dada_dbdisk -k "${ring_key}" -D "${test_root}/output" -s -W \
    >"${test_root}/dbdisk.log" 2>&1 &
reader_pid=$!

"${receiver}" \
    --dmac "${RDMA_TEST_DMAC}" \
    --dip "${RDMA_TEST_DIP}" \
    --dport "${RDMA_TEST_DPORT}" \
    --device "${RDMA_TEST_DEVICE}" \
    --key "${ring_key}" \
    --send_n 8 \
    --nsge 4 \
    --config "${test_root}/pipeline.json" \
    --dump-header "${source_dir}/header/array_GZNU.header" \
    >"${test_root}/receiver.log" 2>&1 &
receiver_pid=$!

for _ in $(seq 1 100); do
    if grep -q "RDMA receiver running" "${test_root}/receiver.log"; then
        break
    fi
    if ! kill -0 "${receiver_pid}" 2>/dev/null; then
        cat "${test_root}/receiver.log" >&2
        echo "rdma2dada exited before becoming ready" >&2
        exit 1
    fi
    sleep 0.1
done
if ! grep -q "RDMA receiver running" "${test_root}/receiver.log"; then
    cat "${test_root}/receiver.log" >&2
    echo "timed out waiting for rdma2dada readiness" >&2
    exit 1
fi

remote_probe="${RDMA_TEST_REMOTE_SOURCE_DIR}/tests/rdma_udp_probe.py"
ssh_options=(
    -o BatchMode=yes
    -o ConnectTimeout=5
    -o StrictHostKeyChecking=yes
    -o UserKnownHostsFile="${RDMA_TEST_SSH_KNOWN_HOSTS}"
)
timeout 20 ssh "${ssh_options[@]}" -- \
    "${RDMA_TEST_SENDER_A}" python3 "${remote_probe}" \
    --destination-ip "${RDMA_TEST_DIP}" \
    --destination-port "${RDMA_TEST_DPORT}" \
    --source-ip "${RDMA_TEST_SENDER_A_IP}" \
    --source-port 41001 \
    --record-bytes 1056 \
    --valid-count 4 \
    --fill 17 \
    --wrong-after 2
timeout 20 ssh "${ssh_options[@]}" -- \
    "${RDMA_TEST_SENDER_B}" python3 "${remote_probe}" \
    --destination-ip "${RDMA_TEST_DIP}" \
    --destination-port "${RDMA_TEST_DPORT}" \
    --source-ip "${RDMA_TEST_SENDER_B_IP}" \
    --source-port 41002 \
    --record-bytes 1056 \
    --valid-count 4 \
    --fill 34

for _ in $(seq 1 100); do
    if grep -q "Blocks written: 1" "${test_root}/receiver.log"; then
        break
    fi
    if ! kill -0 "${receiver_pid}" 2>/dev/null; then
        cat "${test_root}/receiver.log" >&2
        echo "rdma2dada exited while receiving mixed-source traffic" >&2
        exit 1
    fi
    sleep 0.1
done
if ! grep -q "Blocks written: 1" "${test_root}/receiver.log"; then
    cat "${test_root}/receiver.log" >&2
    echo "timed out waiting for one complete raw ring block" >&2
    exit 1
fi

kill -TERM "${receiver_pid}"
timeout 20 tail --pid="${receiver_pid}" -f /dev/null
wait "${receiver_pid}"
receiver_pid=
timeout 20 tail --pid="${reader_pid}" -f /dev/null
wait "${reader_pid}"
reader_pid=

grep -q "Receive summary: accepted=8, wrong_length=1" \
    "${test_root}/receiver.log"

python3 - "${test_root}/output" <<'PY'
import pathlib
import sys

output_dir = pathlib.Path(sys.argv[1])
files = list(output_dir.glob("*.dada"))
if len(files) != 1:
    raise SystemExit(f"expected one output DADA file, found {files}")
content = files[0].read_bytes()
if len(content) != 4096 + 8448:
    raise SystemExit(f"unexpected DADA file size: {len(content)}")
records = [
    content[offset : offset + 1056]
    for offset in range(4096, len(content), 1056)
]
fill_values = [record[0] for record in records]
if any(record != bytes([record[0]]) * 1056 for record in records):
    raise SystemExit("raw ring contains a truncated or mixed record")
if fill_values.count(17) != 4 or fill_values.count(34) != 4:
    raise SystemExit(f"unexpected source record counts: {fill_values}")
if 0x7E in fill_values:
    raise SystemExit("wrong-length record reached the raw ring")
PY

echo "RDMA receiver mixed-source/wrong-length integration passed"
