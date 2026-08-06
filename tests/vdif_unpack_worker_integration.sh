#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 VDIF_UNPACK_WORKER SOURCE_DIR" >&2
    exit 2
fi

worker=$1
source_dir=$2

for command_name in dada_db dada_diskdb dada_dbdisk python3 tail timeout; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "SKIP: ${command_name} is unavailable"
        exit 77
    fi
done

if [[ ! -x "${worker}" ]]; then
    echo "vdif_unpack_worker is not executable: ${worker}" >&2
    exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/vdif-unpack-worker.XXXXXX")
raw_key=000a
compute_key=000c
raw_ring_created=0
compute_ring_created=0
reader_pid=
worker_pid=
raw_ring_pid=
compute_ring_pid=

stop_ring_creator() {
    local ring_pid=$1
    local ring_key=$2
    if [[ -n "${ring_pid}" ]]; then
        if kill -0 "${ring_pid}" 2>/dev/null; then
            kill -INT "${ring_pid}" 2>/dev/null || true
        fi
        wait "${ring_pid}" 2>/dev/null || true
    fi
    dada_db -d -k "${ring_key}" >/dev/null 2>&1 || true
}

cleanup() {
    set +e
    for child_pid in "${worker_pid}" "${reader_pid}"; do
        if [[ -n "${child_pid}" ]]; then
            if kill -0 "${child_pid}" 2>/dev/null; then
                kill "${child_pid}" 2>/dev/null
            fi
            wait "${child_pid}" 2>/dev/null
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
    python3 - "${case_dir}" "${case_name}" "${source_dir}" \
        "${raw_key}" "${compute_key}" <<'PY'
import json
import pathlib
import struct
import sys

case_dir = pathlib.Path(sys.argv[1])
case_name = sys.argv[2]
source_dir = pathlib.Path(sys.argv[3])
raw_key = sys.argv[4]
compute_key = sys.argv[5]

pipeline = {
    "schema_version": 1,
    "observation": {
        "nant": 2,
        "nchan": 2,
        "npol": 1,
        "payload_order": "TFP",
        "utc_start": "2026-08-06-00:00:00",
    },
    "packet": {
        "header_bytes": 32,
        "payload_bytes": 8,
        "samples": 2,
        "nbit": 16,
        "sample_interval_us": 1.0,
    },
    "ring_buffers": {
        "records_per_block": 4,
        "raw_blocks": 4,
        "compute_blocks": 4,
    },
    "disk": {
        "enabled": True,
        "blocks_per_file": 2,
        "direct_io": False,
    },
}
worker_config = {
    "schema_version": 1,
    "rings": {"input_key": f"0x{raw_key}", "output_key": f"0x{compute_key}"},
    "sources": {
        "pipeline_config": "pipeline.json",
        "packet_format": "packet.json",
    },
    "selection": {"first_channel_id": 20, "antenna_map": [10, 11]},
    "window": {"blocks": 2, "max_bytes": 128},
    "output": {"memory": "HOST"},
    "runtime": {"run_once": case_name != "continuous"},
}
(case_dir / "pipeline.json").write_text(json.dumps(pipeline, indent=2) + "\n")
(case_dir / "packet.json").write_text(
    json.dumps(
        {
            **json.loads(
                (source_dir / "config/packet_formats/frontend.example-v1.json").read_text()
            ),
            "record": {
                "application_header_bytes": 32,
                "payload_bytes": 8,
            },
        },
        indent=2,
    )
    + "\n"
)
(case_dir / "worker.json").write_text(
    json.dumps(worker_config, indent=2) + "\n"
)

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

records = [
    record(0, 10, 0),
    record(1, 10, 0),
    record(0, 11, 1),
    record(2, 10, 0),
]
if case_name != "partial":
    records += [
        record(1, 11, 1),
        record(2, 11, 1),
        record(3, 10, 0),
        record(3, 11, 1),
    ]
    frames = [(0, True), (1, True), (2, True), (3, True)]
else:
    records += [
        record(1, 11, 1),
        record(1, 11, 1),
        record(2, 99, 0),
        record(2, 11, 1, invalid=True),
    ]
    frames = [(0, True), (1, True), (2, False)]

def tfpa_group(frame, has_antenna_1):
    station_payloads = [payload(frame, 0), payload(frame, 1)]
    result = bytearray()
    for element in range(4):
        begin = element * 2
        result += station_payloads[0][begin:begin + 2]
        result += (
            station_payloads[1][begin:begin + 2]
            if has_antenna_1
            else b"\0\0"
        )
    return bytes(result)

expected = b"".join(tfpa_group(frame, complete) for frame, complete in frames)
(case_dir / "expected.bin").write_bytes(expected)

header_fields = {
    "HDR_SIZE": 4096,
    "OBS_OFFSET": 0,
    "FILE_SIZE": 320,
    "TRANSFER_SIZE": 320,
    "DATA_STAGE": "RAW",
    "ORDER": "TFP",
    "UTC_START": pipeline["observation"]["utc_start"],
    "PIPELINE_VERSION": 1,
    "NANT": 2,
    "NCHAN": 2,
    "NPOL": 1,
    "NBIT": 16,
    "PKT_HEADER": 32,
    "PKT_DATA": 8,
    "PKT_NSAMP": 2,
    "PKT_TSAMP": 1.0,
    "RECORD_HEADER_BYTES": 32,
    "RECORD_BYTES": 40,
    "RESOLUTION": 40,
    "BYTES_PER_SECOND": 8000000,
    "RAW_BYTES_PER_SECOND": 40000000,
    "TELESCOPE": "PFT_TEST",
}
header_text = "".join(f"{key} {value}\n" for key, value in header_fields.items())
header = header_text.encode("ascii")
header += b"\0" * (4096 - len(header))
(case_dir / "input.dada").write_bytes(header + b"".join(records))
PY
}

verify_case() {
    local case_dir=$1
    local output_dir=$2
    local case_label=$3
    python3 - "${case_dir}" "${output_dir}" "${case_label}" <<'PY'
import pathlib
import sys

case_dir = pathlib.Path(sys.argv[1])
output_dir = pathlib.Path(sys.argv[2])
case_name = sys.argv[3]
files = list(output_dir.glob("*.dada"))
if len(files) != 1:
    raise SystemExit(f"{case_name}: expected one output DADA file, found {files}")
content = files[0].read_bytes()
if len(content) < 4096:
    raise SystemExit(f"{case_name}: output is shorter than its DADA header")
header = content[:4096].split(b"\0", 1)[0].decode("ascii")
fields = {}
for line in header.splitlines():
    parts = line.split(None, 1)
    if len(parts) == 2:
        fields[parts[0]] = parts[1].strip()
required = {
    "HDR_SIZE": "4096",
    "OBS_OFFSET": "0",
    "FILE_SIZE": "64",
    "TRANSFER_SIZE": "0",
    "DATA_STAGE": "UNPACKED",
    "ORDER": "TFPA",
    "UTC_START": "2026-08-06-00:00:00",
    "PIPELINE_VERSION": "1",
    "NANT": "2",
    "NCHAN": "2",
    "NPOL": "1",
    "NBIT": "16",
    "PKT_HEADER": "32",
    "PKT_DATA": "8",
    "PKT_NSAMP": "2",
    "PKT_TSAMP": "1.0",
    "SAMPLE_FORMAT": "CI8",
    "MEMORY": "HOST",
    "LOSS_POLICY": "ZERO_FILL",
    "RECORD_HEADER_BYTES": "0",
    "RECORD_BYTES": "8",
    "RESOLUTION": "8",
    "BYTES_PER_SECOND": "8000000",
    "RAW_BYTES_PER_SECOND": "40000000",
    "FIRST_CHANNEL_ID": "20",
    "UNPACK_WINDOW_BLOCKS": "2",
    "INPUT_BLOCK_BYTES": "160",
    "OUTPUT_BLOCK_BYTES": "32",
    "TELESCOPE": "PFT_TEST",
}
for key, expected in required.items():
    if fields.get(key) != expected:
        raise SystemExit(
            f"{case_name}: header {key}={fields.get(key)!r}, expected {expected!r}"
        )
unexpected = set(fields) - set(required) - {"FILE_NUMBER"}
if unexpected:
    raise SystemExit(f"{case_name}: unexpected output header fields: {sorted(unexpected)}")
actual = content[4096:]
expected = (case_dir / "expected.bin").read_bytes()
if actual != expected:
    raise SystemExit(
        f"{case_name}: TFPA mismatch: got {len(actual)} bytes, "
        f"expected {len(expected)} bytes"
    )
PY
}

create_rings() {
    dada_db -k "${raw_key}" -b 160 -a 4096 -n 4 -p -w -l >/dev/null 2>&1 &
    raw_ring_pid=$!
    raw_ring_created=1
    dada_db -k "${compute_key}" -b 32 -a 4096 -n 4 -p -w -l >/dev/null 2>&1 &
    compute_ring_pid=$!
    compute_ring_created=1
    sleep 1
    if ! kill -0 "${raw_ring_pid}" 2>/dev/null ||
       ! kill -0 "${compute_ring_pid}" 2>/dev/null; then
        echo "failed to create persistent PSRDADA test rings" >&2
        return 1
    fi
}

destroy_rings() {
    if [[ ${compute_ring_created} -eq 1 ]]; then
        stop_ring_creator "${compute_ring_pid}" "${compute_key}"
    fi
    compute_ring_pid=
    compute_ring_created=0
    if [[ ${raw_ring_created} -eq 1 ]]; then
        stop_ring_creator "${raw_ring_pid}" "${raw_key}"
    fi
    raw_ring_pid=
    raw_ring_created=0
}

run_case() {
    local case_name=$1
    local case_dir="${test_root}/${case_name}"
    generate_case "${case_name}"
    create_rings

    dada_dbdisk -k "${compute_key}" -D "${case_dir}/output" -s -W \
        >"${case_dir}/dbdisk.log" 2>&1 &
    reader_pid=$!
    "${worker}" "${case_dir}/worker.json" \
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
    local transfer_index
    local output_dir
    generate_case continuous
    create_rings
    "${worker}" "${case_dir}/worker.json" \
        >"${case_dir}/worker.log" 2>&1 &
    worker_pid=$!

    for transfer_index in 1 2; do
        output_dir="${case_dir}/output-${transfer_index}"
        mkdir -p "${output_dir}"
        dada_dbdisk -k "${compute_key}" -D "${output_dir}" -s -W \
            >"${case_dir}/dbdisk-${transfer_index}.log" 2>&1 &
        reader_pid=$!
        timeout 20 dada_diskdb -k "${raw_key}" \
            -f "${case_dir}/input.dada" -s \
            >"${case_dir}/diskdb-${transfer_index}.log" 2>&1
        timeout 20 tail --pid="${reader_pid}" -f /dev/null
        wait "${reader_pid}"
        reader_pid=
        verify_case "${case_dir}" "${output_dir}" \
            "continuous-${transfer_index}"
    done

    kill -TERM "${worker_pid}" 2>/dev/null || true
    if ! timeout 5 tail --pid="${worker_pid}" -f /dev/null; then
        kill -KILL "${worker_pid}" 2>/dev/null || true
    fi
    wait "${worker_pid}" 2>/dev/null || true
    worker_pid=
    destroy_rings
}

run_case full
run_case partial
run_continuous_case
echo "vdif_unpack_worker integration passed"
