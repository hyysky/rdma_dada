#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 VDIF_UNPACK_WORKER OBSERVATION_COMPILER SOURCE_DIR" >&2
    exit 2
fi

worker=$1
compiler=$2
source_dir=$3

for command_name in dada_db dada_diskdb dada_dbdisk python3 tail timeout; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "SKIP: ${command_name} is unavailable"
        exit 77
    fi
done
if [[ ! -x "${worker}" || ! -x "${compiler}" ]]; then
    echo "worker/compiler binary is not executable" >&2
    exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/vdif-unpack-worker.XXXXXX")
raw_key=000a
compute_key=000c
raw_ring_pid=
compute_ring_pid=
reader_pid=
worker_pid=

stop_ring_creator() {
    local ring_pid=$1
    local ring_key=$2
    if [[ -n "${ring_pid}" ]] && kill -0 "${ring_pid}" 2>/dev/null; then
        kill -INT "${ring_pid}" 2>/dev/null || true
        wait "${ring_pid}" 2>/dev/null || true
    fi
    dada_db -d -k "${ring_key}" >/dev/null 2>&1 || true
}

destroy_rings() {
    stop_ring_creator "${compute_ring_pid}" "${compute_key}"
    stop_ring_creator "${raw_ring_pid}" "${raw_key}"
    raw_ring_pid=
    compute_ring_pid=
}

cleanup() {
    set +e
    for child_pid in "${worker_pid}" "${reader_pid}"; do
        if [[ -n "${child_pid}" ]] && kill -0 "${child_pid}" 2>/dev/null; then
            kill "${child_pid}" 2>/dev/null || true
            wait "${child_pid}" 2>/dev/null || true
        fi
    done
    destroy_rings
    rm -rf "${test_root}"
}
trap cleanup EXIT

generate_case() {
    local case_name=$1
    local case_dir="${test_root}/${case_name}"
    mkdir -p "${case_dir}/output"
    python3 - "${case_dir}" "${case_name}" "${source_dir}" <<'PY'
import json
import pathlib
import struct
import sys

case_dir = pathlib.Path(sys.argv[1])
case_name = sys.argv[2]
source_dir = pathlib.Path(sys.argv[3]).resolve()

if case_name == "missing-group":
    frame_spec = [(0, True), (1, None), (2, True)]
elif case_name == "partial":
    frame_spec = [(0, True), (1, True), (2, False)]
else:
    frame_spec = [(0, True), (1, True), (2, True), (3, True)]

duration_ps = len(frame_spec) * 2_000_000
observation = {
    "schema_version": 1,
    "observation": {
        "observation_id": f"unpack-{case_name}",
        "utc_start": "2026-01-01-00:16:40",
        "duration_seconds": f"0.{duration_ps:012d}",
        "station_ids": [10, 11],
        "first_channel_id": 20,
        "nchan": 2,
        "npol": 1,
        "sample_interval_ps": 1_000_000,
    },
    "metadata": {
        "telescope": "PFT_TEST",
        "bandwidth_hz": 2_000_000,
        "center_frequency_hz": 1_250_000_000,
    },
    "wire": {
        "profile": str(source_dir / "config/packet_formats/frontend.example-v1.json"),
        "samples_per_packet": 2,
    },
    "blocks": {
        "groups_per_block": 2,
        "raw_ring_blocks": 4,
        "compute_ring_blocks": 4,
        "window_blocks": 2,
    },
    "rings": {
        "raw_key": "0x000a",
        "compute_key": "0x000c",
        "output_key": "0x000e",
    },
    "storage": {"enabled": True, "blocks_per_file": 2, "direct_io": False},
    "receiver": {
        "device": "mlx5_0",
        "destination_mac": "98:03:9b:aa:99:d8",
        "destination_ip": "174.0.1.111",
        "destination_port": 1000,
    },
    "processing": {
        "backend": "CUDA",
        "cuda_device": 0,
        "run_once": case_name != "continuous",
        "conversion": {"scale": "0.0078125"},
        "output": {"sample_format": "AUTO"},
        "modules": [],
    },
}
(case_dir / "observation.json").write_text(json.dumps(observation, indent=2) + "\n")

def payload(frame, antenna):
    return bytes((frame * 32 + antenna * 8 + index) & 0xFF for index in range(8))

def record(frame, station, antenna, invalid=False):
    words = [0] * 8
    words[0] = (0x80000000 if invalid else 0) | 1000
    words[1] = (52 << 24) | frame
    words[2] = (31 << 24) | 5
    words[3] = 0x80000000 | (7 << 26) | station
    words[4] = 0xFF010100
    words[5] = (20 << 16) | (2 << 8) | 1
    words[6] = 2
    return struct.pack("<8I", *words) + payload(frame, antenna)

if case_name == "missing-group":
    records = [record(2, 11, 1), record(0, 10, 0),
               record(2, 10, 0), record(0, 11, 1)]
elif case_name == "partial":
    records = [record(0, 10, 0), record(1, 10, 0),
               record(0, 11, 1), record(2, 10, 0),
               record(1, 11, 1), record(2, 10, 0)]
else:
    records = [record(0, 10, 0), record(1, 10, 0),
               record(0, 11, 1), record(2, 10, 0),
               record(1, 11, 1), record(2, 11, 1),
               record(3, 10, 0), record(3, 11, 1)]
(case_dir / "records.bin").write_bytes(b"".join(records))

def atfp_block(block_frames):
    result = bytearray()
    for antenna in range(2):
        for frame, completeness in block_frames:
            if completeness is None or (antenna == 1 and not completeness):
                result += b"\0" * 8
            else:
                result += payload(frame, antenna)
    return bytes(result)

expected = b"".join(
    atfp_block(frame_spec[offset:offset + 2])
    for offset in range(0, len(frame_spec), 2)
)
(case_dir / "expected.bin").write_bytes(expected)
PY
    "${compiler}" --config "${case_dir}/observation.json" \
        --output-dir "${case_dir}/artifacts" >"${case_dir}/compile.log"
    python3 - "${case_dir}" <<'PY'
import pathlib
import sys
case_dir = pathlib.Path(sys.argv[1])
(case_dir / "input.dada").write_bytes(
    (case_dir / "artifacts/raw.header").read_bytes() +
    (case_dir / "records.bin").read_bytes()
)
PY
}

verify_case() {
    local case_dir=$1
    local output_dir=$2
    local case_label=$3
    python3 - "${case_dir}" "${output_dir}" "${case_label}" <<'PY'
import json
import pathlib
import sys

case_dir = pathlib.Path(sys.argv[1])
output_dir = pathlib.Path(sys.argv[2])
case_name = sys.argv[3]
files = list(output_dir.glob("*.dada"))
if len(files) != 1:
    raise SystemExit(f"{case_name}: expected one output DADA file, found {files}")
content = files[0].read_bytes()
header = content[:4096].split(b"\0", 1)[0].decode("ascii")
fields = {}
for line in header.splitlines():
    parts = line.split(None, 1)
    if len(parts) == 2:
        fields[parts[0]] = parts[1].strip()
plan = json.loads((case_dir / "artifacts/resolved_observation.json").read_text())
expected_groups = 4 if case_name.startswith(("full", "continuous")) else 3
expected_bytes = 64 if expected_groups == 4 else 48
required = {
    "CONFIG_ID": plan["config_id"],
    "GEOMETRY_ID": plan["geometry_id"],
    "DATA_STAGE": "UNPACKED",
    "ORDER": "ATFP",
    "LAYOUT_SCOPE": "BLOCK",
    "RECORD_HEADER_BYTES": "0",
    "RECORD_BYTES": "32",
    "RESOLUTION": "8",
    "BLOCK_BYTES": "32",
    "RING_BYTES": "128",
    "BLOCK_NTIME": "4",
    "INPUT_BLOCK_BYTES": "160",
    "OUTPUT_BLOCK_BYTES": "32",
    "TRANSFER_SIZE": str(expected_bytes),
    "EXPECTED_GROUPS": str(expected_groups),
    "GROUP_PERIOD_PS": "2000000",
    "GROUP_START_REFERENCE_EPOCH": "52",
    "GROUP_START_SECONDS": "1000",
    "GROUP_START_FRAME": "0",
    "TELESCOPE": "PFT_TEST",
}
for key, expected in required.items():
    if fields.get(key) != expected:
        raise SystemExit(f"{case_name}: {key}={fields.get(key)!r}, expected {expected!r}")
if "COMPONENT_SIGNED" in fields or "SOURCE_COMPONENT_SIGNED" in fields:
    raise SystemExit(f"{case_name}: obsolete signed field was published")
actual = content[4096:]
expected = (case_dir / "expected.bin").read_bytes()
if actual != expected:
    raise SystemExit(f"{case_name}: ATFP mismatch {len(actual)} != {len(expected)}")
PY
}

create_rings_with_sizes() {
    local raw_block_bytes=$1
    local compute_block_bytes=$2
    dada_db -k "${raw_key}" -b "${raw_block_bytes}" -a 4096 -n 4 -p -w -l >/dev/null 2>&1 &
    raw_ring_pid=$!
    dada_db -k "${compute_key}" -b "${compute_block_bytes}" -a 4096 -n 4 -p -w -l >/dev/null 2>&1 &
    compute_ring_pid=$!
    sleep 1
    kill -0 "${raw_ring_pid}" 2>/dev/null
    kill -0 "${compute_ring_pid}" 2>/dev/null
}

create_rings() {
    create_rings_with_sizes 160 32
}

expect_worker_rejection() {
    local case_name=$1
    local raw_block_bytes=$2
    local compute_block_bytes=$3
    local expected_error=$4
    local tamper_header=${5:-false}
    local case_dir="${test_root}/${case_name}"
    generate_case "${case_name}"
    if [[ "${tamper_header}" == true ]]; then
        python3 - "${case_dir}/input.dada" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
content = bytearray(path.read_bytes())
header = bytes(content[:4096])
start = header.index(b"CONFIG_ID ") + len(b"CONFIG_ID ")
end = header.index(b"\n", start)
content[start:end] = b"0" * (end - start)
path.write_bytes(content)
PY
    fi
    create_rings_with_sizes "${raw_block_bytes}" "${compute_block_bytes}"
    "${worker}" --plan "${case_dir}/artifacts/resolved_observation.json" \
        >"${case_dir}/worker.log" 2>&1 &
    worker_pid=$!
    timeout 20 dada_diskdb -k "${raw_key}" -f "${case_dir}/input.dada" -s \
        >"${case_dir}/diskdb.log" 2>&1
    timeout 20 tail --pid="${worker_pid}" -f /dev/null
    if wait "${worker_pid}"; then
        echo "${case_name}: worker unexpectedly accepted invalid input" >&2
        exit 1
    fi
    worker_pid=
    if ! grep -F "${expected_error}" "${case_dir}/worker.log" >/dev/null; then
        echo "${case_name}: missing expected diagnostic: ${expected_error}" >&2
        cat "${case_dir}/worker.log" >&2
        exit 1
    fi
    destroy_rings
}

run_case() {
    local case_name=$1
    local case_dir="${test_root}/${case_name}"
    generate_case "${case_name}"
    create_rings
    dada_dbdisk -k "${compute_key}" -D "${case_dir}/output" -s -W \
        >"${case_dir}/dbdisk.log" 2>&1 &
    reader_pid=$!
    "${worker}" --plan "${case_dir}/artifacts/resolved_observation.json" \
        >"${case_dir}/worker.log" 2>&1 &
    worker_pid=$!
    timeout 20 dada_diskdb -k "${raw_key}" -f "${case_dir}/input.dada" -s \
        >"${case_dir}/diskdb.log" 2>&1
    timeout 20 tail --pid="${worker_pid}" -f /dev/null
    wait "${worker_pid}"
    worker_pid=
    timeout 20 tail --pid="${reader_pid}" -f /dev/null
    wait "${reader_pid}"
    reader_pid=
    verify_case "${case_dir}" "${case_dir}/output" "${case_name}"
    destroy_rings
}

run_continuous_case() {
    local case_dir="${test_root}/continuous"
    generate_case continuous
    create_rings
    "${worker}" --plan "${case_dir}/artifacts/resolved_observation.json" \
        >"${case_dir}/worker.log" 2>&1 &
    worker_pid=$!
    for transfer_index in 1 2; do
        local output_dir="${case_dir}/output-${transfer_index}"
        mkdir -p "${output_dir}"
        dada_dbdisk -k "${compute_key}" -D "${output_dir}" -s -W \
            >"${case_dir}/dbdisk-${transfer_index}.log" 2>&1 &
        reader_pid=$!
        timeout 20 dada_diskdb -k "${raw_key}" -f "${case_dir}/input.dada" -s \
            >"${case_dir}/diskdb-${transfer_index}.log" 2>&1
        timeout 20 tail --pid="${reader_pid}" -f /dev/null
        wait "${reader_pid}"
        reader_pid=
        verify_case "${case_dir}" "${output_dir}" "continuous-${transfer_index}"
    done
    kill -TERM "${worker_pid}" 2>/dev/null || true
    timeout 5 tail --pid="${worker_pid}" -f /dev/null || \
        kill -KILL "${worker_pid}" 2>/dev/null || true
    wait "${worker_pid}" 2>/dev/null || true
    worker_pid=
    destroy_rings
}

run_case full
run_case partial
run_case missing-group
run_continuous_case
expect_worker_rejection config-id-mismatch 160 32 \
    "raw header CONFIG_ID conflicts with configuration" true
expect_worker_rejection raw-capacity-mismatch 192 32 \
    "input ring block capacity must be 160 bytes"
expect_worker_rejection compute-capacity-mismatch 160 64 \
    "output ring block capacity must be 32 bytes"
echo "vdif_unpack_worker integration passed"
