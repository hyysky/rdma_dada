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
COMPILER = None
if len(sys.argv) >= 2 and not sys.argv[1].startswith("-"):
    COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()
SMALL_SENDER_CONFIG = (
    ROOT / "config" / "testing" / "multi-station-sender-small.json"
)
PRODUCTION_SENDER_CONFIG = (
    ROOT / "config" / "testing" / "multi-station-sender-production.json"
)


def read_npy_v1(path: pathlib.Path):
    data = path.read_bytes()
    if data[:6] != b"\x93NUMPY" or data[6:8] != b"\x01\x00":
        raise AssertionError("fixture is not an NPY v1 file")
    header_size = struct.unpack("<H", data[8:10])[0]
    header = data[10:10 + header_size].decode("latin1")
    return header, data[10 + header_size:]


class GpuPressureFixtureTest(unittest.TestCase):
    def test_checked_in_sender_observations_cover_small_and_production_geometry(self):
        small = json.loads(SMALL_SENDER_CONFIG.read_text())
        production = json.loads(PRODUCTION_SENDER_CONFIG.read_text())

        self.assertEqual(small["observation"]["station_ids"], [1000, 1001, 1002, 1003])
        self.assertEqual(small["observation"]["nchan"], 4)
        self.assertEqual(small["observation"]["sample_interval_ps"], 1_000_000)
        self.assertEqual(small["wire"]["samples_per_packet"], 512)
        self.assertEqual(small["blocks"]["groups_per_block"], 3200)
        self.assertEqual(small["processing"]["modules"], [])

        stations = production["observation"]["station_ids"]
        self.assertEqual(stations, list(range(1000, 1469)))
        self.assertEqual(production["observation"]["nchan"], 4)
        self.assertEqual(production["observation"]["npol"], 1)
        self.assertEqual(production["observation"]["sample_interval_ps"], 1_000_000)
        self.assertEqual(production["wire"]["samples_per_packet"], 512)
        self.assertEqual(production["blocks"]["groups_per_block"], 26)
        self.assertEqual(production["processing"]["modules"], [])

        signal_payload_gbps = (
            len(stations)
            * production["observation"]["nchan"]
            * production["observation"]["npol"]
            * 2
            * 8
            * 1_000_000
            / 1_000_000_000
        )
        datagram_payload_gbps = signal_payload_gbps * 4128 / 4096
        self.assertAlmostEqual(signal_payload_gbps, 30.016)
        self.assertAlmostEqual(datagram_payload_gbps, 30.2505)

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
            processing = config["processing"]
            modules = processing["modules"]

            self.assertEqual(len(observation["station_ids"]), 469)
            self.assertEqual(len(set(observation["station_ids"])), 469)
            self.assertEqual(observation["nchan"], 4)
            self.assertEqual(observation["npol"], 1)
            self.assertEqual(observation["sample_interval_ps"], 1_000_000)
            self.assertEqual(observation["duration_seconds"], "30.000128")
            self.assertEqual(config["wire"]["samples_per_packet"], 512)
            self.assertEqual(config["blocks"]["groups_per_block"], 26)
            payload_gbps = (
                len(observation["station_ids"])
                * observation["nchan"]
                * observation["npol"]
                * 2
                * 8
                * (1_000_000_000_000 // observation["sample_interval_ps"])
                / 1_000_000_000
            )
            self.assertAlmostEqual(payload_gbps, 30.016)
            self.assertEqual(
                processing["cuda_pipeline"],
                {"mode": "STAGED_PIPELINE", "inflight_blocks": 3},
            )
            self.assertEqual(
                [module["type"] for module in modules],
                ["beamform", "power", "integrate"],
            )
            self.assertEqual(modules[0]["weights_order"], "FPAB2")
            self.assertEqual(modules[0]["weights_file"], weights_path.name)
            self.assertEqual(
                modules[2],
                {"type": "integrate", "length": 128, "operation": "MEAN"},
            )

            samples_per_block = (
                config["blocks"]["groups_per_block"]
                * config["wire"]["samples_per_packet"]
            )
            self.assertEqual(samples_per_block, 13_312)
            self.assertEqual(samples_per_block % modules[2]["length"], 0)
            integrated_samples = samples_per_block // modules[2]["length"]
            self.assertEqual(integrated_samples, 104)
            output_block_bytes = (
                integrated_samples * observation["nchan"] * 350 * 4
            )
            self.assertEqual(output_block_bytes, 582_400)

            header, payload = read_npy_v1(weights_path)
            self.assertIn("'descr': '|i1'", header)
            self.assertIn("'shape': (4, 1, 469, 350, 2)", header)
            self.assertEqual(len(payload), 4 * 1 * 469 * 350 * 2)
            self.assertNotEqual(payload, bytes(len(payload)))

    @unittest.skipIf(COMPILER is None, "observation_config_compile not supplied")
    def test_compiler_accepts_production_full_power_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            generated = subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--output-dir",
                    str(output),
                    "--project-root",
                    str(ROOT),
                ],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(generated.returncode, 0, generated.stderr)

            compiled = subprocess.run(
                [
                    str(COMPILER),
                    "--config",
                    str(output / "gpu-pressure-a469-f4-p1-b350.json"),
                    "--budget-payload-gbps",
                    "30",
                    "--output-dir",
                    str(output / "artifacts"),
                ],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            report = json.loads(compiled.stdout)
            self.assertTrue(report["valid"])
            self.assertEqual(
                report["stage_headers"],
                ["RAW", "UNPACKED", "CONVERTED", "BEAMFORMED", "POWER_INTEGRATED"],
            )
            budget = report["gpu_pipeline_budget"]
            self.assertEqual(budget["execution_mode"], "STAGED_PIPELINE")
            self.assertEqual(budget["inflight_blocks"], 3)
            self.assertEqual(
                budget["block_bytes"],
                {
                    "compute": 49_946_624,
                    "converted": 199_786_496,
                    "beamformed": 149_094_400,
                    "product": 74_547_200,
                    "output": 582_400,
                },
            )
            for name in (
                "raw.header",
                "unpacked.header",
                "converted.header",
                "beamformed.header",
                "output.header",
            ):
                header = (output / "artifacts" / name).read_bytes()
                self.assertEqual(len(header), 4096)
                self.assertNotIn(b"STATION_IDS ", header)
                self.assertIn(b"NANT 469\n", header)


if __name__ == "__main__":
    unittest.main()
