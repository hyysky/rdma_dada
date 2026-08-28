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
        gpu_document = json.loads(gpu_config.read_text())
        assert gpu_document["processing"]["modules"] == [
            {
                "type": "beamform",
                "weights_file": "../../tests/data/beamform_cuda_weights_f2_p1_a2_b2_i8.npy",
                "weights_order": "FPAB2",
                "weights_id": "atfp-throughput-f2-p1-a2-b2-i8-v1",
                "weights_scale": "0.5",
                "compute_mode": "FP32",
            },
            {"type": "power"},
            {"type": "integrate", "length": 128, "operation": "MEAN"},
        ]

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
        assert override_budget["block_bytes"] == {
            "compute": 52_428_800,
            "converted": 209_715_200,
            "beamformed": 209_715_200,
            "product": 104_857_600,
            "output": 819_200,
        }
        assert override_report["stage_headers"] == [
            "RAW", "UNPACKED", "CONVERTED", "BEAMFORMED",
            "POWER_INTEGRATED",
        ]
        assert override_budget["required_rates_bytes_per_second"][
            "host_device_transfer_bytes_per_block"
        ] > 0

        staged_config = root / "staged.json"
        staged_document = json.loads(gpu_config.read_text())
        staged_document["wire"]["profile"] = str(
            (gpu_config.parent / staged_document["wire"]["profile"]).resolve()
        )
        staged_document["processing"]["modules"][0]["weights_file"] = str(
            (gpu_config.parent / staged_document["processing"]["modules"][0]["weights_file"])
            .resolve()
        )
        staged_document["processing"]["cuda_pipeline"] = {
            "mode": "STAGED_PIPELINE",
            "inflight_blocks": 3,
        }
        staged_config.write_text(json.dumps(staged_document))
        staged_preflight = run([
            str(binary), "--config", str(staged_config), "--preflight-only",
        ])
        assert staged_preflight.returncode == 0, staged_preflight.stderr
        staged_budget = json.loads(staged_preflight.stdout)["gpu_pipeline_budget"]
        assert staged_budget["execution_mode"] == "STAGED_PIPELINE"
        assert staged_budget["inflight_blocks"] == 3
        assert staged_budget["device_memory"]["slot_device_bytes_total"] == (
            staged_budget["device_memory"]["device_bytes_per_slot"] * 3
        )
        assert staged_budget["pinned_host_memory"] == {
            "pinned_input_bytes": 0,
            "pinned_output_bytes": 2_457_600,
            "planned_pinned_host_bytes": 2_457_600,
        }

        fractional_budget = run([
            str(binary), "--config", str(gpu_config),
            "--budget-payload-gbps", "0.1", "--preflight-only",
        ])
        assert fractional_budget.returncode == 0, fractional_budget.stderr
        assert json.loads(fractional_budget.stdout)["gpu_pipeline_budget"][
            "budget_target_payload_bits_per_second"
        ] == 100_000_000

        invalid_integration = root / "invalid-integration.json"
        invalid_gpu_document = json.loads(gpu_config.read_text())
        invalid_gpu_document["wire"]["profile"] = str(
            (gpu_config.parent / invalid_gpu_document["wire"]["profile"])
            .resolve()
        )
        invalid_gpu_document["processing"]["modules"][0]["weights_file"] = str(
            (gpu_config.parent / invalid_gpu_document["processing"]["modules"][0]["weights_file"])
            .resolve()
        )
        invalid_gpu_document["processing"]["modules"][-1]["length"] = 127
        invalid_integration.write_text(json.dumps(invalid_gpu_document))
        rejected_integration = run([
            str(binary), "--config", str(invalid_integration),
            "--preflight-only",
        ])
        assert rejected_integration.returncode != 0
        assert "integration length must divide block sample count" in (
            rejected_integration.stderr
        )

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
