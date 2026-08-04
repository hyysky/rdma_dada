#!/usr/bin/env python3

import argparse
import struct
from pathlib import Path


SHAPE = (2, 1, 2, 2, 2)
WEIGHTS_SCALE = 0.5
VALUES = (
    # f0: [[1, 2], [i, 1-i]] after applying WEIGHTS_SCALE.
    2, 0, 4, 0, 0, 2, 2, -2,
    # f1: [[1, i], [1, -i]] after applying WEIGHTS_SCALE.
    2, 0, 0, 2, 2, 0, 0, -2,
)


def encode_npy_v1():
    header = (
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 1, 2, 2, 2), }"
    )
    while (10 + len(header) + 1) % 16 != 0:
        header += " "
    header += "\n"
    prefix = b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header))
    payload = struct.pack("16b", *VALUES)
    return prefix + header.encode("latin1") + payload


def main():
    parser = argparse.ArgumentParser(
        description="Generate deterministic FPAB2 int8 weights for the CUDA beamform test."
    )
    parser.add_argument("output", type=Path, help="output .npy path")
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(encode_npy_v1())
    print(
        "generated {}: ORDER=FPAB2 SHAPE={} DTYPE=int8 WEIGHTS_SCALE={}".format(
            args.output, SHAPE, WEIGHTS_SCALE
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
