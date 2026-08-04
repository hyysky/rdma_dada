#!/usr/bin/env python3
"""Convert the legacy KEY=VALUE pipeline config to JSON schema v1."""

import argparse
import json
import math
import pathlib
import sys


REQUIRED_KEYS = {
    "NANT",
    "NCHAN",
    "NPOL",
    "PAYLOAD_ORDER",
    "PKT_HEADER",
    "PKT_DATA",
    "PKT_NSAMP",
    "PKT_NBIT",
    "PKT_TSAMP",
    "RECORDS_PER_BLOCK",
    "RAW_RING_BLOCKS",
    "COMPUTE_RING_BLOCKS",
    "FILE_BLOCKS",
    "DIRECT_IO",
    "UTC_START",
}


def parse_legacy(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            if "=" not in line:
                raise ValueError(
                    f"{path}:{line_number}: expected KEY=VALUE syntax"
                )
            key, value = (part.strip() for part in line.split("=", 1))
            if not key or not value:
                raise ValueError(f"{path}:{line_number}: empty key or value")
            if key in values:
                raise ValueError(f"{path}:{line_number}: duplicate key {key}")
            values[key] = value

    missing = sorted(REQUIRED_KEYS - values.keys())
    unknown = sorted(values.keys() - REQUIRED_KEYS)
    if missing:
        raise ValueError("missing required keys: " + ", ".join(missing))
    if unknown:
        raise ValueError("unknown keys: " + ", ".join(unknown))
    return values


def parse_int(values: dict[str, str], key: str) -> int:
    try:
        value = int(values[key], 10)
    except ValueError as exc:
        raise ValueError(f"{key} must be an integer: {values[key]}") from exc
    if value < 0:
        raise ValueError(f"{key} must be non-negative")
    return value


def parse_float(values: dict[str, str], key: str) -> float:
    try:
        value = float(values[key])
    except ValueError as exc:
        raise ValueError(f"{key} must be a number: {values[key]}") from exc
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"{key} must be greater than zero")
    return value


def parse_bool(values: dict[str, str], key: str) -> bool:
    normalized = values[key].lower()
    if normalized in {"true", "1"}:
        return True
    if normalized in {"false", "0"}:
        return False
    raise ValueError(f"{key} must be true, false, 1, or 0")


def convert(values: dict[str, str]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "observation": {
            "nant": parse_int(values, "NANT"),
            "nchan": parse_int(values, "NCHAN"),
            "npol": parse_int(values, "NPOL"),
            "payload_order": values["PAYLOAD_ORDER"],
            "utc_start": values["UTC_START"],
        },
        "packet": {
            "header_bytes": parse_int(values, "PKT_HEADER"),
            "payload_bytes": parse_int(values, "PKT_DATA"),
            "samples": parse_int(values, "PKT_NSAMP"),
            "nbit": parse_int(values, "PKT_NBIT"),
            "sample_interval_us": parse_float(values, "PKT_TSAMP"),
        },
        "ring_buffers": {
            "records_per_block": parse_int(values, "RECORDS_PER_BLOCK"),
            "raw_blocks": parse_int(values, "RAW_RING_BLOCKS"),
            "compute_blocks": parse_int(values, "COMPUTE_RING_BLOCKS"),
        },
        "disk": {
            # The legacy launcher always enabled dada_dbdisk.
            "enabled": True,
            "blocks_per_file": parse_int(values, "FILE_BLOCKS"),
            "direct_io": parse_bool(values, "DIRECT_IO"),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=pathlib.Path, help="legacy KEY=VALUE file")
    parser.add_argument("output", type=pathlib.Path, help="output JSON file")
    args = parser.parse_args()

    try:
        document = convert(parse_legacy(args.input))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", encoding="utf-8") as destination:
            json.dump(
                document,
                destination,
                indent=2,
                ensure_ascii=False,
                allow_nan=False,
            )
            destination.write("\n")
    except (OSError, ValueError) as exc:
        print(f"conversion failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
