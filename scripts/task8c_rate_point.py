#!/usr/bin/env python3
"""Run one persistent Task 8C UDP rate point from the HF control host."""

from __future__ import annotations

import argparse
import dataclasses
import decimal
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import statistics
import sys
import time
import uuid
from typing import Any, Iterable, Sequence


class StageError(RuntimeError):
    def __init__(
        self,
        stage: str,
        argv: Sequence[str],
        exit_code: int,
        stdout: str = "",
        stderr: str = "",
        classification: str = "HARNESS_FAIL",
    ) -> None:
        super().__init__(f"{stage} failed with exit code {exit_code}")
        self.stage = stage
        self.argv = list(argv)
        self.exit_code = exit_code
        self.stdout = stdout
        self.stderr = stderr
        self.classification = classification

    def as_dict(self) -> dict[str, Any]:
        return {
            "stage": self.stage,
            "argv": self.argv,
            "exit_code": self.exit_code,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "classification": self.classification,
        }


@dataclasses.dataclass(frozen=True)
class RateRequest:
    aggregate_gbps: float
    duration_seconds: float
    batch_packets: int = 16
    compute_consumer: str = "dbdisk"

    def validate(self) -> None:
        if (
            not math.isfinite(self.aggregate_gbps)
            or not math.isfinite(self.duration_seconds)
            or self.aggregate_gbps <= 0
            or self.duration_seconds <= 0
        ):
            raise ValueError("rate and duration must be positive")
        if self.batch_packets <= 0:
            raise ValueError("batch_packets must be positive")
        if self.compute_consumer not in ("dbdisk", "dbnull"):
            raise ValueError("compute_consumer must be dbdisk or dbnull")


def build_ssh_argv(
    host: str, remote_argv: Sequence[str], known_hosts: str | None = None
) -> list[str]:
    argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
    if known_hosts:
        argv += [
            "-o",
            f"UserKnownHostsFile={known_hosts}",
            "-o",
            "StrictHostKeyChecking=yes",
        ]
    argv += [host, "--"]
    argv += list(remote_argv)
    return argv


def _atomic_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


@dataclasses.dataclass(frozen=True)
class RatePlan:
    aggregate_gbps: float
    duration_seconds: float
    batch_packets: int
    per_station_gbps: float
    group_count: int
    compute_consumer: str
    record_bytes: int
    raw_key: str
    compute_key: str
    records_per_block: int
    resolved_plan: dict[str, Any]
    ring_plan: dict[str, Any]
    artifact_files: dict[str, bytes]

    @property
    def raw_block_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["raw_block_bytes"])

    @property
    def compute_block_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["compute_block_bytes"])

    @property
    def raw_ring_blocks(self) -> int:
        return int(self.ring_plan["rings"]["raw"]["blocks"])

    @property
    def compute_ring_blocks(self) -> int:
        return int(self.ring_plan["rings"]["compute"]["blocks"])

    @property
    def config_id(self) -> str:
        return str(self.resolved_plan["config_id"])

    @property
    def geometry_id(self) -> str:
        return str(self.resolved_plan["geometry_id"])

    @property
    def source(self) -> dict[str, Any]:
        value = json.loads(str(self.resolved_plan["source_json"]))
        if not isinstance(value, dict):
            raise ValueError("resolved source_json must contain an object")
        return value

    @property
    def nant(self) -> int:
        return len(self.source["observation"]["station_ids"])

    @property
    def nchan(self) -> int:
        return int(self.source["observation"]["nchan"])

    @property
    def npol(self) -> int:
        return int(self.source["observation"]["npol"])

    @property
    def payload_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["payload_bytes"])

    @classmethod
    def from_artifact_directory(
        cls,
        artifact_directory: pathlib.Path,
        aggregate_gbps: float,
        duration_seconds: float,
        batch_packets: int = 16,
        compute_consumer: str = "dbdisk",
    ) -> "RatePlan":
        root = pathlib.Path(artifact_directory)
        required = (
            "resolved_observation.json",
            "ring_plan.json",
            "raw.header",
            "unpacked.header",
            "validation_report.json",
        )
        files = {name: (root / name).read_bytes() for name in required}
        manifest_path = root / "MANIFEST.sha256"
        manifest = manifest_path.read_text()
        expected_manifest: dict[str, str] = {}
        for line in manifest.splitlines():
            parts = line.split()
            if len(parts) != 2:
                raise ValueError("invalid compiler MANIFEST.sha256 line")
            expected_manifest[parts[1]] = parts[0]
        if set(expected_manifest) != set(required):
            raise ValueError("compiler manifest file set is incomplete")
        for name, contents in files.items():
            if hashlib.sha256(contents).hexdigest() != expected_manifest[name]:
                raise ValueError(f"compiler artifact SHA256 mismatch: {name}")
        files["MANIFEST.sha256"] = manifest.encode("ascii")
        resolved = json.loads(files["resolved_observation.json"])
        rings = json.loads(files["ring_plan.json"])
        report = json.loads(files["validation_report.json"])
        derived = resolved["resolved"]
        raw = rings["rings"]["raw"]
        compute = rings["rings"]["compute"]
        if (
            raw["block_bytes"] != derived["raw_block_bytes"]
            or compute["block_bytes"] != derived["compute_block_bytes"]
            or rings.get("config_id") != resolved.get("config_id")
            or rings.get("geometry_id") != resolved.get("geometry_id")
        ):
            raise ValueError("ring plan conflicts with resolved observation")
        if (
            report.get("valid") is not True
            or report.get("config_id") != resolved.get("config_id")
            or report.get("geometry_id") != resolved.get("geometry_id")
        ):
            raise ValueError("validation report conflicts with resolved observation")
        for name, expected_stage in (
            ("raw.header", "RAW"),
            ("unpacked.header", "UNPACKED"),
        ):
            header_text = files[name].split(b"\0", 1)[0].decode("ascii")
            fields = {}
            for line in header_text.splitlines():
                parts = line.split(None, 1)
                if len(parts) == 2:
                    fields[parts[0]] = parts[1].strip()
            if (
                fields.get("DATA_STAGE") != expected_stage
                or fields.get("CONFIG_ID") != resolved.get("config_id")
                or fields.get("GEOMETRY_ID") != resolved.get("geometry_id")
            ):
                raise ValueError(f"{name} conflicts with resolved observation")
        if aggregate_gbps <= 0 or duration_seconds <= 0 or batch_packets <= 0:
            raise ValueError("rate, duration and batch size must be positive")
        if compute_consumer not in ("dbdisk", "dbnull"):
            raise ValueError("compute_consumer must be dbdisk or dbnull")
        source = json.loads(resolved["source_json"])
        station_count = len(source["observation"]["station_ids"])
        if station_count == 0:
            raise ValueError("resolved observation has no Stations")
        def key_text(value: str) -> str:
            parsed = int(value, 16)
            return f"{parsed:04x}"
        return cls(
            aggregate_gbps=aggregate_gbps,
            duration_seconds=duration_seconds,
            batch_packets=batch_packets,
            per_station_gbps=aggregate_gbps / station_count,
            group_count=int(derived["expected_groups"]),
            compute_consumer=compute_consumer,
            record_bytes=int(derived["raw_record_bytes"]),
            raw_key=key_text(str(raw["key"])),
            compute_key=key_text(str(compute["key"])),
            records_per_block=int(derived["records_per_block"]),
            resolved_plan=resolved,
            ring_plan=rings,
            artifact_files=files,
        )

    def as_dict(self) -> dict[str, Any]:
        result = dataclasses.asdict(self)
        result.pop("artifact_files")
        result.update(
            {
                "raw_block_bytes": self.raw_block_bytes,
                "compute_block_bytes": self.compute_block_bytes,
                "raw_ring_blocks": self.raw_ring_blocks,
                "compute_ring_blocks": self.compute_ring_blocks,
                "config_id": self.config_id,
                "geometry_id": self.geometry_id,
                "artifact_sha256": {
                    name: hashlib.sha256(contents).hexdigest()
                    for name, contents in self.artifact_files.items()
                },
            }
        )
        return result


def _seconds_from_picoseconds(value: int) -> str:
    if value <= 0:
        raise ValueError("duration picoseconds must be positive")
    whole, fraction = divmod(value, 1_000_000_000_000)
    if fraction == 0:
        return str(whole)
    return f"{whole}.{fraction:012d}".rstrip("0")


def _group_count_for_request(
    request: RateRequest, station_count: int, payload_bytes: int
) -> int:
    request.validate()
    if station_count <= 0 or payload_bytes <= 0:
        raise ValueError("station count and payload bytes must be positive")
    payload_bits = decimal.Decimal(payload_bytes * 8 * station_count)
    requested_bits = (
        decimal.Decimal(str(request.aggregate_gbps))
        * decimal.Decimal(1_000_000_000)
        * decimal.Decimal(str(request.duration_seconds))
    )
    return int(
        (requested_bits / payload_bits).to_integral_value(
            rounding=decimal.ROUND_CEILING
        )
    )


def _run_observation_compiler(
    compiler: pathlib.Path,
    config_path: pathlib.Path,
    output_directory: pathlib.Path | None,
) -> None:
    argv = [str(compiler), "--config", str(config_path)]
    if output_directory is None:
        argv.append("--preflight-only")
    else:
        argv += ["--output-dir", str(output_directory)]
    completed = subprocess.run(argv, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise StageError(
            "CONFIG_READY", argv, completed.returncode, completed.stdout,
            completed.stderr, "HARNESS_FAIL"
        )


def compile_rate_plan(
    request: RateRequest,
    observation_template: pathlib.Path,
    compiler: pathlib.Path,
    run_directory: pathlib.Path,
) -> RatePlan:
    request.validate()
    template_path = pathlib.Path(observation_template).resolve()
    compiler_path = pathlib.Path(compiler).resolve()
    if not template_path.is_file():
        raise StageError(
            "CONFIG_READY", ["read", str(template_path)], 2, "",
            "observation template does not exist", "ENV_BLOCKED"
        )
    if not compiler_path.is_file() or not os.access(compiler_path, os.X_OK):
        raise StageError(
            "CONFIG_READY", [str(compiler_path), "--help"], 126, "",
            "observation compiler is not executable", "ENV_BLOCKED"
        )
    root = pathlib.Path(run_directory)
    root.mkdir(parents=True, exist_ok=True)
    try:
        observation = json.loads(template_path.read_text())
        wire_reference = pathlib.Path(observation["wire"]["profile"])
        if not wire_reference.is_absolute():
            observation["wire"]["profile"] = str(
                (template_path.parent / wire_reference).resolve()
            )
        for module in observation["processing"]["modules"]:
            if module.get("type") == "beamform":
                weight = pathlib.Path(module["weights_file"])
                if not weight.is_absolute():
                    module["weights_file"] = str(
                        (template_path.parent / weight).resolve()
                    )
        observation["observation"]["observation_id"] = root.name
        observation["processing"]["run_once"] = True
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise StageError(
            "CONFIG_READY", ["parse", str(template_path)], 1, "", repr(error),
            "HARNESS_FAIL"
        ) from error

    bootstrap_config = root / "observation.bootstrap.json"
    bootstrap_config.write_text(json.dumps(observation, indent=2) + "\n")
    bootstrap_artifacts = root / "observation-bootstrap-artifacts"
    _run_observation_compiler(compiler_path, bootstrap_config, None)
    _run_observation_compiler(
        compiler_path, bootstrap_config, bootstrap_artifacts
    )
    bootstrap = RatePlan.from_artifact_directory(
        bootstrap_artifacts, request.aggregate_gbps,
        request.duration_seconds, request.batch_packets,
        request.compute_consumer
    )
    group_count = _group_count_for_request(
        request, bootstrap.nant, bootstrap.payload_bytes
    )
    group_period_ps = int(
        bootstrap.resolved_plan["resolved"]["group_period_ps"]
    )
    observation["observation"]["duration_seconds"] = (
        _seconds_from_picoseconds(group_count * group_period_ps)
    )
    final_config = root / "observation.json"
    final_config.write_text(json.dumps(observation, indent=2) + "\n")
    final_artifacts = root / "observation-artifacts"
    _run_observation_compiler(compiler_path, final_config, None)
    _run_observation_compiler(compiler_path, final_config, final_artifacts)
    plan = RatePlan.from_artifact_directory(
        final_artifacts, request.aggregate_gbps, request.duration_seconds,
        request.batch_packets, request.compute_consumer
    )
    if plan.group_count != group_count:
        raise StageError(
            "CONFIG_READY", ["validate", "expected_groups"], 1,
            str(plan.group_count), str(group_count), "HARNESS_FAIL"
        )
    return plan


def derive_sender_source_ports(run_identity: str) -> tuple[int, int]:
    if not run_identity:
        raise ValueError("run identity must not be empty")
    digest = hashlib.sha256(run_identity.encode("utf-8")).digest()
    offset = int.from_bytes(digest[:2], byteorder="big") % 10000
    return 40000 + offset, 50000 + offset


def build_sender_endpoint_probe() -> str:
    return '''#!/usr/bin/env python3
import socket
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: probe_sender_endpoint.py SOURCE_IP SOURCE_PORT")

endpoint = (sys.argv[1], int(sys.argv[2]))
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    probe.bind(endpoint)
except OSError as error:
    print(f"sender endpoint unavailable: {endpoint[0]}:{endpoint[1]}: {error}", file=sys.stderr)
    raise SystemExit(error.errno or 1)
finally:
    probe.close()
'''


def build_sender_config(
    plan: RatePlan,
    station_id: int,
    source_ip: str,
    source_port: int,
    start_utc: str,
) -> dict[str, Any]:
    if plan.resolved_plan is None:
        raise ValueError("sender config requires a compiler-resolved plan")
    source = plan.source
    observation = source["observation"]
    wire = source["wire"]
    receiver = source["receiver"]
    resolved = plan.resolved_plan["resolved"]
    return {
        "schema_version": 2,
        "source": {"ip": source_ip, "port": source_port},
        "destination": {
            "ip": receiver["destination_ip"],
            "port": receiver["destination_port"],
            "path_mtu": 9000,
        },
        "station": {"station_id": station_id},
        "packet": {
            "first_channel_id": observation["first_channel_id"],
            "nchan": observation["nchan"],
            "npol": observation["npol"],
            "nsamp_per_packet": wire["samples_per_packet"],
            "component_bits": 8,
            "sample_interval_ps": str(observation["sample_interval_ps"]),
        },
        "time": {
            "reference_epoch": resolved["group_start_reference_epoch"],
            "start_seconds": resolved["group_start_seconds"],
            "group_count": plan.group_count,
            "mode": "PACED",
            "start_utc": start_utc,
        },
        "transmit": {
            "target_gbps": plan.per_station_gbps,
            "batch_packets": plan.batch_packets,
            "payload_mode": "REPEAT_TEMPLATE",
        },
        "faults": {
            "drop_groups": [],
            "duplicate_groups": [],
            "invalid_header_groups": [],
        },
    }


def build_qths_bundle(
    plan: RatePlan,
    remote_run_dir: str,
    dada_db_path: str = "dada_db",
    dbdisk_path: str = "dada_dbdisk",
    dbnull_path: str = "dada_dbnull",
    qths_binary_dir: str = "/home/user/wy/rdma_dada/build-linux",
) -> dict[str, str | bytes]:
    if not plan.artifact_files:
        raise ValueError("compiler artifact bundle is required")
    if plan.compute_consumer == "dbnull":
        reader_start = f'''(
  set +e
  {dbnull_path} -k {plan.compute_key} -s -z -q
  code=$?
  printf '%s\\n' "$code" >"$run_dir/reader.exit"
  exit "$code"
) >"$run_dir/reader.log" 2>&1 &
echo $! >"$run_dir/reader.pid"'''
    else:
        reader_start = f'''{dbdisk_path} -k {plan.compute_key} -D "$run_dir/compute" -s -W >"$run_dir/reader.log" 2>&1 &
echo $! >"$run_dir/reader.pid"'''
    prepare = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
mkdir -p "$run_dir/compute"
{dada_db_path} -k {plan.raw_key} -b {plan.raw_block_bytes} -a 4096 -n {plan.raw_ring_blocks} -r 1 -p -w -l >"$run_dir/raw-ring.log" 2>&1 &
echo $! >"$run_dir/raw-ring.pid"
touch "$run_dir/raw-ring.created"
{dada_db_path} -k {plan.compute_key} -b {plan.compute_block_bytes} -a 4096 -n {plan.compute_ring_blocks} -r 1 -p -w -l >"$run_dir/compute-ring.log" 2>&1 &
echo $! >"$run_dir/compute-ring.pid"
touch "$run_dir/compute-ring.created"
sleep 1
kill -0 "$(cat "$run_dir/raw-ring.pid")"
kill -0 "$(cat "$run_dir/compute-ring.pid")"
"""
    start = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
project={qths_binary_dir}
{reader_start}
"$project/vdif_unpack_worker" --plan "$run_dir/resolved_observation.json" >"$run_dir/worker.log" 2>&1 &
echo $! >"$run_dir/worker.pid"
"$project/rdma2dada" --plan "$run_dir/resolved_observation.json" --send_n 64 --nsge 4 >"$run_dir/receiver.log" 2>&1 &
echo $! >"$run_dir/receiver.pid"
for _ in $(seq 1 300); do
  grep -q 'RDMA receiver running' "$run_dir/receiver.log" && exit 0
  kill -0 "$(cat "$run_dir/receiver.pid")" || exit 1
  sleep 0.1
done
exit 1
"""
    finish = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
attempts=${{TASK8C_WAIT_ATTEMPTS:-300}}
wait_sleep=${{TASK8C_WAIT_SLEEP:-0.1}}
kill -TERM "$(cat "$run_dir/receiver.pid")"
for name in receiver worker reader; do
  pid=$(cat "$run_dir/$name.pid")
  for _ in $(seq 1 "$attempts"); do
    kill -0 "$pid" 2>/dev/null || break
    sleep "$wait_sleep"
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "$name pid $pid did not exit" >&2
    exit 1
  fi
done
"""
    cleanup = f"""#!/usr/bin/env bash
set +e
run_dir={remote_run_dir}
status=0
attempts=${{TASK8C_WAIT_ATTEMPTS:-100}}
wait_sleep=${{TASK8C_WAIT_SLEEP:-0.1}}
for name in receiver worker reader raw-ring compute-ring; do
  file="$run_dir/$name.pid"
  if [[ -f "$file" ]]; then
    pid=$(cat "$file")
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 "$attempts"); do
      kill -0 "$pid" 2>/dev/null || break
      sleep "$wait_sleep"
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null
      sleep "$wait_sleep"
    fi
    if kill -0 "$pid" 2>/dev/null; then
      echo "$name pid $pid still exists after cleanup" >&2
      status=1
    fi
  fi
done
if [[ -f "$run_dir/raw-ring.created" ]]; then
  {dada_db_path} -d -k {plan.raw_key} || status=1
fi
if [[ -f "$run_dir/compute-ring.created" ]]; then
  {dada_db_path} -d -k {plan.compute_key} || status=1
fi
if [[ -f "$run_dir/capability.added" ]]; then
  binary={qths_binary_dir}/rdma2dada
  sudo -n setcap -r "$binary" || status=1
  if [[ -n "$(getcap "$binary")" ]]; then
    echo "capability still present on $binary" >&2
    status=1
  fi
fi
exit "$status"
"""
    summarize = f'''#!/usr/bin/env python3
import json
from pathlib import Path

run_dir = Path({remote_run_dir!r})
files = sorted((run_dir / "compute").glob("*.dada"))
if not files:
    raise SystemExit("no compute DADA output")
data_bytes = 0
header_fields = None
sample = b""
for index, path in enumerate(files):
    size = path.stat().st_size
    if size < 4096:
        raise SystemExit(f"compute file shorter than header: {{path}}")
    data_bytes += size - 4096
    with path.open("rb") as stream:
        header_text = stream.read(4096).split(b"\\0", 1)[0].decode("ascii")
        if index == 0:
            sample = stream.read(32)
    fields = {{}}
    for line in header_text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1].strip()
    if header_fields is None:
        header_fields = fields
summary = {{
    "file_count": len(files),
    "data_bytes": data_bytes,
    "header": header_fields or {{}},
    "sample_hex": sample.hex(),
}}
(run_dir / "compute-summary.json").write_text(json.dumps(summary, indent=2) + "\\n")
'''
    if plan.compute_consumer == "dbnull":
        summarize = f'''#!/usr/bin/env python3
import json
from pathlib import Path

run_dir = Path({remote_run_dir!r})
exit_path = run_dir / "reader.exit"
if not exit_path.exists():
    raise SystemExit("dada_dbnull exit status is missing")
summary = {{
    "consumer": "dada_dbnull",
    "exit_code": int(exit_path.read_text().strip()),
    "zero_copy": True,
    "single_transfer": True,
}}
(run_dir / "compute-summary.json").write_text(json.dumps(summary, indent=2) + "\\n")
'''
    bundle: dict[str, str | bytes] = dict(plan.artifact_files)
    bundle.update({
        "prepare.sh": prepare,
        "start.sh": start,
        "finish.sh": finish,
        "cleanup.sh": cleanup,
        "summarize.py": summarize,
    })
    return bundle


class SubprocessTransport:
    def run(
        self, argv: Sequence[str], stage: str, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        completed = subprocess.run(
            list(argv), text=True, capture_output=True, check=False
        )
        if check and completed.returncode != 0:
            raise StageError(
                stage,
                argv,
                completed.returncode,
                completed.stdout,
                completed.stderr,
            )
        return completed

    def start(self, argv: Sequence[str], stdout_path: pathlib.Path) -> subprocess.Popen[str]:
        stdout_path.parent.mkdir(parents=True, exist_ok=True)
        stream = stdout_path.open("w")
        process = subprocess.Popen(
            list(argv), text=True, stdout=stream, stderr=subprocess.STDOUT
        )
        process._task8c_stream = stream  # type: ignore[attr-defined]
        process._task8c_output_path = stdout_path  # type: ignore[attr-defined]
        return process


class SshBackend:
    """Real HF-side backend. It connects directly to qths/qtp hosts."""

    def __init__(
        self,
        transport: Any | None = None,
        project_root: pathlib.Path = pathlib.Path("/home/user/wy/rdma_dada"),
        known_hosts: pathlib.Path = pathlib.Path("/tmp/task8c-known-hosts"),
        qths_binary_dir: pathlib.Path | None = None,
        sender_binary_dir: pathlib.Path | None = None,
    ) -> None:
        self.transport = transport or SubprocessTransport()
        self.project_root = pathlib.Path(project_root)
        self.qths_binary_dir = pathlib.Path(
            qths_binary_dir or self.project_root / "build-linux"
        )
        self.sender_binary_dir = pathlib.Path(
            sender_binary_dir or self.project_root / "build-linux"
        )
        self.known_hosts = pathlib.Path(known_hosts)
        self.remote_run_dir = ""
        self.local_run_dir = pathlib.Path()
        self.preparation: dict[str, Any] = {}
        self._rings_acquired: list[str] = []
        self._capability_added = False
        self._sender_processes: list[Any] = []
        self._sender_endpoints: list[tuple[str, str, int]] = []
        self._compute_consumer = "dbdisk"
        self._psrdada_paths = {
            "dada_db": "dada_db",
            "dada_dbdisk": "dada_dbdisk",
            "dada_dbnull": "dada_dbnull",
        }

    def _ssh(
        self,
        host: str,
        remote_argv: Sequence[str],
        stage: str,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        return self.transport.run(
            build_ssh_argv(host, remote_argv, str(self.known_hosts)),
            stage,
            check,
        )

    def _scp_argv(self, source: str, target: str, recursive: bool = False) -> list[str]:
        argv = [
            "scp",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=10",
            "-o",
            f"UserKnownHostsFile={self.known_hosts}",
            "-o",
            "StrictHostKeyChecking=yes",
        ]
        if recursive:
            argv.append("-r")
        argv += [source, target]
        return argv

    def _bootstrap_host(self, host: str) -> None:
        argv = [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=10",
            "-o",
            f"UserKnownHostsFile={self.known_hosts}",
            "-o",
            "StrictHostKeyChecking=accept-new",
            host,
            "--",
            "true",
        ]
        self.transport.run(argv, "PREPARE", True)

    def prepare(self, plan: RatePlan, run_dir: pathlib.Path) -> dict[str, Any]:
        self.local_run_dir = run_dir
        identity = re.sub(
            r"[^A-Za-z0-9_.-]+", "-", f"{run_dir.parent.name}-{run_dir.name}"
        ).strip("-.")
        if not identity:
            raise StageError(
                "PREPARE", ["derive", "run-identity"], 1, str(run_dir), "empty run ID"
            )
        self.remote_run_dir = f"/tmp/task8c-{identity}"
        self.known_hosts = self.known_hosts.with_name(
            f"{self.known_hosts.name}.{identity}"
        )
        self.known_hosts.parent.mkdir(parents=True, exist_ok=True)
        if self.known_hosts.exists():
            self.known_hosts.unlink()
        for host in ("qths1", "qtpulsar1", "qtpulsar2"):
            self._bootstrap_host(host)
        start = self._future_start_utc("PREPARE")
        self.preparation = {
            "start_utc": start,
            "remote_run_dir": self.remote_run_dir,
        }
        return dict(self.preparation)

    def _write_bundle(self, plan: RatePlan, start_utc: str) -> pathlib.Path:
        bundle_root = self.local_run_dir / "bundle"
        qths_root = bundle_root / "qths"
        qths_root.mkdir(parents=True, exist_ok=True)
        for name, content in build_qths_bundle(
            plan,
            self.remote_run_dir,
            self._psrdada_paths["dada_db"],
            self._psrdada_paths["dada_dbdisk"],
            self._psrdada_paths["dada_dbnull"],
            str(self.qths_binary_dir),
        ).items():
            path = qths_root / name
            if isinstance(content, bytes):
                path.write_bytes(content)
            else:
                path.write_text(content)
            if path.suffix == ".sh":
                path.chmod(0o755)
        sender_a_port, sender_b_port = derive_sender_source_ports(
            self.remote_run_dir
        )
        stations = plan.source.get("observation", {}).get("station_ids", [])
        if len(stations) != 2:
            raise StageError(
                "CONFIG_READY", ["validate", "station_ids"], 1,
                json.dumps(stations),
                "Task 8C topology requires exactly two Stations",
            )
        sender_specs = (
            (stations[0], "qtpulsar1", "174.0.1.100", sender_a_port, "sender101.json"),
            (stations[1], "qtpulsar2", "174.0.1.101", sender_b_port, "sender102.json"),
        )
        self._sender_endpoints = []
        for station, host, source_ip, source_port, name in sender_specs:
            self._sender_endpoints.append((host, source_ip, source_port))
            (bundle_root / name).write_text(
                json.dumps(
                    build_sender_config(
                        plan, station, source_ip, source_port, start_utc
                    ),
                    indent=2,
                )
                + "\n"
            )
        (bundle_root / "probe_sender_endpoint.py").write_text(
            build_sender_endpoint_probe()
        )
        return bundle_root

    def _preflight_sender_endpoint(
        self, host: str, source_ip: str, source_port: int
    ) -> None:
        argv = [
            "/usr/bin/python3",
            f"{self.remote_run_dir}/probe_sender_endpoint.py",
            source_ip,
            str(source_port),
        ]
        completed = self._ssh(host, argv, "CONFIG_READY", check=False)
        if completed.returncode != 0:
            raise StageError(
                "CONFIG_READY",
                build_ssh_argv(host, argv, str(self.known_hosts)),
                completed.returncode,
                completed.stdout,
                completed.stderr,
                "ENV_BLOCKED",
            )

    def prepare_configs(
        self,
        plan: RatePlan,
        run_dir: pathlib.Path,
        preparation: dict[str, Any],
    ) -> dict[str, Any]:
        self.local_run_dir = run_dir
        self.remote_run_dir = str(preparation["remote_run_dir"])
        required_psrdada_tools = ["dada_db"]
        required_psrdada_tools.append(
            "dada_dbnull" if plan.compute_consumer == "dbnull" else "dada_dbdisk"
        )
        for tool in required_psrdada_tools:
            self._psrdada_paths[tool] = self._resolve_psrdada_tool(tool)
        bundle_root = self._write_bundle(plan, str(preparation["start_utc"]))
        for host in ("qths1", "qtpulsar1", "qtpulsar2"):
            self._ssh(host, ["mkdir", "-p", self.remote_run_dir], "CONFIG_READY")
        probe_hashes: dict[str, str] = {}
        probe_path = bundle_root / "probe_sender_endpoint.py"
        local_probe_sha = self.transport.run(
            ["sha256sum", str(probe_path)], "CONFIG_READY", True
        ).stdout.split()[0]
        for host, source_ip, source_port in self._sender_endpoints:
            remote_probe = f"{self.remote_run_dir}/probe_sender_endpoint.py"
            self.transport.run(
                self._scp_argv(str(probe_path), f"{host}:{remote_probe}"),
                "CONFIG_READY",
                True,
            )
            remote_probe_sha = self._ssh(
                host, ["sha256sum", remote_probe], "CONFIG_READY"
            ).stdout.split()[0]
            if local_probe_sha != remote_probe_sha:
                raise StageError(
                    "CONFIG_READY",
                    ["sha256sum", host, remote_probe],
                    1,
                    f"local={local_probe_sha} remote={remote_probe_sha}",
                    "sender endpoint probe SHA mismatch",
                )
            probe_hashes[f"{host}:probe_sender_endpoint.py"] = local_probe_sha
            self._preflight_sender_endpoint(host, source_ip, source_port)
        qths_files = sorted((bundle_root / "qths").iterdir())
        qths_hashes: dict[str, str] = {}
        for path in qths_files:
            self.transport.run(
                self._scp_argv(str(path), f"qths1:{self.remote_run_dir}/{path.name}"),
                "CONFIG_READY",
                True,
            )
            local_sha = self.transport.run(
                ["sha256sum", str(path)], "CONFIG_READY", True
            ).stdout.split()[0]
            remote_sha = self._ssh(
                "qths1",
                ["sha256sum", f"{self.remote_run_dir}/{path.name}"],
                "CONFIG_READY",
            ).stdout.split()[0]
            if local_sha != remote_sha:
                raise StageError(
                    "CONFIG_READY",
                    ["sha256sum", "qths1", path.name],
                    1,
                    f"local={local_sha} remote={remote_sha}",
                    "qths config SHA mismatch",
                )
            qths_hashes[f"qths1:{path.name}"] = local_sha
        hashes = dict(qths_hashes)
        hashes.update(probe_hashes)
        hashes.update(self._transfer_sender_configs(bundle_root))
        binaries = (
            ("qths1", "rdma2dada", self.qths_binary_dir / "rdma2dada"),
            ("qths1", "vdif_unpack_worker", self.qths_binary_dir / "vdif_unpack_worker"),
            ("qtpulsar1", "fpga_sender_sim", self.sender_binary_dir / "fpga_sender_sim"),
            ("qtpulsar2", "fpga_sender_sim", self.sender_binary_dir / "fpga_sender_sim"),
        )
        binary_hashes = {}
        binary_paths = {}
        for host, name, binary_path in binaries:
            path = str(binary_path)
            output = self._ssh(
                host, ["sha256sum", path], "CONFIG_READY"
            ).stdout.split()
            if not output:
                raise StageError(
                    "CONFIG_READY",
                    ["sha256sum", host, path],
                    1,
                    "",
                    "missing binary SHA256",
                )
            binary_hashes[f"{host}:{name}"] = output[0]
            binary_paths[f"{host}:{name}"] = path
        for tool in required_psrdada_tools:
            tool_path = self._psrdada_paths[tool]
            output = self._ssh(
                "qths1", ["sha256sum", tool_path], "CONFIG_READY"
            ).stdout.split()
            if not output:
                raise StageError(
                    "CONFIG_READY",
                    ["sha256sum", tool_path],
                    1,
                    "",
                    f"missing {tool} SHA256",
                )
            binary_hashes[f"qths1:{tool}"] = output[0]
        preflight_argv = [
            str(self.qths_binary_dir / "rdma2dada"),
            "--plan",
            f"{self.remote_run_dir}/resolved_observation.json",
            "--preflight-only",
        ]
        receiver_preflight = self._ssh(
            "qths1", preflight_argv, "CONFIG_READY"
        )
        return {
            "config_sha": hashes,
            "binary_sha": binary_hashes,
            "binary_path": binary_paths,
            "source_ports": [item[2] for item in self._sender_endpoints],
            "sender_endpoints": [
                f"{item[1]}:{item[2]}" for item in self._sender_endpoints
            ],
            "config_id": plan.config_id,
            "geometry_id": plan.geometry_id,
            "receiver_preflight": receiver_preflight.stdout,
        }

    def _resolve_psrdada_tool(self, tool: str) -> str:
        discovered = self._ssh(
            "qths1", ["which", tool], "CONFIG_READY", check=False
        )
        if discovered.returncode == 0 and discovered.stdout.strip():
            return discovered.stdout.strip().splitlines()[0]
        candidates = (
            f"/home/user/psrdada/bin/{tool}",
            f"/usr/local/psrdada/bin/{tool}",
            f"/home/user/psrdada/apps/.libs/{tool}",
        )
        for candidate in candidates:
            executable = self._ssh(
                "qths1", ["test", "-x", candidate], "CONFIG_READY", check=False
            )
            if executable.returncode == 0:
                return candidate
        raise StageError(
            "CONFIG_READY",
            ["which", tool],
            127,
            discovered.stdout,
            f"{tool} was not found in PATH or a preflighted install path",
            "ENV_BLOCKED",
        )

    def _future_start_utc(self, stage: str) -> str:
        epoch_text = self._ssh(
            "qths1", ["date", "-u", "+%s"], stage
        ).stdout.strip()
        try:
            epoch = int(epoch_text)
        except ValueError as error:
            raise StageError(
                stage,
                ["date", "-u", "+%s"],
                1,
                epoch_text,
                "invalid remote UTC epoch",
                "ENV_BLOCKED",
            ) from error
        return dt.datetime.fromtimestamp(
            epoch + 180, tz=dt.timezone.utc
        ).strftime("%Y-%m-%d-%H:%M:%S")

    def _transfer_sender_configs(self, bundle_root: pathlib.Path) -> dict[str, str]:
        sender_targets = (
            (bundle_root / "sender101.json", "qtpulsar1", "101.json"),
            (bundle_root / "sender102.json", "qtpulsar2", "102.json"),
        )
        hashes: dict[str, str] = {}
        for path, host, remote_name in sender_targets:
            self.transport.run(
                self._scp_argv(str(path), f"{host}:{self.remote_run_dir}/{remote_name}"),
                "CONFIG_READY",
                True,
            )
            local_sha = self.transport.run(
                ["sha256sum", str(path)], "CONFIG_READY", True
            ).stdout.split()[0]
            remote_sha = self._ssh(
                host,
                ["sha256sum", f"{self.remote_run_dir}/{remote_name}"],
                "CONFIG_READY",
            ).stdout.split()[0]
            if local_sha != remote_sha:
                raise StageError(
                    "CONFIG_READY",
                    ["sha256sum", host, remote_name],
                    1,
                    f"local={local_sha} remote={remote_sha}",
                    "sender config SHA mismatch",
                )
            hashes[f"{host}:{remote_name}"] = local_sha
        return hashes

    def start_pipeline(self, plan: RatePlan, run_dir: pathlib.Path) -> dict[str, Any]:
        self._compute_consumer = plan.compute_consumer
        try:
            self._ssh(
                "qths1",
                ["bash", f"{self.remote_run_dir}/prepare.sh"],
                "RINGS_READY",
            )
        except StageError as error:
            raise StageError(
                error.stage,
                error.argv,
                error.exit_code,
                error.stdout,
                error.stderr,
                "ENV_BLOCKED",
            ) from error
        self._rings_acquired = [plan.raw_key, plan.compute_key]
        binary = str(self.qths_binary_dir / "rdma2dada")
        try:
            self._ssh(
                "qths1",
                ["sudo", "-n", "setcap", "cap_net_raw+ep", binary],
                "PIPELINE_READY",
            )
        except StageError as error:
            raise StageError(
                error.stage,
                error.argv,
                error.exit_code,
                error.stdout,
                error.stderr,
                "ENV_BLOCKED",
            ) from error
        self._capability_added = True
        self._ssh(
            "qths1",
            ["touch", f"{self.remote_run_dir}/capability.added"],
            "PIPELINE_READY",
        )
        try:
            self._ssh(
                "qths1",
                ["bash", f"{self.remote_run_dir}/start.sh"],
                "PIPELINE_READY",
            )
        except StageError as error:
            raise StageError(
                error.stage,
                error.argv,
                error.exit_code,
                error.stdout,
                error.stderr,
                "PRODUCT_FAIL",
            ) from error
        return {
            "rings": [plan.raw_key, plan.compute_key],
            "capability_added": True,
        }

    def _remaining_start_margin(self) -> float:
        now_text = self._ssh(
            "qths1", ["date", "-u", "+%s"], "SENDERS_WAITING"
        ).stdout.strip()
        start = dt.datetime.strptime(
            str(self.preparation["start_utc"]), "%Y-%m-%d-%H:%M:%S"
        ).replace(tzinfo=dt.timezone.utc)
        return start.timestamp() - float(now_text)

    def _refresh_sender_start(self, plan: RatePlan) -> None:
        start = self._future_start_utc("SENDERS_WAITING")
        self.preparation["start_utc"] = start
        bundle_root = self._write_bundle(plan, start)
        self._transfer_sender_configs(bundle_root)

    def start_senders(self, plan: RatePlan, run_dir: pathlib.Path) -> list[Any]:
        if self._remaining_start_margin() < 90.0:
            self._refresh_sender_start(plan)
            if self._remaining_start_margin() < 90.0:
                raise StageError(
                    "SENDERS_WAITING",
                    ["start_utc", "margin"],
                    1,
                    str(self.preparation),
                    "refreshed start_utc margin is below 90 seconds",
                    "ENV_BLOCKED",
                )
        binary = str(self.sender_binary_dir / "fpga_sender_sim")
        commands = (
            (
                "qtpulsar1",
                f"{self.remote_run_dir}/101.json",
                run_dir / "sender101.log",
            ),
            (
                "qtpulsar2",
                f"{self.remote_run_dir}/102.json",
                run_dir / "sender102.log",
            ),
        )
        started: list[Any] = []
        for host, config, log in commands:
            argv = build_ssh_argv(
                host, [binary, config], str(self.known_hosts)
            )
            try:
                process = self.transport.start(argv, log)
            except Exception as error:
                raise StageError(
                    "SENDERS_WAITING",
                    argv,
                    1,
                    "",
                    repr(error),
                    "HARNESS_FAIL",
                ) from error
            started.append(process)
            self._sender_processes.append(process)
        return started

    def wait_senders(self, plan: RatePlan, processes: Sequence[Any]) -> list[str]:
        outputs = [""] * len(processes)
        pending = set(range(len(processes)))
        deadline = time.monotonic() + plan.duration_seconds + 300.0

        def terminate_running() -> None:
            for peer in processes:
                try:
                    if peer.poll() is None:
                        peer.terminate()
                except Exception:
                    pass

        def read_output(process: Any) -> str:
            stream = getattr(process, "_task8c_stream", None)
            if stream:
                stream.flush()
                stream.close()
            output_path = getattr(process, "_task8c_output_path", None)
            if output_path:
                return output_path.read_text()
            return str(getattr(process, "output", ""))

        while pending:
            completed_any = False
            for index in list(pending):
                process = processes[index]
                returncode = process.poll()
                if returncode is None:
                    continue
                completed_any = True
                pending.remove(index)
                output = read_output(process)
                outputs[index] = output
                if returncode != 0:
                    terminate_running()
                    classification = (
                        "ENV_BLOCKED"
                        if "bind source endpoint:" in output
                        else "PRODUCT_FAIL"
                    )
                    raise StageError(
                        "SENDERS_RUNNING",
                        ["fpga_sender_sim", str(index)],
                        int(returncode),
                        output,
                        "sender exited non-zero; aborting all Station streams",
                        classification,
                    )
            if not pending:
                break
            if time.monotonic() >= deadline:
                terminate_running()
                raise StageError(
                    "SENDERS_RUNNING",
                    ["fpga_sender_sim", "all"],
                    124,
                    "",
                    "sender group timed out; aborting all Station streams",
                    "PRODUCT_FAIL",
                )
            if not completed_any:
                time.sleep(0.02)
        return outputs

    def collect(self, plan: RatePlan, run_dir: pathlib.Path) -> dict[str, Any]:
        self._ssh(
            "qths1",
            ["bash", f"{self.remote_run_dir}/finish.sh"],
            "COLLECTING",
        )
        self._ssh(
            "qths1",
            ["python3", f"{self.remote_run_dir}/summarize.py"],
            "COLLECTING",
        )
        qths_copy = run_dir / "qths"
        qths_copy.mkdir(exist_ok=True)
        artifact_names = [
            "receiver.log",
            "worker.log",
            "reader.log",
            "compute-summary.json",
            "MANIFEST.sha256",
            "resolved_observation.json",
            "ring_plan.json",
            "raw.header",
            "unpacked.header",
            "validation_report.json",
        ]
        if plan.compute_consumer == "dbnull":
            artifact_names.append("reader.exit")
        for name in artifact_names:
            self.transport.run(
                self._scp_argv(
                    f"qths1:{self.remote_run_dir}/{name}", str(qths_copy / name)
                ),
                "COLLECTING",
                True,
            )
        return parse_qths_statistics(
            (qths_copy / "receiver.log").read_text(),
            (qths_copy / "worker.log").read_text(),
            json.loads((qths_copy / "compute-summary.json").read_text()),
        )

    def cleanup(self, resources: RunResources, run_dir: pathlib.Path) -> dict[str, Any]:
        errors: list[str] = []
        diagnostic_errors: list[str] = []

        def nonzero_diagnostic(
            completed: subprocess.CompletedProcess[str],
        ) -> str | None:
            if completed.returncode != 0:
                return json.dumps(
                    {
                        "argv": list(completed.args),
                        "exit_code": completed.returncode,
                        "stdout": completed.stdout,
                        "stderr": completed.stderr,
                    },
                    sort_keys=True,
                )
            return None

        def record_nonzero(completed: subprocess.CompletedProcess[str]) -> None:
            diagnostic = nonzero_diagnostic(completed)
            if diagnostic is not None:
                errors.append(diagnostic)

        owned_processes: list[Any] = []
        for process in list(resources.processes) + self._sender_processes:
            if not any(existing is process for existing in owned_processes):
                owned_processes.append(process)
        for process in owned_processes:
            try:
                if process.poll() is None:
                    process.terminate()
            except Exception as error:
                errors.append(repr(error))
        owned_rings = list(dict.fromkeys(resources.rings + self._rings_acquired))
        capability_added = resources.capability_added or self._capability_added
        has_runtime_resources = bool(
            owned_processes or owned_rings or capability_added
        )
        if self.remote_run_dir and has_runtime_resources:
            diagnostic_root = run_dir / "qths-cleanup"
            diagnostic_root.mkdir(exist_ok=True)
            diagnostic_names = [
                "receiver.log",
                "worker.log",
                "reader.log",
                "raw-ring.log",
                "compute-ring.log",
                "compute-summary.json",
            ]
            if self._compute_consumer == "dbnull":
                diagnostic_names.append("reader.exit")
            for name in diagnostic_names:
                try:
                    completed = self.transport.run(
                        self._scp_argv(
                            f"qths1:{self.remote_run_dir}/{name}",
                            str(diagnostic_root / name),
                        ),
                        "CLEANUP",
                        check=False,
                    )
                    diagnostic = nonzero_diagnostic(completed)
                    if diagnostic is not None:
                        diagnostic_errors.append(diagnostic)
                except Exception as error:
                    diagnostic_errors.append(repr(error))
        if self.remote_run_dir and (owned_rings or capability_added):
            try:
                completed = self._ssh(
                    "qths1",
                    ["bash", f"{self.remote_run_dir}/cleanup.sh"],
                    "CLEANUP",
                    check=False,
                )
                record_nonzero(completed)
            except Exception as error:
                errors.append(repr(error))
        if self.remote_run_dir:
            try:
                completed = self._ssh(
                    "qths1",
                    ["rm", "-rf", self.remote_run_dir],
                    "CLEANUP",
                    check=False,
                )
                record_nonzero(completed)
            except Exception as error:
                errors.append(repr(error))
        for host in ("qtpulsar1", "qtpulsar2"):
            if self.remote_run_dir:
                try:
                    completed = self._ssh(
                        host,
                        ["rm", "-rf", self.remote_run_dir],
                        "CLEANUP",
                        check=False,
                    )
                    record_nonzero(completed)
                except Exception as error:
                    errors.append(repr(error))
        try:
            if self.known_hosts.exists():
                self.known_hosts.unlink()
        except Exception as error:
            errors.append(repr(error))
        return {
            "rings_destroyed": owned_rings,
            "capability_removed": capability_added,
            "errors": errors,
            "diagnostic_errors": diagnostic_errors,
            "CLEANUP_RESULT": "PASS" if not errors else "FAIL",
        }


@dataclasses.dataclass
class RunResources:
    processes: list[Any] = dataclasses.field(default_factory=list)
    rings: list[str] = dataclasses.field(default_factory=list)
    capability_added: bool = False


def _parse_sender_summary(output: str) -> dict[str, Any]:
    for line in reversed(output.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                return value
    raise StageError(
        "SENDERS_RUNNING",
        ["fpga_sender_sim"],
        1,
        output,
        "missing sender JSON",
        "PRODUCT_FAIL",
    )


def _validate_sender(
    summary: dict[str, Any], target: float, expected_station: int
) -> None:
    scheduled = int(summary.get("scheduled_packets", -1))
    sent = int(summary.get("sent_packets", -2))
    failed = int(summary.get("failed_packets", -1))
    backend = summary.get("backend")
    station_id = int(summary.get("station_id", -1))
    payload_prefix = str(summary.get("payload_prefix_hex", ""))
    actual = float(summary.get("actual_payload_gbps", -1.0))
    error = abs(actual - target) / target if target else float("inf")
    if (
        scheduled != sent
        or failed != 0
        or backend != "SENDMMSG"
        or station_id != expected_station
        or re.fullmatch(r"[0-9a-f]{8}", payload_prefix) is None
        or error > 0.02
    ):
        raise StageError(
            "SENDERS_RUNNING",
            ["fpga_sender_sim"],
            1,
            json.dumps(summary, sort_keys=True),
            f"sender acceptance failed; relative_rate_error={error:.6f}",
            "PRODUCT_FAIL",
        )


def _validate_statistics(
    statistics: dict[str, Any], plan: RatePlan, expected_sample_prefix: str
) -> None:
    expected_records = plan.group_count * plan.nant
    expected_data_bytes = plan.group_count * plan.payload_bytes * plan.nant
    required = {
        ("receiver", "accepted"): expected_records,
        ("receiver", "published"): expected_records,
        ("receiver", "wrong_length"): 0,
        ("receiver", "cq_errors"): 0,
        ("unpack", "records"): expected_records,
        ("unpack", "accepted"): expected_records,
        ("unpack", "bad_header"): 0,
        ("unpack", "invalid_data"): 0,
        ("unpack", "unknown_station"): 0,
        ("unpack", "duplicate"): 0,
        ("unpack", "late"): 0,
        ("unpack", "out_of_range"): 0,
        ("unpack", "complete_groups"): plan.group_count,
        ("unpack", "incomplete_groups"): 0,
        ("unpack", "fully_missing_groups"): 0,
        ("unpack", "missing_station"): 0,
    }
    if plan.compute_consumer == "dbnull":
        required.update(
            {
                ("compute", "consumer"): "dada_dbnull",
                ("compute", "exit_code"): 0,
                ("compute", "zero_copy"): True,
                ("compute", "single_transfer"): True,
            }
        )
    else:
        required.update(
            {
                ("compute", "data_bytes"): expected_data_bytes,
                ("compute", "DATA_STAGE"): "UNPACKED",
                ("compute", "ORDER"): "ATFP",
                ("compute", "NANT"): plan.nant,
                ("compute", "NCHAN"): plan.nchan,
                ("compute", "NPOL"): plan.npol,
                ("compute", "CONFIG_ID"): plan.config_id,
                ("compute", "GEOMETRY_ID"): plan.geometry_id,
                ("compute", "sample_prefix_hex"): expected_sample_prefix,
            }
        )
    mismatches = []
    for (section, key), expected in required.items():
        actual = statistics.get(section, {}).get(key)
        if actual != expected:
            mismatches.append(
                {"field": f"{section}.{key}", "expected": expected, "actual": actual}
            )
    if mismatches:
        raise StageError(
            "COLLECTING",
            ["validate", "pipeline-statistics"],
            1,
            json.dumps(statistics, sort_keys=True),
            json.dumps(mismatches, sort_keys=True),
            "PRODUCT_FAIL",
        )


def parse_qths_statistics(
    receiver_log: str,
    worker_log: str,
    compute_summary: dict[str, Any],
) -> dict[str, Any]:
    receiver_match = re.search(
        r"Receive summary:\s*accepted=(\d+),\s*wrong_length=(\d+),\s*"
        r"published=(\d+),\s*blocks=(\d+),\s*partial_blocks=(\d+),\s*"
        r"cq_tail_records=(\d+)",
        receiver_log,
    )
    if not receiver_match:
        raise StageError(
            "COLLECTING",
            ["parse", "receiver.log"],
            1,
            receiver_log,
            "missing receiver summary",
        )
    unpack_match = re.search(
        r"VDIF unpack statistics:\s*records=(\d+)\s+accepted=(\d+)\s+"
        r"bad_header=(\d+)\s+invalid_data=(\d+)\s+unknown_station=(\d+)\s+"
        r"duplicate=(\d+)\s+late=(\d+)\s+out_of_range=(\d+)\s+"
        r"complete_groups=(\d+)\s+incomplete_groups=(\d+)\s+"
        r"fully_missing_groups=(\d+)\s+missing_station=(\d+)/(\d+)",
        worker_log,
    )
    if not unpack_match or "VDIF unpack transfer completed" not in worker_log:
        raise StageError(
            "COLLECTING",
            ["parse", "worker.log"],
            1,
            worker_log,
            "missing unpack summary or completed marker",
        )
    unpack_names = (
        "records",
        "accepted",
        "bad_header",
        "invalid_data",
        "unknown_station",
        "duplicate",
        "late",
        "out_of_range",
        "complete_groups",
        "incomplete_groups",
        "fully_missing_groups",
        "missing_station",
        "expected_station",
    )
    if compute_summary.get("consumer") == "dada_dbnull":
        try:
            compute = {
                "consumer": "dada_dbnull",
                "exit_code": int(compute_summary["exit_code"]),
                "zero_copy": compute_summary["zero_copy"] is True,
                "single_transfer": compute_summary["single_transfer"] is True,
            }
        except (KeyError, TypeError, ValueError) as error:
            raise StageError(
                "COLLECTING",
                ["parse", "compute-summary.json"],
                1,
                json.dumps(compute_summary, sort_keys=True),
                repr(error),
            ) from error
    else:
        header = compute_summary.get("header", {})
        try:
            compute = {
                "data_bytes": int(compute_summary["data_bytes"]),
                "DATA_STAGE": header["DATA_STAGE"],
                "ORDER": header["ORDER"],
                "NANT": int(header["NANT"]),
                "NCHAN": int(header["NCHAN"]),
                "NPOL": int(header["NPOL"]),
                "CONFIG_ID": header["CONFIG_ID"],
                "GEOMETRY_ID": header["GEOMETRY_ID"],
                "sample_hex": str(compute_summary.get("sample_hex", "")),
                "sample_prefix_hex": str(compute_summary.get("sample_hex", ""))[:8],
            }
        except (KeyError, TypeError, ValueError) as error:
            raise StageError(
                "COLLECTING",
                ["parse", "compute-summary.json"],
                1,
                json.dumps(compute_summary, sort_keys=True),
                repr(error),
            ) from error
    cq_patterns = (
        "CQ status error",
        "invalid WR ID",
        "unexpected opcode",
        "repost failed",
    )
    return {
        "receiver": {
            "accepted": int(receiver_match.group(1)),
            "wrong_length": int(receiver_match.group(2)),
            "published": int(receiver_match.group(3)),
            "blocks": int(receiver_match.group(4)),
            "partial_blocks": int(receiver_match.group(5)),
            "cq_tail_records": int(receiver_match.group(6)),
            "cq_errors": sum(receiver_log.count(pattern) for pattern in cq_patterns),
        },
        "unpack": {
            name: int(value)
            for name, value in zip(unpack_names, unpack_match.groups())
            if name != "expected_station"
        },
        "compute": compute,
    }


class RatePointController:
    def __init__(
        self,
        backend: Any,
        result_root: pathlib.Path,
        run_id: str | None = None,
    ) -> None:
        self.backend = backend
        self.result_root = pathlib.Path(result_root)
        self.run_id = run_id or (
            time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
            + f"-{uuid.uuid4().hex[:8]}"
        )
        self.run_dir = self.result_root / self.run_id

    def _state(self, state: str, **extra: Any) -> None:
        _atomic_json(self.run_dir / "state.json", {"state": state, **extra})

    def _manifest(self, plan: RatePlan) -> dict[str, Any]:
        controller_path = pathlib.Path(__file__).resolve()
        return {
            "run_id": self.run_id,
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "hosts": ["HF", "qths1", "qtpulsar1", "qtpulsar2"],
            "plan": plan.as_dict(),
            "controller": str(controller_path),
            "controller_sha256": hashlib.sha256(controller_path.read_bytes()).hexdigest(),
            "git_commit": None,
            "git_required_on_test_host": False,
            "python_version": sys.version,
        }

    def _compile_failure_result(
        self,
        request: RateRequest,
        error: StageError | Exception,
        mode: str | None = None,
    ) -> dict[str, Any]:
        failure = (
            error
            if isinstance(error, StageError)
            else StageError(
                "CONFIG_READY",
                ["observation_config_compile"],
                1,
                "",
                repr(error),
                "HARNESS_FAIL",
            )
        )
        try:
            cleanup = self.backend.cleanup(RunResources(), self.run_dir)
        except Exception as cleanup_error:
            cleanup = {
                "CLEANUP_RESULT": "FAIL",
                "rings_destroyed": [],
                "capability_removed": False,
                "errors": [repr(cleanup_error)],
            }
        result = {
            "TEST_RESULT": failure.classification,
            "run_id": self.run_id,
            "request": dataclasses.asdict(request),
            "failure": failure.as_dict(),
            "cleanup": cleanup,
        }
        if mode is not None:
            result["mode"] = mode
        _atomic_json(self.run_dir / "result.json", result)
        state = {"test_result": failure.classification}
        if mode is not None:
            state["mode"] = mode
        self._state("CLEANED", **state)
        return result

    def run(self, plan: RatePlan) -> dict[str, Any]:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        _atomic_json(self.run_dir / "manifest.json", self._manifest(plan))
        resources = RunResources()
        result: dict[str, Any] = {
            "TEST_RESULT": "HARNESS_FAIL",
            "run_id": self.run_id,
            "plan": plan.as_dict(),
        }
        try:
            self._state("PREPARE")
            preparation = self.backend.prepare(plan, self.run_dir)
            self._state("CONFIG_READY", **preparation)
            config = self.backend.prepare_configs(plan, self.run_dir, preparation)
            manifest = json.loads((self.run_dir / "manifest.json").read_text())
            manifest.update({"preparation": preparation, "config": config})
            _atomic_json(self.run_dir / "manifest.json", manifest)
            self._state("RINGS_READY")
            acquired = self.backend.start_pipeline(plan, self.run_dir)
            resources.rings = list(acquired.get("rings", []))
            resources.capability_added = bool(acquired.get("capability_added", False))
            self._state("PIPELINE_READY", **config)
            processes = self.backend.start_senders(plan, self.run_dir)
            resources.processes = list(processes)
            self._state("SENDERS_WAITING")
            sender_outputs = self.backend.wait_senders(plan, processes)
            self._state("SENDERS_RUNNING")
            summaries = [_parse_sender_summary(output) for output in sender_outputs]
            stations = plan.source["observation"]["station_ids"]
            if len(summaries) != len(stations):
                raise StageError(
                    "SENDERS_RUNNING",
                    ["validate", "sender-count"],
                    1,
                    str(len(summaries)),
                    str(len(stations)),
                    "PRODUCT_FAIL",
                )
            for summary, station in zip(summaries, stations):
                _validate_sender(summary, plan.per_station_gbps, int(station))
            self._state("COLLECTING")
            statistics = self.backend.collect(plan, self.run_dir)
            _validate_statistics(
                statistics, plan, str(summaries[0]["payload_prefix_hex"])
            )
            result.update(
                {
                    "TEST_RESULT": "PASS",
                    "senders": summaries,
                    "statistics": statistics,
                    "preparation": preparation,
                }
            )
            self._state("PASS", test_result="PASS")
        except StageError as error:
            result["TEST_RESULT"] = error.classification
            result["failure"] = error.as_dict()
            self._state(
                "FAIL",
                test_result=error.classification,
                failure=error.as_dict(),
            )
        except Exception as error:  # retain unexpected diagnostics as data
            failure = {
                "stage": "INTERNAL",
                "argv": [],
                "exit_code": 1,
                "stdout": "",
                "stderr": repr(error),
            }
            result["failure"] = failure
            self._state("FAIL", test_result="HARNESS_FAIL", failure=failure)
        finally:
            try:
                cleanup = self.backend.cleanup(resources, self.run_dir)
            except Exception as error:
                cleanup = {
                    "CLEANUP_RESULT": "FAIL",
                    "rings_destroyed": [],
                    "capability_removed": False,
                    "errors": [repr(error)],
                }
            result["cleanup"] = cleanup
            _atomic_json(self.run_dir / "result.json", result)
            self._state("CLEANED", test_result=result["TEST_RESULT"])
        return result

    def preflight(self, plan: RatePlan) -> dict[str, Any]:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        _atomic_json(self.run_dir / "manifest.json", self._manifest(plan))
        result: dict[str, Any] = {
            "TEST_RESULT": "HARNESS_FAIL",
            "run_id": self.run_id,
            "mode": "preflight-only",
            "plan": plan.as_dict(),
        }
        try:
            self._state("PREPARE", mode="preflight-only")
            preparation = self.backend.prepare(plan, self.run_dir)
            self._state("CONFIG_READY", mode="preflight-only", **preparation)
            config = self.backend.prepare_configs(plan, self.run_dir, preparation)
            manifest = json.loads((self.run_dir / "manifest.json").read_text())
            manifest.update({"preparation": preparation, "config": config})
            _atomic_json(self.run_dir / "manifest.json", manifest)
            result.update(
                {
                    "TEST_RESULT": "PASS",
                    "preparation": preparation,
                    "config": config,
                }
            )
            self._state("PASS", mode="preflight-only", test_result="PASS")
        except StageError as error:
            result["TEST_RESULT"] = error.classification
            result["failure"] = error.as_dict()
            self._state(
                "FAIL",
                mode="preflight-only",
                test_result=error.classification,
                failure=error.as_dict(),
            )
        except Exception as error:
            failure = {
                "stage": "INTERNAL",
                "argv": [],
                "exit_code": 1,
                "stdout": "",
                "stderr": repr(error),
            }
            result["failure"] = failure
            self._state(
                "FAIL",
                mode="preflight-only",
                test_result="HARNESS_FAIL",
                failure=failure,
            )
        finally:
            try:
                cleanup = self.backend.cleanup(RunResources(), self.run_dir)
            except Exception as error:
                cleanup = {
                    "CLEANUP_RESULT": "FAIL",
                    "rings_destroyed": [],
                    "capability_removed": False,
                    "errors": [repr(error)],
                }
            result["cleanup"] = cleanup
            _atomic_json(self.run_dir / "result.json", result)
            self._state(
                "CLEANED",
                mode="preflight-only",
                test_result=result["TEST_RESULT"],
            )
        return result

    def run_request(
        self,
        request: RateRequest,
        observation_template: pathlib.Path,
        compiler: pathlib.Path,
    ) -> dict[str, Any]:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self._state("CONFIG_PREFLIGHT")
        try:
            plan = compile_rate_plan(
                request, observation_template, compiler, self.run_dir
            )
        except StageError as error:
            return self._compile_failure_result(request, error)
        except Exception as error:
            return self._compile_failure_result(request, error)
        return self.run(plan)

    def preflight_request(
        self,
        request: RateRequest,
        observation_template: pathlib.Path,
        compiler: pathlib.Path,
    ) -> dict[str, Any]:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self._state("CONFIG_PREFLIGHT", mode="preflight-only")
        try:
            plan = compile_rate_plan(
                request, observation_template, compiler, self.run_dir
            )
        except StageError as error:
            return self._compile_failure_result(
                request, error, mode="preflight-only"
            )
        except Exception as error:
            return self._compile_failure_result(
                request, error, mode="preflight-only"
            )
        return self.preflight(plan)


def run_rate_sequence(
    plan: RatePlan,
    backend_factory: Any,
    result_root: pathlib.Path,
    warmup_runs: int,
    measured_runs: int,
    suite_id: str | None = None,
) -> dict[str, Any]:
    if warmup_runs < 0 or measured_runs <= 0:
        raise ValueError("warmup_runs must be non-negative and measured_runs positive")
    resolved_suite_id = suite_id or (
        time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        + f"-{uuid.uuid4().hex[:8]}"
    )
    suite_root = pathlib.Path(result_root) / resolved_suite_id
    suite_root.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, Any]] = []
    stop_sequence = False
    for role, count in (("warmup", warmup_runs), ("measured", measured_runs)):
        for index in range(1, count + 1):
            run_id = f"{role}-{index:02d}"
            result = RatePointController(
                backend_factory(), suite_root, run_id=run_id
            ).run(plan)
            runs.append(
                {
                    "role": role,
                    "index": index,
                    "run_id": run_id,
                    "TEST_RESULT": result["TEST_RESULT"],
                    "CLEANUP_RESULT": result.get("cleanup", {}).get(
                        "CLEANUP_RESULT", "FAIL"
                    ),
                    "result_path": f"{run_id}/result.json",
                }
            )
            if (
                result["TEST_RESULT"] != "PASS"
                or result.get("cleanup", {}).get("CLEANUP_RESULT") != "PASS"
            ):
                stop_sequence = True
                break
        if stop_sequence:
            break
    measured_results = [
        json.loads((suite_root / run["result_path"]).read_text())
        for run in runs
        if run["role"] == "measured"
    ]
    measured_rates = [
        sum(float(sender["actual_payload_gbps"]) for sender in result["senders"])
        for result in measured_results
        if result["TEST_RESULT"] == "PASS"
        and result.get("cleanup", {}).get("CLEANUP_RESULT") == "PASS"
    ]
    failures = [
        run
        for run in runs
        if run["TEST_RESULT"] != "PASS" or run["CLEANUP_RESULT"] != "PASS"
    ]
    aggregate: dict[str, float] | None = None
    if len(measured_rates) == measured_runs:
        aggregate = {
            "median": statistics.median(measured_rates),
            "minimum": min(measured_rates),
            "maximum": max(measured_rates),
            "spread": max(measured_rates) - min(measured_rates),
        }
    summary = {
        "TEST_RESULT": (
            failures[0]["TEST_RESULT"]
            if failures and failures[0]["TEST_RESULT"] != "PASS"
            else "HARNESS_FAIL"
            if failures
            else "PASS"
        ),
        "suite_id": resolved_suite_id,
        "plan": plan.as_dict(),
        "warmup_count": warmup_runs,
        "measured_count": measured_runs,
        "runs": runs,
        "actual_aggregate_gbps": aggregate,
    }
    _atomic_json(suite_root / "summary.json", summary)
    return summary


def run_rate_request_sequence(
    request: RateRequest,
    observation_template: pathlib.Path,
    compiler: pathlib.Path,
    backend_factory: Any,
    result_root: pathlib.Path,
    warmup_runs: int,
    measured_runs: int,
    suite_id: str | None = None,
) -> dict[str, Any]:
    if warmup_runs < 0 or measured_runs <= 0:
        raise ValueError("warmup_runs must be non-negative and measured_runs positive")
    resolved_suite_id = suite_id or (
        time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        + f"-{uuid.uuid4().hex[:8]}"
    )
    suite_root = pathlib.Path(result_root) / resolved_suite_id
    suite_root.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, Any]] = []
    for role, count in (("warmup", warmup_runs), ("measured", measured_runs)):
        for index in range(1, count + 1):
            run_id = f"{role}-{index:02d}"
            result = RatePointController(
                backend_factory(), suite_root, run_id=run_id
            ).run_request(request, observation_template, compiler)
            run = {
                "role": role,
                "index": index,
                "run_id": run_id,
                "TEST_RESULT": result["TEST_RESULT"],
                "CLEANUP_RESULT": result.get("cleanup", {}).get(
                    "CLEANUP_RESULT", "FAIL"
                ),
                "result_path": f"{run_id}/result.json",
            }
            runs.append(run)
            if run["TEST_RESULT"] != "PASS" or run["CLEANUP_RESULT"] != "PASS":
                break
        if runs and (
            runs[-1]["TEST_RESULT"] != "PASS"
            or runs[-1]["CLEANUP_RESULT"] != "PASS"
        ):
            break
    measured_rates = []
    for run in runs:
        if run["role"] != "measured" or run["TEST_RESULT"] != "PASS":
            continue
        result = json.loads((suite_root / run["result_path"]).read_text())
        measured_rates.append(
            sum(float(sender["actual_payload_gbps"]) for sender in result["senders"])
        )
    failures = [
        run for run in runs
        if run["TEST_RESULT"] != "PASS" or run["CLEANUP_RESULT"] != "PASS"
    ]
    aggregate = None
    if len(measured_rates) == measured_runs:
        aggregate = {
            "median": statistics.median(measured_rates),
            "minimum": min(measured_rates),
            "maximum": max(measured_rates),
            "spread": max(measured_rates) - min(measured_rates),
        }
    summary = {
        "TEST_RESULT": (
            failures[0]["TEST_RESULT"]
            if failures and failures[0]["TEST_RESULT"] != "PASS"
            else "HARNESS_FAIL"
            if failures
            else "PASS"
        ),
        "suite_id": resolved_suite_id,
        "request": dataclasses.asdict(request),
        "warmup_count": warmup_runs,
        "measured_count": measured_runs,
        "runs": runs,
        "actual_aggregate_gbps": aggregate,
    }
    _atomic_json(suite_root / "summary.json", summary)
    return summary


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aggregate-gbps", type=float, required=True)
    parser.add_argument("--duration-seconds", type=float, default=10.0)
    parser.add_argument(
        "--compute-consumer",
        choices=("dbdisk", "dbnull"),
        default="dbdisk",
    )
    parser.add_argument("--result-root", type=pathlib.Path, default=pathlib.Path("/tmp/task8c-results"))
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--preflight-only", action="store_true")
    mode.add_argument("--execute", action="store_true")
    parser.add_argument("--warmup-runs", type=int, default=0)
    parser.add_argument("--measured-runs", type=int, default=1)
    parser.add_argument("--suite-id")
    parser.add_argument(
        "--project-root",
        type=pathlib.Path,
        default=pathlib.Path("/home/user/wy/rdma_dada"),
    )
    parser.add_argument(
        "--known-hosts",
        type=pathlib.Path,
        default=pathlib.Path("/tmp/task8c-known-hosts"),
    )
    parser.add_argument(
        "--qths-binary-dir",
        type=pathlib.Path,
        help=(
            "required with --execute: qths1 directory containing the exact "
            "rdma2dada and vdif_unpack_worker build under test"
        ),
    )
    parser.add_argument(
        "--sender-binary-dir",
        type=pathlib.Path,
        help=(
            "required with --preflight-only or --execute: identical directory "
            "on qtpulsar1/2 containing the exact fpga_sender_sim build under test"
        ),
    )
    parser.add_argument(
        "--observation-config",
        type=pathlib.Path,
        help="required with --execute: unified observation JSON template",
    )
    parser.add_argument(
        "--config-compiler",
        type=pathlib.Path,
        help="required with --execute: observation_config_compile executable",
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    request = RateRequest(
        args.aggregate_gbps,
        args.duration_seconds,
        compute_consumer=args.compute_consumer,
    )
    if args.dry_run:
        request.validate()
        print(json.dumps(dataclasses.asdict(request), indent=2, sort_keys=True))
        return 0
    if args.qths_binary_dir is None or args.sender_binary_dir is None:
        print(
            "--qths-binary-dir and --sender-binary-dir are required with "
            "--preflight-only or --execute",
            file=sys.stderr,
        )
        return 2
    if args.observation_config is None or args.config_compiler is None:
        print(
            "--observation-config and --config-compiler are required with --execute",
            file=sys.stderr,
        )
        return 2
    backend = SshBackend(
        project_root=args.project_root,
        known_hosts=args.known_hosts,
        qths_binary_dir=args.qths_binary_dir,
        sender_binary_dir=args.sender_binary_dir,
    )
    if args.preflight_only:
        if args.warmup_runs != 0 or args.measured_runs != 1:
            print(
                "--preflight-only does not accept repetition counts",
                file=sys.stderr,
            )
            return 2
        result = RatePointController(backend, args.result_root).preflight_request(
            request, args.observation_config, args.config_compiler
        )
    elif args.warmup_runs == 0 and args.measured_runs == 1:
        result = RatePointController(backend, args.result_root).run_request(
            request, args.observation_config, args.config_compiler
        )
    else:
        result = run_rate_request_sequence(
            request,
            args.observation_config,
            args.config_compiler,
            lambda: SshBackend(
                project_root=args.project_root,
                known_hosts=args.known_hosts,
                qths_binary_dir=args.qths_binary_dir,
                sender_binary_dir=args.sender_binary_dir,
            ),
            args.result_root,
            args.warmup_runs,
            args.measured_runs,
            args.suite_id,
        )
    print(json.dumps(result, sort_keys=True), flush=True)
    cleanup_passed = (
        "cleanup" not in result
        or result["cleanup"].get("CLEANUP_RESULT") == "PASS"
    )
    return 0 if result["TEST_RESULT"] == "PASS" and cleanup_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
