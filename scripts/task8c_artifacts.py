#!/usr/bin/env python3
"""Validation helpers for compact, reproducible Task 8C result suites."""

from __future__ import annotations

import hashlib
import pathlib
from typing import Any, Iterable


_PROCESS_FIELDS = {
    "host",
    "role",
    "argv",
    "env",
    "cpu_affinity",
    "numa_node",
    "thread_mapping",
    "binary_sha256",
    "config_sha256",
    "pid",
    "started_utc",
    "ended_utc",
    "returncode",
}


def validate_process_ledger(
    processes: list[dict[str, Any]], required_roles: Iterable[str],
    required_counts: dict[str, int] | None = None,
) -> None:
    """Fail when a PASS run would lose a required process identity."""
    counts = required_counts or {}
    for role in required_roles:
        matches = [item for item in processes if item.get("role") == role]
        if not matches:
            raise ValueError(f"process ledger is missing required role: {role}")
        expected_count = counts.get(role, 1)
        if len(matches) != expected_count:
            raise ValueError(
                f"process ledger role {role} count is {len(matches)}, "
                f"expected {expected_count}"
            )
        for item in matches:
            missing = _PROCESS_FIELDS - set(item)
            if missing:
                raise ValueError(
                    f"process ledger role {role} is missing fields: {sorted(missing)}"
                )
            if not isinstance(item["argv"], list) or not item["argv"]:
                raise ValueError(f"process ledger role {role} has invalid argv")
            if not isinstance(item["env"], dict):
                raise ValueError(f"process ledger role {role} has invalid env")
            if not isinstance(item["cpu_affinity"], list):
                raise ValueError(
                    f"process ledger role {role} has invalid CPU affinity"
                )
            if not isinstance(item["thread_mapping"], list):
                raise ValueError(
                    f"process ledger role {role} has invalid thread mapping"
                )
            if not isinstance(item["binary_sha256"], str) or len(
                item["binary_sha256"]
            ) != 64:
                raise ValueError(
                    f"process ledger role {role} has invalid binary SHA256"
                )
            if not isinstance(item["config_sha256"], str) or len(
                item["config_sha256"]
            ) != 64:
                raise ValueError(
                    f"process ledger role {role} has invalid config SHA256"
                )
            if not isinstance(item["pid"], int) or item["pid"] <= 0:
                raise ValueError(f"process ledger role {role} has invalid PID")
            if item["ended_utc"] is None or item["returncode"] is None:
                raise ValueError(
                    f"process ledger role {role} has incomplete lifecycle"
                )


def write_manifest(root: pathlib.Path) -> None:
    entries = []
    for path in sorted(pathlib.Path(root).rglob("*")):
        if not path.is_file() or path.name == "MANIFEST.sha256":
            continue
        entries.append(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  "
            f"{path.relative_to(root)}\n"
        )
    (pathlib.Path(root) / "MANIFEST.sha256").write_text("".join(entries))


def expected_success_files(run_ids: Iterable[str]) -> set[str]:
    shared = {
        "observation.json",
        "resolved_observation.json",
        "preflight.json",
        "summary.json",
        "MANIFEST.sha256",
    }
    for run_id in run_ids:
        shared.add(f"runs/{run_id}.json")
        shared.add(f"runs/{run_id}.evidence.log")
    return shared
