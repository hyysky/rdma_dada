#!/usr/bin/env python3

import ast
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    raise AssertionError(message)


def read_npy_v1(path):
    content = path.read_bytes()
    if content[:8] != b"\x93NUMPY\x01\x00":
        fail("fixture must use the NumPy v1 file format")
    header_length = struct.unpack("<H", content[8:10])[0]
    header_end = 10 + header_length
    header = ast.literal_eval(content[10:header_end].decode("latin1").strip())
    return header, content[header_end:]


def main():
    if len(sys.argv) != 2:
        print("usage: beamform_test_weights_generator_test.py GENERATOR", file=sys.stderr)
        return 2

    generator = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        output_path = Path(directory) / "beamform_cuda_test_weights.npy"
        result = subprocess.run(
            [sys.executable, str(generator), str(output_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            fail("generator failed: " + result.stderr)
        if not output_path.is_file():
            fail("generator did not create the requested NPY file")

        header, payload = read_npy_v1(output_path)
        if header.get("descr") != "|i1":
            fail("fixture dtype must be signed int8")
        if header.get("fortran_order") is not False:
            fail("fixture must use C order")
        if header.get("shape") != (2, 1, 2, 2, 2):
            fail("fixture shape must be F=2,P=1,A=2,B=2,components=2")

        values = struct.unpack("16b", payload)
        expected = (
            2, 0, 4, 0, 0, 2, 2, -2,
            2, 0, 0, 2, 2, 0, 0, -2,
        )
        if values != expected:
            fail("fixture values do not match the CUDA known-result weights")
        if "WEIGHTS_SCALE=0.5" not in result.stdout:
            fail("generator must report the external dequantization scale")

    print("beamform_test_weights_generator_test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
