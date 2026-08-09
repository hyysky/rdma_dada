#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 PIPELINE_WORKER OBSERVATION_COMPILER WEIGHT_GENERATOR SOURCE_DIR PSRDADA_BIN_DIR" >&2
    exit 2
fi

worker=$1
compiler=$2
weight_generator=$3
source_dir=$4
psrdada_bin=$5
dada_db="${psrdada_bin}/dada_db"
dada_diskdb="${psrdada_bin}/dada_diskdb"
dada_dbdisk="${psrdada_bin}/dada_dbdisk"

for executable in "${worker}" "${compiler}" "${dada_db}" \
                  "${dada_diskdb}" "${dada_dbdisk}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "SKIP: executable is unavailable: ${executable}"
        exit 77
    fi
done
for command_name in python3 tail timeout; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "SKIP: ${command_name} is unavailable"
        exit 77
    fi
done

if [[ -n "${PIPELINE_TEST_RESULT_ROOT:-}" ]]; then
    run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
    test_root="${PIPELINE_TEST_RESULT_ROOT%/}/${run_id}"
    mkdir -p "${test_root}"
    keep_results=1
else
    test_root=$(mktemp -d "${TMPDIR:-/tmp}/pipeline-worker-resolved.XXXXXX")
    keep_results=0
fi
key_base=$((0x7000 + ($$ % 0x0300) * 8))
raw_key=$(printf '%04x' "${key_base}")
# PSRDADA uses the ring key and its adjacent IPC key internally. Keep every
# candidate ring key at least two values apart even when a ring is not created.
compute_key=$(printf '%04x' "$((key_base + 2))")
output_key=$(printf '%04x' "$((key_base + 4))")
compute_ring_pid=
output_ring_pid=
compute_ring_owned=0
output_ring_owned=0
worker_pid=
reader_pid=

stop_ring() {
    local pid=$1
    local key=$2
    local owned=$3
    if [[ "${owned}" != 1 ]]; then
        return
    fi
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -INT "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    "${dada_db}" -d -k "${key}" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    set +e
    for pid in "${worker_pid}" "${reader_pid}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    stop_ring "${output_ring_pid}" "${output_key}" "${output_ring_owned}"
    stop_ring "${compute_ring_pid}" "${compute_key}" "${compute_ring_owned}"
    python3 - "${test_root}" "${status}" <<'PY' 2>/dev/null || true
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
(root / "result.json").write_text(json.dumps({
    "test_result": "PASS" if int(sys.argv[2]) == 0 else "FAIL",
    "exit_code": int(sys.argv[2]),
    "cleanup_result": "PASS",
}, indent=2) + "\n")
PY
    if [[ "${keep_results}" == 0 &&
          "${test_root}" == "${TMPDIR:-/tmp}"/pipeline-worker-resolved.* ]]; then
        rm -rf -- "${test_root}"
    fi
    return "${status}"
}
trap cleanup EXIT

python3 - "${test_root}" "${raw_key}" "${compute_key}" "${output_key}" <<'PY'
import datetime
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
(root / "run_manifest.json").write_text(json.dumps({
    "task": "pipeline_worker_resolved_integration",
    "purpose": "resolved-plan compute-ring to output-ring acceptance",
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "keys": {"raw": sys.argv[2], "compute": sys.argv[3], "output": sys.argv[4]},
    "retention": "persistent when PIPELINE_TEST_RESULT_ROOT is set; otherwise removed after cleanup",
}, indent=2) + "\n")
PY

python3 "${weight_generator}" "${test_root}/weights.npy" >/dev/null
python3 - "${test_root}" "${source_dir}" "${raw_key}" \
    "${compute_key}" "${output_key}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
source = pathlib.Path(sys.argv[2]).resolve()
raw_key, compute_key, output_key = sys.argv[3:6]
observation = {
    "schema_version": 1,
    "observation": {
        "observation_id": "pipeline-worker-resolved-integration",
        "utc_start": "2026-01-01-00:16:40",
        "duration_seconds": "0.000008",
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
        "profile": str(source / "config/packet_formats/frontend.example-v1.json"),
        "samples_per_packet": 2,
    },
    "blocks": {
        "groups_per_block": 2,
        "raw_ring_blocks": 4,
        "compute_ring_blocks": 4,
        "window_blocks": 2,
    },
    "rings": {
        "raw_key": "0x" + raw_key,
        "compute_key": "0x" + compute_key,
        "output_key": "0x" + output_key,
    },
    "storage": {"enabled": False, "blocks_per_file": 0, "direct_io": False},
    "receiver": {
        "device": "mlx5_0",
        "destination_mac": "98:03:9b:aa:99:d8",
        "destination_ip": "174.0.1.111",
        "destination_port": 1000,
    },
    "processing": {
        "backend": "CUDA",
        "cuda_device": 0,
        "run_once": True,
        "conversion": {"scale": "0.0078125"},
        "modules": [
            {
                "type": "beamform",
                "weights_file": str(root / "weights.npy"),
                "weights_order": "FPAB2",
                "weights_id": "pipeline-worker-resolved-test",
                "weights_scale": "0.5",
                "compute_mode": "FP32",
            },
            {"type": "power"},
            {"type": "integrate", "length": 2, "operation": "MEAN"},
        ],
        "output": {"sample_format": "AUTO"},
    },
}
(root / "observation.json").write_text(json.dumps(observation, indent=2) + "\n")
PY

"${compiler}" --config "${test_root}/observation.json" \
    --output-dir "${test_root}/artifacts" >/dev/null
read -r compute_block_bytes compute_blocks output_block_bytes output_blocks \
    expected_output_payload < <(python3 - "${test_root}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
ring_plan = json.loads((root / "artifacts/ring_plan.json").read_text())
resolved = json.loads(
    (root / "artifacts/resolved_observation.json").read_text())
compute = ring_plan["rings"]["compute"]
output = ring_plan["rings"]["output"]
groups_per_block = json.loads(resolved["source_json"])["blocks"]["groups_per_block"]
block_count = resolved["resolved"]["expected_groups"] // groups_per_block
print(compute["block_bytes"], compute["blocks"], output["block_bytes"],
      output["blocks"], block_count * output["block_bytes"])
PY
)
python3 - "${test_root}" "${compute_block_bytes}" <<'PY'
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
header = (root / "artifacts/unpacked.header").read_bytes()
(root / "input.dada").write_bytes(header + bytes(int(sys.argv[2]) * 2))
bad_header = header.replace(b"ORDER ATFP", b"ORDER TAFP", 1)
if bad_header == header:
    raise SystemExit("cannot construct wrong-order header fixture")
(root / "bad-header-input.dada").write_bytes(
    bad_header + bytes(int(sys.argv[2])))
PY

create_rings() {
    local compute_bytes=$1
    local output_bytes=$2
    "${dada_db}" -k "${compute_key}" -b "${compute_bytes}" -a 4096 \
        -n "${compute_blocks}" -p -w -l >"${test_root}/compute-ring.log" 2>&1 &
    compute_ring_pid=$!
    sleep 1
    if ! kill -0 "${compute_ring_pid}" 2>/dev/null; then
        echo "compute ring creation failed:" >&2
        cat "${test_root}/compute-ring.log" >&2
        return 1
    fi
    compute_ring_owned=1
    "${dada_db}" -k "${output_key}" -b "${output_bytes}" -a 4096 \
        -n "${output_blocks}" -p -w -l >"${test_root}/output-ring.log" 2>&1 &
    output_ring_pid=$!
    sleep 1
    if ! kill -0 "${output_ring_pid}" 2>/dev/null; then
        echo "output ring creation failed:" >&2
        cat "${test_root}/output-ring.log" >&2
        return 1
    fi
    output_ring_owned=1
}

destroy_rings() {
    stop_ring "${output_ring_pid}" "${output_key}" "${output_ring_owned}"
    stop_ring "${compute_ring_pid}" "${compute_key}" "${compute_ring_owned}"
    output_ring_pid=
    compute_ring_pid=
    output_ring_owned=0
    compute_ring_owned=0
}

create_rings "${compute_block_bytes}" "${output_block_bytes}"
mkdir -p "${test_root}/output"
"${dada_dbdisk}" -k "${output_key}" -D "${test_root}/output" -s -W \
    >"${test_root}/reader.log" 2>&1 &
reader_pid=$!
"${worker}" "${test_root}/artifacts/resolved_observation.json" \
    >"${test_root}/worker.log" 2>&1 &
worker_pid=$!
timeout 20 "${dada_diskdb}" -k "${compute_key}" \
    -f "${test_root}/input.dada" -s >"${test_root}/writer.log" 2>&1
timeout 20 tail --pid="${worker_pid}" -f /dev/null
wait "${worker_pid}"
worker_pid=
timeout 20 tail --pid="${reader_pid}" -f /dev/null
wait "${reader_pid}"
reader_pid=

python3 - "${test_root}" "${expected_output_payload}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
outputs = list((root / "output").glob("*.dada"))
if len(outputs) != 1:
    raise SystemExit(f"expected one output file, got {outputs}")
content = outputs[0].read_bytes()
header_text = content[:4096].split(b"\0", 1)[0].decode("ascii")
fields = {}
for line in header_text.splitlines():
    parts = line.split(None, 1)
    if len(parts) == 2:
        fields[parts[0]] = parts[1].strip()
plan = json.loads((root / "artifacts/resolved_observation.json").read_text())
expected = {
    "CONFIG_ID": plan["config_id"],
    "GEOMETRY_ID": plan["geometry_id"],
    "DATA_STAGE": "POWER_INTEGRATED",
    "ORDER": "TFPB",
    "SAMPLE_FORMAT": "F32",
    "NBEAM": "2",
    "BLOCK_NTIME": "2",
    "BLOCK_BYTES": "32",
    "OUTPUT_BLOCK_BYTES": "32",
    "RING_BYTES": "128",
}
for key, value in expected.items():
    if fields.get(key) != value:
        raise SystemExit(f"{key}={fields.get(key)!r}, expected {value!r}")
if content[4096:] != bytes(int(sys.argv[2])):
    raise SystemExit("zero-input output payload mismatch")
PY
destroy_rings

create_rings "${compute_block_bytes}" "${output_block_bytes}"
"${worker}" "${test_root}/artifacts/resolved_observation.json" \
    >"${test_root}/header-reject.log" 2>&1 &
worker_pid=$!
timeout 20 "${dada_diskdb}" -k "${compute_key}" \
    -f "${test_root}/bad-header-input.dada" -s >/dev/null 2>&1
timeout 20 tail --pid="${worker_pid}" -f /dev/null
if wait "${worker_pid}"; then
    echo "worker accepted an input header outside the compiled contract" >&2
    exit 1
fi
worker_pid=
grep -F "input DADA header ORDER must be ATFP" \
    "${test_root}/header-reject.log" >/dev/null
destroy_rings

bad_compute_block_bytes=$((compute_block_bytes * 2))
create_rings "${bad_compute_block_bytes}" "${output_block_bytes}"
"${worker}" "${test_root}/artifacts/resolved_observation.json" \
    >"${test_root}/capacity-reject.log" 2>&1 &
worker_pid=$!
timeout 20 "${dada_diskdb}" -k "${compute_key}" \
    -f "${test_root}/input.dada" -s >/dev/null 2>&1
timeout 20 tail --pid="${worker_pid}" -f /dev/null
if wait "${worker_pid}"; then
    echo "worker accepted mismatched input ring capacity" >&2
    exit 1
fi
worker_pid=
grep -F "connected input ring geometry does not match resolved compute ring" \
    "${test_root}/capacity-reject.log" >/dev/null
destroy_rings

echo "pipeline_worker resolved-plan integration passed"
