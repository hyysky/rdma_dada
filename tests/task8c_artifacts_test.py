#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "scripts"))
import task8c_artifacts as artifacts


def valid_process(role: str) -> dict:
    return {
        "host": "qths1",
        "role": role,
        "argv": [role],
        "env": {},
        "cpu_affinity": [1],
        "numa_node": 1,
        "thread_mapping": [],
        "binary_sha256": "a" * 64,
        "config_sha256": "b" * 64,
        "pid": 10,
        "started_utc": "2026-08-21T00:00:00+00:00",
        "ended_utc": "2026-08-21T00:00:01+00:00",
        "returncode": 0,
    }


class Task8cArtifactsTest(unittest.TestCase):
    def test_complete_ledger_passes(self):
        processes = [valid_process("compute-ring"), valid_process("pipeline_worker")]
        artifacts.validate_process_ledger(
            processes, ("compute-ring", "pipeline_worker")
        )

    def test_missing_role_or_lifecycle_fails(self):
        with self.assertRaisesRegex(ValueError, "missing required role"):
            artifacts.validate_process_ledger([], ("compute-ring",))
        process = valid_process("compute-ring")
        process["ended_utc"] = None
        with self.assertRaisesRegex(ValueError, "incomplete lifecycle"):
            artifacts.validate_process_ledger([process], ("compute-ring",))

    def test_required_role_count_is_exact(self):
        with self.assertRaisesRegex(ValueError, "expected 2"):
            artifacts.validate_process_ledger(
                [valid_process("sender")], ("sender",), {"sender": 2}
            )

    def test_manifest_is_stable_and_excludes_itself(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "summary.json").write_text("{}\n")
            artifacts.write_manifest(root)
            first = (root / "MANIFEST.sha256").read_text()
            artifacts.write_manifest(root)
            second = (root / "MANIFEST.sha256").read_text()
        self.assertEqual(first, second)
        self.assertIn("summary.json", first)
        self.assertNotIn("MANIFEST.sha256", first)

    def test_success_file_contract_is_exact(self):
        self.assertEqual(
            artifacts.expected_success_files(("warmup-01", "measured-01")),
            {
                "observation.json",
                "resolved_observation.json",
                "preflight.json",
                "summary.json",
                "MANIFEST.sha256",
                "runs/warmup-01.json",
                "runs/warmup-01.evidence.log",
                "runs/measured-01.json",
                "runs/measured-01.evidence.log",
            },
        )


if __name__ == "__main__":
    unittest.main()
