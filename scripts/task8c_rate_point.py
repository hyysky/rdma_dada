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
import shutil
import subprocess
import statistics
import sys
import time
import uuid
from typing import Any, Iterable, Sequence

import task8c_profiles
import task8c_artifacts


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
class PipelineTopology:
    """Process/ring boundary for one independently runnable pipeline slice."""

    stage: str
    rings: tuple[str, ...]
    consumer_ring: str
    input_kind: str
    uses_receiver: bool
    uses_unpack_worker: bool
    uses_pipeline_worker: bool
    uses_network_senders: bool
    requires_rdma_capability: bool


_PIPELINE_TOPOLOGIES = {
    "receive": PipelineTopology(
        "receive", ("raw",), "raw", "network",
        True, False, False, True, True
    ),
    "unpack": PipelineTopology(
        "unpack", ("raw", "compute"), "compute", "network",
        True, True, False, True, True,
    ),
    "gpu": PipelineTopology(
        "gpu", ("compute", "output"), "output", "pressure_writer",
        False, False, True, False, False,
    ),
    "full": PipelineTopology(
        "full", ("raw", "compute", "output"), "output", "network",
        True, True, True, True, True,
    ),
}


def pipeline_topology(stage: str) -> PipelineTopology:
    try:
        return _PIPELINE_TOPOLOGIES[stage]
    except KeyError as error:
        raise ValueError(
            "pipeline_stage must be receive, unpack, gpu or full"
        ) from error


def required_process_roles(topology: PipelineTopology) -> tuple[str, ...]:
    """Return the complete PASS process ledger expected for one topology."""
    roles = [f"{ring}-ring" for ring in topology.rings]
    if topology.input_kind == "pressure_writer":
        roles.append("gpu_pressure_writer")
    if topology.uses_receiver:
        roles.append("rdma2dada")
    if topology.uses_unpack_worker:
        roles.append("vdif_unpack_worker")
    if topology.uses_pipeline_worker:
        roles.append("pipeline_worker")
    roles.append(f"{topology.consumer_ring}-consumer")
    if topology.uses_network_senders:
        roles.append("sender")
    return tuple(roles)


def expected_sender_process_count(station_count: int) -> int:
    """Return the host-sharded sender process count for one Observation."""
    if station_count <= 0:
        raise ValueError("Station count must be positive")
    return 1 if station_count == 1 else 2


@dataclasses.dataclass(frozen=True)
class RateRequest:
    aggregate_gbps: float
    duration_seconds: float
    batch_packets: int = 16
    compute_consumer: str = "dbdisk"
    pipeline_stage: str = "full"
    missing_wait_ms: float = 200.0
    station_skew_reserve_ms: float = 200.0
    worker_cpu_list: str | None = None
    gpu_worker_cpu: int | None = None
    sink_cpu_list: str | None = None
    receiver_poll_cpu: int | None = None
    numa_node: int | None = None
    ingress_numa_node: int | None = None
    processing_numa_node: int | None = None
    receiver_poll_batch: int = 32
    receiver_wr_num: int = 1024
    unpack_missing_per_second: bool = False
    unpack_start_delay_seconds: int = 0
    station_id: int | None = None
    sender_source_port: int | None = None
    sender_source_port_101: int | None = None
    sender_source_port_102: int | None = None
    baseline_profile_path: str | None = None
    baseline_profile_sha256: str | None = None
    experiment_name: str | None = None
    pipeline_profiler: str = "none"

    def validate(self) -> None:
        if (
            not math.isfinite(self.aggregate_gbps)
            or not math.isfinite(self.duration_seconds)
            or not math.isfinite(self.missing_wait_ms)
            or not math.isfinite(self.station_skew_reserve_ms)
            or self.aggregate_gbps <= 0
            or self.duration_seconds <= 0
            or self.missing_wait_ms <= 0
            or self.station_skew_reserve_ms <= 0
        ):
            raise ValueError(
                "rate, duration, missing wait and Station skew reserve "
                "must be positive"
            )
        if self.batch_packets <= 0:
            raise ValueError("batch_packets must be positive")
        if (
            self.receiver_poll_batch <= 0
            or self.receiver_wr_num <= 0
            or self.receiver_poll_batch > self.receiver_wr_num
        ):
            raise ValueError("receiver queue parameters are invalid")
        if self.compute_consumer not in ("dbdisk", "dbnull"):
            raise ValueError("compute_consumer must be dbdisk or dbnull")
        topology = pipeline_topology(self.pipeline_stage)
        if self.pipeline_profiler not in ("none", "nsys"):
            raise ValueError("pipeline_profiler must be none or nsys")
        if self.pipeline_profiler != "none" and self.pipeline_stage != "gpu":
            raise ValueError("pipeline_profiler requires gpu pipeline_stage")
        if self.pipeline_stage in ("receive", "unpack") and self.compute_consumer != "dbnull":
            raise ValueError(f"{self.pipeline_stage} pipeline_stage requires dbnull")
        if self.pipeline_stage == "receive" and self.unpack_missing_per_second:
            raise ValueError(
                "unpack_missing_per_second requires unpack or full stage"
            )
        if (self.station_id is None) != (self.sender_source_port is None):
            raise ValueError(
                "station_id and sender_source_port must be supplied together"
            )
        if self.station_id is not None:
            if self.pipeline_stage != "receive":
                raise ValueError("station_id requires receive pipeline_stage")
            if self.station_id not in (101, 102):
                raise ValueError("station_id must be 101 or 102")
            if not 1 <= self.sender_source_port <= 65535:
                raise ValueError("sender_source_port must be in 1..65535")
        fixed_source_ports = (
            self.sender_source_port_101,
            self.sender_source_port_102,
        )
        if (fixed_source_ports[0] is None) != (fixed_source_ports[1] is None):
            raise ValueError("Station source ports must be supplied together")
        if fixed_source_ports[0] is not None:
            if self.station_id is not None or self.pipeline_stage == "gpu":
                raise ValueError(
                    "two-Station source ports require a network multi-Station stage"
                )
            if not all(1 <= int(port) <= 65535 for port in fixed_source_ports):
                raise ValueError("Station source ports must be in 1..65535")
            if fixed_source_ports[0] == fixed_source_ports[1]:
                raise ValueError("Station source ports must be distinct")
        if (
            type(self.unpack_start_delay_seconds) is not int
            or self.unpack_start_delay_seconds < 0
        ):
            raise ValueError("unpack_start_delay_seconds must be a nonnegative integer")
        if self.unpack_start_delay_seconds and (
            self.pipeline_stage not in ("receive", "unpack", "full")
            or self.compute_consumer != "dbnull"
        ):
            raise ValueError(
                "unpack_start_delay_seconds requires receive, unpack or full stage "
                "with dbnull"
            )
        if self.worker_cpu_list is not None:
            if not re.fullmatch(r"[0-9]+(?:,[0-9]+){2,}",
                                self.worker_cpu_list):
                raise ValueError(
                    "worker_cpu_list must be ordered coordinator,worker...,writer CPUs"
                )
            unpack_cpus = [int(value) for value in self.worker_cpu_list.split(",")]
            if len(set(unpack_cpus)) != len(unpack_cpus):
                raise ValueError("unpack thread CPUs must be distinct")
        if self.numa_node is not None and (
            self.ingress_numa_node is not None
            or self.processing_numa_node is not None
        ):
            raise ValueError(
                "numa_node cannot be combined with split NUMA placement"
            )
        ingress_numa = (
            self.ingress_numa_node
            if self.ingress_numa_node is not None else self.numa_node
        )
        processing_numa = (
            self.processing_numa_node
            if self.processing_numa_node is not None else self.numa_node
        )
        placement_active = any(value is not None for value in (
            self.receiver_poll_cpu, self.sink_cpu_list, ingress_numa,
            processing_numa, self.worker_cpu_list, self.gpu_worker_cpu,
        ))
        if placement_active:
            receiver_required = topology.uses_receiver
            unpack_required = topology.uses_unpack_worker
            gpu_required = topology.uses_pipeline_worker
            processing_required = unpack_required or gpu_required
            if (self.sink_cpu_list is None or
                    (receiver_required and ingress_numa is None) or
                    (processing_required and processing_numa is None) or
                    (receiver_required and self.receiver_poll_cpu is None) or
                    (unpack_required and self.worker_cpu_list is None) or
                    (gpu_required and self.gpu_worker_cpu is None)):
                raise ValueError(
                    "used receiver, processing, worker, sink and NUMA "
                    "placement must be supplied together"
                )
            if (
                (receiver_required and self.receiver_poll_cpu < 0)
                or (gpu_required and self.gpu_worker_cpu < 0)
                or (ingress_numa is not None and ingress_numa < 0)
                or (processing_numa is not None and processing_numa < 0)
                or not re.fullmatch(r"[0-9]+", self.sink_cpu_list)
            ):
                raise ValueError("explicit CPU/NUMA placement is invalid")
            roles = [int(self.sink_cpu_list)]
            if receiver_required:
                roles.append(self.receiver_poll_cpu)
            if gpu_required:
                roles.append(self.gpu_worker_cpu)
            if unpack_required:
                roles.extend(
                    int(value) for value in self.worker_cpu_list.split(",")
                )
            if len(set(roles)) != len(roles):
                raise ValueError("receive, worker and sink CPUs must be distinct")


def result_directory_name(
    request: RateRequest, timestamp_utc: str | None = None
) -> str:
    timestamp = timestamp_utc or time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())

    def compact(value: float) -> str:
        return str(int(value)) if value.is_integer() else format(value, "g")

    stage = "rdma2dada" if request.pipeline_stage == "receive" else request.pipeline_stage
    return (
        f"{stage}-{compact(request.aggregate_gbps)}Gbps-"
        f"{compact(request.duration_seconds)}s-{timestamp}"
    )


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
    pipeline_stage: str = "full"
    gpu_pipeline_budget: dict[str, Any] = dataclasses.field(default_factory=dict)
    reorder_horizon_groups: int = 0
    worker_cpu_list: str | None = None
    gpu_worker_cpu: int | None = None
    sink_cpu_list: str | None = None
    receiver_poll_cpu: int | None = None
    numa_node: int | None = None
    ingress_numa_node: int | None = None
    processing_numa_node: int | None = None
    receiver_poll_batch: int = 32
    receiver_wr_num: int = 1024
    unpack_missing_per_second: bool = False
    unpack_start_delay_seconds: int = 0
    preparation_groups: int = 0
    packets_per_second: int = 0
    missing_wait_ms: float = 200.0
    station_skew_reserve_ms: float = 200.0
    sender_source_port: int | None = None
    sender_source_port_101: int | None = None
    sender_source_port_102: int | None = None
    profile_evidence: dict[str, Any] = dataclasses.field(default_factory=dict)

    @property
    def sender_group_count(self) -> int:
        return self.group_count + self.preparation_groups

    @property
    def uses_pipeline_worker(self) -> bool:
        return pipeline_topology(self.pipeline_stage).uses_pipeline_worker

    @property
    def uses_unpack_worker(self) -> bool:
        return pipeline_topology(self.pipeline_stage).uses_unpack_worker

    @property
    def topology(self) -> PipelineTopology:
        return pipeline_topology(self.pipeline_stage)

    @property
    def effective_ingress_numa_node(self) -> int | None:
        return (
            self.ingress_numa_node
            if self.ingress_numa_node is not None else self.numa_node
        )

    @property
    def effective_processing_numa_node(self) -> int | None:
        return (
            self.processing_numa_node
            if self.processing_numa_node is not None else self.numa_node
        )

    @property
    def raw_block_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["raw_block_bytes"])

    @property
    def compute_block_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["compute_block_bytes"])

    @property
    def window_bytes(self) -> int:
        derived = self.resolved_plan["resolved"]
        return int(derived.get("window_payload_bytes",
                               self.compute_block_bytes * 2))

    @property
    def raw_ring_blocks(self) -> int:
        return int(self.ring_plan["rings"]["raw"]["blocks"])

    @property
    def compute_ring_blocks(self) -> int:
        return int(self.ring_plan["rings"]["compute"]["blocks"])

    @property
    def output_key(self) -> str:
        return f"{int(str(self.ring_plan['rings']['output']['key']), 16):04x}"

    @property
    def output_block_bytes(self) -> int:
        return int(self.resolved_plan["resolved"]["output_block_bytes"])

    @property
    def output_ring_blocks(self) -> int:
        return int(self.ring_plan["rings"]["output"]["blocks"])

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
        pipeline_stage: str = "full",
        worker_cpu_list: str | None = None,
    ) -> "RatePlan":
        root = pathlib.Path(artifact_directory)
        base_required = (
            "resolved_observation.json",
            "ring_plan.json",
            "raw.header",
            "unpacked.header",
            "validation_report.json",
        )
        resolved_probe = json.loads((root / "resolved_observation.json").read_bytes())
        required = base_required + (
            ("output.header",)
            if int(resolved_probe["resolved"].get("output_block_bytes", 0)) > 0
            else ()
        )
        manifest_path = root / "MANIFEST.sha256"
        manifest = manifest_path.read_text()
        expected_manifest: dict[str, str] = {}
        for line in manifest.splitlines():
            parts = line.split()
            if len(parts) != 2:
                raise ValueError("invalid compiler MANIFEST.sha256 line")
            name = parts[1]
            relative = pathlib.PurePosixPath(name)
            if (
                relative.is_absolute()
                or len(relative.parts) != 1
                or name in expected_manifest
            ):
                raise ValueError("invalid compiler MANIFEST.sha256 path")
            expected_manifest[name] = parts[0]
        if not set(required).issubset(expected_manifest):
            raise ValueError("compiler manifest file set is incomplete")
        files = {
            name: (root / name).read_bytes() for name in expected_manifest
        }
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
        output = rings["rings"].get("output")
        if (
            raw["block_bytes"] != derived["raw_block_bytes"]
            or compute["block_bytes"] != derived["compute_block_bytes"]
            or (
                int(derived.get("output_block_bytes", 0)) > 0
                and (
                    output is None
                    or output["block_bytes"] != derived["output_block_bytes"]
                )
            )
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
        header_stages = [
            ("raw.header", "RAW"),
            ("unpacked.header", "UNPACKED"),
            ("converted.header", "CONVERTED"),
            ("beamformed.header", "BEAMFORMED"),
        ]
        if "output.header" in files:
            header_stages.append(("output.header", None))
        for name, expected_stage in header_stages:
            if name not in files:
                continue
            header_text = files[name].split(b"\0", 1)[0].decode("ascii")
            fields = {}
            for line in header_text.splitlines():
                parts = line.split(None, 1)
                if len(parts) == 2:
                    fields[parts[0]] = parts[1].strip()
            if (
                (expected_stage is not None and fields.get("DATA_STAGE") != expected_stage)
                or fields.get("CONFIG_ID") != resolved.get("config_id")
                or fields.get("GEOMETRY_ID") != resolved.get("geometry_id")
            ):
                raise ValueError(f"{name} conflicts with resolved observation")
        if aggregate_gbps <= 0 or duration_seconds <= 0 or batch_packets <= 0:
            raise ValueError("rate, duration and batch size must be positive")
        if compute_consumer not in ("dbdisk", "dbnull"):
            raise ValueError("compute_consumer must be dbdisk or dbnull")
        pipeline_topology(pipeline_stage)
        if pipeline_stage in ("receive", "unpack") and compute_consumer != "dbnull":
            raise ValueError(f"{pipeline_stage} pipeline_stage requires dbnull")
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
            pipeline_stage=pipeline_stage,
            gpu_pipeline_budget=(
                report.get("gpu_pipeline_budget", {})
                if isinstance(report.get("gpu_pipeline_budget", {}), dict)
                else {}
            ),
            worker_cpu_list=worker_cpu_list,
        )

    def as_dict(self) -> dict[str, Any]:
        result = dataclasses.asdict(self)
        result.pop("artifact_files")
        result.update({
            "raw_block_bytes": self.raw_block_bytes,
            "compute_block_bytes": self.compute_block_bytes,
            "raw_ring_blocks": self.raw_ring_blocks,
            "compute_ring_blocks": self.compute_ring_blocks,
            "nant": self.nant,
            "nchan": self.nchan,
            "npol": self.npol,
            "config_id": self.config_id,
            "geometry_id": self.geometry_id,
            "artifact_sha256": {
                name: hashlib.sha256(contents).hexdigest()
                for name, contents in self.artifact_files.items()
            },
        })
        if self.pipeline_stage == "receive":
            result.pop("compute_key", None)
            result.pop("compute_block_bytes", None)
            result.pop("compute_ring_blocks", None)
        elif self.pipeline_stage == "gpu":
            result.pop("raw_key", None)
            result.pop("raw_block_bytes", None)
            result.pop("raw_ring_blocks", None)
            result.update({
                "output_block_bytes": self.output_block_bytes,
                "output_ring_blocks": self.output_ring_blocks,
                "output_key": self.output_key,
            })
            pressure = derive_gpu_pressure_input(self)
            result["gpu_input"] = {
                "target_payload_gbps": self.aggregate_gbps,
                "blocks_per_second": pressure.blocks_per_second,
                "block_count": pressure.block_count,
                "bytes_per_second": pressure.bytes_per_second,
                "actual_payload_gbps": (
                    pressure.bytes_per_second * 8 / 1_000_000_000
                ),
                "total_bytes": pressure.total_bytes,
                "duration_seconds": pressure.duration_seconds,
            }
        elif self.pipeline_stage != "unpack" and "output" in self.ring_plan.get("rings", {}):
            result.update({
                "output_block_bytes": self.output_block_bytes,
                "output_ring_blocks": self.output_ring_blocks,
                "output_key": self.output_key,
            })
        return result


@dataclasses.dataclass(frozen=True)
class GpuPressureInputPlan:
    blocks_per_second: int
    block_count: int
    total_bytes: int
    bytes_per_second: int
    duration_seconds: int


def derive_gpu_pressure_input(plan: RatePlan) -> GpuPressureInputPlan:
    """Round the configured target up to whole compute blocks per second."""
    if not plan.duration_seconds.is_integer():
        raise ValueError("GPU pressure duration must be whole seconds")
    duration = int(plan.duration_seconds)
    requested_rate = (
        decimal.Decimal(str(plan.aggregate_gbps))
        * decimal.Decimal(1_000_000_000) / decimal.Decimal(8)
    )
    blocks_per_second = int(
        (requested_rate / decimal.Decimal(plan.compute_block_bytes))
        .to_integral_value(rounding=decimal.ROUND_CEILING)
    )
    block_count = blocks_per_second * duration
    total_bytes = block_count * plan.compute_block_bytes
    bytes_per_second = blocks_per_second * plan.compute_block_bytes
    return GpuPressureInputPlan(
        blocks_per_second, block_count, total_bytes, bytes_per_second, duration
    )


def _seconds_from_picoseconds(value: int) -> str:
    if value <= 0:
        raise ValueError("duration picoseconds must be positive")
    whole, fraction = divmod(value, 1_000_000_000_000)
    if fraction == 0:
        return str(whole)
    return f"{whole}.{fraction:012d}".rstrip("0")


def _group_count_for_request(
    request: RateRequest, station_count: int, record_bytes: int
) -> int:
    request.validate()
    if station_count <= 0 or record_bytes <= 0:
        raise ValueError("station count and record bytes must be positive")
    record_bits = decimal.Decimal(record_bytes * 8 * station_count)
    requested_bits = (
        decimal.Decimal(str(request.aggregate_gbps))
        * decimal.Decimal(1_000_000_000)
        * decimal.Decimal(str(request.duration_seconds))
    )
    return int(
        (requested_bits / record_bits).to_integral_value(
            rounding=decimal.ROUND_CEILING
        )
    )


def _fixed_packets_per_second(
    aggregate_gbps: float, station_count: int, record_bytes: int
) -> int:
    if aggregate_gbps <= 0 or station_count <= 0 or record_bytes <= 0:
        raise ValueError("packet-rate inputs must be positive")
    packets = (
        decimal.Decimal(str(aggregate_gbps))
        * decimal.Decimal(1_000_000_000)
        / decimal.Decimal(station_count * record_bytes * 8)
    )
    return int(packets.to_integral_value(rounding=decimal.ROUND_HALF_UP))


def _aggregate_gbps_for_packet_rate(
    packets_per_second: int, station_count: int, record_bytes: int
) -> float:
    if packets_per_second <= 0 or station_count <= 0 or record_bytes <= 0:
        raise ValueError("packet-rate inputs must be positive")
    return float(
        decimal.Decimal(packets_per_second * station_count * record_bytes * 8)
        / decimal.Decimal(1_000_000_000)
    )


def _sender_target_gbps(plan: RatePlan, station_count: int) -> float:
    if station_count <= 0:
        raise ValueError("sender station count must be positive")
    if plan.packets_per_second:
        target_bits_per_second = (
            plan.packets_per_second
            * plan.record_bytes
            * 8
            * station_count
        )
    else:
        target_bits_per_second = int((
            decimal.Decimal(str(plan.per_station_gbps))
            * decimal.Decimal(station_count)
            * decimal.Decimal(1_000_000_000)
        ).to_integral_value(rounding=decimal.ROUND_HALF_UP))
    return float(
        decimal.Decimal(target_bits_per_second)
        / decimal.Decimal(1_000_000_000)
    )


def _serialize_sender_config(
    config: dict[str, Any], expected_target_bits_per_second: int
) -> tuple[str, str]:
    text = json.dumps(config, indent=2) + "\n"
    match = re.search(r'"target_gbps"\s*:\s*([^,}\s]+)', text)
    token = match.group(1) if match is not None else ""
    if re.fullmatch(r"[0-9]+(?:\.[0-9]{1,9})?", token) is None:
        raise ValueError(
            "serialized target_gbps must be a decimal JSON number with at "
            "most 9 fractional digits"
        )
    observed_bits_per_second = (
        decimal.Decimal(token) * decimal.Decimal(1_000_000_000)
    )
    if observed_bits_per_second != decimal.Decimal(
        expected_target_bits_per_second
    ):
        raise ValueError(
            "serialized target_gbps does not match integer target bits/s"
        )
    return text, token


def _window_blocks_for_horizon(
    aggregate_gbps: float,
    missing_wait_ms: float,
    station_skew_reserve_ms: float,
    station_count: int,
    record_bytes: int,
    groups_per_block: int,
) -> tuple[int, int]:
    if (
        not math.isfinite(aggregate_gbps)
        or not math.isfinite(missing_wait_ms)
        or not math.isfinite(station_skew_reserve_ms)
        or aggregate_gbps <= 0
        or missing_wait_ms <= 0
        or station_skew_reserve_ms <= 0
        or station_count <= 0
        or record_bytes <= 0
        or groups_per_block <= 0
    ):
        raise ValueError("window horizon inputs must be positive")
    group_bits = decimal.Decimal(record_bytes * 8 * station_count)
    groups_per_ms = (
        decimal.Decimal(str(aggregate_gbps))
        * decimal.Decimal(1_000_000)
        / group_bits
    )

    def blocks_for(milliseconds: float) -> int:
        groups = int(
            (groups_per_ms * decimal.Decimal(str(milliseconds)))
            .to_integral_value(rounding=decimal.ROUND_CEILING)
        )
        return (groups + groups_per_block - 1) // groups_per_block

    missing_wait_blocks = blocks_for(missing_wait_ms)
    station_skew_blocks = blocks_for(station_skew_reserve_ms)
    return (
        1 + missing_wait_blocks + station_skew_blocks,
        missing_wait_blocks * groups_per_block,
    )


def _run_observation_compiler(
    compiler: pathlib.Path,
    config_path: pathlib.Path,
    output_directory: pathlib.Path | None,
    budget_payload_gbps: float,
    executor: Any | None = None,
) -> None:
    if executor is not None:
        executor(config_path, output_directory, budget_payload_gbps)
        return
    budget_text = format(
        decimal.Decimal(str(budget_payload_gbps)), "f"
    ).rstrip("0").rstrip(".")
    argv = [
        str(compiler), "--config", str(config_path),
        "--budget-payload-gbps", budget_text,
    ]
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
    compiler_executor: Any | None = None,
) -> RatePlan:
    request.validate()
    template_path = pathlib.Path(observation_template).resolve()
    compiler_path = pathlib.Path(compiler).resolve()
    if not template_path.is_file():
        raise StageError(
            "CONFIG_READY", ["read", str(template_path)], 2, "",
            "observation template does not exist", "ENV_BLOCKED"
        )
    if (
        compiler_executor is None
        and (not compiler_path.is_file() or not os.access(compiler_path, os.X_OK))
    ):
        raise StageError(
            "CONFIG_READY", [str(compiler_path), "--help"], 126, "",
            "observation compiler is not executable", "ENV_BLOCKED"
        )
    root = pathlib.Path(run_directory)
    root.mkdir(parents=True, exist_ok=True)

    def runtime_path(path: pathlib.Path) -> str:
        resolved_path = path.resolve()
        mapper = getattr(compiler_executor, "map_runtime_path", None)
        return str(mapper(resolved_path) if mapper is not None else resolved_path)

    try:
        observation = json.loads(template_path.read_text())
        wire_reference = pathlib.Path(observation["wire"]["profile"])
        if not wire_reference.is_absolute():
            observation["wire"]["profile"] = runtime_path(
                template_path.parent / wire_reference
            )
        for module in observation["processing"]["modules"]:
            if module.get("type") == "beamform":
                weight = pathlib.Path(module["weights_file"])
                if not weight.is_absolute():
                    module["weights_file"] = runtime_path(
                        template_path.parent / weight
                    )
        observation["observation"]["observation_id"] = (
            observation_id_for_run_directory(root)
        )
        if request.station_id is not None:
            configured_stations = observation["observation"]["station_ids"]
            if request.station_id not in configured_stations:
                raise ValueError(
                    f"station_id {request.station_id} is absent from observation"
                )
            observation["observation"]["station_ids"] = [request.station_id]
        # Receive and unpack plans never consume GPU processing modules.  Strip
        # them before compiler validation so stage artifacts describe only the
        # rings and contracts that the selected topology actually owns.
        if request.pipeline_stage in ("receive", "unpack"):
            observation["processing"]["modules"] = []
        observation["processing"]["run_once"] = True
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise StageError(
            "CONFIG_READY", ["parse", str(template_path)], 1, "", repr(error),
            "HARNESS_FAIL"
        ) from error

    bootstrap_config = root / "observation.bootstrap.json"
    bootstrap_config.write_text(json.dumps(observation, indent=2) + "\n")
    bootstrap_artifacts = root / "observation-bootstrap-artifacts"
    _run_observation_compiler(
        compiler_path, bootstrap_config, None, request.aggregate_gbps,
        compiler_executor
    )
    _run_observation_compiler(
        compiler_path, bootstrap_config, bootstrap_artifacts,
        request.aggregate_gbps, compiler_executor
    )
    bootstrap = RatePlan.from_artifact_directory(
        bootstrap_artifacts, request.aggregate_gbps,
        request.duration_seconds, request.batch_packets,
        request.compute_consumer, request.pipeline_stage
    )
    group_count = _group_count_for_request(
        request, bootstrap.nant, bootstrap.record_bytes
    )
    packets_per_second = 0
    if request.unpack_start_delay_seconds:
        if not request.duration_seconds.is_integer():
            raise StageError(
                "CONFIG_READY", ["validate", "duration_seconds"], 1, "",
                "fixed packets/second requires a whole-second duration",
                "HARNESS_FAIL",
            )
        packets_per_second = _fixed_packets_per_second(
            request.aggregate_gbps, bootstrap.nant, bootstrap.record_bytes
        )
        group_count = packets_per_second * int(request.duration_seconds)
    if bootstrap.records_per_block % bootstrap.nant != 0:
        raise StageError(
            "CONFIG_READY", ["validate", "records_per_block"], 1, "",
            "records_per_block is not divisible by Station count",
            "HARNESS_FAIL",
        )
    groups_per_block = bootstrap.records_per_block // bootstrap.nant
    window_blocks, reorder_horizon_groups = _window_blocks_for_horizon(
        request.aggregate_gbps,
        request.missing_wait_ms,
        request.station_skew_reserve_ms,
        bootstrap.nant,
        bootstrap.record_bytes,
        groups_per_block,
    )
    observation["blocks"]["window_blocks"] = window_blocks
    group_period_ps = int(
        bootstrap.resolved_plan["resolved"]["group_period_ps"]
    )
    observation["observation"]["duration_seconds"] = (
        _seconds_from_picoseconds(group_count * group_period_ps)
    )
    final_config = root / "observation.json"
    final_config.write_text(json.dumps(observation, indent=2) + "\n")
    final_artifacts = root / "observation-artifacts"
    _run_observation_compiler(
        compiler_path, final_config, None, request.aggregate_gbps,
        compiler_executor
    )
    _run_observation_compiler(
        compiler_path, final_config, final_artifacts,
        request.aggregate_gbps, compiler_executor
    )
    plan = RatePlan.from_artifact_directory(
        final_artifacts, request.aggregate_gbps, request.duration_seconds,
        request.batch_packets, request.compute_consumer, request.pipeline_stage
    )
    if plan.group_count != group_count:
        raise StageError(
            "CONFIG_READY", ["validate", "expected_groups"], 1,
            str(plan.group_count), str(group_count), "HARNESS_FAIL"
        )
    preparation_groups = (
        request.unpack_start_delay_seconds * packets_per_second
    )
    plan = dataclasses.replace(
        plan, reorder_horizon_groups=reorder_horizon_groups,
        worker_cpu_list=request.worker_cpu_list,
        gpu_worker_cpu=request.gpu_worker_cpu,
        sink_cpu_list=request.sink_cpu_list,
        receiver_poll_cpu=request.receiver_poll_cpu,
        numa_node=request.numa_node,
        ingress_numa_node=request.ingress_numa_node,
        processing_numa_node=request.processing_numa_node,
        receiver_poll_batch=request.receiver_poll_batch,
        receiver_wr_num=request.receiver_wr_num,
        unpack_missing_per_second=request.unpack_missing_per_second,
        unpack_start_delay_seconds=request.unpack_start_delay_seconds,
        preparation_groups=preparation_groups,
        packets_per_second=packets_per_second,
        missing_wait_ms=request.missing_wait_ms,
        station_skew_reserve_ms=request.station_skew_reserve_ms,
        sender_source_port=request.sender_source_port,
        sender_source_port_101=request.sender_source_port_101,
        sender_source_port_102=request.sender_source_port_102,
    )
    profile_evidence: dict[str, Any]
    if request.baseline_profile_path is not None:
        try:
            profile = task8c_profiles.load_profile(
                pathlib.Path(request.baseline_profile_path)
            )
        except ValueError as error:
            raise StageError(
                "CONFIG_PREFLIGHT", ["load-profile", request.baseline_profile_path],
                1, "", str(error), "HARNESS_FAIL"
            ) from error
        profile_stage_compatible = (
            profile.pipeline_stage == request.pipeline_stage
            or (
                request.pipeline_stage == "full"
                and profile.pipeline_stage == "unpack"
            )
        )
        if profile.target_host != "qths1" or not profile_stage_compatible:
            raise StageError(
                "CONFIG_PREFLIGHT", ["validate-profile", str(profile.path)], 1,
                "", "baseline profile host or pipeline stage does not match request",
                "HARNESS_FAIL",
            )
        if (
            request.baseline_profile_sha256 is not None
            and profile.sha256 != request.baseline_profile_sha256
        ):
            raise StageError(
                "CONFIG_PREFLIGHT", ["verify-profile", str(profile.path)], 1,
                profile.sha256, request.baseline_profile_sha256,
                "SYNC_FAIL",
            )
        differences = task8c_profiles.compare_profile(plan, profile)
        if differences and not request.experiment_name:
            raise StageError(
                "CONFIG_PREFLIGHT", ["compare-profile", str(profile.path)], 1,
                json.dumps(differences, sort_keys=True),
                "baseline profile differs; name the experiment explicitly",
                "HARNESS_FAIL",
            )
        profile_evidence = {
            "status": "EXPERIMENT" if differences else "EXACT",
            "profile_id": profile.profile_id,
            "path": str(profile.path),
            "sha256": profile.sha256,
            "source_result": profile.source_result,
            "experiment_name": request.experiment_name,
            "differences": differences,
        }
    else:
        profile_evidence = {
            "status": "BOOTSTRAP_CANDIDATE",
            "profile_id": None,
            "path": None,
            "sha256": None,
            "source_result": None,
            "experiment_name": request.experiment_name,
            "differences": [],
        }
    return dataclasses.replace(plan, profile_evidence=profile_evidence)


def observation_id_for_run_directory(run_directory: pathlib.Path) -> str:
    """Use one observation identity across warm-up/measured repetitions."""
    path = pathlib.Path(run_directory)
    if re.fullmatch(r"(?:warmup|measured)-[0-9]+", path.name):
        return path.parent.name
    return path.name


def derive_sender_source_ports(run_identity: str) -> tuple[int, int]:
    if not run_identity:
        raise ValueError("run identity must not be empty")
    digest = hashlib.sha256(run_identity.encode("utf-8")).digest()
    offset = int.from_bytes(digest[:2], byteorder="big") % 10000
    return 40000 + offset, 50000 + offset


def sender_source_identity(plan: RatePlan, run_identity: str) -> str:
    """Keep sender endpoints stable for every repetition of one suite."""
    observation_id = plan.source.get("observation", {}).get("observation_id")
    if isinstance(observation_id, str) and observation_id:
        return observation_id
    return run_identity


@dataclasses.dataclass(frozen=True)
class SenderSpec:
    station_ids: tuple[int, ...]
    host: str
    source_ip: str
    source_port: int
    bundle_name: str
    remote_name: str
    log_name: str

    @property
    def station_id(self) -> int:
        return self.station_ids[0]


def sender_specs_for_plan(plan: RatePlan, run_identity: str) -> list[SenderSpec]:
    first_port, second_port = derive_sender_source_ports(run_identity)
    if (
        plan.sender_source_port_101 is not None
        and plan.sender_source_port_102 is not None
    ):
        first_port = plan.sender_source_port_101
        second_port = plan.sender_source_port_102
    stations = [int(value) for value in
                plan.source.get("observation", {}).get("station_ids", [])]
    if not stations or len(set(stations)) != len(stations):
        raise ValueError("Task 8C requires distinct Observation Stations")
    if len(stations) == 1:
        station_id = stations[0]
        if station_id not in (101, 102):
            raise ValueError(f"unsupported single-Station diagnostic: {station_id}")
        if station_id == 101:
            host, source_ip, source_port = "qtpulsar1", "174.0.1.100", first_port
            names = ("sender101.json", "101.json", "sender101.log")
        else:
            host, source_ip, source_port = "qtpulsar2", "174.0.1.101", second_port
            names = ("sender102.json", "102.json", "sender102.log")
        if plan.sender_source_port is not None:
            source_port = plan.sender_source_port
        return [SenderSpec(
            (station_id,), host, source_ip, source_port, *names
        )]

    split = (len(stations) + 1) // 2
    partitions = (tuple(stations[:split]), tuple(stations[split:]))
    endpoints = (
        ("qtpulsar1", "174.0.1.100", first_port,
         "sender101.json", "101.json", "sender101.log"),
        ("qtpulsar2", "174.0.1.101", second_port,
         "sender102.json", "102.json", "sender102.log"),
    )
    return [
        SenderSpec(partition, *endpoint)
        for partition, endpoint in zip(partitions, endpoints)
    ]


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
    station_ids: int | Sequence[int],
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
    formal_start_seconds = int(resolved["group_start_seconds"])
    if formal_start_seconds < plan.unpack_start_delay_seconds:
        raise ValueError("unpack start delay crosses the VDIF reference epoch")
    configured_stations = (
        [int(station_ids)]
        if isinstance(station_ids, int)
        else [int(value) for value in station_ids]
    )
    if not configured_stations or len(set(configured_stations)) != len(configured_stations):
        raise ValueError("sender config requires distinct Station IDs")
    target_gbps = _sender_target_gbps(plan, len(configured_stations))
    result = {
        "schema_version": 2 if len(configured_stations) == 1 else 3,
        "source": {"ip": source_ip, "port": source_port},
        "destination": {
            "ip": receiver["destination_ip"],
            "port": receiver["destination_port"],
            "path_mtu": 9000,
        },
        "station": (
            {"station_id": configured_stations[0]}
            if len(configured_stations) == 1
            else {"station_ids": configured_stations}
        ),
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
            "start_seconds": (
                formal_start_seconds - plan.unpack_start_delay_seconds
            ),
            "group_count": plan.sender_group_count,
            "mode": "PACED",
            "start_utc": start_utc,
        },
        "transmit": {
            "target_gbps": target_gbps,
            "batch_packets": plan.batch_packets,
            "payload_mode": "REPEAT_TEMPLATE",
        },
        "faults": {
            "drop_groups": [],
            "duplicate_groups": [],
            "invalid_header_groups": [],
        },
    }
    if plan.packets_per_second:
        result["time"]["groups_per_second"] = plan.packets_per_second
    return result


def build_process_supervisor() -> str:
    """Return the remote wrapper that persists one qths process lifecycle."""
    return '''#!/usr/bin/env python3
import datetime
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys

if len(sys.argv) < 3:
    raise SystemExit("usage: supervise.py EXIT_PATH COMMAND [ARG ...]")

exit_path = pathlib.Path(sys.argv[1])
role = exit_path.stem
process_path = exit_path.with_name(role + ".process.json")
child_pid_path = exit_path.with_name(role + ".child.pid")
argv = sys.argv[2:]
child = None
pending_signal = None


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write(value):
    temporary = process_path.with_suffix(process_path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\\n")
    temporary.replace(process_path)


def forward(signum, _frame):
    global pending_signal
    pending_signal = signum
    if child is not None and child.poll() is None:
        child.send_signal(signum)


signal.signal(signal.SIGTERM, forward)
signal.signal(signal.SIGINT, forward)
started = now()
child = subprocess.Popen(argv)
child_pid_path.write_text(str(child.pid) + "\\n")
allowed_env = {
    name: os.environ[name]
    for name in (
        "PATH", "CUDA_VISIBLE_DEVICES", "OMP_NUM_THREADS",
        "MALLOC_ARENA_MAX", "TASK8C_NUMA_NODE"
    )
    if name in os.environ
}
try:
    cpu_affinity = sorted(os.sched_getaffinity(child.pid))
except (AttributeError, OSError):
    cpu_affinity = []
record = {
    "schema_version": 1,
    "host": socket.gethostname(),
    "role": role,
    "argv": argv,
    "env": allowed_env,
    "supervisor_pid": os.getpid(),
    "pid": child.pid,
    "cpu_affinity": cpu_affinity,
    "numa_node": os.environ.get("TASK8C_NUMA_NODE"),
    "started_utc": started,
    "ended_utc": None,
    "returncode": None,
    "state": "RUNNING",
}
write(record)
if pending_signal is not None and child.poll() is None:
    child.send_signal(pending_signal)
return_code = child.wait()
exit_code = return_code if return_code >= 0 else 128 - return_code
exit_path.write_text(str(exit_code) + "\\n")
record.update({
    "ended_utc": now(),
    "returncode": exit_code,
    "state": "EXITED",
})
write(record)
raise SystemExit(exit_code)
'''


def build_qths_bundle(
    plan: RatePlan,
    remote_run_dir: str,
    dada_db_path: str = "dada_db",
    dbdisk_path: str = "dada_dbdisk",
    dbnull_path: str = "dada_dbnull",
    qths_binary_dir: str = "/home/user/wy/rdma_dada/build-linux",
    ethtool_path: str = "ethtool",
    pipeline_profiler: str = "none",
    nsys_path: str = "/usr/local/cuda-12.8/bin/nsys",
) -> dict[str, str | bytes]:
    if not plan.artifact_files:
        raise ValueError("compiler artifact bundle is required")
    if pipeline_profiler not in ("none", "nsys"):
        raise ValueError("pipeline_profiler must be none or nsys")
    if pipeline_profiler != "none" and plan.pipeline_stage != "gpu":
        raise ValueError("pipeline profiler requires gpu pipeline_stage")
    if pipeline_profiler == "nsys" and not pathlib.PurePosixPath(nsys_path).is_absolute():
        raise ValueError("nsys_path must be absolute")
    if plan.pipeline_stage == "gpu":
        return _build_gpu_qths_bundle(
            plan, remote_run_dir, dada_db_path, dbdisk_path, dbnull_path,
            qths_binary_dir, pipeline_profiler, nsys_path,
        )
    receive_only = plan.pipeline_stage == "receive"
    full_gpu_pipeline = plan.uses_pipeline_worker
    consumer_key = (
        plan.raw_key if receive_only
        else plan.output_key if full_gpu_pipeline
        else plan.compute_key
    )
    consumer_name = plan.topology.consumer_ring
    receiver_device = str(plan.source["receiver"]["device"])
    receiver_args = ""
    if plan.receiver_poll_cpu is not None:
        receiver_args += f" --poll-cpu {plan.receiver_poll_cpu}"
    capture_nic_counters = '''#!/usr/bin/env python3
import datetime
import json
import pathlib
import re
import subprocess
import sys

if len(sys.argv) != 4:
    raise SystemExit(
        "usage: capture_nic_counters.py RDMA_DEVICE ETHTOOL OUTPUT_JSON"
    )

rdma_device, ethtool, output_name = sys.argv[1:]
net_root = pathlib.Path("/sys/class/infiniband") / rdma_device / "device" / "net"
interfaces = sorted(path.name for path in net_root.iterdir() if path.is_dir())
if len(interfaces) != 1:
    raise SystemExit(
        f"expected one netdev for {rdma_device}, found {interfaces}"
    )
interface = interfaces[0]
statistics_root = pathlib.Path("/sys/class/net") / interface / "statistics"
sysfs = {}
for path in sorted(statistics_root.iterdir()):
    value = path.read_text().strip()
    if re.fullmatch(r"[0-9]+", value):
        sysfs[path.name] = int(value)

completed = subprocess.run(
    [ethtool, "-S", interface], text=True, capture_output=True, check=False
)
if completed.returncode != 0:
    raise SystemExit(
        f"ethtool -S failed ({completed.returncode}): {completed.stderr.strip()}"
    )
driver = {}
for line in completed.stdout.splitlines():
    match = re.fullmatch(r"\\s*([^:]+):\\s*([0-9]+)\\s*", line)
    if match:
        driver[match.group(1).strip()] = int(match.group(2))
if not sysfs or not driver:
    raise SystemExit("NIC counter snapshot is empty")

snapshot = {
    "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "rdma_device": rdma_device,
    "interface": interface,
    "sysfs": sysfs,
    "ethtool": driver,
}
path = pathlib.Path(output_name)
temporary = path.with_suffix(path.suffix + ".tmp")
temporary.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\\n")
temporary.replace(path)
'''
    supervise = build_process_supervisor()
    validate_worker_ready = '''#!/usr/bin/env python3
import json
import pathlib
import sys

if len(sys.argv) != 5:
    raise SystemExit(
        "usage: validate_worker_ready.py READY CONFIG_ID GEOMETRY_ID WINDOW_BYTES"
    )
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
expected = {
    "config_id": sys.argv[2],
    "geometry_id": sys.argv[3],
    "window_bytes": int(sys.argv[4]),
}
for name, wanted in expected.items():
    if value.get(name) != wanted:
        raise SystemExit(f"worker ready {name} mismatch")
if value.get("schema_version") != 1 or int(value.get("pid", 0)) <= 0:
    raise SystemExit("worker ready metadata is invalid")
'''
    ingress_numa = plan.effective_ingress_numa_node
    processing_numa = plan.effective_processing_numa_node

    def numa_prefix(node: int | None) -> str:
        return (
            f"/usr/bin/numactl --membind={node} "
            if node is not None else ""
        )

    def numa_env(node: int | None) -> str:
        return f"TASK8C_NUMA_NODE={node} " if node is not None else ""

    ingress_prefix = numa_prefix(ingress_numa)
    ingress_env = numa_env(ingress_numa)
    processing_prefix = numa_prefix(processing_numa)
    processing_env = numa_env(processing_numa)
    sink_numa = ingress_numa if receive_only else processing_numa
    sink_prefix = numa_prefix(sink_numa)
    sink_env = numa_env(sink_numa)
    if plan.sink_cpu_list is not None:
        sink_prefix += f"/usr/bin/taskset -c {plan.sink_cpu_list} "
    if plan.compute_consumer == "dbnull":
        reader_start = f'''{sink_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/reader.exit" {sink_prefix}{dbnull_path} -k {consumer_key} -s -z -q >"$run_dir/reader.log" 2>&1 &
echo $! >"$run_dir/reader.pid"'''
    else:
        reader_start = f'''{sink_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/reader.exit" {sink_prefix}{dbdisk_path} -k {consumer_key} -D "$run_dir/{consumer_name}" -s -W >"$run_dir/reader.log" 2>&1 &
echo $! >"$run_dir/reader.pid"'''
    compute_ring_prepare = "" if receive_only else f'''{processing_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/compute-ring.exit" {processing_prefix}{dada_db_path} -k {plan.compute_key} -b {plan.compute_block_bytes} -a 4096 -n {plan.compute_ring_blocks} -r 1 -p -w -l >"$run_dir/compute-ring.log" 2>&1 &
echo $! >"$run_dir/compute-ring.pid"
touch "$run_dir/compute-ring.created"
'''
    prepare = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
mkdir -p "$run_dir/{consumer_name}"
check_ring_owner() {{
  name=$1
  pid=$(cat "$run_dir/$name-ring.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "$name ring owner exited during prepare" >&2
    if [[ -f "$run_dir/$name-ring.log" ]]; then
      tail -n 80 "$run_dir/$name-ring.log" >&2
    fi
    exit 1
  fi
}}
{ingress_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/raw-ring.exit" {ingress_prefix}{dada_db_path} -k {plan.raw_key} -b {plan.raw_block_bytes} -a 4096 -n {plan.raw_ring_blocks} -r 1 -p -w -l >"$run_dir/raw-ring.log" 2>&1 &
echo $! >"$run_dir/raw-ring.pid"
touch "$run_dir/raw-ring.created"
{compute_ring_prepare}""" + (f'''{processing_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/output-ring.exit" {processing_prefix}{dada_db_path} -k {plan.output_key} -b {plan.output_block_bytes} -a 4096 -n {plan.output_ring_blocks} -r 1 -p -w -l >"$run_dir/output-ring.log" 2>&1 &
echo $! >"$run_dir/output-ring.pid"
touch "$run_dir/output-ring.created"
''' if full_gpu_pipeline else "") + f"""
sleep 1
check_ring_owner raw
""" + ("check_ring_owner compute\n" if not receive_only else "") + ("check_ring_owner output\n" if full_gpu_pipeline else "")
    pipeline_worker_start = ""
    if full_gpu_pipeline:
        gpu_worker_prefix = processing_prefix
        if plan.gpu_worker_cpu is not None:
            gpu_worker_prefix += f"/usr/bin/taskset -c {plan.gpu_worker_cpu} "
        pipeline_worker_start = f'''{processing_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/pipeline-worker.exit" {gpu_worker_prefix}"$project/pipeline_worker" "$run_dir/resolved_observation.json" --metrics-json "$run_dir/pipeline-worker-metrics.json" >"$run_dir/pipeline-worker.log" 2>&1 &
echo $! >"$run_dir/pipeline-worker.pid"'''
    process_names = (
        ("reader", "pipeline-worker", "worker", "receiver")
        if full_gpu_pipeline
        else ("reader", "receiver") if receive_only
        else ("reader", "worker", "receiver")
    )
    readiness_checks = '\n'.join(
        f'''  if ! kill -0 "$(cat "$run_dir/{name}.pid")" 2>/dev/null; then
    report_readiness_failure {name}
    exit 1
  fi'''
        for name in process_names
    )
    readiness_diagnostics = '''report_readiness_state() {
  name=$1
  pid=$(cat "$run_dir/$name.pid")
  code=UNKNOWN
  state=EXITED
  if kill -0 "$pid" 2>/dev/null; then
    state=RUNNING
  fi
  if [[ -f "$run_dir/$name.exit" ]]; then
    code=$(cat "$run_dir/$name.exit")
  fi
  echo "$name readiness state=$state pid=$pid exit=$code" >&2
  if [[ -f "$run_dir/$name.log" ]]; then
    tail -n 40 "$run_dir/$name.log" >&2
  fi
}
report_readiness_failure() {
  name=$1
  echo "$name exited during readiness" >&2
  report_readiness_state "$name"
}
'''
    worker_program = processing_prefix + '"$project/vdif_unpack_worker"'
    worker_start = ""
    if not receive_only:
        worker_diagnostics = (
            " --diagnostics missing-per-second"
            if plan.unpack_missing_per_second else ""
        )
        pre_timeline_policy = (
            " --pre-timeline-policy discard"
            if plan.unpack_start_delay_seconds else ""
        )
        fixed_packet_rate = (
            f" --groups-per-second {plan.packets_per_second}"
            if plan.packets_per_second else ""
        )
        thread_cpus = (
            f" --thread-cpus {plan.worker_cpu_list}"
            if plan.worker_cpu_list is not None else ""
        )
        worker_start = f'''rm -f "$run_dir/worker.ready"
{processing_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/worker.exit" {worker_program} --plan "$run_dir/resolved_observation.json" --reorder-horizon-groups {plan.reorder_horizon_groups} --ready-file "$run_dir/worker.ready"{worker_diagnostics}{pre_timeline_policy}{fixed_packet_rate}{thread_cpus} >"$run_dir/worker.log" 2>&1 &
echo $! >"$run_dir/worker.pid"
for _ in $(seq 1 300); do
  if ! kill -0 "$(cat "$run_dir/worker.pid")" 2>/dev/null; then
    report_readiness_failure worker
    exit 1
  fi
  if [[ -f "$run_dir/worker.ready" ]]; then
    /usr/bin/python3 "$run_dir/validate_worker_ready.py" "$run_dir/worker.ready" {plan.config_id} {plan.geometry_id} {plan.window_bytes}
    break
  fi
  sleep 0.1
done
if [[ ! -f "$run_dir/worker.ready" ]]; then
  echo "worker readiness timed out" >&2
  report_readiness_state worker
  exit 1
fi
for _ in $(seq 1 100); do
  [[ -f "$run_dir/worker.child.pid" ]] && break
  sleep 0.01
done
if [[ ! -f "$run_dir/worker.child.pid" ]]; then
  echo "worker child PID was not recorded" >&2
  report_readiness_state worker
  exit 1
fi
worker_child_pid=$(cat "$run_dir/worker.child.pid")
: >"$run_dir/worker-affinity.txt"
for task_path in /proc/"$worker_child_pid"/task/*; do
  task_id=${{task_path##*/}}
  /usr/bin/taskset -pc "$task_id" >>"$run_dir/worker-affinity.txt"
done
'''
    start = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
project={qths_binary_dir}
{readiness_diagnostics}
/usr/bin/python3 "$run_dir/capture_nic_counters.py" {receiver_device} {ethtool_path} "$run_dir/nic-before.json"
{reader_start}
{pipeline_worker_start}
rm -f "$run_dir/pipeline.ready"
{worker_start}
{ingress_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/receiver.exit" {ingress_prefix}"$project/rdma2dada" --plan "$run_dir/resolved_observation.json" --poll-batch {plan.receiver_poll_batch} --recv-wr-num {plan.receiver_wr_num}{receiver_args} >"$run_dir/receiver.log" 2>&1 &
echo $! >"$run_dir/receiver.pid"
for _ in $(seq 1 300); do
{readiness_checks}
  if grep -q 'Receive threads ready' "$run_dir/receiver.log"; then
    touch "$run_dir/pipeline.ready"
    exit 0
  fi
  sleep 0.1
done
echo "readiness timed out waiting for receiver initialization" >&2
for name in {' '.join(process_names)}; do
  report_readiness_state "$name"
done
exit 1
"""
    finish = f"""#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
attempts=${{TASK8C_WAIT_ATTEMPTS:-300}}
wait_sleep=${{TASK8C_WAIT_SLEEP:-0.1}}
/usr/bin/python3 "$run_dir/capture_nic_counters.py" {receiver_device} {ethtool_path} "$run_dir/nic-after.json"
kill -TERM "$(cat "$run_dir/receiver.pid")"
for name in receiver {"worker " if not receive_only else ""}{"pipeline-worker " if full_gpu_pipeline else ""}reader; do
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
for name in receiver {"worker " if not receive_only else ""}{"pipeline-worker " if full_gpu_pipeline else ""}reader raw-ring {"compute-ring " if not receive_only else ""}{"output-ring" if full_gpu_pipeline else ""}; do
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
""" + (f'''if [[ -f "$run_dir/output-ring.created" ]]; then
  {dada_db_path} -d -k {plan.output_key} || status=1
fi
''' if full_gpu_pipeline else "") + f"""
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
files = sorted((run_dir / {consumer_name!r}).glob("*.dada"))
if not files:
    raise SystemExit("no {consumer_name} DADA output")
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
(run_dir / {f'{consumer_name}-summary.json'!r}).write_text(json.dumps(summary, indent=2) + "\\n")
'''
    if plan.compute_consumer == "dbnull":
        summary_name = f"{consumer_name}-summary.json"
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
(run_dir / {summary_name!r}).write_text(json.dumps(summary, indent=2) + "\\n")
'''
    worker_argv: list[str] = []
    if not receive_only:
        if processing_numa is not None:
            worker_argv += [
                "/usr/bin/numactl", f"--membind={processing_numa}"
            ]
        worker_argv += [
            f"{qths_binary_dir}/vdif_unpack_worker",
            "--plan", f"{remote_run_dir}/resolved_observation.json",
            "--reorder-horizon-groups", str(plan.reorder_horizon_groups),
            "--ready-file", f"{remote_run_dir}/worker.ready",
        ]
        if plan.worker_cpu_list is not None:
            worker_argv += ["--thread-cpus", plan.worker_cpu_list]
        if plan.unpack_missing_per_second:
            worker_argv += ["--diagnostics", "missing-per-second"]
        if plan.unpack_start_delay_seconds:
            worker_argv += ["--pre-timeline-policy", "discard"]
        if plan.packets_per_second:
            worker_argv += [
                "--groups-per-second", str(plan.packets_per_second)
            ]
    bundle: dict[str, str | bytes] = dict(plan.artifact_files)
    bundle.update({
        "supervise.py": supervise,
        "capture_nic_counters.py": capture_nic_counters,
        "prepare.sh": prepare,
        "start.sh": start,
        "finish.sh": finish,
        "cleanup.sh": cleanup,
        "summarize.py": summarize,
    })
    if not receive_only:
        bundle["validate_worker_ready.py"] = validate_worker_ready
        bundle["worker-argv.json"] = json.dumps(worker_argv, indent=2) + "\n"
    return bundle


def _build_gpu_qths_bundle(
    plan: RatePlan,
    remote_run_dir: str,
    dada_db_path: str,
    dbdisk_path: str,
    dbnull_path: str,
    qths_binary_dir: str,
    pipeline_profiler: str,
    nsys_path: str,
) -> dict[str, str | bytes]:
    """Build an isolated compute-ring -> GPU worker -> output-ring run."""
    supervise = build_process_supervisor()
    pressure = derive_gpu_pressure_input(plan)
    sink_directory = "$run_dir/output"
    processing_numa = plan.effective_processing_numa_node
    numa_env = (
        f"TASK8C_NUMA_NODE={processing_numa} "
        if processing_numa is not None else ""
    )
    numa_prefix = (
        f"/usr/bin/numactl --membind={processing_numa} "
        if processing_numa is not None else ""
    )
    gpu_worker_prefix = numa_prefix
    if plan.gpu_worker_cpu is not None:
        gpu_worker_prefix += f"/usr/bin/taskset -c {plan.gpu_worker_cpu} "
    sink_prefix = numa_prefix
    if plan.sink_cpu_list is not None:
        sink_prefix += f"/usr/bin/taskset -c {plan.sink_cpu_list} "
    if plan.compute_consumer == "dbnull":
        reader_start = (
            f'{numa_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/reader.exit" '
            f'{sink_prefix}{dbnull_path} -k {plan.output_key} -s -z -q '
            f'>"$run_dir/reader.log" 2>&1 &'
        )
    else:
        reader_start = (
            f'{numa_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/reader.exit" '
            f'{sink_prefix}{dbdisk_path} -k {plan.output_key} '
            f'-D {sink_directory} -s -W >"$run_dir/reader.log" 2>&1 &'
        )
    pipeline_worker_command = '"$project/pipeline_worker"'
    profiler_prepare = ""
    if pipeline_profiler == "nsys":
        profiler_prepare = 'mkdir -p "$run_dir/nsys"\n'
        pipeline_worker_command = (
            f"{nsys_path} profile --trace=cuda,nvtx,osrt --sample=none "
            "--cpuctxsw=none --force-overwrite=true "
            '--output="$run_dir/nsys/pipeline-worker" '
            '"$project/pipeline_worker"'
        )
    prepare = f'''#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
mkdir -p "$run_dir/output"
{numa_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/compute-ring.exit" {numa_prefix}{dada_db_path} -k {plan.compute_key} -b {plan.compute_block_bytes} -a 4096 -n {plan.compute_ring_blocks} -r 1 -p -w -l >"$run_dir/compute-ring.log" 2>&1 &
echo $! >"$run_dir/compute-ring.pid"
touch "$run_dir/compute-ring.created"
{numa_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/output-ring.exit" {numa_prefix}{dada_db_path} -k {plan.output_key} -b {plan.output_block_bytes} -a 4096 -n {plan.output_ring_blocks} -r 1 -p -w -l >"$run_dir/output-ring.log" 2>&1 &
echo $! >"$run_dir/output-ring.pid"
touch "$run_dir/output-ring.created"
sleep 1
kill -0 "$(cat "$run_dir/compute-ring.pid")"
kill -0 "$(cat "$run_dir/output-ring.pid")"
'''
    start = f'''#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
project={qths_binary_dir}
{profiler_prepare}{reader_start}
echo $! >"$run_dir/reader.pid"
{numa_env}/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/pipeline-worker.exit" {gpu_worker_prefix}{pipeline_worker_command} "$run_dir/resolved_observation.json" --metrics-json "$run_dir/pipeline-worker-metrics.json" >"$run_dir/pipeline-worker.log" 2>&1 &
echo $! >"$run_dir/pipeline-worker.pid"
sleep 1
kill -0 "$(cat "$run_dir/reader.pid")"
kill -0 "$(cat "$run_dir/pipeline-worker.pid")"
/usr/bin/python3 "$run_dir/supervise.py" "$run_dir/input-writer.exit" "$project/gpu_pressure_writer" --key {plan.compute_key} --header "$run_dir/unpacked.header" --block-bytes {plan.compute_block_bytes} --block-count {pressure.block_count} --blocks-per-second {pressure.blocks_per_second} --metrics-json "$run_dir/gpu-pressure-writer-metrics.json" >"$run_dir/input-writer.log" 2>&1 &
echo $! >"$run_dir/input-writer.pid"
touch "$run_dir/pipeline.ready"
'''
    finish = f'''#!/usr/bin/env bash
set -euo pipefail
run_dir={remote_run_dir}
attempts=${{TASK8C_WAIT_ATTEMPTS:-600}}
wait_sleep=${{TASK8C_WAIT_SLEEP:-0.1}}
for name in input-writer pipeline-worker reader; do
  pid=$(cat "$run_dir/$name.pid")
  for _ in $(seq 1 "$attempts"); do
    kill -0 "$pid" 2>/dev/null || break
    sleep "$wait_sleep"
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "$name pid $pid did not exit" >&2
    exit 1
  fi
  test "$(cat "$run_dir/$name.exit")" = 0
done
'''
    cleanup = f'''#!/usr/bin/env bash
set +e
run_dir={remote_run_dir}
status=0
for name in input-writer pipeline-worker reader output-ring compute-ring; do
  if [[ -f "$run_dir/$name.pid" ]]; then
    pid=$(cat "$run_dir/$name.pid")
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    kill -0 "$pid" 2>/dev/null && status=1
  fi
done
if [[ -f "$run_dir/compute-ring.created" ]]; then
  {dada_db_path} -d -k {plan.compute_key} || status=1
fi
if [[ -f "$run_dir/output-ring.created" ]]; then
  {dada_db_path} -d -k {plan.output_key} || status=1
fi
exit "$status"
'''
    if plan.compute_consumer == "dbnull":
        summarize = f'''#!/usr/bin/env python3
import json
import pathlib
root = pathlib.Path({remote_run_dir!r})
summary = {{"consumer": "dada_dbnull", "exit_code": int((root / "reader.exit").read_text()), "zero_copy": True, "single_transfer": True}}
(root / "output-summary.json").write_text(json.dumps(summary, indent=2) + "\\n")
'''
    else:
        summarize = f'''#!/usr/bin/env python3
import json
import pathlib
root = pathlib.Path({remote_run_dir!r})
files = sorted((root / "output").glob("*.dada"))
if not files:
    raise SystemExit("no output DADA file")
payload_bytes = sum(path.stat().st_size - 4096 for path in files)
summary = {{"consumer": "dada_dbdisk", "exit_code": int((root / "reader.exit").read_text()), "file_count": len(files), "data_bytes": payload_bytes}}
(root / "output-summary.json").write_text(json.dumps(summary, indent=2) + "\\n")
'''
    bundle: dict[str, str | bytes] = dict(plan.artifact_files)
    bundle.update({
        "supervise.py": supervise,
        "prepare.sh": prepare,
        "start.sh": start,
        "finish.sh": finish,
        "cleanup.sh": cleanup,
        "summarize.py": summarize,
    })
    return bundle


def nic_counter_evidence(
    before: dict[str, Any], after: dict[str, Any]
) -> dict[str, Any]:
    """Return run-scoped NIC counter evidence without hiding resets."""
    if before.get("interface") != after.get("interface"):
        raise ValueError("NIC interface changed between counter snapshots")
    if before.get("rdma_device") != after.get("rdma_device"):
        raise ValueError("RDMA device changed between counter snapshots")

    deltas: dict[str, dict[str, int | None]] = {}
    resets: list[str] = []
    for section in ("sysfs", "ethtool"):
        first = before.get(section)
        last = after.get(section)
        if not isinstance(first, dict) or not isinstance(last, dict):
            raise ValueError(f"missing NIC counter section: {section}")
        section_delta: dict[str, int | None] = {}
        for name in sorted(set(first) & set(last)):
            old = int(first[name])
            new = int(last[name])
            if new < old:
                section_delta[name] = None
                resets.append(f"{section}.{name}")
            else:
                section_delta[name] = new - old
        deltas[section] = section_delta
    return {
        "rdma_device": before.get("rdma_device"),
        "interface": before["interface"],
        "before": before,
        "after": after,
        "delta": deltas,
        "counter_resets": resets,
    }


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


class RemoteObservationCompiler:
    """Run the config compiler on qths1 and return its artifacts to HF."""

    def __init__(
        self,
        transport: Any,
        known_hosts: pathlib.Path,
        compiler: pathlib.Path,
        run_directory: pathlib.Path,
        remote_project_root: pathlib.Path = pathlib.Path(
            "/home/user/wy/rdma_dada"
        ),
    ) -> None:
        self.transport = transport
        self.known_hosts = pathlib.Path(known_hosts).with_name(
            pathlib.Path(known_hosts).name + ".compiler"
        )
        self.compiler = pathlib.Path(compiler)
        self.local_project_root = pathlib.Path(__file__).resolve().parents[1]
        self.remote_project_root = pathlib.PurePosixPath(remote_project_root)
        identity = re.sub(
            r"[^A-Za-z0-9_.-]+", "-", str(run_directory)
        ).strip("-.")
        digest = hashlib.sha256(str(run_directory).encode("utf-8")).hexdigest()[:12]
        self.remote_root = f"/tmp/task8c-compiler-{identity[-40:]}-{digest}"
        self._prepared = False
        self._sequence = 0

    def map_runtime_path(self, path: pathlib.Path) -> str:
        """Map repository-owned runtime inputs into the qths1 mirror."""
        resolved = pathlib.Path(path).resolve()
        try:
            relative = resolved.relative_to(self.local_project_root)
        except ValueError:
            return str(resolved)
        return str(self.remote_project_root.joinpath(*relative.parts))

    def _ssh(self, argv: Sequence[str], check: bool = True) -> subprocess.CompletedProcess[str]:
        return self.transport.run(
            build_ssh_argv("qths1", argv, str(self.known_hosts)),
            "CONFIG_READY",
            check,
        )

    def _scp(self, source: str, target: str, recursive: bool = False) -> None:
        argv = [
            "scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            "-o", f"UserKnownHostsFile={self.known_hosts}",
            "-o", "StrictHostKeyChecking=yes",
        ]
        if recursive:
            argv.append("-r")
        argv += [source, target]
        self.transport.run(argv, "CONFIG_READY", True)

    def _prepare(self) -> None:
        if self._prepared:
            return
        self.known_hosts.parent.mkdir(parents=True, exist_ok=True)
        if self.known_hosts.exists():
            self.known_hosts.unlink()
        bootstrap = [
            "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            "-o", f"UserKnownHostsFile={self.known_hosts}",
            "-o", "StrictHostKeyChecking=accept-new", "qths1", "--", "true",
        ]
        self.transport.run(bootstrap, "CONFIG_READY", True)
        self._ssh(["mkdir", "-p", self.remote_root])
        self._ssh(["test", "-x", str(self.compiler)])
        self._prepared = True

    def __call__(
        self,
        config_path: pathlib.Path,
        output_directory: pathlib.Path | None,
        budget_payload_gbps: float,
    ) -> None:
        self._prepare()
        self._sequence += 1
        remote_config = f"{self.remote_root}/config-{self._sequence}.json"
        self._scp(str(config_path), f"qths1:{remote_config}")
        budget_text = format(
            decimal.Decimal(str(budget_payload_gbps)), "f"
        ).rstrip("0").rstrip(".")
        argv = [
            str(self.compiler), "--config", remote_config,
            "--budget-payload-gbps", budget_text,
        ]
        if output_directory is None:
            argv.append("--preflight-only")
        else:
            if output_directory.exists():
                raise StageError(
                    "CONFIG_READY", ["test", "!", "-e", str(output_directory)],
                    1, "", "local compiler artifact directory already exists"
                )
            remote_output = f"{self.remote_root}/{output_directory.name}"
            argv += ["--output-dir", remote_output]
        self._ssh(argv)
        if output_directory is not None:
            output_directory.parent.mkdir(parents=True, exist_ok=True)
            self._scp(
                f"qths1:{remote_output}", str(output_directory.parent), recursive=True
            )

    def close(self) -> None:
        if not self._prepared:
            return
        completed = self._ssh(["rm", "-rf", self.remote_root], check=False)
        try:
            if completed.returncode != 0:
                raise StageError(
                    "CONFIG_READY", ["rm", "-rf", self.remote_root],
                    completed.returncode, completed.stdout, completed.stderr,
                    "HARNESS_FAIL",
                )
        finally:
            if self.known_hosts.exists():
                self.known_hosts.unlink()
            self._prepared = False


class SshBackend:
    """Real HF-side backend. It connects directly to qths/qtp hosts."""

    def __init__(
        self,
        transport: Any | None = None,
        project_root: pathlib.Path = pathlib.Path("/home/user/wy/rdma_dada"),
        known_hosts: pathlib.Path = pathlib.Path("/tmp/task8c-known-hosts"),
        qths_binary_dir: pathlib.Path | None = None,
        sender_binary_dir: pathlib.Path | None = None,
        pipeline_profiler: str = "none",
        nsys_path: pathlib.PurePosixPath = pathlib.PurePosixPath(
            "/usr/local/cuda-12.8/bin/nsys"
        ),
    ) -> None:
        self.transport = transport or SubprocessTransport()
        self.project_root = pathlib.Path(project_root)
        self.qths_binary_dir = pathlib.Path(
            qths_binary_dir or self.project_root / "build-linux"
        )
        self.sender_binary_dir = pathlib.Path(
            sender_binary_dir or self.project_root / "build-linux"
        )
        self.pipeline_profiler = pipeline_profiler
        self.nsys_path = pathlib.PurePosixPath(nsys_path)
        self.known_hosts = pathlib.Path(known_hosts)
        self.remote_run_dir = ""
        self.local_run_dir = pathlib.Path()
        self.preparation: dict[str, Any] = {}
        self._rings_acquired: list[str] = []
        self._capability_added = False
        self._sender_processes: list[Any] = []
        self._sender_runtime: list[dict[str, Any]] = []
        self._sender_endpoints: list[tuple[str, str, int]] = []
        self._sender_specs: list[SenderSpec] = []
        self._sender_config_evidence: list[dict[str, Any]] = []
        self._compute_consumer = "dbdisk"
        self._pipeline_stage = "full"
        self._psrdada_paths = {
            "dada_db": "dada_db",
            "dada_dbdisk": "dada_dbdisk",
            "dada_dbnull": "dada_dbnull",
        }
        self._diagnostic_paths = {"ethtool": "ethtool"}
        if self.pipeline_profiler == "nsys":
            self._diagnostic_paths["nsys"] = str(self.nsys_path)
        self._sender_ethtool_paths: dict[str, str] = {}

    def observation_compiler(
        self, compiler: pathlib.Path, run_directory: pathlib.Path
    ) -> RemoteObservationCompiler:
        return RemoteObservationCompiler(
            self.transport, self.known_hosts, compiler, run_directory,
            self.project_root,
        )

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
        self._sender_specs = (
            sender_specs_for_plan(plan, sender_source_identity(plan, identity))
            if plan.topology.uses_network_senders else []
        )
        for host in ("qths1", *(spec.host for spec in self._sender_specs)):
            self._bootstrap_host(host)
        start = (
            self._future_start_utc("PREPARE")
            if plan.topology.uses_network_senders else None
        )
        self.preparation = {
            "start_utc": start,
            "remote_run_dir": self.remote_run_dir,
        }
        return dict(self.preparation)

    def _write_bundle(self, plan: RatePlan, start_utc: str) -> pathlib.Path:
        bundle_root = self.local_run_dir / "bundle"
        qths_root = bundle_root / "qths"
        qths_root.mkdir(parents=True, exist_ok=True)
        try:
            self._sender_specs = (
                sender_specs_for_plan(
                    plan,
                    sender_source_identity(plan, self.remote_run_dir),
                )
                if plan.topology.uses_network_senders else []
            )
        except ValueError as error:
            raise StageError(
                "CONFIG_READY", ["validate", "station_ids"], 1,
                json.dumps(plan.source.get("observation", {}).get("station_ids", [])),
                str(error),
            ) from error
        for name, content in build_qths_bundle(
            plan,
            self.remote_run_dir,
            self._psrdada_paths["dada_db"],
            self._psrdada_paths["dada_dbdisk"],
            self._psrdada_paths["dada_dbnull"],
            str(self.qths_binary_dir),
            self._diagnostic_paths["ethtool"],
            self.pipeline_profiler,
            str(self.nsys_path),
        ).items():
            path = qths_root / name
            if isinstance(content, bytes):
                path.write_bytes(content)
            else:
                path.write_text(content)
            if path.suffix == ".sh":
                path.chmod(0o755)
        self._sender_endpoints = []
        self._sender_config_evidence = []
        for spec in self._sender_specs:
            self._sender_endpoints.append(
                (spec.host, spec.source_ip, spec.source_port)
            )
            target_bits_per_second = int(
                decimal.Decimal(str(
                    _sender_target_gbps(plan, len(spec.station_ids))
                )) * decimal.Decimal(1_000_000_000)
            )
            serialized, target_token = _serialize_sender_config(
                build_sender_config(
                    plan, spec.station_ids, spec.source_ip,
                    spec.source_port, start_utc
                ),
                target_bits_per_second,
            )
            path = bundle_root / spec.bundle_name
            path.write_text(serialized)
            self._sender_config_evidence.append({
                "host": spec.host,
                "bundle_name": spec.bundle_name,
                "station_count": len(spec.station_ids),
                "target_bits_per_second": target_bits_per_second,
                "target_gbps_token": target_token,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "python_version": sys.version,
            })
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
        if self.pipeline_profiler == "nsys":
            nsys_path = str(self.nsys_path)
            available = self._ssh(
                "qths1", ["test", "-x", nsys_path], "CONFIG_READY",
                check=False,
            )
            if available.returncode != 0:
                raise StageError(
                    "CONFIG_READY", ["test", "-x", nsys_path],
                    available.returncode, available.stdout, available.stderr,
                    "configured Nsight Systems executable is unavailable",
                    "ENV_BLOCKED",
                )
        if plan.topology.uses_receiver:
            self._diagnostic_paths["ethtool"] = self._resolve_executable(
                "ethtool", ("/usr/sbin/ethtool", "/usr/bin/ethtool"), "qths1"
            )
        self._sender_ethtool_paths = {
            spec.host: self._resolve_executable(
                "ethtool", ("/usr/sbin/ethtool", "/usr/bin/ethtool"),
                spec.host,
            )
            for spec in self._sender_specs
        }
        bundle_root = self._write_bundle(plan, str(preparation["start_utc"]))
        for host in ("qths1", *(spec.host for spec in self._sender_specs)):
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
        if self._sender_specs:
            capture_path = bundle_root / "qths" / "capture_nic_counters.py"
            local_capture_sha = self.transport.run(
                ["sha256sum", str(capture_path)], "CONFIG_READY", True
            ).stdout.split()[0]
            for spec in self._sender_specs:
                remote_capture = f"{self.remote_run_dir}/capture_nic_counters.py"
                self.transport.run(
                    self._scp_argv(
                        str(capture_path), f"{spec.host}:{remote_capture}"
                    ),
                    "CONFIG_READY", True,
                )
                remote_capture_sha = self._ssh(
                    spec.host, ["sha256sum", remote_capture], "CONFIG_READY"
                ).stdout.split()[0]
                if local_capture_sha != remote_capture_sha:
                    raise StageError(
                        "CONFIG_READY",
                        ["sha256sum", spec.host, remote_capture],
                        1,
                        f"local={local_capture_sha} remote={remote_capture_sha}",
                        "sender NIC capture SHA mismatch",
                    )
                probe_hashes[f"{spec.host}:capture_nic_counters.py"] = (
                    local_capture_sha
                )
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
        binaries = [
            *(
                (("qths1", "rdma2dada", self.qths_binary_dir / "rdma2dada"),)
                if plan.topology.uses_receiver else ()
            ),
            *(
                (spec.host, "fpga_sender_sim",
                 self.sender_binary_dir / "fpga_sender_sim")
                for spec in self._sender_specs
            ),
        ]
        if plan.uses_unpack_worker:
            binaries.insert(
                1,
                ("qths1", "vdif_unpack_worker", self.qths_binary_dir / "vdif_unpack_worker"),
            )
        if plan.uses_pipeline_worker:
            binaries.insert(
                2,
                ("qths1", "pipeline_worker", self.qths_binary_dir / "pipeline_worker"),
            )
        if plan.pipeline_stage == "gpu":
            binaries.insert(
                2,
                ("qths1", "gpu_pressure_writer",
                 self.qths_binary_dir / "gpu_pressure_writer"),
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
        profiler_evidence: dict[str, Any] = {"tool": "none"}
        if self.pipeline_profiler == "nsys":
            nsys_path = str(self.nsys_path)
            output = self._ssh(
                "qths1", ["sha256sum", nsys_path], "CONFIG_READY"
            ).stdout.split()
            version = self._ssh(
                "qths1", [nsys_path, "--version"], "CONFIG_READY"
            ).stdout.strip()
            if not output:
                raise StageError(
                    "CONFIG_READY", ["sha256sum", nsys_path], 1, "",
                    "missing Nsight Systems SHA256", "ENV_BLOCKED",
                )
            binary_hashes["qths1:nsys"] = output[0]
            binary_paths["qths1:nsys"] = nsys_path
            profiler_evidence = {
                "tool": "nsys",
                "path": nsys_path,
                "sha256": output[0],
                "version": version,
                "trace": ["cuda", "nvtx", "osrt"],
                "sample": "none",
                "cpuctxsw": "none",
            }
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
            binary_paths[f"qths1:{tool}"] = tool_path
        if plan.topology.uses_receiver:
            ethtool_path = self._diagnostic_paths["ethtool"]
            output = self._ssh(
                "qths1", ["sha256sum", ethtool_path], "CONFIG_READY"
            ).stdout.split()
            if not output:
                raise StageError(
                    "CONFIG_READY", ["sha256sum", ethtool_path], 1, "",
                    "missing ethtool SHA256",
                )
            binary_hashes["qths1:ethtool"] = output[0]
            binary_paths["qths1:ethtool"] = ethtool_path
        for host, sender_ethtool_path in self._sender_ethtool_paths.items():
            output = self._ssh(
                host, ["sha256sum", sender_ethtool_path], "CONFIG_READY"
            ).stdout.split()
            if not output:
                raise StageError(
                    "CONFIG_READY", ["sha256sum", host, sender_ethtool_path],
                    1, "", "missing sender ethtool SHA256",
                )
            binary_hashes[f"{host}:ethtool"] = output[0]
            binary_paths[f"{host}:ethtool"] = sender_ethtool_path
        receiver_preflight = None
        if plan.topology.uses_receiver:
            preflight_argv = [
                str(self.qths_binary_dir / "rdma2dada"),
                "--plan",
                f"{self.remote_run_dir}/resolved_observation.json",
                "--preflight-only",
            ]
            if plan.receiver_poll_cpu is not None:
                preflight_argv += ["--poll-cpu", str(plan.receiver_poll_cpu)]
            receiver_preflight = self._ssh(
                "qths1", preflight_argv, "CONFIG_READY"
            ).stdout
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
            "receiver_preflight": receiver_preflight,
            "sender_config_evidence": list(self._sender_config_evidence),
            "profiler": profiler_evidence,
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

    def _resolve_executable(
        self, tool: str, candidates: Sequence[str], host: str = "qths1"
    ) -> str:
        discovered = self._ssh(
            host, ["which", tool], "CONFIG_READY", check=False
        )
        if discovered.returncode == 0 and discovered.stdout.strip():
            return discovered.stdout.strip().splitlines()[0]
        for candidate in candidates:
            executable = self._ssh(
                host, ["test", "-x", candidate], "CONFIG_READY", check=False
            )
            if executable.returncode == 0:
                return candidate
        raise StageError(
            "CONFIG_READY", ["which", host, tool], 1, "",
            f"required executable is unavailable: {tool}", "ENV_BLOCKED",
        )

    def _capture_sender_nic(self, phase: str) -> None:
        if phase not in ("before", "after"):
            raise ValueError("sender NIC capture phase must be before or after")
        for spec in self._sender_specs:
            self._ssh(
                spec.host,
                [
                    "/usr/bin/python3",
                    f"{self.remote_run_dir}/capture_nic_counters.py",
                    "mlx5_0",
                    self._sender_ethtool_paths.get(
                        spec.host, "/usr/sbin/ethtool"
                    ),
                    f"{self.remote_run_dir}/sender-nic-{phase}.json",
                ],
                "SENDERS_WAITING" if phase == "before" else "COLLECTING",
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
        hashes: dict[str, str] = {}
        for spec in self._sender_specs:
            path = bundle_root / spec.bundle_name
            host = spec.host
            remote_name = spec.remote_name
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
        self._pipeline_stage = plan.pipeline_stage
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
        ring_key_resolvers = {
            "raw": lambda: plan.raw_key,
            "compute": lambda: plan.compute_key,
            "output": lambda: plan.output_key,
        }
        self._rings_acquired = [
            ring_key_resolvers[name]() for name in plan.topology.rings
        ]
        if plan.topology.requires_rdma_capability:
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
        readiness: dict[str, Any] = {
            "rings": list(self._rings_acquired),
            "capability_added": self._capability_added,
        }
        if plan.uses_unpack_worker:
            worker_ready_completed = self._ssh(
                "qths1", ["cat", f"{self.remote_run_dir}/worker.ready"],
                "PIPELINE_READY",
            )
            affinity_completed = self._ssh(
                "qths1", ["cat", f"{self.remote_run_dir}/worker-affinity.txt"],
                "PIPELINE_READY",
            )
            try:
                worker_ready = json.loads(worker_ready_completed.stdout)
            except json.JSONDecodeError as error:
                raise StageError(
                    "PIPELINE_READY", ["parse", "worker.ready"], 1,
                    worker_ready_completed.stdout, repr(error), "HARNESS_FAIL",
                ) from error
            readiness.update({
                "worker_ready": worker_ready,
                "worker_affinity": affinity_completed.stdout.strip(),
            })
        return readiness

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
        manifest_path = self.local_run_dir / "manifest.json"
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
            manifest.setdefault("config", {})["sender_config_evidence"] = list(
                self._sender_config_evidence
            )
            _atomic_json(manifest_path, manifest)

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
        self._capture_sender_nic("before")
        binary = str(self.sender_binary_dir / "fpga_sender_sim")
        started: list[Any] = []
        self._sender_runtime = []
        for spec in self._sender_specs:
            host = spec.host
            config = f"{self.remote_run_dir}/{spec.remote_name}"
            log = run_dir / spec.log_name
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
            self._sender_runtime.append({
                "host": host,
                "role": "sender",
                "station_id": spec.station_id,
                "station_ids": list(spec.station_ids),
                "argv": list(argv),
                "env": {},
                "cpu_affinity": [],
                "numa_node": None,
                "thread_mapping": [],
                "config_sha256": plan.config_id,
                "pid": getattr(process, "pid", None),
                "output_path": str(log),
                "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "ended_utc": None,
                "returncode": None,
                "state": "RUNNING",
                "output": "",
            })
            self._persist_sender_runtime()
        return started

    def _persist_sender_runtime(self) -> None:
        if self._sender_runtime and self.local_run_dir:
            _atomic_json(
                self.local_run_dir / "sender-processes.json",
                self._sender_runtime,
            )

    def wait_senders(self, plan: RatePlan, processes: Sequence[Any]) -> list[str]:
        outputs = [""] * len(processes)
        pending = set(range(len(processes)))
        deadline = time.monotonic() + plan.duration_seconds + 300.0

        def terminate_running(reason: str) -> None:
            for peer_index, peer in enumerate(processes):
                try:
                    if peer.poll() is None:
                        peer.terminate()
                        if peer_index < len(self._sender_runtime):
                            runtime = self._sender_runtime[peer_index]
                            runtime["state"] = reason
                            runtime["ended_utc"] = dt.datetime.now(
                                dt.timezone.utc
                            ).isoformat()
                            runtime["returncode"] = peer.poll()
                except Exception:
                    pass
            self._persist_sender_runtime()

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
                if index < len(self._sender_runtime):
                    runtime = self._sender_runtime[index]
                    runtime["state"] = "EXITED"
                    runtime["ended_utc"] = dt.datetime.now(
                        dt.timezone.utc
                    ).isoformat()
                    runtime["returncode"] = int(returncode)
                    runtime["output"] = output
                    self._persist_sender_runtime()
                if returncode != 0:
                    terminate_running("TERMINATED_BY_PEER_FAILURE")
                    classification = (
                        "ENV_BLOCKED"
                        if "bind source endpoint:" in output
                        else "HARNESS_FAIL" if returncode == 255
                        else "PRODUCT_FAIL"
                    )
                    failure_argv = (
                        self._sender_runtime[index]["argv"]
                        if index < len(self._sender_runtime)
                        else ["fpga_sender_sim", str(index)]
                    )
                    raise StageError(
                        "SENDERS_RUNNING",
                        failure_argv,
                        int(returncode),
                        output,
                        "sender exited non-zero; aborting all Station streams",
                        classification,
                    )
            if not pending:
                break
            if time.monotonic() >= deadline:
                terminate_running("TERMINATED_BY_GROUP_TIMEOUT")
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
        self._capture_sender_nic("after")
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
        qths_process_roles = [
            role for role in required_process_roles(plan.topology)
            if role != "sender"
        ]
        process_files = {
            "rdma2dada": "receiver.process.json",
            "vdif_unpack_worker": "worker.process.json",
            "pipeline_worker": "pipeline-worker.process.json",
            "gpu_pressure_writer": "input-writer.process.json",
            f"{plan.topology.consumer_ring}-consumer": "reader.process.json",
            "raw-ring": "raw-ring.process.json",
            "compute-ring": "compute-ring.process.json",
            "output-ring": "output-ring.process.json",
        }
        process_artifacts = [
            process_files[role] for role in qths_process_roles
            if role in process_files
        ]
        if plan.pipeline_stage == "gpu":
            artifact_names = [
                "pipeline.ready", "pipeline-worker.log", "reader.log",
                "reader.exit", "input-writer.log", "input-writer.exit",
                "gpu-pressure-writer-metrics.json",
                "pipeline-worker-metrics.json",
                "output-summary.json", "MANIFEST.sha256",
                "resolved_observation.json", "ring_plan.json",
                "unpacked.header", "output.header", "validation_report.json",
            ] + process_artifacts
            for name in artifact_names:
                self.transport.run(
                    self._scp_argv(
                        f"qths1:{self.remote_run_dir}/{name}",
                        str(qths_copy / name),
                    ),
                    "COLLECTING", True,
                )
            if self.pipeline_profiler == "nsys":
                self.transport.run(
                    self._scp_argv(
                        f"qths1:{self.remote_run_dir}/nsys",
                        str(qths_copy),
                        recursive=True,
                    ),
                    "COLLECTING", True,
                )
                reports = sorted((qths_copy / "nsys").glob("*.nsys-rep"))
                if len(reports) != 1:
                    raise StageError(
                        "COLLECTING", ["validate", "nsys-report"], 1,
                        "\n".join(str(path) for path in reports),
                        "expected exactly one Nsight Systems report",
                        "HARNESS_FAIL",
                    )
            pipeline_log = (qths_copy / "pipeline-worker.log").read_text()
            if "pipeline transfer completed" not in pipeline_log:
                raise StageError(
                    "COLLECTING", ["parse", "pipeline-worker.log"], 1,
                    pipeline_log, "missing pipeline worker completed marker",
                    "PRODUCT_FAIL",
                )
            return {
                "input_writer": json.loads(
                    (qths_copy / "gpu-pressure-writer-metrics.json").read_text()
                ),
                "gpu": {
                    "completed": True,
                    "metrics": json.loads(
                        (qths_copy / "pipeline-worker-metrics.json").read_text()
                    ),
                },
                "output": json.loads(
                    (qths_copy / "output-summary.json").read_text()
                ),
            }
        summary_name = f"{plan.topology.consumer_ring}-summary.json"
        artifact_names = [
            "receiver.log", "pipeline.ready", "reader.log",
            "nic-before.json", "nic-after.json", summary_name,
            "MANIFEST.sha256", "resolved_observation.json", "ring_plan.json",
            "raw.header", "validation_report.json",
        ] + process_artifacts
        if plan.uses_unpack_worker:
            artifact_names.extend((
                "worker.log", "worker.ready", "worker-affinity.txt",
                "worker-argv.json", "unpacked.header",
            ))
        if plan.compute_consumer == "dbnull":
            artifact_names.append("reader.exit")
        if plan.uses_pipeline_worker:
            artifact_names.extend((
                "pipeline-worker.log", "pipeline-worker-metrics.json",
                "output.header",
            ))
        for name in artifact_names:
            self.transport.run(
                self._scp_argv(
                    f"qths1:{self.remote_run_dir}/{name}", str(qths_copy / name)
                ),
                "COLLECTING",
                True,
            )
        receiver_log = (qths_copy / "receiver.log").read_text()
        summary = json.loads((qths_copy / summary_name).read_text())
        if plan.pipeline_stage == "receive":
            statistics = parse_receive_statistics(receiver_log, summary)
        else:
            pipeline_worker_log = ""
            if plan.uses_pipeline_worker:
                pipeline_worker_log = (qths_copy / "pipeline-worker.log").read_text()
            statistics = parse_qths_statistics(
                receiver_log,
                (qths_copy / "worker.log").read_text(),
                summary,
                pipeline_worker_log,
                expect_pipeline_worker=plan.uses_pipeline_worker,
                expect_missing_per_second=plan.unpack_missing_per_second,
            )
        if plan.uses_pipeline_worker:
            statistics.setdefault("gpu", {})["metrics"] = json.loads(
                (qths_copy / "pipeline-worker-metrics.json").read_text()
            )
        statistics["nic"] = nic_counter_evidence(
            json.loads((qths_copy / "nic-before.json").read_text()),
            json.loads((qths_copy / "nic-after.json").read_text()),
        )
        sender_nic = {}
        for spec in self._sender_specs:
            sender_copy = run_dir / spec.host
            sender_copy.mkdir(exist_ok=True)
            snapshots = {}
            for phase in ("before", "after"):
                name = f"sender-nic-{phase}.json"
                local_path = sender_copy / name
                self.transport.run(
                    self._scp_argv(
                        f"{spec.host}:{self.remote_run_dir}/{name}",
                        str(local_path),
                    ),
                    "COLLECTING", True,
                )
                snapshots[phase] = json.loads(local_path.read_text())
            sender_nic[spec.host] = nic_counter_evidence(
                snapshots["before"], snapshots["after"]
            )
        statistics["sender_nic"] = sender_nic
        return statistics

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
            if self._pipeline_stage == "gpu":
                diagnostic_names = [
                    "pipeline.ready", "reader.log", "reader.exit",
                    "pipeline-worker.log", "input-writer.log",
                    "input-writer.exit", "compute-ring.log",
                    "output-ring.log", "output-summary.json",
                    "pipeline-worker-metrics.json",
                ]
            else:
                diagnostic_names = [
                    "receiver.log", "pipeline.ready", "reader.log",
                    "nic-before.json", "nic-after.json", "raw-ring.log",
                    f"{pipeline_topology(self._pipeline_stage).consumer_ring}"
                    "-summary.json",
                ]
            if self._pipeline_stage in ("unpack", "full"):
                diagnostic_names.extend((
                    "worker.log", "worker.ready", "worker-affinity.txt",
                    "worker-argv.json", "compute-ring.log",
                ))
            if self._compute_consumer == "dbnull":
                diagnostic_names.append("reader.exit")
            if self._pipeline_stage == "full" and self._compute_consumer == "dbnull":
                diagnostic_names.extend(("pipeline-worker.log", "output-ring.log"))
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
            if self._pipeline_stage == "gpu" and self.pipeline_profiler == "nsys":
                try:
                    completed = self.transport.run(
                        self._scp_argv(
                            f"qths1:{self.remote_run_dir}/nsys",
                            str(diagnostic_root),
                            recursive=True,
                        ),
                        "CLEANUP", check=False,
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
            diagnostic_root = run_dir / "qths-cleanup"
            diagnostic_root.mkdir(exist_ok=True)
            for name in (
                "raw-ring.process.json", "compute-ring.process.json",
                "output-ring.process.json", "receiver.process.json",
                "worker.process.json", "pipeline-worker.process.json",
                "reader.process.json", "input-writer.process.json",
            ):
                completed = self.transport.run(
                    self._scp_argv(
                        f"qths1:{self.remote_run_dir}/{name}",
                        str(diagnostic_root / name),
                    ),
                    "CLEANUP", check=False,
                )
                diagnostic = nonzero_diagnostic(completed)
                if diagnostic is not None:
                    diagnostic_errors.append(diagnostic)
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
        for host in dict.fromkeys(spec.host for spec in self._sender_specs):
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
    summary: dict[str, Any], target: float, expected_stations: Sequence[int],
    expected_groups: int,
) -> None:
    scheduled = int(summary.get("scheduled_packets", -1))
    sent = int(summary.get("sent_packets", -2))
    failed = int(summary.get("failed_packets", -1))
    backend = summary.get("backend")
    expected = [int(value) for value in expected_stations]
    if len(expected) == 1:
        station_identity_matches = (
            int(summary.get("station_id", -1)) == expected[0]
            and scheduled == expected_groups
        )
    else:
        observed = [int(value) for value in summary.get("station_ids", [])]
        counts = summary.get("station_counts", [])
        station_identity_matches = (
            observed == expected
            and isinstance(counts, list)
            and len(counts) == len(expected)
            and all(
                int(item.get("station_id", -1)) == station
                and int(item.get("scheduled_packets", -1)) == expected_groups
                and int(item.get("sent_packets", -2)) == expected_groups
                for item, station in zip(counts, expected)
            )
            and scheduled == expected_groups * len(expected)
        )
    payload_prefix = str(summary.get("payload_prefix_hex", ""))
    actual = float(summary.get("actual_payload_gbps", -1.0))
    error = abs(actual - target) / target if target else float("inf")
    if (
        scheduled != sent
        or failed != 0
        or backend != "SENDMMSG"
        or not station_identity_matches
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


def _validate_staged_stream_metrics(
    metrics: dict[str, Any], expected_blocks: int,
    expected_inflight_blocks: int,
) -> list[dict[str, Any]]:
    mismatches: list[dict[str, Any]] = []
    lookahead_enabled = expected_inflight_blocks >= 2
    required = {
        "cuda_h2d_stream_count": 1,
        "cuda_compute_stream_count": expected_inflight_blocks,
        "cuda_d2h_stream_count": expected_inflight_blocks,
        "cuda_submission_policy": (
            "ONE_BLOCK_H2D_LOOKAHEAD" if lookahead_enabled
            else "DEPTH_FIRST_SINGLE_SLOT"
        ),
        "h2d_lookahead_submission_count": (
            max(expected_blocks - 1, 0) if lookahead_enabled else 0
        ),
        "h2d_lookahead_eod_flush_count": (
            1 if lookahead_enabled and expected_blocks else 0
        ),
        "h2d_compute_overlap_sample_count": max(expected_blocks - 1, 0),
    }
    for name, expected in required.items():
        actual = metrics.get(name) if isinstance(metrics, dict) else None
        if actual != expected:
            mismatches.append({
                "field": f"gpu.metrics.{name}",
                "expected": expected,
                "actual": actual,
            })
    overlap_total = (
        metrics.get("h2d_compute_overlap_ns_total")
        if isinstance(metrics, dict) else None
    )
    overlap_max = (
        metrics.get("h2d_compute_overlap_ns_max")
        if isinstance(metrics, dict) else None
    )
    if not isinstance(overlap_total, int) or overlap_total < 0:
        mismatches.append({
            "field": "gpu.metrics.h2d_compute_overlap_ns_total",
            "expected": ">= 0",
            "actual": overlap_total,
        })
    if (
        not isinstance(overlap_max, int)
        or overlap_max < 0
        or isinstance(overlap_total, int) and overlap_max > overlap_total
    ):
        mismatches.append({
            "field": "gpu.metrics.h2d_compute_overlap_ns_max",
            "expected": "in [0, h2d_compute_overlap_ns_total]",
            "actual": overlap_max,
        })
    return mismatches


def _validate_statistics(
    statistics: dict[str, Any], plan: RatePlan, expected_sample_prefix: str
) -> None:
    source_json = plan.resolved_plan.get("source_json", "")
    try:
        source_document = json.loads(source_json)
    except (TypeError, json.JSONDecodeError):
        source_document = {}
    cuda_pipeline = source_document.get("processing", {}).get(
        "cuda_pipeline", {}
    )
    expected_execution_mode = cuda_pipeline.get(
        "mode", "SYNCHRONOUS_DIRECT"
    )
    expected_inflight_blocks = int(cuda_pipeline.get("inflight_blocks", 1))

    if plan.pipeline_stage == "gpu":
        pressure = derive_gpu_pressure_input(plan)
        planned_writer_gbps = (
            pressure.bytes_per_second * 8 / 1_000_000_000
        )
        required = {
            ("input_writer", "blocks_per_second"): pressure.blocks_per_second,
            ("input_writer", "planned_blocks"): pressure.block_count,
            ("input_writer", "published_blocks"): pressure.block_count,
            ("input_writer", "block_bytes"): plan.compute_block_bytes,
            ("input_writer", "published_bytes"): pressure.total_bytes,
            ("input_writer", "eod_sent"): True,
            ("gpu", "completed"): True,
            ("gpu", "metrics", "execution_mode"): expected_execution_mode,
            ("gpu", "metrics", "inflight_blocks"): expected_inflight_blocks,
            ("gpu", "metrics", "blocks"): pressure.block_count,
            ("gpu", "metrics", "input_bytes"): pressure.total_bytes,
            ("gpu", "metrics", "output_bytes"): (
                pressure.block_count * plan.output_block_bytes
            ),
            ("output", "exit_code"): 0,
        }
        if plan.compute_consumer == "dbnull":
            required.update({
                ("output", "consumer"): "dada_dbnull",
                ("output", "zero_copy"): True,
                ("output", "single_transfer"): True,
            })
        mismatches = []
        for path, expected in required.items():
            actual: Any = statistics
            for name in path:
                actual = actual.get(name) if isinstance(actual, dict) else None
            if actual != expected:
                mismatches.append({
                    "field": ".".join(path),
                    "expected": expected,
                    "actual": actual,
                })
        writer_metrics = statistics.get("input_writer", {})
        actual_writer_gbps = (
            writer_metrics.get("actual_payload_gbps")
            if isinstance(writer_metrics, dict) else None
        )
        writer_elapsed_ns = (
            writer_metrics.get("active_elapsed_ns")
            if isinstance(writer_metrics, dict) else None
        )
        rate_error = (
            abs(float(actual_writer_gbps) - planned_writer_gbps)
            / planned_writer_gbps
            if isinstance(actual_writer_gbps, (int, float))
            and actual_writer_gbps > 0.0 else float("inf")
        )
        if rate_error > 0.02:
            mismatches.append({
                "field": "input_writer.actual_payload_gbps",
                "expected": f"within 2% of {planned_writer_gbps}",
                "actual": actual_writer_gbps,
            })
        expected_elapsed_ns = pressure.duration_seconds * 1_000_000_000
        if (
            not isinstance(writer_elapsed_ns, int)
            or writer_elapsed_ns <= 0
            or writer_elapsed_ns > expected_elapsed_ns * 102 // 100
        ):
            mismatches.append({
                "field": "input_writer.active_elapsed_ns",
                "expected": f"in (0, {expected_elapsed_ns * 102 // 100}]",
                "actual": writer_elapsed_ns,
            })
        gpu_metrics = statistics.get("gpu", {}).get("metrics", {})
        if expected_execution_mode == "STAGED_PIPELINE":
            staged_required = {
                "submitted_blocks": pressure.block_count,
                "completed_blocks": pressure.block_count,
                "published_blocks": pressure.block_count,
                "input_staging_bytes": 0,
                "input_staging_copy_ns_total": 0,
                "input_staging_copy_ns_max": 0,
                "input_ring_cuda_registered": True,
                "registered_ring_blocks": plan.compute_ring_blocks,
                "registered_ring_bytes": (
                    plan.compute_ring_blocks * plan.compute_block_bytes
                ),
                "output_staging_bytes": (
                    pressure.block_count * plan.output_block_bytes
                ),
            }
            for name, expected in staged_required.items():
                actual = (
                    gpu_metrics.get(name)
                    if isinstance(gpu_metrics, dict) else None
                )
                if actual != expected:
                    mismatches.append({
                        "field": f"gpu.metrics.{name}",
                        "expected": expected,
                        "actual": actual,
                    })
            mismatches.extend(_validate_staged_stream_metrics(
                gpu_metrics, pressure.block_count,
                expected_inflight_blocks,
            ))
            registration_ns = (
                gpu_metrics.get("input_ring_registration_ns")
                if isinstance(gpu_metrics, dict) else None
            )
            if (
                not isinstance(registration_ns, int)
                or registration_ns <= 0
            ):
                mismatches.append({
                    "field": "gpu.metrics.input_ring_registration_ns",
                    "expected": "> 0",
                    "actual": registration_ns,
                })
            max_inflight = (
                gpu_metrics.get("max_inflight")
                if isinstance(gpu_metrics, dict) else None
            )
            if (
                not isinstance(max_inflight, int)
                or max_inflight < 1
                or max_inflight > expected_inflight_blocks
            ):
                mismatches.append({
                    "field": "gpu.metrics.max_inflight",
                    "expected": f"1..{expected_inflight_blocks}",
                    "actual": max_inflight,
                })
        rate_fields = (
            ("active_elapsed_ns", "active_input_payload_gbps")
            if expected_execution_mode == "STAGED_PIPELINE"
            else ("transfer_elapsed_ns", "input_payload_gbps")
        )
        for name in rate_fields:
            actual = gpu_metrics.get(name) if isinstance(gpu_metrics, dict) else None
            if not isinstance(actual, (int, float)) or actual <= 0:
                mismatches.append({
                    "field": f"gpu.metrics.{name}",
                    "expected": "> 0",
                    "actual": actual,
                })
        if mismatches:
            raise StageError(
                "COLLECTING", ["validate", "gpu-statistics"], 1,
                json.dumps(statistics, sort_keys=True),
                json.dumps(mismatches, sort_keys=True), "PRODUCT_FAIL",
            )
        return
    expected_receiver_records = plan.sender_group_count * plan.nant
    expected_unpack_records = plan.group_count * plan.nant
    expected_data_bytes = plan.group_count * plan.payload_bytes * plan.nant
    required = {
        ("receiver", "wrong_length"): 0,
        ("receiver", "cq_errors"): 0,
    }
    if not plan.unpack_start_delay_seconds:
        required.update({
            ("receiver", "accepted"): expected_receiver_records,
            ("receiver", "published"): expected_receiver_records,
        })
    if plan.pipeline_stage == "receive":
        required.update({
            ("raw", "consumer"): "dada_dbnull",
            ("raw", "exit_code"): 0,
            ("raw", "zero_copy"): True,
            ("raw", "single_transfer"): True,
        })
    else:
        required.update({
            ("unpack", "records"): expected_unpack_records,
            ("unpack", "accepted"): expected_unpack_records,
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
        })
    if plan.pipeline_stage != "receive" and plan.compute_consumer == "dbnull":
        sink_section = "output" if plan.uses_pipeline_worker else "compute"
        required.update(
            {
                (sink_section, "consumer"): "dada_dbnull",
                (sink_section, "exit_code"): 0,
                (sink_section, "zero_copy"): True,
                (sink_section, "single_transfer"): True,
            }
        )
        if plan.uses_pipeline_worker:
            required[("gpu", "completed")] = True
            required[("gpu", "metrics", "execution_mode")] = (
                expected_execution_mode
            )
            required[("gpu", "metrics", "inflight_blocks")] = (
                expected_inflight_blocks
            )
    elif plan.pipeline_stage != "receive":
        if plan.uses_pipeline_worker:
            groups_per_block = plan.records_per_block // plan.nant
            output_blocks = (
                plan.group_count + groups_per_block - 1
            ) // groups_per_block
            output_contract = plan.resolved_plan.get("output_contract", {})
            required.update({
                ("output", "data_bytes"): (
                    output_blocks * plan.output_block_bytes
                ),
                ("output", "DATA_STAGE"): output_contract.get("data_stage"),
                ("output", "ORDER"): output_contract.get("order"),
                ("output", "CONFIG_ID"): plan.config_id,
                ("output", "GEOMETRY_ID"): plan.geometry_id,
            })
        else:
            required.update({
                ("compute", "data_bytes"): expected_data_bytes,
                ("compute", "DATA_STAGE"): "UNPACKED",
                ("compute", "ORDER"): "ATFP",
                ("compute", "NANT"): plan.nant,
                ("compute", "NCHAN"): plan.nchan,
                ("compute", "NPOL"): plan.npol,
                ("compute", "CONFIG_ID"): plan.config_id,
                ("compute", "GEOMETRY_ID"): plan.geometry_id,
                ("compute", "sample_prefix_hex"): expected_sample_prefix,
            })
    mismatches = []
    if plan.unpack_start_delay_seconds:
        receiver = statistics.get("receiver", {})
        accepted = int(receiver.get("accepted", -1))
        published = int(receiver.get("published", -2))
        if (
            accepted != published
            or accepted < expected_unpack_records
            or accepted > expected_receiver_records
        ):
            mismatches.append({
                "field": "receiver.preparation_range",
                "expected": (
                    f"accepted=published in "
                    f"[{expected_unpack_records},{expected_receiver_records}]"
                ),
                "actual": {"accepted": accepted, "published": published},
            })
    for path, expected in required.items():
        actual: Any = statistics
        for name in path:
            actual = actual.get(name) if isinstance(actual, dict) else None
        if actual != expected:
            mismatches.append(
                {"field": ".".join(path), "expected": expected,
                 "actual": actual}
            )
    if plan.uses_pipeline_worker and expected_execution_mode == "STAGED_PIPELINE":
        groups_per_block = plan.records_per_block // plan.nant
        expected_blocks = (
            plan.group_count + groups_per_block - 1
        ) // groups_per_block
        if (plan.output_block_bytes * plan.group_count) % groups_per_block:
            raise StageError(
                "COLLECTING", ["validate", "pipeline-statistics"], 1, "",
                "resolved output bytes are not integral for the final "
                "partial compute block", "HARNESS_FAIL",
            )
        expected_output_bytes = (
            plan.output_block_bytes * plan.group_count // groups_per_block
        )
        metrics = statistics.get("gpu", {}).get("metrics", {})
        staged_required = {
            "blocks": expected_blocks,
            "submitted_blocks": expected_blocks,
            "completed_blocks": expected_blocks,
            "published_blocks": expected_blocks,
            "input_staging_bytes": 0,
            "input_staging_copy_ns_total": 0,
            "input_staging_copy_ns_max": 0,
            "input_ring_cuda_registered": True,
            "registered_ring_blocks": plan.compute_ring_blocks,
            "registered_ring_bytes": (
                plan.compute_ring_blocks * plan.compute_block_bytes
            ),
            "output_staging_bytes": expected_output_bytes,
        }
        for key, expected in staged_required.items():
            actual = metrics.get(key) if isinstance(metrics, dict) else None
            if actual != expected:
                mismatches.append({
                    "field": f"gpu.metrics.{key}",
                    "expected": expected,
                    "actual": actual,
                })
        mismatches.extend(_validate_staged_stream_metrics(
            metrics, expected_blocks, expected_inflight_blocks,
        ))
        registration_ns = (
            metrics.get("input_ring_registration_ns")
            if isinstance(metrics, dict) else None
        )
        if not isinstance(registration_ns, int) or registration_ns <= 0:
            mismatches.append({
                "field": "gpu.metrics.input_ring_registration_ns",
                "expected": "> 0",
                "actual": registration_ns,
            })
        max_inflight = (
            metrics.get("max_inflight") if isinstance(metrics, dict) else None
        )
        if (
            not isinstance(max_inflight, int)
            or max_inflight < 1
            or max_inflight > expected_inflight_blocks
        ):
            mismatches.append({
                "field": "gpu.metrics.max_inflight",
                "expected": f"1..{expected_inflight_blocks}",
                "actual": max_inflight,
            })
        for key in ("active_elapsed_ns", "active_input_payload_gbps"):
            actual = metrics.get(key) if isinstance(metrics, dict) else None
            if not isinstance(actual, (int, float)) or actual <= 0:
                mismatches.append({
                    "field": f"gpu.metrics.{key}",
                    "expected": "> 0",
                    "actual": actual,
                })
    if mismatches:
        performance_fields = {
            "receiver.accepted",
            "receiver.published",
            "receiver.preparation_range",
            "unpack.records",
            "unpack.accepted",
            "unpack.complete_groups",
            "unpack.incomplete_groups",
            "unpack.fully_missing_groups",
            "unpack.missing_station",
        }
        classification = "PRODUCT_FAIL"
        if (
            plan.compute_consumer == "dbnull"
            and all(item["field"] in performance_fields for item in mismatches)
        ):
            classification = "PERFORMANCE_FAIL"
        raise StageError(
            "COLLECTING",
            ["validate", "pipeline-statistics"],
            1,
            json.dumps(statistics, sort_keys=True),
            json.dumps(mismatches, sort_keys=True),
            classification,
        )


def _parse_receiver_statistics(receiver_log: str) -> dict[str, Any]:
    receiver_match = re.search(
        r"Receive summary:\s*accepted=(\d+),\s*wrong_length=(\d+),\s*"
        r"(?:zeroed=(\d+),\s*)?published=(\d+),\s*blocks=(\d+),\s*"
        r"partial_blocks=(\d+),\s*"
        r"cq_tail_records=(\d+)",
        receiver_log,
    )
    if not receiver_match:
        raise StageError(
            "COLLECTING", ["parse", "receiver.log"], 1,
            receiver_log, "missing receiver summary",
        )
    cq_patterns = (
        "Fatal receive completion", "Failed to poll direct raw CQ",
        "Failed to post receive WR batch", "Invalid direct raw slot completion",
    )
    result: dict[str, Any] = {
        "accepted": int(receiver_match.group(1)),
        "wrong_length": int(receiver_match.group(2)),
        "zeroed": int(receiver_match.group(3) or 0),
        "published": int(receiver_match.group(4)),
        "blocks": int(receiver_match.group(5)),
        "partial_blocks": int(receiver_match.group(6)),
        "cq_tail_records": int(receiver_match.group(7)),
        "cq_errors": sum(receiver_log.count(pattern) for pattern in cq_patterns),
    }
    direct_match = re.search(
        r"Direct receive summary:\s*poll_calls=(\d+),\s*"
        r"empty_polls=(\d+),\s*full_polls=(\d+),\s*"
        r"reposted_wrs=(\d+),\s*repost_failures=(\d+),\s*"
        r"repost_batches=(\d+),\s*min_posted_wrs=(\d+),\s*"
        r"poll_batch_high_watermark=(\d+),\s*"
        r"completion_to_repost_ns_total=(\d+),\s*"
        r"completion_to_repost_ns_max=(\d+)"
        r"(?:,\s*drain_duration_ns=(\d+),\s*"
        r"completions_after_stop=(\d+),\s*exit_reason=([A-Z_]+))?",
        receiver_log,
    )
    if direct_match:
        numeric_groups = direct_match.groups()[:10]
        result["direct"] = dict(zip(
            (
                "poll_calls", "empty_polls", "full_polls",
                "reposted_wrs", "repost_failures", "repost_batches",
                "min_posted_wrs", "poll_batch_high_watermark",
                "completion_to_repost_ns_total",
                "completion_to_repost_ns_max",
            ),
            (int(value) for value in numeric_groups),
        ))
        if direct_match.group(11) is not None:
            result["direct"].update({
                "drain_duration_ns": int(direct_match.group(11)),
                "completions_after_stop": int(direct_match.group(12)),
                "exit_reason": direct_match.group(13),
            })
    return result


def _parse_dbnull_summary(summary: dict[str, Any], filename: str) -> dict[str, Any]:
    try:
        return {
            "consumer": "dada_dbnull",
            "exit_code": int(summary["exit_code"]),
            "zero_copy": summary["zero_copy"] is True,
            "single_transfer": summary["single_transfer"] is True,
        }
    except (KeyError, TypeError, ValueError) as error:
        raise StageError(
            "COLLECTING", ["parse", filename], 1,
            json.dumps(summary, sort_keys=True), repr(error),
        ) from error


def parse_receive_statistics(
    receiver_log: str,
    raw_summary: dict[str, Any],
) -> dict[str, Any]:
    return {
        "receiver": _parse_receiver_statistics(receiver_log),
        "raw": _parse_dbnull_summary(raw_summary, "raw-summary.json"),
    }


def parse_qths_statistics(
    receiver_log: str,
    worker_log: str,
    compute_summary: dict[str, Any],
    pipeline_worker_log: str = "",
    expect_pipeline_worker: bool = True,
    expect_missing_per_second: bool = False,
) -> dict[str, Any]:
    receiver = _parse_receiver_statistics(receiver_log)
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
    unpack_diagnostics_match = re.search(
        r"large_gap_advances=(\d+)/(\d+)\s+"
        r"max_station_ordinal_skew=(\d+)\s+"
        r"raw_blocks_single=(\d+)\s+raw_blocks_mixed=(\d+)\s+"
        r"max_station_records_per_raw_block=(\d+)\s+"
        r"max_consecutive_station_records=(\d+)",
        worker_log,
    )
    station_matches = re.findall(
        r"VDIF unpack station statistics:\s*antenna=(\d+)\s+"
        r"station=(\d+)\s+observed=(\d+)\s+accepted=(\d+)\s+"
        r"late=(\d+)\s+highest_ordinal=(\d+)",
        worker_log,
    )
    if not unpack_diagnostics_match or not station_matches:
        raise StageError(
            "COLLECTING",
            ["parse", "worker.log"],
            1,
            worker_log,
            "missing unpack skew or per-Station diagnostics",
        )
    summary_section = "output" if expect_pipeline_worker else "compute"
    summary_filename = f"{summary_section}-summary.json"
    if compute_summary.get("consumer") == "dada_dbnull":
        sink = _parse_dbnull_summary(compute_summary, summary_filename)
    else:
        header = compute_summary.get("header", {})
        try:
            sink = {
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
                ["parse", summary_filename],
                1,
                json.dumps(compute_summary, sort_keys=True),
                repr(error),
            ) from error
    unpack = {
        name: int(value)
        for name, value in zip(unpack_names, unpack_match.groups())
        if name != "expected_station"
    }
    diagnostic_names = (
        "large_gap_advances",
        "large_gap_advanced_groups",
        "max_station_ordinal_skew",
        "single_station_raw_blocks",
        "mixed_station_raw_blocks",
        "max_station_records_per_raw_block",
        "max_consecutive_station_records",
    )
    unpack.update(
        {
            name: int(value)
            for name, value in zip(
                diagnostic_names, unpack_diagnostics_match.groups()
            )
        }
    )
    unpack["station"] = [
        {
            "antenna": int(antenna),
            "station": int(station),
            "observed": int(observed),
            "accepted": int(accepted),
            "late": int(late),
            "highest_ordinal": int(highest),
        }
        for antenna, station, observed, accepted, late, highest
        in station_matches
    ]
    missing_second_matches = re.findall(
        r"VDIF missing per second:\s*second_index=(\d+)\s+"
        r"vdif_seconds=(\d+)\s+missing=(\d+)",
        worker_log,
    )
    if expect_missing_per_second and not missing_second_matches:
        raise StageError(
            "COLLECTING",
            ["parse", "worker.log"],
            1,
            worker_log,
            "missing per-second unpack diagnostics",
        )
    if missing_second_matches:
        missing_per_second = [
            {
                "second_index": int(second_index),
                "vdif_seconds": int(vdif_seconds),
                "missing": int(missing),
            }
            for second_index, vdif_seconds, missing in missing_second_matches
        ]
        first_vdif_second = missing_per_second[0]["vdif_seconds"]
        for expected_index, item in enumerate(missing_per_second):
            if (
                item["second_index"] != expected_index
                or item["vdif_seconds"] != first_vdif_second + expected_index
            ):
                raise StageError(
                    "COLLECTING",
                    ["parse", "worker.log"],
                    1,
                    worker_log,
                    "per-second unpack diagnostics are not contiguous",
                )
        if sum(item["missing"] for item in missing_per_second) != unpack[
            "missing_station"
        ]:
            raise StageError(
                "COLLECTING",
                ["parse", "worker.log"],
                1,
                worker_log,
                "per-second missing sum does not match missing_station",
            )
        unpack["missing_packets_per_second"] = missing_per_second
    result = {
        "receiver": receiver,
        "unpack": unpack,
        summary_section: sink,
    }
    if compute_summary.get("consumer") == "dada_dbnull" and expect_pipeline_worker:
        if "pipeline transfer completed" not in pipeline_worker_log:
            raise StageError(
                "COLLECTING",
                ["parse", "pipeline-worker.log"],
                1,
                pipeline_worker_log,
                "missing pipeline worker completed marker",
                "PRODUCT_FAIL",
            )
        result["gpu"] = {"completed": True, "timing_evidence": "UNAVAILABLE"}
    return result


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
        entry = {
            "state": state,
            "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            **extra,
        }
        _atomic_json(self.run_dir / "state.json", entry)
        history_path = self.run_dir / "state-history.jsonl"
        with history_path.open("a") as stream:
            stream.write(json.dumps(entry, sort_keys=True) + "\n")

    def _manifest(self, plan: RatePlan) -> dict[str, Any]:
        controller_path = pathlib.Path(__file__).resolve()
        sender_hosts = (
            [spec.host for spec in sender_specs_for_plan(plan, self.run_id)]
            if plan.topology.uses_network_senders else []
        )
        return {
            "run_id": self.run_id,
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "hosts": ["HF", "qths1", *sender_hosts],
            "plan": plan.as_dict(),
            "controller": str(controller_path),
            "controller_sha256": hashlib.sha256(controller_path.read_bytes()).hexdigest(),
            "launcher": {
                "argv": list(sys.argv),
                "pid": os.getpid(),
                "pgid": os.getpgrp(),
            },
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
            manifest = json.loads((self.run_dir / "manifest.json").read_text())
            manifest["pipeline_ready"] = {
                name: value for name, value in acquired.items()
                if name not in ("rings", "capability_added")
            }
            _atomic_json(self.run_dir / "manifest.json", manifest)
            self._state("PIPELINE_READY", **config,
                        readiness=manifest["pipeline_ready"])
            summaries: list[dict[str, Any]] = []
            if plan.topology.uses_network_senders:
                processes = self.backend.start_senders(plan, self.run_dir)
                resources.processes = list(processes)
                self._state("SENDERS_WAITING")
                sender_outputs = self.backend.wait_senders(plan, processes)
                self._state("SENDERS_RUNNING")
                summaries = [
                    _parse_sender_summary(output) for output in sender_outputs
                ]
                expected_specs = sender_specs_for_plan(
                    plan, sender_source_identity(plan, self.run_id)
                )
                if len(summaries) != len(expected_specs):
                    raise StageError(
                        "SENDERS_RUNNING",
                        ["validate", "sender-count"],
                        1,
                        str(len(summaries)),
                        str(len(expected_specs)),
                        "PRODUCT_FAIL",
                    )
                for summary, spec in zip(
                    summaries, expected_specs
                ):
                    _validate_sender(
                        summary,
                        _sender_target_gbps(plan, len(spec.station_ids)),
                        spec.station_ids,
                        plan.sender_group_count,
                    )
            self._state("COLLECTING")
            statistics = self.backend.collect(plan, self.run_dir)
            _validate_statistics(
                statistics,
                plan,
                str(summaries[0]["payload_prefix_hex"]) if summaries else "",
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
        compiler_executor = (
            self.backend.observation_compiler(compiler, self.run_dir)
            if hasattr(self.backend, "observation_compiler")
            else None
        )
        try:
            compile_arguments = (
                {"compiler_executor": compiler_executor}
                if compiler_executor is not None
                else {}
            )
            plan = compile_rate_plan(
                request, observation_template, compiler, self.run_dir,
                **compile_arguments,
            )
        except StageError as error:
            return self._compile_failure_result(request, error)
        except Exception as error:
            return self._compile_failure_result(request, error)
        finally:
            if compiler_executor is not None:
                compiler_executor.close()
        return self.run(plan)

    def preflight_request(
        self,
        request: RateRequest,
        observation_template: pathlib.Path,
        compiler: pathlib.Path,
    ) -> dict[str, Any]:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self._state("CONFIG_PREFLIGHT", mode="preflight-only")
        compiler_executor = (
            self.backend.observation_compiler(compiler, self.run_dir)
            if hasattr(self.backend, "observation_compiler")
            else None
        )
        try:
            compile_arguments = (
                {"compiler_executor": compiler_executor}
                if compiler_executor is not None
                else {}
            )
            plan = compile_rate_plan(
                request, observation_template, compiler, self.run_dir,
                **compile_arguments,
            )
        except StageError as error:
            return self._compile_failure_result(
                request, error, mode="preflight-only"
            )
        except Exception as error:
            return self._compile_failure_result(
                request, error, mode="preflight-only"
            )
        finally:
            if compiler_executor is not None:
                compiler_executor.close()
        return self.preflight(plan)


_EVIDENCE_LINE = re.compile(
    r"(?:^\s*\{.*\}\s*$|Receive summary:|Direct receive summary:|"
    r"VDIF unpack statistics:|VDIF unpack station statistics:|"
    r"VDIF missing per second:|transfer (?:opened|completed)|\bEOD\b|"
    r"\b(?:ERROR|WARNING|WARN|FAIL|FATAL)\b|Could not |"
    r"Created DADA |Destroyed DADA )",
    re.IGNORECASE,
)


def _suite_identity_matches(name: str, existing: bytes, candidate: bytes) -> bool:
    if name != "resolved_observation.json":
        return existing == candidate
    try:
        existing_json = json.loads(existing)
        candidate_json = json.loads(candidate)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return existing == candidate
    if not isinstance(existing_json, dict) or not isinstance(candidate_json, dict):
        return existing_json == candidate_json
    existing_json.pop("source_path", None)
    candidate_json.pop("source_path", None)
    return existing_json == candidate_json


def _copy_suite_identity(run_dir: pathlib.Path, suite_root: pathlib.Path,
                         result: dict[str, Any]) -> None:
    """Copy immutable configuration identity once per compact suite."""
    candidates: dict[str, pathlib.Path] = {
        "observation.json": run_dir / "observation.json",
        "resolved_observation.json": (
            run_dir / "observation-artifacts" / "resolved_observation.json"
        ),
    }
    plan = result.get("plan", {})
    resolved = plan.get("resolved_plan", {}) if isinstance(plan, dict) else {}
    generated: dict[str, bytes] = {}
    if not candidates["resolved_observation.json"].is_file() and resolved:
        generated["resolved_observation.json"] = (
            json.dumps(resolved, indent=2, sort_keys=True) + "\n"
        ).encode()
    if not candidates["observation.json"].is_file() and resolved:
        source_json = resolved.get("source_json")
        if isinstance(source_json, str):
            try:
                source = json.loads(source_json)
            except json.JSONDecodeError:
                source = None
            if source is not None:
                generated["observation.json"] = (
                    json.dumps(source, indent=2, sort_keys=True) + "\n"
                ).encode()
    for name, source in candidates.items():
        contents = source.read_bytes() if source.is_file() else generated.get(name)
        if contents is None:
            continue
        destination = suite_root / name
        if destination.exists() and not _suite_identity_matches(
            name, destination.read_bytes(), contents
        ):
            raise ValueError(f"suite configuration identity changed: {name}")
        if not destination.exists():
            destination.write_bytes(contents)


def _load_json_file(path: pathlib.Path, default: Any) -> Any:
    if not path.is_file():
        return default
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return default


def _compact_processes(run_dir: pathlib.Path,
                       manifest: dict[str, Any]) -> list[dict[str, Any]]:
    processes: list[dict[str, Any]] = []
    launcher = manifest.get("launcher")
    if isinstance(launcher, dict):
        processes.append({"host": "HF", "role": "controller", **launcher})
    configured = manifest.get("config", {}).get("processes", [])
    if isinstance(configured, list):
        processes.extend(item for item in configured if isinstance(item, dict))
    sender_runtime = _load_json_file(run_dir / "sender-processes.json", [])
    if isinstance(sender_runtime, list):
        processes.extend(item for item in sender_runtime if isinstance(item, dict))
    role_by_file = {
        "raw-ring.process.json": "raw-ring",
        "compute-ring.process.json": "compute-ring",
        "output-ring.process.json": "output-ring",
        "receiver.process.json": "rdma2dada",
        "worker.process.json": "vdif_unpack_worker",
        "pipeline-worker.process.json": "pipeline_worker",
        "input-writer.process.json": "gpu_pressure_writer",
    }
    plan = manifest.get("plan", {})
    consumer_ring = pipeline_topology(
        str(plan.get("pipeline_stage", "full"))
    ).consumer_ring if isinstance(plan, dict) else "output"
    role_by_file["reader.process.json"] = f"{consumer_ring}-consumer"
    qths_records: dict[str, dict[str, Any]] = {}
    for root_name in ("qths", "qths-cleanup"):
        root = run_dir / root_name
        for file_name, role in role_by_file.items():
            value = _load_json_file(root / file_name, None)
            if isinstance(value, dict):
                value = dict(value)
                value["host"] = "qths1"
                value["role"] = role
                qths_records[role] = value
    binary_sha = manifest.get("config", {}).get("binary_sha", {})
    binary_path = manifest.get("config", {}).get("binary_path", {})
    consumer_tool = (
        "dada_dbnull"
        if isinstance(plan, dict) and plan.get("compute_consumer") == "dbnull"
        else "dada_dbdisk"
    )
    binary_role = {
        "raw-ring": "dada_db",
        "compute-ring": "dada_db",
        "output-ring": "dada_db",
        "rdma2dada": "rdma2dada",
        "vdif_unpack_worker": "vdif_unpack_worker",
        "pipeline_worker": "pipeline_worker",
        "gpu_pressure_writer": "gpu_pressure_writer",
        f"{consumer_ring}-consumer": consumer_tool,
    }
    worker_affinity = ""
    for root_name in ("qths", "qths-cleanup"):
        affinity_path = run_dir / root_name / "worker-affinity.txt"
        if affinity_path.is_file():
            worker_affinity = affinity_path.read_text(errors="replace").strip()
    for role, value in qths_records.items():
        binary_name = binary_role.get(role)
        key = f"qths1:{binary_name}" if binary_name else ""
        value["binary_sha256"] = binary_sha.get(key)
        value["binary_path"] = binary_path.get(key)
        value["config_sha256"] = (
            plan.get("config_id") if isinstance(plan, dict) else None
        )
        value.setdefault("thread_mapping", [])
        if role == "vdif_unpack_worker" and worker_affinity:
            value["thread_mapping"] = worker_affinity.splitlines()
        processes.append(value)
    for value in processes:
        if value.get("role") == "sender":
            host = value.get("host")
            key = f"{host}:fpga_sender_sim"
            value["binary_sha256"] = binary_sha.get(key)
            value["binary_path"] = binary_path.get(key)
            value.setdefault(
                "config_sha256",
                plan.get("config_id") if isinstance(plan, dict) else None,
            )
            value.setdefault("thread_mapping", [])
    return processes


def _compact_evidence(run_dir: pathlib.Path, result: dict[str, Any]) -> str:
    lines = [
        "[controller] TEST_RESULT=" + str(result.get("TEST_RESULT", "UNKNOWN")),
        "[controller] CLEANUP_RESULT=" + str(
            result.get("cleanup", {}).get("CLEANUP_RESULT", "UNKNOWN")
        ),
    ]
    for path in sorted(run_dir.rglob("*.log")):
        try:
            relative = path.relative_to(run_dir)
            source_lines = path.read_text(errors="replace").splitlines()
        except OSError:
            continue
        keep_complete_failure_log = (
            result.get("TEST_RESULT") != "PASS"
            and path.name == "pipeline-worker.log"
        )
        for line in source_lines:
            if line and (keep_complete_failure_log or _EVIDENCE_LINE.search(line)):
                lines.append(f"[{relative}] {line}")
    for relative_name in (
        "qths/pipeline-worker-metrics.json",
        "qths-cleanup/pipeline-worker-metrics.json",
    ):
        path = run_dir / relative_name
        if not path.is_file():
            continue
        for line in path.read_text(errors="replace").splitlines():
            lines.append(f"[{relative_name}] {line}")
    return "\n".join(lines) + "\n"


def _write_failure_debug(suite_root: pathlib.Path, run_id: str,
                         result: dict[str, Any]) -> None:
    failure = result.get("failure")
    if result.get("TEST_RESULT") == "PASS" or not isinstance(failure, dict):
        return
    debug = suite_root / "debug" / run_id
    debug.mkdir(parents=True, exist_ok=True)
    _atomic_json(debug / "failed-command.json", {
        "stage": failure.get("stage"),
        "argv": failure.get("argv", []),
        "exit_code": failure.get("exit_code"),
        "classification": failure.get(
            "classification", result.get("TEST_RESULT")
        ),
    })
    stdout = str(failure.get("stdout", ""))
    stderr = str(failure.get("stderr", ""))
    (debug / "failed-process.log").write_text(
        stdout.rstrip("\n") + "\n" + stderr.rstrip("\n") + "\n"
    )
    run_dir = suite_root / run_id
    for candidate in (
        run_dir / "qths" / "pipeline-worker.log",
        run_dir / "qths-cleanup" / "pipeline-worker.log",
    ):
        if candidate.is_file():
            shutil.copyfile(candidate, debug / "pipeline-worker.log")
            break
    _atomic_json(debug / "resource-snapshot.json", {
        "cleanup": result.get("cleanup", {}),
        "statistics": result.get("statistics", {}),
    })


def _compact_profiler_artifacts(
    suite_root: pathlib.Path, run_dir: pathlib.Path, run_id: str
) -> dict[str, Any] | None:
    source_roots = [
        root for root in (
            run_dir / "qths" / "nsys",
            run_dir / "qths-cleanup" / "nsys",
        )
        if root.is_dir()
    ]
    if not source_roots:
        return None
    destination_root = suite_root / "profiles" / run_id
    reports: list[dict[str, Any]] = []
    sources: dict[str, pathlib.Path] = {}
    for source_root in source_roots:
        for source in source_root.iterdir():
            if source.is_file():
                sources.setdefault(source.name, source)
    for source in (sources[name] for name in sorted(sources)):
        destination_root.mkdir(parents=True, exist_ok=True)
        destination = destination_root / source.name
        shutil.copy2(source, destination)
        reports.append({
            "path": destination.relative_to(suite_root).as_posix(),
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            "bytes": destination.stat().st_size,
        })
    return {"tool": "nsys", "reports": reports} if reports else None


def compact_suite_run(suite_root: pathlib.Path, run_id: str,
                      result: dict[str, Any]) -> dict[str, Any]:
    """Replace a completed verbose run directory with JSON plus raw evidence."""
    suite_root = pathlib.Path(suite_root)
    run_dir = suite_root / run_id
    runs_root = suite_root / "runs"
    runs_root.mkdir(parents=True, exist_ok=True)
    _copy_suite_identity(run_dir, suite_root, result)
    manifest = _load_json_file(run_dir / "manifest.json", {})
    preflight_path = suite_root / "preflight.json"
    if not preflight_path.exists():
        _atomic_json(preflight_path, {
            "run_id": run_id,
            "created_utc": manifest.get("created_utc"),
            "hosts": manifest.get("hosts", []),
            "plan": manifest.get("plan", {}),
            "profile_evidence": manifest.get("plan", {}).get(
                "profile_evidence", {}
            ),
            "controller": manifest.get("controller"),
            "controller_sha256": manifest.get("controller_sha256"),
            "preparation": manifest.get("preparation", result.get("preparation", {})),
            "config": manifest.get("config", {}),
            "pipeline_ready": manifest.get("pipeline_ready", {}),
        })
    stages = []
    history = run_dir / "state-history.jsonl"
    if history.is_file():
        for line in history.read_text().splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                stages.append(value)
    compact = dict(result)
    profiling = _compact_profiler_artifacts(suite_root, run_dir, run_id)
    if profiling is not None:
        compact["profiling"] = profiling
    compact["processes"] = _compact_processes(run_dir, manifest)
    manifest_plan = manifest.get("plan")
    binary_sha = manifest.get("config", {}).get("binary_sha")
    if (
        result.get("TEST_RESULT") == "PASS"
        and isinstance(manifest_plan, dict)
        and isinstance(binary_sha, dict)
        and binary_sha
    ):
        topology = pipeline_topology(str(manifest_plan["pipeline_stage"]))
        try:
            task8c_artifacts.validate_process_ledger(
                compact["processes"], required_process_roles(topology),
                {"sender": expected_sender_process_count(
                    int(manifest_plan.get("nant", 1))
                )}
                if topology.uses_network_senders else None,
            )
        except ValueError as error:
            compact["TEST_RESULT"] = "HARNESS_FAIL"
            compact["failure"] = {
                "stage": "ARTIFACT_COMPACTION",
                "argv": ["validate", "process-ledger"],
                "exit_code": 1,
                "stdout": "",
                "stderr": str(error),
                "classification": "HARNESS_FAIL",
            }
    compact["stages"] = stages
    evidence_path = runs_root / f"{run_id}.evidence.log"
    evidence_path.write_text(_compact_evidence(run_dir, compact))
    compact["evidence_file"] = evidence_path.name
    compact["evidence_sha256"] = hashlib.sha256(
        evidence_path.read_bytes()
    ).hexdigest()
    _atomic_json(runs_root / f"{run_id}.json", compact)
    _write_failure_debug(suite_root, run_id, compact)
    if run_dir.exists():
        shutil.rmtree(run_dir)
    return compact


def _write_suite_manifest(suite_root: pathlib.Path) -> None:
    task8c_artifacts.write_manifest(suite_root)


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
            result = compact_suite_run(suite_root, run_id, result)
            runs.append(
                {
                    "role": role,
                    "index": index,
                    "run_id": run_id,
                    "TEST_RESULT": result["TEST_RESULT"],
                    "CLEANUP_RESULT": result.get("cleanup", {}).get(
                        "CLEANUP_RESULT", "FAIL"
                    ),
                    "result_path": f"runs/{run_id}.json",
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
        _actual_result_payload_gbps(result)
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
        "actual_aggregate_gbps_source": (
            "input_writer.actual_payload_gbps"
            if plan.pipeline_stage == "gpu"
            else "senders.actual_payload_gbps_sum"
        ),
    }
    _atomic_json(suite_root / "summary.json", summary)
    _write_suite_manifest(suite_root)
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
            result = compact_suite_run(suite_root, run_id, result)
            run = {
                "role": role,
                "index": index,
                "run_id": run_id,
                "TEST_RESULT": result["TEST_RESULT"],
                "CLEANUP_RESULT": result.get("cleanup", {}).get(
                    "CLEANUP_RESULT", "FAIL"
                ),
                "result_path": f"runs/{run_id}.json",
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
        measured_rates.append(_actual_result_payload_gbps(result))
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
        "actual_aggregate_gbps_source": (
            "input_writer.actual_payload_gbps"
            if request.pipeline_stage == "gpu"
            else "senders.actual_payload_gbps_sum"
        ),
    }
    _atomic_json(suite_root / "summary.json", summary)
    _write_suite_manifest(suite_root)
    return summary


def _actual_result_payload_gbps(result: dict[str, Any]) -> float:
    """Return the measured payload rate for the selected input."""
    plan = result.get("plan", {})
    pipeline_stage = (
        plan.get("pipeline_stage") if isinstance(plan, dict) else None
    )
    senders = result.get("senders", [])
    if pipeline_stage != "gpu" and senders:
        return sum(float(sender["actual_payload_gbps"]) for sender in senders)
    writer_metrics = result.get("statistics", {}).get("input_writer", {})
    if isinstance(writer_metrics, dict) and float(
        writer_metrics.get("actual_payload_gbps", 0.0)
    ) > 0.0:
        return float(writer_metrics["actual_payload_gbps"])
    if pipeline_stage == "gpu":
        raise ValueError(
            "GPU result is missing a positive "
            "statistics.input_writer.actual_payload_gbps measurement"
        )
    return sum(float(sender["actual_payload_gbps"]) for sender in senders)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aggregate-gbps", type=float, required=True)
    parser.add_argument("--duration-seconds", type=float, default=10.0)
    parser.add_argument(
        "--missing-wait-ms",
        type=float,
        default=None,
        help=(
            "maximum wait for a missing Station packet in milliseconds"
        ),
    )
    parser.add_argument(
        "--station-skew-reserve-ms",
        type=float,
        default=None,
        help=(
            "additional window capacity for Station watermark skew in "
            "milliseconds"
        ),
    )
    parser.add_argument(
        "--compute-consumer",
        choices=("dbdisk", "dbnull"),
        default="dbdisk",
    )
    parser.add_argument(
        "--pipeline-stage",
        choices=("receive", "unpack", "gpu", "full"),
        default="full",
        help=(
            "receive drains the raw ring; unpack stops at the compute ring; "
            "gpu runs compute ring through pipeline_worker to output ring; "
            "full composes receive, unpack and gpu"
        ),
    )
    parser.add_argument(
        "--pipeline-profiler",
        choices=("none", "nsys"),
        default="none",
        help=(
            "diagnostic-only wrapper for pipeline_worker; nsys is restricted "
            "to the gpu stage and preserves a task-owned .nsys-rep"
        ),
    )
    parser.add_argument(
        "--worker-cpu-list",
        help="ordered coordinator,worker...,writer CPU mapping for unpack",
    )
    parser.add_argument(
        "--gpu-worker-cpu", type=int,
        help="CPU used to launch pipeline_worker in gpu/full stages",
    )
    parser.add_argument("--sink-cpu-list")
    parser.add_argument("--receiver-poll-cpu", type=int)
    parser.add_argument(
        "--numa-node", type=int,
        help="legacy placement: bind ingress and processing to one NUMA node",
    )
    parser.add_argument(
        "--ingress-numa-node", type=int,
        help="NUMA node for raw ring and rdma2dada",
    )
    parser.add_argument(
        "--processing-numa-node", type=int,
        help=(
            "NUMA node for unpack, compute/output rings, GPU worker and sink"
        ),
    )
    parser.add_argument("--receiver-poll-batch", type=int, default=None)
    parser.add_argument("--receiver-wr-num", type=int, default=None)
    parser.add_argument(
        "--station-id",
        type=int,
        choices=(101, 102),
        help="run receive-only with one selected Station",
    )
    parser.add_argument(
        "--sender-source-port",
        type=int,
        help="source UDP port for an explicitly selected Station",
    )
    parser.add_argument(
        "--sender-source-port-101",
        type=int,
        help="fixed UDP source port for Station 101 in multi-Station runs",
    )
    parser.add_argument(
        "--sender-source-port-102",
        type=int,
        help="fixed UDP source port for Station 102 in multi-Station runs",
    )
    parser.add_argument(
        "--unpack-missing-per-second",
        action="store_true",
        help="enable opt-in VDIF missing-packet counts by expected second",
    )
    parser.add_argument(
        "--unpack-start-delay-seconds",
        type=int,
        default=None,
        help=(
            "start sender this many whole VDIF/actual seconds before the "
            "formal acceptance timeline (receive/unpack/full with dbnull)"
        ),
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
        "--baseline-profile",
        type=pathlib.Path,
        help="versioned passing profile for the selected host and topology",
    )
    parser.add_argument(
        "--experiment-name",
        help=(
            "required for a named baseline deviation, or use exactly "
            "bootstrap-<pipeline-stage>-v1 when creating the first baseline"
        ),
    )
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
            "rdma2dada and, when used, vdif_unpack_worker build under test"
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
    explicit_profile_fields = {
        name
        for name, value in (
            ("missing_wait_ms", args.missing_wait_ms),
            ("station_skew_reserve_ms", args.station_skew_reserve_ms),
            ("worker_cpu_list", args.worker_cpu_list),
            ("gpu_worker_cpu", args.gpu_worker_cpu),
            ("sink_cpu_list", args.sink_cpu_list),
            ("receiver_poll_cpu", args.receiver_poll_cpu),
            ("numa_node", args.numa_node),
            ("ingress_numa_node", args.ingress_numa_node),
            ("processing_numa_node", args.processing_numa_node),
            ("receiver_poll_batch", args.receiver_poll_batch),
            ("receiver_wr_num", args.receiver_wr_num),
            ("unpack_start_delay_seconds", args.unpack_start_delay_seconds),
            ("sender_source_port_101", args.sender_source_port_101),
            ("sender_source_port_102", args.sender_source_port_102),
        )
        if value is not None
    }
    request = RateRequest(
        args.aggregate_gbps,
        args.duration_seconds,
        compute_consumer=args.compute_consumer,
        pipeline_stage=args.pipeline_stage,
        missing_wait_ms=args.missing_wait_ms if args.missing_wait_ms is not None else 200.0,
        station_skew_reserve_ms=(
            args.station_skew_reserve_ms
            if args.station_skew_reserve_ms is not None else 200.0
        ),
        worker_cpu_list=args.worker_cpu_list,
        gpu_worker_cpu=args.gpu_worker_cpu,
        sink_cpu_list=args.sink_cpu_list,
        receiver_poll_cpu=args.receiver_poll_cpu,
        numa_node=args.numa_node,
        ingress_numa_node=args.ingress_numa_node,
        processing_numa_node=args.processing_numa_node,
        receiver_poll_batch=(
            args.receiver_poll_batch if args.receiver_poll_batch is not None else 32
        ),
        receiver_wr_num=args.receiver_wr_num if args.receiver_wr_num is not None else 1024,
        unpack_missing_per_second=args.unpack_missing_per_second,
        unpack_start_delay_seconds=(
            args.unpack_start_delay_seconds
            if args.unpack_start_delay_seconds is not None else 0
        ),
        station_id=args.station_id,
        sender_source_port=args.sender_source_port,
        sender_source_port_101=args.sender_source_port_101,
        sender_source_port_102=args.sender_source_port_102,
        experiment_name=args.experiment_name,
        pipeline_profiler=args.pipeline_profiler,
    )
    if args.baseline_profile is not None:
        try:
            profile = task8c_profiles.load_profile(args.baseline_profile)
        except ValueError as error:
            print(str(error), file=sys.stderr)
            return 2
        profile_stage_compatible = (
            profile.pipeline_stage == request.pipeline_stage
            or (
                request.pipeline_stage == "full"
                and profile.pipeline_stage == "unpack"
            )
        )
        if profile.target_host != "qths1" or not profile_stage_compatible:
            print(
                "baseline profile host or pipeline stage does not match request",
                file=sys.stderr,
            )
            return 2
        request = task8c_profiles.apply_profile(
            request, profile, explicit_profile_fields
        )
        request = dataclasses.replace(
            request,
            baseline_profile_path=str(profile.path),
            baseline_profile_sha256=profile.sha256,
        )
    if args.dry_run:
        request.validate()
        print(json.dumps(dataclasses.asdict(request), indent=2, sort_keys=True))
        return 0
    if args.baseline_profile is None:
        expected_bootstrap = f"bootstrap-{request.pipeline_stage}-v1"
        if args.experiment_name != expected_bootstrap:
            print(
                "a baseline profile is required for formal execution; "
                f"use --baseline-profile or explicitly bootstrap with "
                f"--experiment-name {expected_bootstrap}",
                file=sys.stderr,
            )
            return 2
    if args.qths_binary_dir is None or (
        request.pipeline_stage != "gpu" and args.sender_binary_dir is None
    ):
        print(
            "--qths-binary-dir is required; --sender-binary-dir is also "
            "required for receive, unpack and full execution",
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
        pipeline_profiler=args.pipeline_profiler,
    )
    default_result_id = result_directory_name(request)
    if args.preflight_only:
        if args.warmup_runs != 0 or args.measured_runs != 1:
            print(
                "--preflight-only does not accept repetition counts",
                file=sys.stderr,
            )
            return 2
        result = RatePointController(
            backend, args.result_root, run_id=f"{default_result_id}-preflight"
        ).preflight_request(
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
                pipeline_profiler=args.pipeline_profiler,
            ),
            args.result_root,
            args.warmup_runs,
            args.measured_runs,
            args.suite_id or default_result_id,
        )
    print(json.dumps(result, sort_keys=True), flush=True)
    cleanup_passed = (
        "cleanup" not in result
        or result["cleanup"].get("CLEANUP_RESULT") == "PASS"
    )
    return 0 if result["TEST_RESULT"] == "PASS" and cleanup_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
