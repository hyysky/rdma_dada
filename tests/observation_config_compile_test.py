#!/usr/bin/env python3
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(argv):
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, check=False)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: observation_config_compile_test.py BINARY CONFIG GPU_CONFIG"
        )
    binary = pathlib.Path(sys.argv[1]).resolve()
    config = pathlib.Path(sys.argv[2]).resolve()
    gpu_config = pathlib.Path(sys.argv[3]).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix="rdma_dada_compile_"))
    try:
        preflight = run([str(binary), "--config", str(config),
                         "--preflight-only"])
        assert preflight.returncode == 0, preflight.stderr
        report = json.loads(preflight.stdout)
        assert report["valid"] is True
        assert not list(root.iterdir())

        budget_override = run([
            str(binary), "--config", str(gpu_config),
            "--budget-payload-gbps", "30", "--preflight-only",
        ])
        assert budget_override.returncode == 0, budget_override.stderr
        override_report = json.loads(budget_override.stdout)
        assert override_report["valid"] is True
        override_budget = override_report["gpu_pipeline_budget"]
        assert override_budget["budget_target_payload_bits_per_second"] == 30_000_000_000
        assert override_budget["rate_source"] == "PERFORMANCE_OVERRIDE"
        assert override_budget["deadline_reserve_percent"] == 20
        assert override_budget["required_rates_bytes_per_second"][
            "host_device_transfer_bytes_per_block"
        ] > 0

        fractional_budget = run([
            str(binary), "--config", str(gpu_config),
            "--budget-payload-gbps", "0.1", "--preflight-only",
        ])
        assert fractional_budget.returncode == 0, fractional_budget.stderr
        assert json.loads(fractional_budget.stdout)["gpu_pipeline_budget"][
            "budget_target_payload_bits_per_second"
        ] == 100_000_000

        output = root / "artifacts"
        written = run([str(binary), "--config", str(config),
                       "--output-dir", str(output)])
        assert written.returncode == 0, written.stderr
        expected = {
            "resolved_observation.json", "ring_plan.json", "raw.header",
            "unpacked.header", "validation_report.json", "MANIFEST.sha256",
        }
        assert {item.name for item in output.iterdir()} == expected
        manifest_lines = (output / "MANIFEST.sha256").read_text().splitlines()
        assert len(manifest_lines) == 5
        for line in manifest_lines:
            digest, name = line.split("  ", 1)
            assert digest == hashlib.sha256((output / name).read_bytes()).hexdigest()

        refused = run([str(binary), "--config", str(config),
                       "--output-dir", str(output)])
        assert refused.returncode != 0

        invalid = root / "invalid.json"
        document = json.loads(config.read_text())
        del document["observation"]["utc_start"]
        invalid.write_text(json.dumps(document))
        rejected = run([str(binary), "--config", str(invalid),
                        "--preflight-only"])
        assert rejected.returncode != 0

        invalid_budget = run([
            str(binary), "--config", str(config),
            "--budget-payload-gbps", "0", "--preflight-only",
        ])
        assert invalid_budget.returncode != 0

        assert not list(root.glob(".artifacts.staging-*"))
    finally:
        shutil.rmtree(root)
    print("observation_config_compile_test passed")


if __name__ == "__main__":
    main()
