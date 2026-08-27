#!/usr/bin/env python3
"""Validate the deterministic production-geometry GPU pressure fixture."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "generate_gpu_pressure_fixture.py"


def read_npy_v1(path: pathlib.Path):
    data = path.read_bytes()
    if data[:6] != b"\x93NUMPY" or data[6:8] != b"\x01\x00":
        raise AssertionError("fixture is not an NPY v1 file")
    header_size = struct.unpack("<H", data[8:10])[0]
    header = data[10:10 + header_size].decode("latin1")
    return header, data[10 + header_size:]


class GpuPressureFixtureTest(unittest.TestCase):
    def test_generates_a469_f4_p1_b350_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            completed = subprocess.run(
                [sys.executable, str(GENERATOR), "--output-dir", str(output)],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)

            config_path = output / "gpu-pressure-a469-f4-p1-b350.json"
            weights_path = output / "gpu-pressure-f4-p1-a469-b350-i8.npy"
            config = json.loads(config_path.read_text())
            observation = config["observation"]
            module = config["processing"]["modules"][0]

            self.assertEqual(len(observation["station_ids"]), 469)
            self.assertEqual(len(set(observation["station_ids"])), 469)
            self.assertEqual(observation["nchan"], 4)
            self.assertEqual(observation["npol"], 1)
            self.assertEqual(observation["sample_interval_ps"], 1_000_000)
            self.assertEqual(module["type"], "beamform")
            self.assertEqual(module["weights_order"], "FPAB2")
            self.assertEqual(module["weights_file"], weights_path.name)

            header, payload = read_npy_v1(weights_path)
            self.assertIn("'descr': '|i1'", header)
            self.assertIn("'shape': (4, 1, 469, 350, 2)", header)
            self.assertEqual(len(payload), 4 * 1 * 469 * 350 * 2)
            self.assertNotEqual(payload, bytes(len(payload)))


if __name__ == "__main__":
    unittest.main()
