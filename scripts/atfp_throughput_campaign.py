#!/usr/bin/env python3
"""Run the versioned ATFP full-pipeline physical-wire-rate campaign."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import sys
import time
import uuid
from decimal import Decimal, ROUND_HALF_UP
from typing import Any


@dataclasses.dataclass(frozen=True)
class WireModel:
    ethernet_header_bytes: int
    ipv4_header_bytes: int
    udp_header_bytes: int
    ethernet_fcs_bytes: int
    preamble_sfd_bytes: int
    interpacket_gap_bytes: int

    @classmethod
    def untagged_ipv4(cls) -> "WireModel":
        return cls(14, 20, 8, 4, 8, 12)

    @property
    def overhead_bytes(self) -> int:
        return (
            self.ethernet_header_bytes
            + self.ipv4_header_bytes
            + self.udp_header_bytes
            + self.ethernet_fcs_bytes
            + self.preamble_sfd_bytes
            + self.interpacket_gap_bytes
        )

    def physical_bytes(self, record_bytes: int) -> int:
        if record_bytes <= 0:
            raise ValueError("record_bytes must be positive")
        return record_bytes + self.overhead_bytes


def wire_gbps_to_record_gbps(
    target_wire_gbps: float,
    record_bytes: int,
    station_count: int,
    model: WireModel | None = None,
) -> tuple[float, float]:
    if (
        not math.isfinite(target_wire_gbps)
        or target_wire_gbps <= 0
        or record_bytes <= 0
        or station_count <= 0
    ):
        raise ValueError("wire rate, record bytes and station count must be positive")
    resolved_model = model or WireModel.untagged_ipv4()
    per_station_decimal = (
        Decimal(str(target_wire_gbps))
        * Decimal(record_bytes)
        / Decimal(resolved_model.physical_bytes(record_bytes))
        / Decimal(station_count)
    ).quantize(
        Decimal("0.000000001"), rounding=ROUND_HALF_UP
    )
    per_station = float(per_station_decimal)
    aggregate = float(per_station_decimal * Decimal(station_count))
    return aggregate, per_station


@dataclasses.dataclass(frozen=True)
class CampaignConfig:
    target_wire_gbps: tuple[int, ...]
    duration_seconds: int
    warmup_runs: int
    measured_runs: int
    bisection_tolerance_gbps: float
    compute_consumer: str
    wire_model: str


_CAMPAIGN_FIELDS = {
    "schema_version",
    "target_wire_gbps",
    "duration_seconds",
    "warmup_runs",
    "measured_runs",
    "bisection_tolerance_gbps",
    "compute_consumer",
    "wire_model",
}


def load_campaign_config(path: pathlib.Path) -> CampaignConfig:
    try:
        value: Any = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read campaign config: {error}") from error
    if not isinstance(value, dict) or set(value) != _CAMPAIGN_FIELDS:
        raise ValueError("campaign config contains missing or unknown fields")
    if value["schema_version"] != 1:
        raise ValueError("unsupported campaign schema_version")
    rates = value["target_wire_gbps"]
    if (
        not isinstance(rates, list)
        or any(type(rate) is not int or rate <= 0 for rate in rates)
        or rates != sorted(set(rates))
        or rates != [1, 5, 10, 20, 30, 35, 40]
    ):
        raise ValueError("target_wire_gbps must be the formal ascending rate list")
    if value["duration_seconds"] != 30:
        raise ValueError("duration_seconds must be 30")
    if value["warmup_runs"] != 1 or value["measured_runs"] != 3:
        raise ValueError("formal campaign requires one warm-up and three measured runs")
    if value["bisection_tolerance_gbps"] != 0.5:
        raise ValueError("bisection_tolerance_gbps must be 0.5")
    if value["compute_consumer"] != "dbnull":
        raise ValueError("formal campaign requires dbnull")
    if value["wire_model"] != "UNTAGGED_IPV4_ETHERNET":
        raise ValueError("formal campaign requires untagged IPv4 Ethernet")
    return CampaignConfig(
        target_wire_gbps=tuple(rates),
        duration_seconds=30,
        warmup_runs=1,
        measured_runs=3,
        bisection_tolerance_gbps=0.5,
        compute_consumer="dbnull",
        wire_model="UNTAGGED_IPV4_ETHERNET",
    )


def _decimal_text(value: Decimal | None) -> str | None:
    if value is None:
        return None
    return format(value, "f")


def run_campaign_schedule(config: CampaignConfig, run_point: Any) -> dict[str, Any]:
    points: list[dict[str, Any]] = []
    last_pass: Decimal | None = None
    first_fail: Decimal | None = None
    terminal_result = "PASS"
    aborted = False

    def execute(rate: Decimal) -> dict[str, Any]:
        nonlocal terminal_result
        result = run_point(rate, config.warmup_runs, config.measured_runs)
        test_result = str(result.get("TEST_RESULT", "HARNESS_FAIL"))
        cleanup_result = str(result.get("CLEANUP_RESULT", "FAIL"))
        if cleanup_result != "PASS":
            test_result = "HARNESS_FAIL"
        terminal_result = test_result
        point = dict(result)
        point.update({
            "target_wire_gbps": _decimal_text(rate),
            "TEST_RESULT": test_result,
            "CLEANUP_RESULT": cleanup_result,
        })
        points.append(point)
        return points[-1]

    for configured_rate in config.target_wire_gbps:
        rate = Decimal(str(configured_rate))
        point = execute(rate)
        if point["TEST_RESULT"] == "PASS" and point["CLEANUP_RESULT"] == "PASS":
            last_pass = rate
            continue
        if point["TEST_RESULT"] == "PERFORMANCE_FAIL":
            first_fail = rate
        else:
            aborted = True
        break

    tolerance = Decimal(str(config.bisection_tolerance_gbps))
    while (
        last_pass is not None
        and first_fail is not None
        and first_fail - last_pass > tolerance
        and terminal_result != "HARNESS_FAIL"
    ):
        midpoint = (last_pass + first_fail) / Decimal(2)
        point = execute(midpoint)
        if point["TEST_RESULT"] == "PASS" and point["CLEANUP_RESULT"] == "PASS":
            last_pass = midpoint
        else:
            first_fail = midpoint

    width = (
        first_fail - last_pass
        if first_fail is not None and last_pass is not None
        else None
    )
    return {
        "TEST_RESULT": terminal_result if first_fail is not None or aborted else "PASS",
        "points": points,
        "stable_lower_gbps": _decimal_text(last_pass),
        "failing_upper_gbps": _decimal_text(first_fail),
        "boundary_width_gbps": _decimal_text(width),
        "bottleneck_reached": first_fail is not None,
    }


def verify_source_manifest(
    project_root: pathlib.Path, manifest_path: pathlib.Path
) -> dict[str, str]:
    root = pathlib.Path(project_root).resolve()
    manifest = pathlib.Path(manifest_path)
    verified: dict[str, str] = {}
    try:
        lines = manifest.read_text().splitlines()
    except OSError as error:
        raise ValueError(f"cannot read source manifest: {error}") from error
    if not lines:
        raise ValueError("source manifest is empty")
    for line in lines:
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise ValueError("invalid source manifest line")
        expected, relative_text = parts
        relative = pathlib.PurePosixPath(relative_text.strip())
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("source manifest path escapes project root")
        path = root.joinpath(*relative.parts)
        if not path.is_file():
            raise ValueError(f"source manifest file is missing: {relative}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"source manifest SHA256 mismatch: {relative}")
        verified[str(relative)] = actual
    return verified


def _load_rate_point_module() -> Any:
    path = pathlib.Path(__file__).with_name("task8c_rate_point.py")
    spec = importlib.util.spec_from_file_location("atfp_campaign_rate_point", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load versioned single-rate controller")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _atomic_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def _campaign_id() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + f"-{uuid.uuid4().hex[:8]}"


class CampaignLock:
    def __init__(self, path: pathlib.Path, campaign_id: str) -> None:
        self.path = pathlib.Path(path)
        self.campaign_id = campaign_id
        self._owned = False

    def __enter__(self) -> "CampaignLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(
            {"campaign_id": self.campaign_id, "pid": os.getpid(), "started": time.time()},
            sort_keys=True,
        ) + "\n"
        try:
            descriptor = os.open(
                self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
            )
        except FileExistsError as error:
            raise RuntimeError(f"formal campaign lock already exists: {self.path}") from error
        with os.fdopen(descriptor, "w") as stream:
            stream.write(payload)
        self._owned = True
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        if self._owned:
            try:
                current = json.loads(self.path.read_text())
                if current.get("campaign_id") == self.campaign_id:
                    self.path.unlink()
            finally:
                self._owned = False


def compile_plan_on_backend(
    rate_point: Any,
    backend: Any,
    request: Any,
    observation_config: pathlib.Path,
    config_compiler: pathlib.Path,
    run_directory: pathlib.Path,
) -> Any:
    executor = backend.observation_compiler(config_compiler, run_directory)
    try:
        return rate_point.compile_rate_plan(
            request,
            observation_config,
            config_compiler,
            run_directory,
            compiler_executor=executor,
        )
    finally:
        executor.close()


def run_remote_campaign(args: argparse.Namespace, preflight_only: bool) -> dict[str, Any]:
    config = load_campaign_config(args.campaign_config)
    source_sha = verify_source_manifest(args.project_root, args.source_manifest)
    rate_point = _load_rate_point_module()
    campaign_id = args.campaign_id or _campaign_id()
    campaign_root = pathlib.Path(args.result_root) / campaign_id
    campaign_root.mkdir(parents=True, exist_ok=False)
    bootstrap_request = rate_point.RateRequest(
        aggregate_gbps=1.0,
        duration_seconds=float(config.duration_seconds),
        compute_consumer=config.compute_consumer,
    )

    def backend_factory() -> Any:
        return rate_point.SshBackend(
            project_root=args.project_root,
            known_hosts=args.known_hosts,
            qths_binary_dir=args.qths_binary_dir,
            sender_binary_dir=args.sender_binary_dir,
        )

    bootstrap_plan = compile_plan_on_backend(
        rate_point,
        backend_factory(),
        bootstrap_request,
        args.observation_config,
        args.config_compiler,
        campaign_root / "bootstrap",
    )
    wire_model = WireModel.untagged_ipv4()
    manifest = {
        "campaign_id": campaign_id,
        "campaign_config": dataclasses.asdict(config),
        "source_sha256": source_sha,
        "record_bytes": bootstrap_plan.record_bytes,
        "wire_model": dataclasses.asdict(wire_model),
        "qths_binary_dir": str(args.qths_binary_dir),
        "sender_binary_dir": str(args.sender_binary_dir),
    }
    _atomic_json(campaign_root / "campaign.json", manifest)

    first_record_gbps, _ = wire_gbps_to_record_gbps(
        float(config.target_wire_gbps[0]),
        bootstrap_plan.record_bytes,
        bootstrap_plan.nant,
        wire_model,
    )
    first_request = rate_point.RateRequest(
        aggregate_gbps=first_record_gbps,
        duration_seconds=float(config.duration_seconds),
        compute_consumer=config.compute_consumer,
    )
    if preflight_only:
        result = rate_point.RatePointController(
            backend_factory(), campaign_root, run_id="preflight"
        ).preflight_request(
            first_request, args.observation_config, args.config_compiler
        )
        summary = {
            "TEST_RESULT": result["TEST_RESULT"],
            "CLEANUP_RESULT": result.get("cleanup", {}).get(
                "CLEANUP_RESULT", "PASS"
            ),
            "mode": "preflight-only",
            "campaign_id": campaign_id,
            "target_wire_gbps": str(config.target_wire_gbps[0]),
            "derived_record_gbps": first_record_gbps,
        }
        _atomic_json(campaign_root / "summary.json", summary)
        return summary

    def run_point(rate: Decimal, warmups: int, measured: int) -> dict[str, Any]:
        record_gbps, _ = wire_gbps_to_record_gbps(
            float(rate), bootstrap_plan.record_bytes, bootstrap_plan.nant, wire_model
        )
        rate_request = rate_point.RateRequest(
            aggregate_gbps=record_gbps,
            duration_seconds=float(config.duration_seconds),
            compute_consumer=config.compute_consumer,
        )
        rate_id = "rate-" + format(rate, "06.3f")
        result = rate_point.run_rate_request_sequence(
            rate_request,
            args.observation_config,
            args.config_compiler,
            backend_factory,
            campaign_root,
            warmups,
            measured,
            rate_id,
        )
        cleanup_result = (
            "PASS"
            if result.get("runs")
            and all(run.get("CLEANUP_RESULT") == "PASS" for run in result["runs"])
            else "FAIL"
        )
        record_statistics = result.get("actual_aggregate_gbps")
        wire_statistics = None
        if record_statistics is not None:
            factor = wire_model.physical_bytes(bootstrap_plan.record_bytes) / float(
                bootstrap_plan.record_bytes
            )
            wire_statistics = {
                name: float(value) * factor
                for name, value in record_statistics.items()
            }
        result["target_wire_gbps"] = _decimal_text(rate)
        result["derived_target_record_gbps"] = record_gbps
        result["actual_wire_gbps"] = wire_statistics
        _atomic_json(campaign_root / rate_id / "summary.json", result)
        return {
            "TEST_RESULT": result["TEST_RESULT"],
            "CLEANUP_RESULT": cleanup_result,
            "rate_result": f"{rate_id}/summary.json",
            "derived_target_record_gbps": record_gbps,
            "actual_wire_gbps": wire_statistics,
        }

    summary = run_campaign_schedule(config, run_point)
    summary["CLEANUP_RESULT"] = (
        "PASS"
        if all(point.get("CLEANUP_RESULT") == "PASS" for point in summary["points"])
        else "FAIL"
    )
    summary.update({"campaign_id": campaign_id, "wire_model": dataclasses.asdict(wire_model)})
    _atomic_json(campaign_root / "summary.json", summary)
    _atomic_json(
        campaign_root / "bottleneck_report.json",
        {
            "classification": "UNDETERMINED" if summary["bottleneck_reached"] else "NOT_REACHED",
            "stable_lower_gbps": summary["stable_lower_gbps"],
            "failing_upper_gbps": summary["failing_upper_gbps"],
        },
    )
    return summary


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign-config", type=pathlib.Path, required=True)
    parser.add_argument("--result-root", type=pathlib.Path, default=pathlib.Path("/tmp/atfp-campaign-results"))
    parser.add_argument("--project-root", type=pathlib.Path, default=pathlib.Path("/home/user/wy/rdma_dada"))
    parser.add_argument("--known-hosts", type=pathlib.Path, default=pathlib.Path("/tmp/atfp-campaign-known-hosts"))
    parser.add_argument("--observation-config", type=pathlib.Path)
    parser.add_argument("--config-compiler", type=pathlib.Path)
    parser.add_argument("--qths-binary-dir", type=pathlib.Path)
    parser.add_argument("--sender-binary-dir", type=pathlib.Path)
    parser.add_argument("--source-manifest", type=pathlib.Path)
    parser.add_argument("--campaign-id")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--preflight-only", action="store_true")
    mode.add_argument("--execute", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    config = load_campaign_config(args.campaign_config)
    if args.dry_run:
        print(json.dumps(dataclasses.asdict(config), sort_keys=True))
        return 0
    required = (
        args.observation_config,
        args.config_compiler,
        args.qths_binary_dir,
        args.sender_binary_dir,
        args.source_manifest,
    )
    if any(value is None for value in required):
        print(
            "formal preflight/execute requires observation config, compiler, "
            "qths/sender Release directories and source manifest",
            file=sys.stderr,
        )
        return 2
    try:
        if args.campaign_id is None:
            args.campaign_id = _campaign_id()
        with CampaignLock(
            pathlib.Path(args.result_root) / ".atfp-throughput-campaign.lock",
            args.campaign_id,
        ):
            result = run_remote_campaign(args, args.preflight_only)
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True), flush=True)
    return 0 if result["TEST_RESULT"] == "PASS" and result["CLEANUP_RESULT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
