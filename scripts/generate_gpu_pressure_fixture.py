#!/usr/bin/env python3
"""Generate the deterministic A469/F4/P1/B350 GPU pressure fixture."""

import argparse
import json
import pathlib
import struct


NANT = 469
NCHAN = 4
NPOL = 1
NBEAM = 350
GROUPS_PER_BLOCK = 14


def write_npy_int8(path: pathlib.Path) -> None:
    shape = (NCHAN, NPOL, NANT, NBEAM, 2)
    header = (
        "{'descr': '|i1', 'fortran_order': False, "
        f"'shape': {shape}, }}"
    ).encode("latin1")
    padding = (64 - ((10 + len(header) + 1) % 64)) % 64
    header = header + (b" " * padding) + b"\n"
    if len(header) > 0xFFFF:
        raise ValueError("NPY v1 header is too large")

    complex_count = NCHAN * NPOL * NANT * NBEAM
    payload = bytearray(complex_count * 2)
    index = 0
    for frequency in range(NCHAN):
        for antenna in range(NANT):
            for beam in range(NBEAM):
                payload[index] = 1
                payload[index + 1] = ((frequency + antenna + beam) % 3 - 1) & 0xFF
                index += 2

    path.write_bytes(
        b"\x93NUMPY\x01\x00"
        + struct.pack("<H", len(header))
        + header
        + payload
    )


def build_config(project_root: pathlib.Path, weights_name: str) -> dict:
    return {
        "schema_version": 1,
        "observation": {
            "observation_id": "gpu-pressure-a469-f4-p1-b350",
            "utc_start": "2026-08-26-00:00:00",
            "duration_seconds": "30",
            "station_ids": list(range(1000, 1000 + NANT)),
            "first_channel_id": 100,
            "nchan": NCHAN,
            "npol": NPOL,
            "sample_interval_ps": 1_000_000,
        },
        "metadata": {
            "telescope": "CA",
            "bandwidth_hz": 4_000_000,
            "center_frequency_hz": 1_250_000_000,
        },
        "wire": {
            "profile": str(
                (project_root / "config/packet_formats/frontend.example-v1.json")
                .resolve()
            ),
            "samples_per_packet": 1024,
        },
        "blocks": {
            "groups_per_block": GROUPS_PER_BLOCK,
            "raw_ring_blocks": 16,
            "compute_ring_blocks": 8,
            "window_blocks": 4,
        },
        "rings": {
            "raw_key": "0x00d2",
            "compute_key": "0x00d4",
            "output_key": "0x00d6",
        },
        "storage": {
            "enabled": False,
            "blocks_per_file": 0,
            "direct_io": False,
        },
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
            "output": {"sample_format": "AUTO"},
            "modules": [{
                "type": "beamform",
                "weights_file": weights_name,
                "weights_order": "FPAB2",
                "weights_id": "gpu-pressure-f4-p1-a469-b350-i8-v1",
                "weights_scale": "0.0078125",
                "compute_mode": "FP32",
            }],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument(
        "--project-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    config_path = args.output_dir / "gpu-pressure-a469-f4-p1-b350.json"
    weights_path = args.output_dir / "gpu-pressure-f4-p1-a469-b350-i8.npy"
    for path in (config_path, weights_path):
        if path.exists() and not args.force:
            parser.error(f"refusing to overwrite {path}; pass --force")

    write_npy_int8(weights_path)
    config_path.write_text(
        json.dumps(
            build_config(args.project_root, weights_path.name),
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    print(json.dumps({
        "config": str(config_path),
        "weights": str(weights_path),
        "nant": NANT,
        "nchan": NCHAN,
        "npol": NPOL,
        "nbeam": NBEAM,
        "groups_per_block": GROUPS_PER_BLOCK,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
