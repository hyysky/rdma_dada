#!/usr/bin/env python3
"""Versioned runtime profiles for reproducible Task 8C test topologies."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import pathlib
import re
from typing import Any, Iterable


_TOP_LEVEL_FIELDS = {
    "schema_version",
    "profile_id",
    "target_host",
    "pipeline_stage",
    "source_result",
    "runtime",
    "geometry",
}
_PIPELINE_STAGES = {"receive", "unpack", "gpu", "full"}
_RUNTIME_FIELDS = {
    "receiver_poll_cpu",
    "worker_cpu_list",
    "gpu_worker_cpu",
    "sink_cpu_list",
    "numa_node",
    "ingress_numa_node",
    "processing_numa_node",
    "receiver_poll_batch",
    "receiver_wr_num",
    "unpack_start_delay_seconds",
    "missing_wait_ms",
    "station_skew_reserve_ms",
    "sender_source_port_101",
    "sender_source_port_102",
}
_GEOMETRY_FIELDS = {
    "target_payload_gbps",
    "duration_seconds",
    "raw_block_bytes",
    "raw_ring_blocks",
    "compute_block_bytes",
    "compute_ring_blocks",
    "output_block_bytes",
    "output_ring_blocks",
    "window_blocks",
    "reorder_horizon_groups",
}
_REQUIRED_GEOMETRY = {
    "receive": {"raw_block_bytes", "raw_ring_blocks"},
    "unpack": {
        "raw_block_bytes",
        "raw_ring_blocks",
        "compute_block_bytes",
        "compute_ring_blocks",
        "window_blocks",
        "reorder_horizon_groups",
    },
    "gpu": {
        "compute_block_bytes",
        "compute_ring_blocks",
        "output_block_bytes",
        "output_ring_blocks",
    },
    "full": {
        "raw_block_bytes",
        "raw_ring_blocks",
        "compute_block_bytes",
        "compute_ring_blocks",
        "output_block_bytes",
        "output_ring_blocks",
        "window_blocks",
        "reorder_horizon_groups",
    },
}
_REQUIRED_RUNTIME = {
    "receive": {
        "receiver_poll_cpu", "sink_cpu_list",
        "receiver_poll_batch", "receiver_wr_num",
        "unpack_start_delay_seconds",
    },
    "unpack": {
        "receiver_poll_cpu", "worker_cpu_list", "sink_cpu_list",
        "sender_source_port_101", "sender_source_port_102",
        "receiver_poll_batch", "receiver_wr_num",
        "unpack_start_delay_seconds", "missing_wait_ms",
        "station_skew_reserve_ms",
    },
    "gpu": {"gpu_worker_cpu", "sink_cpu_list"},
    "full": {
        "receiver_poll_cpu", "worker_cpu_list", "gpu_worker_cpu",
        "sink_cpu_list", "receiver_poll_batch",
        "receiver_wr_num", "unpack_start_delay_seconds",
        "sender_source_port_101", "sender_source_port_102",
        "missing_wait_ms", "station_skew_reserve_ms",
    },
}


@dataclasses.dataclass(frozen=True)
class BaselineProfile:
    schema_version: int
    profile_id: str
    target_host: str
    pipeline_stage: str
    source_result: str
    runtime: dict[str, object]
    geometry: dict[str, int | float]
    sha256: str
    path: pathlib.Path


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _positive_number(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        and float(value) > 0
    )


def _require_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"profile field {field} must be non-empty text")
    return value


def _validate_runtime(stage: str, runtime: object) -> dict[str, object]:
    if not isinstance(runtime, dict):
        raise ValueError("profile runtime must be an object")
    unknown = set(runtime) - _RUNTIME_FIELDS
    if unknown:
        raise ValueError(f"profile runtime has unknown fields: {sorted(unknown)}")
    if (
        ("sender_source_port_101" in runtime)
        != ("sender_source_port_102" in runtime)
    ):
        raise ValueError("profile source ports must be supplied together")
    missing = _REQUIRED_RUNTIME[stage] - set(runtime)
    if missing:
        raise ValueError(f"profile runtime is missing fields: {sorted(missing)}")
    legacy_numa = "numa_node" in runtime
    split_numa = (
        "ingress_numa_node" in runtime or "processing_numa_node" in runtime
    )
    if legacy_numa and split_numa:
        raise ValueError(
            "profile runtime numa_node cannot be combined with split NUMA fields"
        )
    ingress_required = stage in ("receive", "unpack", "full")
    processing_required = stage in ("unpack", "gpu", "full")
    missing_numa = []
    if not legacy_numa:
        if ingress_required and "ingress_numa_node" not in runtime:
            missing_numa.append("ingress_numa_node")
        if processing_required and "processing_numa_node" not in runtime:
            missing_numa.append("processing_numa_node")
    if missing_numa:
        raise ValueError(
            f"profile runtime is missing fields: {sorted(missing_numa)}"
        )
    for name in (
        "receiver_poll_cpu",
        "gpu_worker_cpu",
        "numa_node",
        "ingress_numa_node",
        "processing_numa_node",
        "receiver_poll_batch",
        "receiver_wr_num",
        "unpack_start_delay_seconds",
        "sender_source_port_101",
        "sender_source_port_102",
    ):
        if name in runtime and (not _is_int(runtime[name]) or int(runtime[name]) < 0):
            raise ValueError(f"profile runtime field {name} must be a nonnegative integer")
    source_ports = (
        runtime.get("sender_source_port_101"),
        runtime.get("sender_source_port_102"),
    )
    if (source_ports[0] is None) != (source_ports[1] is None):
        raise ValueError("profile source ports must be supplied together")
    if source_ports[0] is not None:
        if not all(1 <= int(port) <= 65535 for port in source_ports):
            raise ValueError("profile source ports must be in 1..65535")
        if source_ports[0] == source_ports[1]:
            raise ValueError("profile source ports must be distinct")
    for name in ("receiver_poll_batch", "receiver_wr_num"):
        if name in runtime and int(runtime[name]) == 0:
            raise ValueError(f"profile runtime field {name} must be positive")
    for name in ("missing_wait_ms", "station_skew_reserve_ms"):
        if name in runtime and not _positive_number(runtime[name]):
            raise ValueError(f"profile runtime field {name} must be positive")
    cpu_roles = []
    if "receiver_poll_cpu" in runtime:
        cpu_roles.append(int(runtime["receiver_poll_cpu"]))
    if "gpu_worker_cpu" in runtime:
        cpu_roles.append(int(runtime["gpu_worker_cpu"]))
    if "sink_cpu_list" in runtime:
        sink = _require_text(runtime["sink_cpu_list"], "runtime.sink_cpu_list")
        if re.fullmatch(r"[0-9]+", sink) is None:
            raise ValueError("profile runtime sink_cpu_list must be one CPU")
        cpu_roles.append(int(sink))
    if "worker_cpu_list" in runtime:
        workers = _require_text(
            runtime["worker_cpu_list"], "runtime.worker_cpu_list"
        )
        if re.fullmatch(r"[0-9]+(?:,[0-9]+){2,}", workers) is None:
            raise ValueError(
                "profile runtime worker_cpu_list must be coordinator,workers,writer"
            )
        cpu_roles.extend(int(value) for value in workers.split(","))
    if len(cpu_roles) != len(set(cpu_roles)):
        raise ValueError("profile CPU roles must be distinct")
    return dict(runtime)


def _validate_geometry(stage: str, geometry: object) -> dict[str, int | float]:
    if not isinstance(geometry, dict):
        raise ValueError("profile geometry must be an object")
    unknown = set(geometry) - _GEOMETRY_FIELDS
    if unknown:
        raise ValueError(f"profile geometry has unknown fields: {sorted(unknown)}")
    required = {"target_payload_gbps", "duration_seconds"} | _REQUIRED_GEOMETRY[stage]
    missing = required - set(geometry)
    if missing:
        raise ValueError(f"profile geometry is missing fields: {sorted(missing)}")
    result: dict[str, int | float] = {}
    for name, value in geometry.items():
        if name in ("target_payload_gbps", "duration_seconds"):
            if not _positive_number(value):
                raise ValueError(f"profile geometry field {name} must be positive")
            result[name] = float(value)
        else:
            if not _is_int(value) or int(value) <= 0:
                raise ValueError(
                    f"profile geometry field {name} must be a positive integer"
                )
            result[name] = int(value)
    return result


def load_profile(path: pathlib.Path) -> BaselineProfile:
    """Load one exact host/topology baseline and retain its byte identity."""
    resolved = pathlib.Path(path).resolve()
    try:
        contents = resolved.read_bytes()
        value = json.loads(contents)
    except OSError as error:
        raise ValueError(f"cannot read baseline profile: {resolved}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"baseline profile is invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("baseline profile must be a JSON object")
    missing = _TOP_LEVEL_FIELDS - set(value)
    unknown = set(value) - _TOP_LEVEL_FIELDS
    if missing:
        raise ValueError(f"baseline profile is missing fields: {sorted(missing)}")
    if unknown:
        raise ValueError(f"baseline profile has unknown top-level fields: {sorted(unknown)}")
    if not _is_int(value["schema_version"]) or value["schema_version"] != 1:
        raise ValueError("baseline profile schema_version must be 1")
    stage = _require_text(value["pipeline_stage"], "pipeline_stage")
    if stage not in _PIPELINE_STAGES:
        raise ValueError("baseline profile pipeline_stage is invalid")
    return BaselineProfile(
        schema_version=1,
        profile_id=_require_text(value["profile_id"], "profile_id"),
        target_host=_require_text(value["target_host"], "target_host"),
        pipeline_stage=stage,
        source_result=_require_text(value["source_result"], "source_result"),
        runtime=_validate_runtime(stage, value["runtime"]),
        geometry=_validate_geometry(stage, value["geometry"]),
        sha256=hashlib.sha256(contents).hexdigest(),
        path=resolved,
    )


def apply_profile(request: Any, profile: BaselineProfile,
                  explicit_fields: Iterable[str] = ()) -> Any:
    """Apply profile runtime fields while preserving explicit CLI overrides."""
    if not dataclasses.is_dataclass(request):
        raise TypeError("profile target must be a dataclass instance")
    explicit = set(explicit_fields)
    available = {field.name for field in dataclasses.fields(request)}
    updates = {
        name: value
        for name, value in profile.runtime.items()
        if name in available and name not in explicit
    }
    return dataclasses.replace(request, **updates)


def _effective_geometry(plan: Any, name: str) -> object:
    if name == "target_payload_gbps":
        return getattr(plan, "aggregate_gbps")
    if name == "window_blocks":
        compute_block_bytes = int(getattr(plan, "compute_block_bytes"))
        window_bytes = int(getattr(plan, "window_bytes"))
        if compute_block_bytes <= 0 or window_bytes % compute_block_bytes != 0:
            raise ValueError("effective window geometry is not block aligned")
        return window_bytes // compute_block_bytes
    return getattr(plan, name)


def compare_profile(plan: Any, profile: BaselineProfile) -> list[dict[str, object]]:
    """Return stable field-level differences between a plan and its baseline."""
    differences = []
    for section, expected in (
        ("runtime", profile.runtime),
        ("geometry", profile.geometry),
    ):
        for name in sorted(expected):
            actual = (
                getattr(plan, name)
                if section == "runtime"
                else _effective_geometry(plan, name)
            )
            if actual != expected[name]:
                differences.append({
                    "field": f"{section}.{name}",
                    "baseline": expected[name],
                    "effective": actual,
                })
    return differences
