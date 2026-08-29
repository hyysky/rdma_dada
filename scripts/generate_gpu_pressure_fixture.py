#!/usr/bin/env python3
"""Generate deterministic production Power or coherency GPU fixtures."""

import argparse
import json
import pathlib
import struct


NANT = 469
NBEAM = 350
GROUPS_PER_BLOCK = 26


def geometry(product: str) -> tuple:
    if product == "power":
        return 4, 1
    return 2, 2


def write_npy_int8(path: pathlib.Path, nchan: int, npol: int) -> None:
    shape = (nchan, npol, NANT, NBEAM, 2)
    header = (
        "{'descr': '|i1', 'fortran_order': False, "
        f"'shape': {shape}, }}"
    ).encode("latin1")
    padding = (64 - ((10 + len(header) + 1) % 64)) % 64
    header = header + (b" " * padding) + b"\n"
    if len(header) > 0xFFFF:
        raise ValueError("NPY v1 header is too large")

    complex_count = nchan * npol * NANT * NBEAM
    payload = bytearray(complex_count * 2)
    index = 0
    for frequency in range(nchan):
        for polarization in range(npol):
            for antenna in range(NANT):
                for beam in range(NBEAM):
                    payload[index] = 1
                    payload[index + 1] = (
                        (frequency + polarization + antenna + beam) % 3 - 1
                    ) & 0xFF
                    index += 2

    path.write_bytes(
        b"\x93NUMPY\x01\x00"
        + struct.pack("<H", len(header))
        + header
        + payload
    )


def build_config(
    project_root: pathlib.Path,
    weights_name: str,
    product: str,
    duration_seconds: str = "30.000128",
    cuda_mode: str = "STAGED_PIPELINE",
    inflight_blocks: int = 3,
) -> dict:
    nchan, npol = geometry(product)
    product_module = "power" if product == "power" else "stokes"
    profile_name = f"a469-f{nchan}-p{npol}-b350"
    observation_id = (
        f"gpu-pressure-{profile_name}"
        if product == "power"
        else f"gpu-pressure-{profile_name}-coherency"
    )
    return {
        "schema_version": 1,
        "observation": {
            "observation_id": observation_id,
            "utc_start": "2026-08-26-00:00:00",
            "duration_seconds": duration_seconds,
            "station_ids": list(range(1000, 1000 + NANT)),
            "first_channel_id": 100,
            "nchan": nchan,
            "npol": npol,
            "sample_interval_ps": 1_000_000,
        },
        "metadata": {
            "telescope": "CA",
            "bandwidth_hz": nchan * 1_000_000,
            "center_frequency_hz": 1_250_000_000,
        },
        "wire": {
            "profile": str(
                (project_root / "config/packet_formats/frontend.example-v1.json")
                .resolve()
            ),
            "samples_per_packet": 512,
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
            "cuda_pipeline": {
                "mode": cuda_mode,
                "inflight_blocks": inflight_blocks,
            },
            "conversion": {"scale": "0.0078125"},
            "output": {"sample_format": "AUTO"},
            "modules": [
                {
                    "type": "beamform",
                    "weights_file": weights_name,
                    "weights_order": "FPAB2",
                    "weights_id": (
                        f"gpu-pressure-f{nchan}-p{npol}-a469-b350-i8-v1"
                    ),
                    "weights_scale": "0.0078125",
                    "compute_mode": "FP32",
                },
                {"type": product_module},
                {"type": "integrate", "length": 128, "operation": "MEAN"},
            ],
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
    parser.add_argument(
        "--product",
        choices=("power", "coherency"),
        default="power",
        help="Generate the Power (F4/P1) or coherency (F2/P2) profile",
    )
    parser.add_argument(
        "--duration-seconds",
        default="30.000128",
        help="Observation duration written verbatim as a positive decimal",
    )
    parser.add_argument(
        "--cuda-mode",
        choices=("SYNCHRONOUS_DIRECT", "STAGED_PIPELINE"),
        default="STAGED_PIPELINE",
    )
    parser.add_argument("--inflight-blocks", type=int, default=3)
    args = parser.parse_args()

    try:
        duration = float(args.duration_seconds)
    except ValueError:
        parser.error("--duration-seconds must be a positive decimal")
    if not duration > 0.0:
        parser.error("--duration-seconds must be a positive decimal")
    if args.cuda_mode == "SYNCHRONOUS_DIRECT" and args.inflight_blocks != 1:
        parser.error("SYNCHRONOUS_DIRECT requires --inflight-blocks 1")
    if args.cuda_mode == "STAGED_PIPELINE" and not 1 <= args.inflight_blocks <= 4:
        parser.error("STAGED_PIPELINE requires --inflight-blocks in [1,4]")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    nchan, npol = geometry(args.product)
    profile_name = f"a469-f{nchan}-p{npol}-b350"
    config_suffix = "" if args.product == "power" else "-coherency"
    config_path = args.output_dir / f"gpu-pressure-{profile_name}{config_suffix}.json"
    weights_path = (
        args.output_dir / f"gpu-pressure-f{nchan}-p{npol}-a469-b350-i8.npy"
    )
    for path in (config_path, weights_path):
        if path.exists() and not args.force:
            parser.error(f"refusing to overwrite {path}; pass --force")

    write_npy_int8(weights_path, nchan, npol)
    config_path.write_text(
        json.dumps(
            build_config(
                args.project_root,
                weights_path.name,
                args.product,
                args.duration_seconds,
                args.cuda_mode,
                args.inflight_blocks,
            ),
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    print(json.dumps({
        "config": str(config_path),
        "weights": str(weights_path),
        "nant": NANT,
        "nchan": nchan,
        "npol": npol,
        "nbeam": NBEAM,
        "groups_per_block": GROUPS_PER_BLOCK,
        "product": args.product,
        "cuda_mode": args.cuda_mode,
        "inflight_blocks": args.inflight_blocks,
        "duration_seconds": args.duration_seconds,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
