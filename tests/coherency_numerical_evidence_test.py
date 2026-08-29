#!/usr/bin/env python3
"""Validate the machine-readable coherency numerical evidence contract."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
    raise SystemExit("usage: coherency_numerical_evidence_test.py STOKES_TEST")
STOKES_TEST = pathlib.Path(sys.argv.pop(1)).resolve()
CUDA_STOKES_TEST = None
if len(sys.argv) >= 2 and not sys.argv[1].startswith("-"):
    CUDA_STOKES_TEST = pathlib.Path(sys.argv.pop(1)).resolve()


class CoherencyNumericalEvidenceTest(unittest.TestCase):
    def test_cpu_reference_emits_complete_error_evidence(self):
        """Catches loss of quantitative evidence behind a numerical PASS."""
        with tempfile.TemporaryDirectory() as directory:
            result_path = pathlib.Path(directory) / "stokes-cpu.json"
            completed = subprocess.run(
                [str(STOKES_TEST), "--result-json", str(result_path)],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(
                result_path.is_file(),
                "stokes test did not write the requested numerical evidence",
            )
            result = json.loads(result_path.read_text())

        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["test_result"], "PASS")
        self.assertEqual(result["module"], "stokes")
        self.assertEqual(result["backend"], "CPU_REFERENCE")
        self.assertEqual(result["input"], {
            "order": "TFPB",
            "sample_format": "CF32",
            "shape": [2, 2, 2, 2],
            "bytes": 128,
        })
        self.assertEqual(result["output"], {
            "order": "TFBS",
            "sample_format": "F32",
            "products": ["AA", "BB", "AB_REAL", "AB_IMAG"],
            "shape": [2, 2, 2, 4],
            "bytes": 128,
        })
        self.assertEqual(result["derived_reference"]["products"], [
            "I", "Q", "U", "V",
        ])
        self.assertEqual(result["derived_reference"]["shape"], [2, 2, 2, 4])
        self.assertEqual(result["integration"], {"enabled": False})

        errors = result["errors"]
        self.assertEqual(errors["absolute_tolerance"], 1e-6)
        self.assertEqual(errors["relative_tolerance"], 1e-6)
        self.assertLessEqual(errors["max_absolute"], errors["absolute_tolerance"])
        self.assertLessEqual(errors["max_relative"], errors["relative_tolerance"])
        self.assertEqual(errors["nan_count"], 0)
        self.assertEqual(errors["inf_count"], 0)

        derived_errors = result["derived_reference"]["errors"]
        self.assertLessEqual(
            derived_errors["max_absolute"], errors["absolute_tolerance"]
        )
        self.assertLessEqual(
            derived_errors["max_relative"], errors["relative_tolerance"]
        )
        self.assertEqual(derived_errors["nan_count"], 0)
        self.assertEqual(derived_errors["inf_count"], 0)

    @unittest.skipIf(CUDA_STOKES_TEST is None, "CUDA Stokes test not supplied")
    def test_cuda_emits_complete_error_evidence(self):
        """Catches a CUDA PASS that omits quantitative error evidence."""
        with tempfile.TemporaryDirectory() as directory:
            result_path = pathlib.Path(directory) / "stokes-cuda.json"
            completed = subprocess.run(
                [str(CUDA_STOKES_TEST), "--result-json", str(result_path)],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(
                result_path.is_file(),
                "CUDA Stokes test did not write numerical evidence",
            )
            result = json.loads(result_path.read_text())

        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["test_result"], "PASS")
        self.assertEqual(result["module"], "stokes")
        self.assertEqual(result["backend"], "CUDA")
        self.assertEqual(result["input"]["shape"], [2, 2, 2, 2])
        self.assertEqual(result["input"]["bytes"], 128)
        self.assertEqual(result["output"]["shape"], [2, 2, 2, 4])
        self.assertEqual(result["output"]["bytes"], 128)
        self.assertEqual(result["output"]["products"], [
            "AA", "BB", "AB_REAL", "AB_IMAG",
        ])
        self.assertEqual(result["derived_reference"]["products"], [
            "I", "Q", "U", "V",
        ])
        self.assertEqual(result["integration"], {"enabled": False})
        errors = result["errors"]
        self.assertEqual(errors["absolute_tolerance"], 1e-5)
        self.assertEqual(errors["relative_tolerance"], 1e-5)
        self.assertLessEqual(errors["max_absolute"], errors["absolute_tolerance"])
        self.assertLessEqual(errors["max_relative"], errors["relative_tolerance"])
        self.assertEqual(errors["nan_count"], 0)
        self.assertEqual(errors["inf_count"], 0)
        derived_errors = result["derived_reference"]["errors"]
        self.assertLessEqual(
            derived_errors["max_absolute"], errors["absolute_tolerance"]
        )
        self.assertLessEqual(
            derived_errors["max_relative"], errors["relative_tolerance"]
        )
        self.assertEqual(derived_errors["nan_count"], 0)
        self.assertEqual(derived_errors["inf_count"], 0)


if __name__ == "__main__":
    unittest.main()
