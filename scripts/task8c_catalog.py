#!/usr/bin/env python3
"""Import, index, query, and promote compact Task 8C result suites."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import os
import pathlib
import re
import shutil
import tempfile
import uuid
from datetime import datetime, timezone
import sys


CSV_FIELDS = [
    "suite_id", "started_utc", "test_topology", "modules",
    "target_payload_gbps", "actual_payload_gbps_median", "duration_seconds",
    "warmup_count", "measured_count", "test_result", "cleanup_result",
    "first_failure_stage", "first_failure_classification",
    "baseline_profile_id", "source_manifest_sha256", "config_id",
    "geometry_id", "receiver_poll_cpu", "worker_cpu_list", "gpu_worker_cpu",
    "sink_cpu_list", "numa_node", "ingress_numa_node",
    "processing_numa_node", "receiver_poll_batch", "receiver_wr_num",
    "result_path",
]
HEX64 = re.compile(r"^[0-9a-f]{64}$")
SAFE_SUITE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
TOPOLOGIES = {"receive", "unpack", "gpu", "full"}
TEST_RESULTS = {
    "PASS", "PRODUCT_FAIL", "PERFORMANCE_FAIL", "HARNESS_FAIL",
    "ENV_BLOCKED", "SYNC_FAIL", "BUILD_FAIL", "BLOCKED", "INCOMPLETE",
    "STOPPED",
}
CLEANUP_RESULTS = {"PASS", "FAIL", "NOT_APPLICABLE"}
MODULES_BY_TOPOLOGY = {
    "receive": ["rdma2dada", "dada_dbnull"],
    "unpack": ["rdma2dada", "vdif_unpack_worker", "dada_dbnull"],
    "gpu": ["dada_junkdb", "pipeline_worker", "dada_dbnull"],
    "full": [
        "rdma2dada", "vdif_unpack_worker", "pipeline_worker", "dada_dbnull"
    ],
}
CATALOG_ENTRY_FIELDS = {
    "suite_id", "test_topology", "modules", "started_utc",
    "target_payload_gbps", "actual_payload_gbps", "duration_seconds",
    "warmup_count", "measured_count", "test_result", "cleanup_result",
    "first_failure", "identity", "configuration", "result_path",
    "summary_sha256", "suite_manifest_sha256",
}
IDENTITY_FIELDS = {
    "source_manifest_sha256", "config_id", "geometry_id",
    "baseline_profile_id", "baseline_profile_sha256", "binary_sha256",
}
CONFIGURATION_FIELDS = {
    "receiver_poll_cpu", "worker_cpu_list", "gpu_worker_cpu", "sink_cpu_list",
    "numa_node", "ingress_numa_node", "processing_numa_node",
    "receiver_poll_batch", "receiver_wr_num", "rings",
    "window_groups", "reorder_horizon_groups", "unpack_start_delay_seconds",
}
LEGACY_CONFIGURATION_FIELDS = CONFIGURATION_FIELDS - {
    "ingress_numa_node", "processing_numa_node"
}


def _atomic_write(path, data):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".%s." % path.name, dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, str(path))
    except Exception:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def sha256_file(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def load_json_object(path):
    value = json.loads(pathlib.Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError("JSON artifact must contain an object: %s" % path)
    return value


def parse_manifest(root):
    root = pathlib.Path(root).resolve()
    manifest_path = root / "MANIFEST.sha256"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise ValueError("manifest is missing or is a symlink")
    entries = {}
    for line_number, raw_line in enumerate(manifest_path.read_text().splitlines(), 1):
        if not raw_line.strip():
            continue
        match = re.match(r"^([0-9a-f]{64})  (.+)$", raw_line)
        if not match:
            raise ValueError("malformed manifest line %d" % line_number)
        digest, relative_text = match.groups()
        relative = pathlib.PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or relative_text == "MANIFEST.sha256"
            or relative_text in entries
        ):
            raise ValueError("unsafe or duplicate manifest path: %s" % relative_text)
        path = root.joinpath(*relative.parts)
        if path.is_symlink():
            raise ValueError("manifest path is a symlink: %s" % relative_text)
        if not path.is_file():
            raise ValueError("manifest path is missing: %s" % relative_text)
        if root not in path.resolve().parents:
            raise ValueError("manifest path escapes suite: %s" % relative_text)
        entries[relative_text] = digest
    if not entries:
        raise ValueError("manifest has no entries")
    return entries


def verify_manifest(root):
    root = pathlib.Path(root)
    entries = parse_manifest(root)
    for relative, expected in entries.items():
        actual = sha256_file(root / relative)
        if actual != expected:
            raise ValueError("manifest SHA256 mismatch: %s" % relative)
    actual_files = {
        str(path.relative_to(root))
        for path in root.rglob("*")
        if path.is_file() and path.name not in ("MANIFEST.sha256", "origin.json")
    }
    if actual_files != set(entries):
        missing = sorted(actual_files.symmetric_difference(entries))
        raise ValueError("manifest file set mismatch: %s" % ", ".join(missing))
    return entries


def _summary_request(summary):
    request = summary.get("request", summary.get("plan"))
    if not isinstance(request, dict):
        raise ValueError("suite summary has no request or plan")
    return request


def validate_suite(path, expected_suite_id=None):
    suite = pathlib.Path(path)
    if not suite.is_dir() or suite.is_symlink():
        raise ValueError("suite path must be a real directory")
    for candidate in suite.rglob("*"):
        if candidate.is_symlink():
            raise ValueError(
                "suite contains a symlink: %s" % candidate.relative_to(suite)
            )
    suite_id = expected_suite_id or suite.name
    if not SAFE_SUITE_ID.fullmatch(suite_id):
        raise ValueError("unsafe suite id: %s" % suite_id)
    for required in (
        "observation.json", "resolved_observation.json", "summary.json",
        "preflight.json", "MANIFEST.sha256",
    ):
        if not (suite / required).is_file():
            raise ValueError("suite is missing %s" % required)
    verify_manifest(suite)
    summary = load_json_object(suite / "summary.json")
    preflight = load_json_object(suite / "preflight.json")
    if summary.get("suite_id") != suite_id:
        raise ValueError("summary suite_id does not match directory")
    request = _summary_request(summary)
    topology = request.get("pipeline_stage")
    if topology not in TOPOLOGIES:
        raise ValueError("invalid pipeline topology: %r" % topology)
    run_items = summary.get("runs")
    if not isinstance(run_items, list) or not run_items:
        raise ValueError("suite summary has no runs")
    runs = []
    expected_run_files = set()
    seen_run_ids = set()
    for item in run_items:
        if not isinstance(item, dict):
            raise ValueError("suite run entry must be an object")
        relative_text = item.get("result_path")
        if not isinstance(relative_text, str):
            raise ValueError("run result_path must be a string")
        relative = pathlib.PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or len(relative.parts) != 2
            or relative.parts[0] != "runs"
            or relative.suffix != ".json"
        ):
            raise ValueError("unsafe run result_path: %s" % relative_text)
        run_id = item.get("run_id")
        if (
            not isinstance(run_id, str)
            or not run_id
            or run_id in seen_run_ids
            or relative.name != run_id + ".json"
        ):
            raise ValueError("invalid or duplicate run_id: %r" % run_id)
        seen_run_ids.add(run_id)
        if item.get("role") not in ("warmup", "measured"):
            raise ValueError("invalid run role: %r" % item.get("role"))
        run_path = suite.joinpath(*relative.parts)
        evidence_path = run_path.with_suffix(".evidence.log")
        expected_run_files.add(run_path.name)
        expected_run_files.add(evidence_path.name)
        if not run_path.is_file() or not evidence_path.is_file():
            raise ValueError("run result or evidence is missing: %s" % relative_text)
        run = load_json_object(run_path)
        if run.get("TEST_RESULT") != item.get("TEST_RESULT"):
            raise ValueError("summary/run TEST_RESULT mismatch: %s" % relative_text)
        cleanup = run.get("cleanup")
        run_cleanup = cleanup.get("CLEANUP_RESULT") if isinstance(cleanup, dict) else None
        if run_cleanup != item.get("CLEANUP_RESULT"):
            raise ValueError("summary/run CLEANUP_RESULT mismatch: %s" % relative_text)
        if run.get("evidence_file") != evidence_path.name:
            raise ValueError("run evidence_file mismatch: %s" % relative_text)
        evidence_sha = run.get("evidence_sha256")
        if not isinstance(evidence_sha, str) or not HEX64.fullmatch(evidence_sha):
            raise ValueError("run has invalid evidence SHA256: %s" % relative_text)
        if sha256_file(evidence_path) != evidence_sha:
            raise ValueError("run evidence SHA256 mismatch: %s" % relative_text)
        if item.get("TEST_RESULT") != "PASS" and not isinstance(
            run.get("failure"), dict
        ):
            raise ValueError("failed run has no structured failure: %s" % relative_text)
        runs.append(run)
    actual_run_files = {
        path.name for path in (suite / "runs").iterdir() if path.is_file()
    }
    if actual_run_files != expected_run_files:
        difference = sorted(actual_run_files.symmetric_difference(expected_run_files))
        raise ValueError("unreferenced run artifacts: %s" % ", ".join(difference))
    for role, field in (("warmup", "warmup_count"), ("measured", "measured_count")):
        requested = summary.get(field)
        completed = sum(item.get("role") == role for item in run_items)
        if (
            isinstance(requested, bool)
            or not isinstance(requested, int)
            or requested < completed
        ):
            raise ValueError("summary %s is smaller than completed runs" % field)
        clean_suite = all(
            item.get("CLEANUP_RESULT") == "PASS" for item in run_items
        )
        if (
            summary.get("TEST_RESULT") == "PASS"
            and clean_suite
            and requested != completed
        ):
            raise ValueError("PASS repetition count does not match completed runs")
    failed_items = [
        item for item in run_items
        if item.get("TEST_RESULT") != "PASS"
        or item.get("CLEANUP_RESULT") != "PASS"
    ]
    product_failures = [
        item for item in run_items if item.get("TEST_RESULT") != "PASS"
    ]
    expected_suite_result = (
        product_failures[0].get("TEST_RESULT") if product_failures else "PASS"
    )
    if summary.get("TEST_RESULT") != expected_suite_result:
        raise ValueError("summary suite TEST_RESULT does not match run outcomes")
    expected_files = {
        "observation.json", "resolved_observation.json", "summary.json",
        "preflight.json",
    }
    for name in expected_run_files:
        expected_files.add("runs/%s" % name)
    allowed_debug = set()
    for item in failed_items:
        run_id = item.get("run_id")
        for name in (
            "failed-command.json", "failed-process.log", "resource-snapshot.json",
        ):
            debug_path = "debug/%s/%s" % (run_id, name)
            allowed_debug.add(debug_path)
            expected_files.add(debug_path)
    actual_files = {
        str(path.relative_to(suite))
        for path in suite.rglob("*")
        if path.is_file() and path.name not in ("MANIFEST.sha256", "origin.json")
    }
    unexpected = sorted(actual_files - expected_files - allowed_debug)
    missing = sorted(expected_files - actual_files)
    if unexpected or missing:
        raise ValueError(
            "compact suite file set mismatch: %s"
            % ", ".join(missing + unexpected)
        )
    return {"summary": summary, "preflight": preflight, "runs": runs}


def _utc_now():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def _normalized_utc(value):
    if not isinstance(value, str):
        raise ValueError("UTC timestamp must be a string")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("UTC timestamp must contain a timezone")
    return parsed.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _positive_number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("%s must be a number" % field)
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise ValueError("%s must be positive and finite" % field)
    return number


def suite_cleanup_result(summary):
    runs = summary.get("runs", [])
    if not runs:
        raise ValueError("suite summary has no runs")
    for item in runs:
        state = item.get("CLEANUP_RESULT")
        if not isinstance(state, str) or not state:
            raise ValueError("suite run has no CLEANUP_RESULT")
        if state != "PASS":
            return state
    return "PASS"


def first_failure(summary, runs):
    for item, run in zip(summary.get("runs", []), runs):
        result = item.get("TEST_RESULT")
        cleanup_result = item.get("CLEANUP_RESULT")
        if result == "PASS" and cleanup_result == "PASS":
            continue
        if result == "PASS":
            return {
                "run_id": str(item.get("run_id", "unknown")),
                "stage": "CLEANUP",
                "classification": "CLEANUP_FAIL",
                "exit_code": None,
                "message": "cleanup result was %s" % cleanup_result,
            }
        failure = run.get("failure")
        if not isinstance(failure, dict):
            failure = {}
        message = failure.get("stderr", failure.get("message", ""))
        if not isinstance(message, str):
            message = str(message)
        return {
            "run_id": str(item.get("run_id", "unknown")),
            "stage": str(failure.get("stage", "UNKNOWN")),
            "classification": str(failure.get("classification", result)),
            "exit_code": failure.get("exit_code"),
            "message": message.splitlines()[0] if message else "",
        }
    return None


def _ring(block_bytes, blocks, enabled):
    if not enabled:
        return None
    if not isinstance(block_bytes, int) or block_bytes <= 0:
        raise ValueError("ring block_bytes must be positive")
    if not isinstance(blocks, int) or blocks <= 0:
        raise ValueError("ring blocks must be positive")
    return {"block_bytes": block_bytes, "blocks": blocks}


def derive_catalog_entry(suite_root):
    suite = pathlib.Path(suite_root)
    loaded = validate_suite(suite)
    summary = loaded["summary"]
    preflight = loaded["preflight"]
    runs = loaded["runs"]
    request = _summary_request(summary)
    plan = preflight.get("plan")
    if not isinstance(plan, dict):
        raise ValueError("preflight has no plan")
    topology = request.get("pipeline_stage")
    if plan.get("pipeline_stage") != topology:
        raise ValueError("summary/preflight topology conflict")
    target_rate = _positive_number(request.get("aggregate_gbps"), "aggregate_gbps")
    duration = _positive_number(request.get("duration_seconds"), "duration_seconds")
    if float(plan.get("aggregate_gbps")) != target_rate:
        raise ValueError("summary/preflight rate conflict")
    if float(plan.get("duration_seconds")) != duration:
        raise ValueError("summary/preflight duration conflict")
    origin_path = suite / "origin.json"
    if not origin_path.is_file() or origin_path.is_symlink():
        raise ValueError("imported suite is missing origin.json")
    origin = load_json_object(origin_path)
    if set(origin) != {
        "schema_version", "suite_id", "source_host", "remote_suite_root",
        "imported_utc", "source_manifest_sha256",
    }:
        raise ValueError("origin has invalid fields")
    if origin["schema_version"] != 1 or origin["suite_id"] != suite.name:
        raise ValueError("origin identity does not match suite")
    if not isinstance(origin["source_host"], str) or not origin["source_host"]:
        raise ValueError("origin has invalid source host")
    if (
        not isinstance(origin["remote_suite_root"], str)
        or not origin["remote_suite_root"].startswith("/")
    ):
        raise ValueError("origin has invalid remote suite root")
    _normalized_utc(origin["imported_utc"])
    source_sha = origin.get("source_manifest_sha256")
    if not isinstance(source_sha, str) or not HEX64.fullmatch(source_sha):
        raise ValueError("origin has invalid source manifest SHA256")
    config_id = plan.get("config_id")
    geometry_id = plan.get("geometry_id")
    for field, value in (("config_id", config_id), ("geometry_id", geometry_id)):
        if not isinstance(value, str) or not HEX64.fullmatch(value):
            raise ValueError("invalid %s" % field)
    profile = plan.get("profile_evidence") or {}
    profile_id = profile.get("profile_id")
    profile_sha = request.get("baseline_profile_sha256")
    if profile_sha is not None and not HEX64.fullmatch(str(profile_sha)):
        raise ValueError("invalid baseline profile SHA256")
    config = preflight.get("config")
    binary_sha = config.get("binary_sha") if isinstance(config, dict) else None
    if not isinstance(binary_sha, dict) or not binary_sha:
        raise ValueError("preflight has no binary SHA256 identities")
    for name, digest in binary_sha.items():
        if not isinstance(name, str) or not isinstance(digest, str) or not HEX64.fullmatch(digest):
            raise ValueError("invalid binary SHA256 identity")
    actual = summary.get("actual_aggregate_gbps")
    actual_entry = None
    if actual is not None:
        if not isinstance(actual, dict):
            raise ValueError("actual aggregate rate must be an object")
        actual_entry = {
            "median": float(actual["median"]),
            "minimum": float(actual["minimum"]),
            "maximum": float(actual["maximum"]),
        }
    rings = {
        "raw": _ring(
            plan.get("raw_block_bytes"), plan.get("raw_ring_blocks"),
            topology in ("receive", "unpack", "full"),
        ),
        "compute": _ring(
            plan.get("compute_block_bytes"), plan.get("compute_ring_blocks"),
            topology in ("unpack", "gpu", "full"),
        ),
        "output": _ring(
            plan.get("output_block_bytes"), plan.get("output_ring_blocks"),
            topology in ("gpu", "full"),
        ),
    }
    return {
        "suite_id": suite.name,
        "test_topology": topology,
        "modules": MODULES_BY_TOPOLOGY[topology],
        "started_utc": _normalized_utc(preflight.get("created_utc")),
        "target_payload_gbps": target_rate,
        "actual_payload_gbps": actual_entry,
        "duration_seconds": duration,
        "warmup_count": sum(
            item.get("role") == "warmup" for item in summary["runs"]
        ),
        "measured_count": sum(
            item.get("role") == "measured" for item in summary["runs"]
        ),
        "test_result": str(summary.get("TEST_RESULT")),
        "cleanup_result": suite_cleanup_result(summary),
        "first_failure": first_failure(summary, runs),
        "identity": {
            "source_manifest_sha256": source_sha,
            "config_id": config_id,
            "geometry_id": geometry_id,
            "baseline_profile_id": profile_id,
            "baseline_profile_sha256": profile_sha,
            "binary_sha256": dict(sorted(binary_sha.items())),
        },
        "configuration": {
            "receiver_poll_cpu": request.get("receiver_poll_cpu"),
            "worker_cpu_list": request.get("worker_cpu_list"),
            "gpu_worker_cpu": request.get("gpu_worker_cpu"),
            "sink_cpu_list": request.get("sink_cpu_list"),
            "numa_node": request.get("numa_node"),
            "ingress_numa_node": request.get("ingress_numa_node"),
            "processing_numa_node": request.get("processing_numa_node"),
            "receiver_poll_batch": request.get("receiver_poll_batch"),
            "receiver_wr_num": request.get("receiver_wr_num"),
            "rings": rings,
            "window_groups": (
                plan.get("window_groups") if topology in ("unpack", "full") else None
            ),
            "reorder_horizon_groups": (
                plan.get("reorder_horizon_groups")
                if topology in ("unpack", "full") else None
            ),
            "unpack_start_delay_seconds": (
                request.get("unpack_start_delay_seconds")
                if topology in ("unpack", "full") else None
            ),
        },
        "result_path": "suites/%s" % suite.name,
        "summary_sha256": sha256_file(suite / "summary.json"),
        "suite_manifest_sha256": sha256_file(suite / "MANIFEST.sha256"),
    }


def validate_catalog_value(value):
    if not isinstance(value, dict) or set(value) != {
        "schema_version", "generated_utc", "suite_count", "suites"
    }:
        raise ValueError("invalid catalog object fields")
    if value["schema_version"] != 1:
        raise ValueError("unsupported catalog schema version")
    _normalized_utc(value["generated_utc"])
    suites = value["suites"]
    if not isinstance(suites, list) or value["suite_count"] != len(suites):
        raise ValueError("catalog suite count mismatch")
    seen = set()
    previous = None
    for entry in suites:
        if not isinstance(entry, dict):
            raise ValueError("catalog suite entry must be an object")
        if set(entry) != CATALOG_ENTRY_FIELDS:
            raise ValueError("invalid catalog suite fields")
        suite_id = entry.get("suite_id")
        if not isinstance(suite_id, str) or not SAFE_SUITE_ID.fullmatch(suite_id):
            raise ValueError("invalid catalog suite_id")
        if suite_id in seen:
            raise ValueError("duplicate suite id")
        seen.add(suite_id)
        key = (entry.get("started_utc"), suite_id)
        if previous is not None and key < previous:
            raise ValueError("catalog entries are not ordered")
        previous = key
        if entry.get("test_topology") not in TOPOLOGIES:
            raise ValueError("invalid catalog topology")
        if entry.get("modules") != MODULES_BY_TOPOLOGY[entry["test_topology"]]:
            raise ValueError("invalid catalog module list")
        _normalized_utc(entry.get("started_utc"))
        if entry.get("result_path") != "suites/%s" % suite_id:
            raise ValueError("invalid catalog result path")
        _positive_number(entry.get("target_payload_gbps"), "target_payload_gbps")
        _positive_number(entry.get("duration_seconds"), "duration_seconds")
        for field in ("summary_sha256", "suite_manifest_sha256"):
            if not HEX64.fullmatch(str(entry.get(field, ""))):
                raise ValueError("invalid catalog %s" % field)
        for field in ("warmup_count", "measured_count"):
            if (
                isinstance(entry.get(field), bool)
                or not isinstance(entry.get(field), int)
                or entry[field] < 0
            ):
                raise ValueError("invalid catalog %s" % field)
        if entry.get("test_result") not in TEST_RESULTS:
            raise ValueError("invalid catalog test_result")
        if entry.get("cleanup_result") not in CLEANUP_RESULTS:
            raise ValueError("invalid catalog cleanup_result")
        actual = entry["actual_payload_gbps"]
        if actual is not None:
            if not isinstance(actual, dict) or set(actual) != {
                "median", "minimum", "maximum"
            }:
                raise ValueError("invalid actual payload rate fields")
            for field, number in actual.items():
                if (
                    isinstance(number, bool)
                    or not isinstance(number, (int, float))
                    or not math.isfinite(float(number))
                    or float(number) < 0
                ):
                    raise ValueError("invalid actual payload rate %s" % field)
        failure = entry["first_failure"]
        if failure is not None:
            if not isinstance(failure, dict) or set(failure) != {
                "run_id", "stage", "classification", "exit_code", "message"
            }:
                raise ValueError("invalid first_failure fields")
            for field in ("run_id", "stage", "classification", "message"):
                if not isinstance(failure[field], str) or (
                    field != "message" and not failure[field]
                ):
                    raise ValueError("invalid first_failure %s" % field)
            if failure["exit_code"] is not None and (
                isinstance(failure["exit_code"], bool)
                or not isinstance(failure["exit_code"], int)
            ):
                raise ValueError("invalid first_failure exit_code")
        identity = entry["identity"]
        if not isinstance(identity, dict) or set(identity) != IDENTITY_FIELDS:
            raise ValueError("invalid identity fields")
        for field in (
            "source_manifest_sha256", "config_id", "geometry_id"
        ):
            if not HEX64.fullmatch(str(identity.get(field, ""))):
                raise ValueError("invalid identity %s" % field)
        profile_sha = identity["baseline_profile_sha256"]
        profile_id = identity["baseline_profile_id"]
        if profile_id is not None and (
            not isinstance(profile_id, str) or not profile_id
        ):
            raise ValueError("invalid identity baseline_profile_id")
        if profile_sha is not None and not HEX64.fullmatch(str(profile_sha)):
            raise ValueError("invalid identity baseline_profile_sha256")
        binaries = identity["binary_sha256"]
        if not isinstance(binaries, dict) or not binaries:
            raise ValueError("invalid binary identities")
        for digest in binaries.values():
            if not isinstance(digest, str) or not HEX64.fullmatch(digest):
                raise ValueError("invalid binary identity SHA256")
        configuration = entry["configuration"]
        if (
            not isinstance(configuration, dict)
            or set(configuration) not in (
                CONFIGURATION_FIELDS, LEGACY_CONFIGURATION_FIELDS
            )
        ):
            raise ValueError("invalid configuration fields")
        rings = configuration["rings"]
        if not isinstance(rings, dict) or set(rings) != {"raw", "compute", "output"}:
            raise ValueError("invalid ring fields")
        for ring in rings.values():
            if ring is None:
                continue
            if (
                not isinstance(ring, dict)
                or set(ring) != {"block_bytes", "blocks"}
                or not isinstance(ring["block_bytes"], int)
                or ring["block_bytes"] <= 0
                or not isinstance(ring["blocks"], int)
                or ring["blocks"] <= 0
            ):
                raise ValueError("invalid ring geometry")
        for field in (
            "receiver_poll_cpu", "gpu_worker_cpu", "numa_node",
            "ingress_numa_node", "processing_numa_node",
        ):
            field_value = configuration.get(field)
            if field_value is not None and (
                isinstance(field_value, bool) or not isinstance(field_value, int)
            ):
                raise ValueError("invalid configuration %s" % field)
            if field_value is not None and field_value < 0:
                raise ValueError("invalid configuration %s" % field)
        for field in ("receiver_poll_batch", "receiver_wr_num", "window_groups"):
            field_value = configuration[field]
            if field_value is not None and (
                isinstance(field_value, bool)
                or not isinstance(field_value, int)
                or field_value <= 0
            ):
                raise ValueError("invalid configuration %s" % field)
        horizon = configuration["reorder_horizon_groups"]
        if horizon is not None and (
            isinstance(horizon, bool) or not isinstance(horizon, int) or horizon < 0
        ):
            raise ValueError("invalid configuration reorder_horizon_groups")
        for field in ("worker_cpu_list", "sink_cpu_list"):
            field_value = configuration[field]
            if field_value is not None and not isinstance(field_value, str):
                raise ValueError("invalid configuration %s" % field)
        delay = configuration["unpack_start_delay_seconds"]
        if delay is not None and (
            isinstance(delay, bool) or not isinstance(delay, (int, float))
            or not math.isfinite(float(delay)) or float(delay) < 0
        ):
            raise ValueError("invalid configuration unpack_start_delay_seconds")


def _csv_row(entry):
    failure = entry["first_failure"] or {}
    identity = entry["identity"]
    configuration = entry["configuration"]
    actual = entry["actual_payload_gbps"] or {}
    return {
        "suite_id": entry["suite_id"],
        "started_utc": entry["started_utc"],
        "test_topology": entry["test_topology"],
        "modules": "+".join(entry["modules"]),
        "target_payload_gbps": entry["target_payload_gbps"],
        "actual_payload_gbps_median": actual.get("median", ""),
        "duration_seconds": entry["duration_seconds"],
        "warmup_count": entry["warmup_count"],
        "measured_count": entry["measured_count"],
        "test_result": entry["test_result"],
        "cleanup_result": entry["cleanup_result"],
        "first_failure_stage": failure.get("stage", ""),
        "first_failure_classification": failure.get("classification", ""),
        "baseline_profile_id": identity.get("baseline_profile_id") or "",
        "source_manifest_sha256": identity["source_manifest_sha256"],
        "config_id": identity["config_id"],
        "geometry_id": identity["geometry_id"],
        "receiver_poll_cpu": configuration.get("receiver_poll_cpu"),
        "worker_cpu_list": configuration.get("worker_cpu_list") or "",
        "gpu_worker_cpu": configuration.get("gpu_worker_cpu"),
        "sink_cpu_list": configuration.get("sink_cpu_list") or "",
        "numa_node": configuration.get("numa_node"),
        "ingress_numa_node": configuration.get("ingress_numa_node"),
        "processing_numa_node": configuration.get("processing_numa_node"),
        "receiver_poll_batch": configuration.get("receiver_poll_batch"),
        "receiver_wr_num": configuration.get("receiver_wr_num"),
        "result_path": entry["result_path"],
    }


def _catalog_texts(value):
    json_text = json.dumps(value, indent=2, sort_keys=True) + "\n"
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS, lineterminator="\n")
    writer.writeheader()
    for entry in value["suites"]:
        writer.writerow(_csv_row(entry))
    return json_text, stream.getvalue()


def _root_manifest_text(root, json_digest, csv_digest):
    entries = [("catalog.json", json_digest), ("catalog.csv", csv_digest)]
    suites = root / "suites"
    if suites.is_dir():
        for suite in sorted(path for path in suites.iterdir() if path.is_dir()):
            entries.extend([
                ("suites/%s/MANIFEST.sha256" % suite.name,
                 sha256_file(suite / "MANIFEST.sha256")),
                ("suites/%s/origin.json" % suite.name,
                 sha256_file(suite / "origin.json")),
            ])
    return "".join("%s  %s\n" % (digest, path) for path, digest in entries)


def _parse_root_manifest(root):
    root = pathlib.Path(root).resolve()
    entries = {}
    for line_number, line in enumerate(
        (root / "MANIFEST.sha256").read_text().splitlines(), 1
    ):
        match = re.match(r"^([0-9a-f]{64})  (.+)$", line)
        if not match:
            raise ValueError("malformed root manifest line %d" % line_number)
        digest, relative_text = match.groups()
        relative = pathlib.PurePosixPath(relative_text)
        if relative.is_absolute() or ".." in relative.parts or relative_text in entries:
            raise ValueError("unsafe root manifest path")
        path = root.joinpath(*relative.parts)
        if path.is_symlink() or not path.is_file() or root not in path.resolve().parents:
            raise ValueError("invalid root manifest path: %s" % relative_text)
        entries[relative_text] = digest
    return entries


def _origin_value(suite_id, source_host, remote_suite_root, source_manifest_sha256):
    if not isinstance(source_host, str) or not source_host:
        raise ValueError("source host is required")
    if not isinstance(remote_suite_root, str) or not remote_suite_root.startswith("/"):
        raise ValueError("remote suite root must be absolute")
    if not isinstance(source_manifest_sha256, str) or not HEX64.fullmatch(
        source_manifest_sha256
    ):
        raise ValueError("invalid source manifest SHA256")
    return {
        "schema_version": 1,
        "suite_id": suite_id,
        "source_host": source_host,
        "remote_suite_root": remote_suite_root,
        "imported_utc": _utc_now(),
        "source_manifest_sha256": source_manifest_sha256,
    }


def _same_origin(existing, requested):
    keys = (
        "schema_version", "suite_id", "source_host", "remote_suite_root",
        "source_manifest_sha256",
    )
    return all(existing.get(key) == requested.get(key) for key in keys)


def import_suite(
    results_root,
    source_dir,
    source_host,
    remote_suite_root,
    source_manifest_sha256,
):
    root = pathlib.Path(results_root)
    source = pathlib.Path(source_dir)
    requested_origin = _origin_value(
        source.name, source_host, remote_suite_root, source_manifest_sha256
    )
    validate_suite(source)
    suites = root / "suites"
    suites.mkdir(parents=True, exist_ok=True)
    target = suites / source.name
    if target.exists():
        validate_suite(target)
        existing_origin = load_json_object(target / "origin.json")
        if (
            sha256_file(source / "MANIFEST.sha256")
            == sha256_file(target / "MANIFEST.sha256")
            and _same_origin(existing_origin, requested_origin)
        ):
            rebuild_catalog(root)
            verify_catalog(root)
            return target
        raise ValueError("conflicting suite id: %s" % source.name)
    temporary = root / (".import-%s" % uuid.uuid4().hex)
    try:
        shutil.copytree(str(source), str(temporary), symlinks=True)
        validate_suite(temporary, expected_suite_id=source.name)
        _atomic_write(
            temporary / "origin.json",
            json.dumps(requested_origin, indent=2, sort_keys=True) + "\n",
        )
        temporary.rename(target)
        rebuild_catalog(root)
        verify_catalog(root)
        return target
    except Exception:
        if temporary.exists():
            shutil.rmtree(str(temporary))
        raise


def rebuild_catalog(results_root, generated_utc=None):
    root = pathlib.Path(results_root)
    root.mkdir(parents=True, exist_ok=True)
    entries = []
    suites = root / "suites"
    if suites.is_dir():
        for suite in sorted(path for path in suites.iterdir() if path.is_dir()):
            entries.append(derive_catalog_entry(suite))
    entries.sort(key=lambda item: (item["started_utc"], item["suite_id"]))
    value = {
        "schema_version": 1,
        "generated_utc": generated_utc or _utc_now(),
        "suite_count": len(entries),
        "suites": entries,
    }
    validate_catalog_value(value)
    json_text, csv_text = _catalog_texts(value)
    json_digest = hashlib.sha256(json_text.encode()).hexdigest()
    csv_digest = hashlib.sha256(csv_text.encode()).hexdigest()
    manifest = _root_manifest_text(root, json_digest, csv_digest)
    publications = (
        (root / "catalog.json", json_text),
        (root / "catalog.csv", csv_text),
        (root / "MANIFEST.sha256", manifest),
    )
    previous = {
        path: path.read_text() if path.is_file() else None
        for path, _ in publications
    }
    try:
        for path, text in publications:
            _atomic_write(path, text)
    except Exception:
        for path, _ in publications:
            old_text = previous[path]
            if old_text is None:
                if path.exists():
                    path.unlink()
            else:
                _atomic_write(path, old_text)
        raise
    return value


def verify_catalog(results_root):
    root = pathlib.Path(results_root)
    value = load_json_object(root / "catalog.json")
    validate_catalog_value(value)
    manifest_entries = _parse_root_manifest(root)
    required = {"catalog.json", "catalog.csv"}
    indexed_suite_ids = {entry["suite_id"] for entry in value["suites"]}
    suites_root = root / "suites"
    actual_suite_ids = (
        {path.name for path in suites_root.iterdir() if path.is_dir()}
        if suites_root.is_dir() else set()
    )
    if actual_suite_ids != indexed_suite_ids:
        difference = sorted(actual_suite_ids.symmetric_difference(indexed_suite_ids))
        raise ValueError("unindexed or missing suite directories: %s" % ", ".join(difference))
    for entry in value["suites"]:
        suite = root / entry["result_path"]
        loaded = derive_catalog_entry(suite)
        if loaded != entry:
            raise ValueError("catalog entry drift: %s" % entry["suite_id"])
        required.add("%s/MANIFEST.sha256" % entry["result_path"])
        required.add("%s/origin.json" % entry["result_path"])
    if set(manifest_entries) != required:
        raise ValueError("root manifest file set mismatch")
    for relative, expected in manifest_entries.items():
        if sha256_file(root / relative) != expected:
            raise ValueError("root manifest SHA256 mismatch: %s" % relative)
    expected_json, expected_csv = _catalog_texts(value)
    if (root / "catalog.json").read_text() != expected_json:
        raise ValueError("catalog JSON is not canonical")
    if (root / "catalog.csv").read_text() != expected_csv:
        raise ValueError("catalog CSV does not match JSON")


def _query_date(value, name):
    if value is None:
        return None
    try:
        return _normalized_utc(value)
    except (TypeError, ValueError) as error:
        raise ValueError("invalid %s date: %s" % (name, error))


def query_catalog(
    catalog_value,
    suite_id=None,
    topology=None,
    rate_gbps=None,
    test_result=None,
    cleanup_result=None,
    started_from=None,
    started_to=None,
    baseline_profile_id=None,
    latest=None,
):
    validate_catalog_value(catalog_value)
    if topology is not None and topology not in TOPOLOGIES:
        raise ValueError("invalid topology")
    if test_result is not None and test_result not in TEST_RESULTS:
        raise ValueError("invalid test result")
    if cleanup_result is not None and cleanup_result not in CLEANUP_RESULTS:
        raise ValueError("invalid cleanup result")
    if rate_gbps is not None:
        _positive_number(rate_gbps, "rate-gbps")
    if latest is not None and (isinstance(latest, bool) or latest <= 0):
        raise ValueError("latest must be a positive integer")
    from_utc = _query_date(started_from, "started-from")
    to_utc = _query_date(started_to, "started-to")
    if from_utc is not None and to_utc is not None and from_utc > to_utc:
        raise ValueError("started-from date is after started-to date")
    selected = []
    for entry in catalog_value["suites"]:
        if suite_id is not None and entry["suite_id"] != suite_id:
            continue
        if topology is not None and entry["test_topology"] != topology:
            continue
        if rate_gbps is not None and entry["target_payload_gbps"] != rate_gbps:
            continue
        if test_result is not None and entry["test_result"] != test_result:
            continue
        if cleanup_result is not None and entry["cleanup_result"] != cleanup_result:
            continue
        if from_utc is not None and entry["started_utc"] < from_utc:
            continue
        if to_utc is not None and entry["started_utc"] > to_utc:
            continue
        if (
            baseline_profile_id is not None
            and entry["identity"]["baseline_profile_id"] != baseline_profile_id
        ):
            continue
        selected.append(entry)
    if latest is not None:
        selected = selected[-latest:]
    return selected


def _validate_accepted_value(value):
    if not isinstance(value, dict) or set(value) != {"schema_version", "results"}:
        raise ValueError("invalid accepted evidence index")
    if value["schema_version"] != 1 or not isinstance(value["results"], list):
        raise ValueError("invalid accepted evidence schema")
    seen = set()
    for item in value["results"]:
        if not isinstance(item, dict) or set(item) != {
            "catalog_entry", "origin", "promoted_utc",
        }:
            raise ValueError("invalid promoted evidence entry")
        entry = item["catalog_entry"]
        if not isinstance(entry, dict) or not isinstance(entry.get("suite_id"), str):
            raise ValueError("invalid promoted catalog entry")
        validate_catalog_value({
            "schema_version": 1,
            "generated_utc": item["promoted_utc"],
            "suite_count": 1,
            "suites": [entry],
        })
        origin = item["origin"]
        if not isinstance(origin, dict) or set(origin) != {
            "schema_version", "suite_id", "source_host", "remote_suite_root",
            "imported_utc", "source_manifest_sha256",
        }:
            raise ValueError("invalid promoted origin")
        if origin["schema_version"] != 1 or origin["suite_id"] != entry["suite_id"]:
            raise ValueError("promoted origin identity mismatch")
        if not isinstance(origin["source_host"], str) or not origin["source_host"]:
            raise ValueError("invalid promoted source host")
        if (
            not isinstance(origin["remote_suite_root"], str)
            or not origin["remote_suite_root"].startswith("/")
        ):
            raise ValueError("invalid promoted remote suite root")
        _normalized_utc(origin["imported_utc"])
        if not HEX64.fullmatch(str(origin["source_manifest_sha256"])):
            raise ValueError("invalid promoted source manifest SHA256")
        if (
            origin["source_manifest_sha256"]
            != entry["identity"]["source_manifest_sha256"]
        ):
            raise ValueError("promoted origin/source identity mismatch")
        _normalized_utc(item["promoted_utc"])
        if entry["suite_id"] in seen:
            raise ValueError("duplicate promoted suite id")
        seen.add(entry["suite_id"])


def promote_suite(results_root, suite_id, accepted_output):
    root = pathlib.Path(results_root)
    verify_catalog(root)
    catalog_value = load_json_object(root / "catalog.json")
    matches = [item for item in catalog_value["suites"] if item["suite_id"] == suite_id]
    if not matches:
        raise ValueError("unknown suite id: %s" % suite_id)
    entry = matches[0]
    origin = load_json_object(root / entry["result_path"] / "origin.json")
    output = pathlib.Path(accepted_output)
    if output.exists():
        value = load_json_object(output)
    else:
        value = {"schema_version": 1, "results": []}
    _validate_accepted_value(value)
    for item in value["results"]:
        existing = item["catalog_entry"]
        if existing["suite_id"] != suite_id:
            continue
        if existing == entry and item["origin"] == origin:
            return value
        raise ValueError("conflicting promoted suite id: %s" % suite_id)
    value["results"].append({
        "catalog_entry": entry,
        "origin": origin,
        "promoted_utc": _utc_now(),
    })
    value["results"].sort(
        key=lambda item: (
            item["catalog_entry"]["started_utc"],
            item["catalog_entry"]["suite_id"],
        )
    )
    _validate_accepted_value(value)
    _atomic_write(output, json.dumps(value, indent=2, sort_keys=True) + "\n")
    return value


def _build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    import_parser = subparsers.add_parser("import")
    import_parser.add_argument("--results-root", required=True)
    import_parser.add_argument("--source-dir", required=True)
    import_parser.add_argument("--source-host", required=True)
    import_parser.add_argument("--remote-suite-root", required=True)
    import_parser.add_argument("--source-manifest-sha256", required=True)

    rebuild_parser = subparsers.add_parser("rebuild")
    rebuild_parser.add_argument("--results-root", required=True)

    query_parser = subparsers.add_parser("query")
    query_parser.add_argument("--results-root", required=True)
    query_parser.add_argument("--suite-id")
    query_parser.add_argument("--topology", choices=sorted(TOPOLOGIES))
    query_parser.add_argument("--rate-gbps", type=float)
    query_parser.add_argument("--result")
    query_parser.add_argument("--cleanup-result")
    query_parser.add_argument("--started-from")
    query_parser.add_argument("--started-to")
    query_parser.add_argument("--baseline-profile-id")
    query_parser.add_argument("--latest", type=int)
    query_parser.add_argument("--format", choices=("json", "paths"), default="json")

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--results-root", required=True)

    promote_parser = subparsers.add_parser("promote")
    promote_parser.add_argument("--results-root", required=True)
    promote_parser.add_argument("--suite-id", required=True)
    promote_parser.add_argument(
        "--accepted-output", "--output", dest="accepted_output", required=True
    )
    return parser


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "import":
            target = import_suite(
                args.results_root, args.source_dir, args.source_host,
                args.remote_suite_root, args.source_manifest_sha256,
            )
            print(str(target.resolve()))
        elif args.command == "rebuild":
            print(json.dumps(rebuild_catalog(args.results_root), sort_keys=True))
        elif args.command == "verify":
            verify_catalog(args.results_root)
        elif args.command == "query":
            verify_catalog(args.results_root)
            value = load_json_object(pathlib.Path(args.results_root) / "catalog.json")
            selected = query_catalog(
                value,
                suite_id=args.suite_id,
                topology=args.topology,
                rate_gbps=args.rate_gbps,
                test_result=args.result,
                cleanup_result=args.cleanup_result,
                started_from=args.started_from,
                started_to=args.started_to,
                baseline_profile_id=args.baseline_profile_id,
                latest=args.latest,
            )
            if args.format == "paths":
                root = pathlib.Path(args.results_root).resolve()
                for entry in selected:
                    print(str(root / entry["result_path"]))
            else:
                print(json.dumps(selected, indent=2, sort_keys=True))
        elif args.command == "promote":
            repository = pathlib.Path(__file__).resolve().parents[1]
            allowed = (repository / "docs" / "results").resolve()
            output = pathlib.Path(args.accepted_output).resolve()
            if allowed != output.parent and allowed not in output.parents:
                raise ValueError("promote output must be below repository docs/results")
            value = promote_suite(
                args.results_root, args.suite_id, args.accepted_output
            )
            print(json.dumps(value, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print("task8c_catalog: %s" % error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
