#!/usr/bin/env python3

import importlib.util
import dataclasses
import hashlib
import io
import json
import math
import pathlib
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "task8c_rate_point.py"
SPEC = importlib.util.spec_from_file_location("task8c_rate_point", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_plan(
    aggregate_gbps=1.0,
    duration_seconds=10.0,
    batch_packets=16,
    compute_consumer="dbdisk",
    raw_ring_blocks=8,
    compute_ring_blocks=8,
    pipeline_stage="full",
):
    per_station = aggregate_gbps / 2.0
    group_count = math.ceil(
        per_station * 1_000_000_000 * duration_seconds / 8.0 / 4128
    )
    source = {
        "observation": {
            "station_ids": [101, 102],
            "first_channel_id": 100,
            "nchan": 2,
            "npol": 2,
            "sample_interval_ps": 1000000,
        },
        "wire": {"samples_per_packet": 512},
        "receiver": {
            "device": "mlx5_0",
            "destination_mac": "98:03:9b:aa:99:d8",
            "destination_ip": "174.0.1.111",
            "destination_port": 1000,
        },
        "rings": {"raw_key": "0x00d2", "compute_key": "0x00d4", "output_key": "0x00d6"},
        "processing": {
            "backend": "CUDA",
            "cuda_device": 0,
            "run_once": True,
            "conversion": {"scale": "0.0078125"},
            "modules": [{
                "type": "beamform",
                "weights_file": "/tmp/weights.npy",
                "weights_order": "FPAB2",
                "weights_id": "campaign-test",
                "weights_scale": "0.5",
                "compute_mode": "FP32",
            }],
            "output": {"sample_format": "AUTO"},
        },
    }
    resolved = {
        "schema_version": 1,
        "config_id": "a" * 64,
        "geometry_id": "b" * 64,
        "source_json": json.dumps(source),
        "resolved": {
            "expected_groups": group_count,
            "group_period_ps": 512000000,
            "group_start_reference_epoch": 53,
            "group_start_seconds": 3283200,
            "group_start_frame": 0,
            "payload_bytes": 4096,
            "raw_record_bytes": 4128,
            "records_per_block": 2048,
            "raw_block_bytes": 8454144,
            "compute_block_bytes": 8388608,
            "output_block_bytes": 33554432,
        },
    }
    rings = {
        "config_id": resolved["config_id"],
        "geometry_id": resolved["geometry_id"],
        "rings": {
            "raw": {
                "key": "0x00d2",
                "blocks": raw_ring_blocks,
                "block_bytes": 8454144,
            },
            "compute": {
                "key": "0x00d4",
                "blocks": compute_ring_blocks,
                "block_bytes": 8388608,
            },
            "output": {
                "key": "0x00d6",
                "blocks": compute_ring_blocks,
                "block_bytes": 33554432,
            },
        },
    }
    artifacts = {
        "resolved_observation.json": (json.dumps(resolved) + "\n").encode(),
        "ring_plan.json": (json.dumps(rings) + "\n").encode(),
        "raw.header": (
            f"DATA_STAGE RAW\nCONFIG_ID {resolved['config_id']}\n"
            f"GEOMETRY_ID {resolved['geometry_id']}\n"
        ).encode(),
        "unpacked.header": (
            f"DATA_STAGE UNPACKED\nCONFIG_ID {resolved['config_id']}\n"
            f"GEOMETRY_ID {resolved['geometry_id']}\n"
        ).encode(),
        "output.header": (
            f"DATA_STAGE BEAMFORMED\nCONFIG_ID {resolved['config_id']}\n"
            f"GEOMETRY_ID {resolved['geometry_id']}\n"
        ).encode(),
        "validation_report.json": (
            json.dumps(
                {
                    "valid": True,
                    "config_id": resolved["config_id"],
                    "geometry_id": resolved["geometry_id"],
                }
            )
            + "\n"
        ).encode(),
    }
    artifacts["MANIFEST.sha256"] = "".join(
        f"{hashlib.sha256(value).hexdigest()}  {name}\n"
        for name, value in sorted(artifacts.items())
    ).encode()
    plan = MODULE.RatePlan(
        aggregate_gbps=aggregate_gbps,
        duration_seconds=duration_seconds,
        batch_packets=batch_packets,
        per_station_gbps=per_station,
        group_count=group_count,
        compute_consumer=compute_consumer,
        record_bytes=4128,
        raw_key="00d2",
        compute_key="00d4",
        records_per_block=2048,
        resolved_plan=resolved,
        ring_plan=rings,
        artifact_files=artifacts,
        pipeline_stage=pipeline_stage,
    )
    return plan


class FakeProcess:
    def __init__(self, returncode=0, output=""):
        self.returncode = returncode
        self.output = output
        self.terminated = False

    def terminate(self):
        self.terminated = True

    def poll(self):
        return -15 if self.terminated else None


class PolledProcess:
    def __init__(self, returncode, output=""):
        self.returncode = returncode
        self.output = output
        self.terminated = False

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        raise AssertionError("sender monitoring must not wait sequentially")

    def terminate(self):
        self.terminated = True
        self.returncode = -15


class FakeBackend:
    def __init__(self, fail_stage=None):
        self.fail_stage = fail_stage
        self.calls = []
        self.processes = [
            FakeProcess(output='{"station_id":101,"sent_packets":10,"scheduled_packets":10,"failed_packets":0,"backend":"SENDMMSG","actual_payload_gbps":0.5,"payload_prefix_hex":"65667071"}'),
            FakeProcess(output='{"station_id":102,"sent_packets":10,"scheduled_packets":10,"failed_packets":0,"backend":"SENDMMSG","actual_payload_gbps":0.5,"payload_prefix_hex":"66677172"}'),
        ]

    def _stage(self, name, value):
        self.calls.append(name)
        if self.fail_stage == name:
            raise MODULE.StageError(name, [name], 17, "stdout", "diagnostic stderr")
        return value

    def prepare(self, plan, run_dir):
        return self._stage("PREPARE", {"start_utc": "2030-01-01-00:03:00"})

    def prepare_configs(self, plan, run_dir, preparation):
        return self._stage("CONFIG_READY", {"config_sha_match": True})

    def start_pipeline(self, plan, run_dir):
        return self._stage(
            "PIPELINE_READY",
            {"rings": ["00d2", "00d4"], "capability_added": True},
        )

    def start_senders(self, plan, run_dir):
        return self._stage("SENDERS_WAITING", self.processes)

    def wait_senders(self, plan, processes):
        outputs = []
        for process in processes:
            summary = json.loads(process.output)
            summary["scheduled_packets"] = plan.group_count
            summary["sent_packets"] = plan.group_count
            summary["actual_payload_gbps"] = plan.per_station_gbps
            outputs.append(json.dumps(summary))
        return self._stage("SENDERS_RUNNING", outputs)

    def collect(self, plan, run_dir):
        return self._stage(
            "COLLECTING",
            {
                "receiver": {
                    "accepted": plan.group_count * 2,
                    "published": plan.group_count * 2,
                    "wrong_length": 0,
                    "cq_errors": 0,
                },
                "unpack": {
                    "records": plan.group_count * 2,
                    "accepted": plan.group_count * 2,
                    "bad_header": 0,
                    "invalid_data": 0,
                    "unknown_station": 0,
                    "duplicate": 0,
                    "late": 0,
                    "out_of_range": 0,
                    "complete_groups": plan.group_count,
                    "incomplete_groups": 0,
                    "fully_missing_groups": 0,
                    "missing_station": 0,
                },
                "compute": {
                    "data_bytes": plan.group_count * 8192,
                    "DATA_STAGE": "UNPACKED",
                    "ORDER": "ATFP",
                    "NANT": 2,
                    "NCHAN": 2,
                    "NPOL": 2,
                    "CONFIG_ID": plan.config_id,
                    "GEOMETRY_ID": plan.geometry_id,
                    "sample_prefix_hex": "65667071",
                },
            },
        )

    def cleanup(self, resources, run_dir):
        self.calls.append("CLEANUP")
        return {
            "rings_destroyed": list(resources.rings),
            "capability_removed": resources.capability_added,
            "CLEANUP_RESULT": "PASS",
            "errors": [],
        }


class RecordingTransport:
    def __init__(self):
        self.calls = []

    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if "date" in argv:
            stdout = "1893456000\n"
        elif "which" in argv and "dada_dbnull" in argv:
            stdout = "/usr/bin/dada_dbnull\n"
        elif "sha256sum" in argv:
            stdout = "abc123  fixture\n"
        else:
            stdout = ""
        return MODULE.subprocess.CompletedProcess(argv, 0, stdout, "")

    def start(self, argv, stdout_path):
        self.calls.append(("start", "SENDERS_WAITING", list(argv)))
        return FakeProcess(
            output='{"sent_packets":10,"scheduled_packets":10,"failed_packets":0,"backend":"SENDMMSG","actual_payload_gbps":0.5}'
        )


class ExpiringStartTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if "+%s" in argv:
            stdout = "1893456170\n"
        elif "+180 seconds" in argv:
            stdout = "2030-01-01-00:06:00\n"
        elif "sha256sum" in argv:
            stdout = "abc123  fixture\n"
        else:
            stdout = ""
        return MODULE.subprocess.CompletedProcess(argv, 0, stdout, "")


class PipelineStartFailureTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if stage == "PIPELINE_READY" and any("start.sh" in item for item in argv):
            raise MODULE.StageError(stage, argv, 19, "", "receiver failed")
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class CleanupFailureTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if stage == "CLEANUP" and any("cleanup.sh" in item for item in argv):
            return MODULE.subprocess.CompletedProcess(
                argv, 23, "cleanup stdout", "ring 00d4 still exists"
            )
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class BusySenderEndpointTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if (
            stage == "CONFIG_READY"
            and "/usr/bin/python3" in argv
            and any("probe_sender_endpoint.py" in item for item in argv)
        ):
            return MODULE.subprocess.CompletedProcess(
                argv, 98, "", "Address already in use"
            )
        if "date" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "1893456000\n", "")
        if "sha256sum" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "abc123  fixture\n", "")
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class MissingDiagnosticTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if (
            stage == "CLEANUP"
            and argv[0] == "scp"
            and any("compute-summary.json" in item for item in argv)
        ):
            return MODULE.subprocess.CompletedProcess(
                argv, 1, "", "compute-summary.json does not exist"
            )
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class EpochTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if "date" in argv:
            stdout = "1893456000\n"
        elif "sha256sum" in argv:
            stdout = "abc123  fixture\n"
        else:
            stdout = ""
        return MODULE.subprocess.CompletedProcess(argv, 0, stdout, "")


class DbnullFallbackTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if "which" in argv and "ethtool" in argv:
            return MODULE.subprocess.CompletedProcess(
                argv, 0, "/usr/sbin/ethtool\n", ""
            )
        if "which" in argv and any(
            tool in argv for tool in ("dada_db", "dada_dbnull")
        ):
            return MODULE.subprocess.CompletedProcess(argv, 1, "", "not in PATH")
        if "test" in argv and "-x" in argv:
            return MODULE.subprocess.CompletedProcess(
                argv,
                0
                if argv[-1]
                in {
                    "/home/user/psrdada/bin/dada_db",
                    "/home/user/psrdada/bin/dada_dbnull",
                }
                else 1,
                "",
                "",
            )
        if "date" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "1893456000\n", "")
        if "sha256sum" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "abc123  fixture\n", "")
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class PsrdadaFallbackTransport(RecordingTransport):
    def run(self, argv, stage, check=True):
        self.calls.append(("run", stage, list(argv)))
        if "which" in argv and "ethtool" in argv:
            return MODULE.subprocess.CompletedProcess(
                argv, 0, "/usr/sbin/ethtool\n", ""
            )
        if "which" in argv and any(
            tool in argv for tool in ("dada_db", "dada_dbdisk", "dada_dbnull")
        ):
            return MODULE.subprocess.CompletedProcess(argv, 1, "", "not in PATH")
        if "test" in argv and "-x" in argv:
            candidate = argv[-1]
            available = candidate in {
                "/home/user/psrdada/bin/dada_db",
                "/home/user/psrdada/bin/dada_dbdisk",
                "/home/user/psrdada/bin/dada_dbnull",
            }
            return MODULE.subprocess.CompletedProcess(
                argv, 0 if available else 1, "", ""
            )
        if "date" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "1893456000\n", "")
        if "sha256sum" in argv:
            return MODULE.subprocess.CompletedProcess(argv, 0, "abc123  fixture\n", "")
        return MODULE.subprocess.CompletedProcess(argv, 0, "", "")


class SecondSenderStartFailureTransport(ExpiringStartTransport):
    def __init__(self):
        super().__init__()
        self.started_process = FakeProcess()
        self.start_count = 0

    def start(self, argv, stdout_path):
        self.start_count += 1
        if self.start_count == 2:
            raise OSError("cannot create second SSH process")
        self.calls.append(("start", "SENDERS_WAITING", list(argv)))
        return self.started_process


class Task8cRatePointTest(unittest.TestCase):
    def test_result_directory_name_contains_stage_rate_duration_and_utc_time(self):
        request = MODULE.RateRequest(
            aggregate_gbps=5.0,
            duration_seconds=30.0,
            compute_consumer="dbnull",
            pipeline_stage="unpack",
        )

        self.assertEqual(
            MODULE.result_directory_name(request, "20260811T143000Z"),
            "unpack-5Gbps-30s-20260811T143000Z",
        )

    def test_remote_compiler_uses_qths_binary_and_scoped_artifacts(self):
        class RecordingTransport:
            def __init__(self):
                self.calls = []

            def run(self, argv, stage, check=True):
                self.calls.append((list(argv), stage, check))
                return MODULE.subprocess.CompletedProcess(argv, 0, "", "")

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "observation.json"
            config.write_text("{}\n")
            output = root / "artifacts"
            transport = RecordingTransport()
            compiler = MODULE.RemoteObservationCompiler(
                transport,
                root / "known-hosts",
                pathlib.Path(
                    "/home/user/wy/rdma_dada/build-release/observation_config_compile"
                ),
                root / "run",
            )
            compiler(config, None)
            compiler(config, output)
            compiler.close()

        flattened = [call[0] for call in transport.calls]
        remote_invocations = [
            argv for argv in flattened
            if "/home/user/wy/rdma_dada/build-release/observation_config_compile"
            in argv and "--config" in argv
        ]
        self.assertEqual(len(remote_invocations), 2)
        self.assertIn("--preflight-only", remote_invocations[0])
        self.assertIn("--output-dir", remote_invocations[1])
        self.assertTrue(
            any(argv[0] == "scp" and "-r" in argv for argv in flattened)
        )
        self.assertTrue(
            any("rm" in argv and "-rf" in argv for argv in flattened)
        )

    def test_group_count_uses_exact_decimal_rate_arithmetic(self):
        request = MODULE.RateRequest(
            aggregate_gbps=0.1,
            duration_seconds=10.0,
        )
        self.assertEqual(
            MODULE._group_count_for_request(request, 2, 4128),
            15141,
        )

    def test_window_blocks_include_wait_and_skew_reserve_at_30gbps(self):
        self.assertEqual(
            MODULE._window_blocks_for_horizon(
                aggregate_gbps=30.0,
                missing_wait_ms=200.0,
                station_skew_reserve_ms=200.0,
                station_count=2,
                record_bytes=4128,
                groups_per_block=6400,
            ),
            (31, 96000),
        )

    def test_compile_rate_plan_renders_rate_derived_window_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            template = root / "observation.template.json"
            template.write_text(json.dumps({
                "observation": {"observation_id": "fixture"},
                "wire": {"profile": "/tmp/wire.json"},
                "blocks": {"window_blocks": 4},
                "processing": {"modules": []},
            }))
            bootstrap = dataclasses.replace(
                make_plan(
                    aggregate_gbps=15.0,
                    duration_seconds=30.0,
                    compute_consumer="dbnull",
                    pipeline_stage="unpack",
                ),
                records_per_block=12800,
            )
            final = dataclasses.replace(
                bootstrap,
                group_count=MODULE._group_count_for_request(
                    MODULE.RateRequest(
                        15.0,
                        30.0,
                        compute_consumer="dbnull",
                        pipeline_stage="unpack",
                        missing_wait_ms=200.0,
                        station_skew_reserve_ms=200.0,
                    ),
                    bootstrap.nant,
                    bootstrap.record_bytes,
                ),
            )
            with mock.patch.object(MODULE, "_run_observation_compiler"), \
                    mock.patch.object(
                        MODULE.RatePlan,
                        "from_artifact_directory",
                        side_effect=[bootstrap, final],
                    ):
                MODULE.compile_rate_plan(
                    MODULE.RateRequest(
                        15.0,
                        30.0,
                        compute_consumer="dbnull",
                        pipeline_stage="unpack",
                        missing_wait_ms=200.0,
                        station_skew_reserve_ms=200.0,
                    ),
                    template,
                    pathlib.Path("/tmp/compiler"),
                    root / "run",
                    compiler_executor=object(),
                )

            rendered = json.loads((root / "run" / "observation.json").read_text())
            self.assertEqual(rendered["blocks"]["window_blocks"], 17)

    def test_dry_run_accepts_missing_wait_and_station_skew_times(self):
        with mock.patch("sys.stdout", new_callable=io.StringIO) as stdout:
            return_code = MODULE.main([
                "--aggregate-gbps", "15",
                "--duration-seconds", "30",
                "--compute-consumer", "dbnull",
                "--pipeline-stage", "unpack",
                "--missing-wait-ms", "200",
                "--station-skew-reserve-ms", "200",
                "--dry-run",
            ])
        self.assertEqual(return_code, 0)
        request = json.loads(stdout.getvalue())
        self.assertEqual(request["missing_wait_ms"], 200.0)
        self.assertEqual(request["station_skew_reserve_ms"], 200.0)

    def test_rate_plan_loads_all_geometry_from_compiler_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = {
                "observation": {
                    "station_ids": [101, 102],
                    "first_channel_id": 100,
                    "nchan": 2,
                    "npol": 2,
                    "sample_interval_ps": 1000000,
                },
                "wire": {"samples_per_packet": 512},
            }
            resolved = {
                "schema_version": 1,
                "config_id": "a" * 64,
                "geometry_id": "b" * 64,
                "source_json": json.dumps(source),
                "resolved": {
                    "expected_groups": 15141,
                    "group_period_ps": 512000000,
                    "group_start_reference_epoch": 53,
                    "group_start_seconds": 3283200,
                    "group_start_frame": 0,
                    "payload_bytes": 4096,
                    "raw_record_bytes": 4128,
                    "records_per_block": 2048,
                    "raw_block_bytes": 8454144,
                    "compute_block_bytes": 8388608,
                    "output_block_bytes": 33554432,
                },
            }
            ring_plan = {
                "config_id": "a" * 64,
                "geometry_id": "b" * 64,
                "rings": {
                    "raw": {"key": "0x00d2", "blocks": 8,
                            "block_bytes": 8454144},
                    "compute": {"key": "0x00d4", "blocks": 6,
                                "block_bytes": 8388608},
                    "output": {"key": "0x00d6", "blocks": 6,
                               "block_bytes": 33554432},
                }
            }
            contents = {
                "resolved_observation.json": json.dumps(resolved).encode(),
                "ring_plan.json": json.dumps(ring_plan).encode(),
                "raw.header": (
                    f"DATA_STAGE RAW\nCONFIG_ID {resolved['config_id']}\n"
                    f"GEOMETRY_ID {resolved['geometry_id']}\n"
                ).encode(),
                "unpacked.header": (
                    f"DATA_STAGE UNPACKED\nCONFIG_ID {resolved['config_id']}\n"
                    f"GEOMETRY_ID {resolved['geometry_id']}\n"
                ).encode(),
                "converted.header": (
                    f"DATA_STAGE CONVERTED\nCONFIG_ID {resolved['config_id']}\n"
                    f"GEOMETRY_ID {resolved['geometry_id']}\n"
                ).encode(),
                "beamformed.header": (
                    f"DATA_STAGE BEAMFORMED\nCONFIG_ID {resolved['config_id']}\n"
                    f"GEOMETRY_ID {resolved['geometry_id']}\n"
                ).encode(),
                "output.header": (
                    f"DATA_STAGE BEAMFORMED\nCONFIG_ID {resolved['config_id']}\n"
                    f"GEOMETRY_ID {resolved['geometry_id']}\n"
                ).encode(),
                "validation_report.json": json.dumps(
                    {
                        "valid": True,
                        "config_id": resolved["config_id"],
                        "geometry_id": resolved["geometry_id"],
                    }
                ).encode(),
            }
            manifest = []
            for name, value in sorted(contents.items()):
                (root / name).write_bytes(value)
                manifest.append(f"{hashlib.sha256(value).hexdigest()}  {name}")
            (root / "MANIFEST.sha256").write_text("\n".join(manifest) + "\n")
            plan = MODULE.RatePlan.from_artifact_directory(
                root, aggregate_gbps=1.0, duration_seconds=10.0,
                batch_packets=16, compute_consumer="dbdisk"
            )
            (root / "raw.header").write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
                MODULE.RatePlan.from_artifact_directory(
                    root,
                    aggregate_gbps=1.0,
                    duration_seconds=10.0,
                    batch_packets=16,
                    compute_consumer="dbdisk",
                )
        self.assertEqual(plan.group_count, 15141)
        self.assertEqual(plan.raw_key, "00d2")
        self.assertEqual(plan.compute_key, "00d4")
        self.assertEqual(plan.output_key, "00d6")
        self.assertEqual(plan.raw_ring_blocks, 8)
        self.assertEqual(plan.compute_ring_blocks, 6)
        self.assertEqual(plan.output_ring_blocks, 6)
        self.assertEqual(plan.output_block_bytes, 33554432)
        self.assertEqual(plan.config_id, "a" * 64)
        self.assertEqual(plan.geometry_id, "b" * 64)

    def test_qths_bundle_uses_resolved_plan_and_no_legacy_geometry_json(self):
        plan = dataclasses.replace(
            make_plan(0.1, 10.0, compute_consumer="dbnull"),
            reorder_horizon_groups=1234,
        )
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-test"
        )
        self.assertIn("resolved_observation.json", bundle)
        self.assertIn("ring_plan.json", bundle)
        self.assertNotIn("pipeline.json", bundle)
        self.assertNotIn("packet.json", bundle)
        self.assertNotIn("worker.json", bundle)
        self.assertIn(
            'vdif_unpack_worker" --plan "$run_dir/resolved_observation.json"',
            bundle["start.sh"],
        )
        self.assertIn(
            '--reorder-horizon-groups 1234', bundle["start.sh"]
        )
        self.assertIn(
            'rdma2dada" --plan "$run_dir/resolved_observation.json"',
            bundle["start.sh"],
        )
        self.assertIn(
            'pipeline_worker" "$run_dir/resolved_observation.json"',
            bundle["start.sh"],
        )
        self.assertNotIn('pipeline_worker" --plan', bundle["start.sh"])

    def test_ring_creation_uses_compiler_ring_plan_values(self):
        plan = make_plan(0.1, 10.0, compute_consumer="dbnull")
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-test"
        )
        prepare = bundle["prepare.sh"]
        self.assertIn(
            f"-k {plan.raw_key} -b {plan.raw_block_bytes} -a 4096 "
            f"-n {plan.raw_ring_blocks}", prepare
        )
        self.assertIn(
            f"-k {plan.compute_key} -b {plan.compute_block_bytes} -a 4096 "
            f"-n {plan.compute_ring_blocks}", prepare
        )
        self.assertIn(
            f"-k {plan.output_key} -b {plan.output_block_bytes} -a 4096 "
            f"-n {plan.output_ring_blocks}", prepare
        )

    def test_dbnull_consumes_one_complete_output_transfer_zero_copy(self):
        plan = make_plan(1.0, 30.0, compute_consumer="dbnull")
        bundle = MODULE.build_qths_bundle(plan, "/tmp/task8c-full-pipeline")
        self.assertIn(
            f"dada_dbnull -k {plan.output_key} -s -z -q",
            bundle["start.sh"],
        )
        self.assertNotIn(
            f"dada_dbnull -k {plan.compute_key}", bundle["start.sh"]
        )
        self.assertIn("pipeline-worker.pid", bundle["start.sh"])
        self.assertIn("output-ring.pid", bundle["prepare.sh"])
        self.assertIn("output-ring.created", bundle["cleanup.sh"])
        for process in ("reader", "pipeline-worker", "worker", "receiver"):
            self.assertIn(
                f'kill -0 "$(cat "$run_dir/{process}.pid")"',
                bundle["start.sh"],
            )
            self.assertIn(f'"$run_dir/{process}.exit"', bundle["start.sh"])
        self.assertIn('exited during readiness', bundle["start.sh"])
        self.assertIn('tail -n 40 "$run_dir/$name.log"', bundle["start.sh"])
        self.assertIn(
            "Initialization complete, ready to start",
            bundle["start.sh"],
        )
        self.assertIn("readiness timed out", bundle["start.sh"])
        self.assertIn("report_readiness_state", bundle["start.sh"])

    def test_unpack_stage_drains_compute_ring_without_gpu_pipeline(self):
        plan = make_plan(
            1.0,
            30.0,
            compute_consumer="dbnull",
            pipeline_stage="unpack",
        )
        bundle = MODULE.build_qths_bundle(plan, "/tmp/task8c-unpack-only")
        self.assertIn(
            f"dada_dbnull -k {plan.compute_key} -s -z -q",
            bundle["start.sh"],
        )
        self.assertNotIn("pipeline_worker", bundle["start.sh"])
        self.assertNotIn("output-ring", bundle["prepare.sh"])
        self.assertNotIn("output-ring", bundle["cleanup.sh"])
        self.assertNotIn(f"-k {plan.output_key}", bundle["prepare.sh"])
        self.assertNotIn("output_key", plan.as_dict())
        self.assertNotIn("output_block_bytes", plan.as_dict())
        for process in ("reader", "worker", "receiver"):
            self.assertIn(
                f'kill -0 "$(cat "$run_dir/{process}.pid")"',
                bundle["start.sh"],
            )

    def test_unpack_stage_dbnull_statistics_do_not_require_gpu_marker(self):
        plan = make_plan(
            0.1,
            10.0,
            compute_consumer="dbnull",
            pipeline_stage="unpack",
        )
        records = plan.group_count * plan.nant
        receiver = (
            f"Receive summary: accepted={records}, wrong_length=0, "
            f"published={records}, blocks=1, partial_blocks=1, "
            "cq_tail_records=0\n"
        )
        worker = (
            "VDIF unpack statistics: "
            f"records={records} accepted={records} bad_header=0 invalid_data=0 "
            "unknown_station=0 duplicate=0 late=0 out_of_range=0 "
            f"complete_groups={plan.group_count} incomplete_groups=0 "
            f"fully_missing_groups=0 missing_station=0/{records} "
            "large_gap_advances=0/0 max_station_ordinal_skew=0 "
            "raw_blocks_single=0 raw_blocks_mixed=1 "
            "max_station_records_per_raw_block=1 max_consecutive_station_records=1\n"
            f"VDIF unpack station statistics: antenna=0 station=101 "
            f"observed={plan.group_count} accepted={plan.group_count} late=0 "
            f"highest_ordinal={plan.group_count - 1}\n"
            f"VDIF unpack station statistics: antenna=1 station=102 "
            f"observed={plan.group_count} accepted={plan.group_count} late=0 "
            f"highest_ordinal={plan.group_count - 1}\n"
            "VDIF unpack transfer completed\n"
        )
        statistics = MODULE.parse_qths_statistics(
            receiver,
            worker,
            {
                "consumer": "dada_dbnull",
                "exit_code": 0,
                "zero_copy": True,
                "single_transfer": True,
            },
            expect_pipeline_worker=False,
        )
        self.assertNotIn("gpu", statistics)
        MODULE._validate_statistics(statistics, plan, "")

    def test_hf_controller_does_not_ssh_back_through_hf(self):
        argv = MODULE.build_ssh_argv(
            "qths1", ["date", "-u", "+%s"], "/tmp/task8c-known-hosts"
        )
        self.assertEqual(argv[0], "ssh")
        self.assertIn("qths1", argv)
        self.assertNotIn("HF", argv)
        self.assertEqual(argv[-3:], ["date", "-u", "+%s"])

    def test_rate_plan_preserves_unaligned_finite_transfer_boundary(self):
        plan = make_plan(1.0, 10.0, batch_packets=16)
        self.assertEqual(plan.per_station_gbps, 0.5)
        self.assertEqual(plan.record_bytes, 4128)
        self.assertEqual(plan.group_count, math.ceil(625_000_000 / 4128))
        self.assertNotEqual(plan.group_count % 16, 0)
        self.assertNotEqual((plan.group_count * 2) % plan.records_per_block, 0)
        self.assertGreaterEqual(plan.group_count * plan.record_bytes * 8, 5_000_000_000)

    def test_sender_configs_share_geometry_and_start_time(self):
        plan = make_plan(1.0, 10.0, batch_packets=16)
        first = MODULE.build_sender_config(
            plan, 101, "174.0.1.100", 41001, "2030-01-01-00:03:00"
        )
        second = MODULE.build_sender_config(
            plan, 102, "174.0.1.101", 42001, "2030-01-01-00:03:00"
        )
        self.assertEqual(first["time"], second["time"])
        self.assertEqual(first["time"]["group_count"], plan.group_count)
        self.assertEqual(first["transmit"]["target_gbps"], 0.5)
        self.assertEqual(first["station"]["station_id"], 101)
        self.assertEqual(second["station"]["station_id"], 102)
        self.assertEqual(first["destination"], second["destination"])

    def test_sender_source_ports_are_deterministic_per_run_and_distinct(self):
        first = MODULE.derive_sender_source_ports("suite-a-measured-01")
        repeated = MODULE.derive_sender_source_ports("suite-a-measured-01")
        second_run = MODULE.derive_sender_source_ports("suite-a-measured-02")
        self.assertEqual(first, repeated)
        self.assertNotEqual(first, second_run)
        self.assertTrue(40000 <= first[0] < 50000)
        self.assertTrue(50000 <= first[1] < 60000)
        self.assertNotEqual(first[0], first[1])

    def test_qths_bundle_uses_exact_pid_files_and_no_wildcard_kill(self):
        plan = make_plan(1.0, 10.0)
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-run-abc"
        )
        combined = "\n".join(
            value for value in bundle.values() if isinstance(value, str)
        )
        self.assertIn("receiver.pid", combined)
        self.assertIn("worker.pid", combined)
        self.assertIn("reader.pid", combined)
        self.assertIn("raw-ring.pid", combined)
        self.assertIn("compute-ring.pid", combined)
        self.assertNotIn("pkill", combined)
        self.assertNotIn("killall", combined)
        self.assertIn(str(plan.raw_block_bytes), bundle["prepare.sh"])
        self.assertIn(str(plan.compute_block_bytes), bundle["prepare.sh"])

    def test_qths_bundle_does_not_regenerate_wire_or_geometry(self):
        plan = make_plan(0.1, 10.0)
        bundle = MODULE.build_qths_bundle(
            plan,
            "/tmp/task8c-wire-only",
        )
        self.assertEqual(
            bundle["resolved_observation.json"],
            plan.artifact_files["resolved_observation.json"],
        )
        self.assertNotIn("packet.json", bundle)
        self.assertNotIn("pipeline.json", bundle)
        self.assertNotIn("worker.json", bundle)

    def test_qths_bundle_can_select_an_isolated_release_binary_directory(self):
        plan = make_plan(0.1, 10.0)
        bundle = MODULE.build_qths_bundle(
            plan,
            "/tmp/task8c-release",
            qths_binary_dir="/home/user/wy/rdma_dada/build-task8c-release",
        )
        self.assertIn(
            'project=/home/user/wy/rdma_dada/build-task8c-release',
            bundle["start.sh"],
        )
        self.assertIn('"$project/vdif_unpack_worker"', bundle["start.sh"])
        self.assertIn('"$project/rdma2dada"', bundle["start.sh"])
        self.assertIn(
            "binary=/home/user/wy/rdma_dada/build-task8c-release/rdma2dada",
            bundle["cleanup.sh"],
        )

    def test_backend_uses_explicit_release_sender_binary_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            transport = RecordingTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
                sender_binary_dir=pathlib.Path(
                    "/home/user/wy/rdma_dada/build-observation-task7-release"
                ),
            )
            plan = make_plan(0.1, 10.0)
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
            backend.start_senders(plan, run_dir)
        expected = (
            "/home/user/wy/rdma_dada/"
            "build-observation-task7-release/fpga_sender_sim"
        )
        self.assertEqual(evidence["binary_path"]["qtpulsar1:fpga_sender_sim"], expected)
        sender_commands = [
            call[2] for call in transport.calls if call[0] == "start"
        ]
        self.assertEqual(len(sender_commands), 2)
        self.assertTrue(all(expected in command for command in sender_commands))

    def test_dbnull_consumer_uses_single_transfer_zero_copy_without_disk_output(self):
        plan = make_plan(
            5.0, 10.0, compute_consumer="dbnull"
        )
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-dbnull"
        )
        self.assertIn("dada_dbnull -k 00d6 -s -z -q", bundle["start.sh"])
        self.assertIn(
            '/usr/bin/python3 "$run_dir/supervise.py" '
            '"$run_dir/reader.exit"',
            bundle["start.sh"],
        )
        self.assertNotIn("dada_dbdisk", bundle["start.sh"])
        self.assertIn('"consumer": "dada_dbnull"', bundle["summarize.py"])

    def test_qths_bundle_captures_run_scoped_nic_counters(self):
        plan = make_plan(5.0, 30.0, compute_consumer="dbnull")
        bundle = MODULE.build_qths_bundle(
            plan,
            "/tmp/task8c-nic-counters",
            ethtool_path="/usr/sbin/ethtool",
        )
        self.assertIn("capture_nic_counters.py", bundle)
        self.assertIn(
            'capture_nic_counters.py" mlx5_0 /usr/sbin/ethtool '
            '"$run_dir/nic-before.json"',
            bundle["start.sh"],
        )
        self.assertIn(
            'capture_nic_counters.py" mlx5_0 /usr/sbin/ethtool '
            '"$run_dir/nic-after.json"',
            bundle["finish.sh"],
        )

    def test_nic_counter_delta_preserves_before_after_and_positive_delta(self):
        before = {
            "interface": "ens2np0",
            "sysfs": {"rx_packets": 100, "rx_dropped": 3},
            "ethtool": {"rx_out_of_buffer": 7, "rx_discards_phy": 11},
        }
        after = {
            "interface": "ens2np0",
            "sysfs": {"rx_packets": 140, "rx_dropped": 4},
            "ethtool": {"rx_out_of_buffer": 9, "rx_discards_phy": 11},
        }
        result = MODULE.nic_counter_evidence(before, after)
        self.assertEqual(result["interface"], "ens2np0")
        self.assertEqual(result["delta"]["sysfs"]["rx_packets"], 40)
        self.assertEqual(result["delta"]["sysfs"]["rx_dropped"], 1)
        self.assertEqual(result["delta"]["ethtool"]["rx_out_of_buffer"], 2)
        self.assertEqual(result["delta"]["ethtool"]["rx_discards_phy"], 0)

    def test_nic_counter_delta_rejects_interface_change(self):
        before = {"interface": "ens2np0", "sysfs": {}, "ethtool": {}}
        after = {"interface": "ens3np0", "sysfs": {}, "ethtool": {}}
        with self.assertRaises(ValueError):
            MODULE.nic_counter_evidence(before, after)

    def test_dbnull_statistics_require_consumer_eod_and_exact_unpack_counts(self):
        plan = make_plan(
            5.0, 10.0, compute_consumer="dbnull"
        )
        records = plan.group_count * 2
        receiver = (
            f"[RDMA] Receive summary: accepted={records}, wrong_length=0, "
            f"published={records}, blocks=149, partial_blocks=1, "
            "cq_tail_records=12, wrong_length_ratio=0.000000000\n"
        )
        worker = (
            "VDIF unpack statistics: "
            f"records={records} accepted={records} bad_header=0 invalid_data=0 "
            "unknown_station=0 duplicate=0 late=0 out_of_range=0 "
            f"complete_groups={plan.group_count} incomplete_groups=0 "
            "fully_missing_groups=0 "
            f"missing_station=0/{records} (0.000000%) "
            "large_gap_advances=0/0 max_station_ordinal_skew=0 "
            "raw_blocks_single=0 raw_blocks_mixed=149 "
            "max_station_records_per_raw_block=1024 "
            "max_consecutive_station_records=16\n"
            f"VDIF unpack station statistics: antenna=0 station=101 "
            f"observed={plan.group_count} accepted={plan.group_count} "
            f"late=0 highest_ordinal={plan.group_count - 1}\n"
            f"VDIF unpack station statistics: antenna=1 station=102 "
            f"observed={plan.group_count} accepted={plan.group_count} "
            f"late=0 highest_ordinal={plan.group_count - 1}\n"
            "VDIF unpack transfer completed\n"
        )
        statistics = MODULE.parse_qths_statistics(
            receiver,
            worker,
            {
                "consumer": "dada_dbnull",
                "exit_code": 0,
                "zero_copy": True,
                "single_transfer": True,
            },
            "pipeline transfer completed\n",
        )
        self.assertTrue(statistics["gpu"]["completed"])
        MODULE._validate_statistics(statistics, plan, "65667071")
        statistics["compute"]["exit_code"] = 1
        with self.assertRaises(MODULE.StageError) as raised:
            MODULE._validate_statistics(statistics, plan, "65667071")
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")

    def test_dbnull_count_deficit_is_a_performance_failure(self):
        plan = make_plan(5.0, 30.0, compute_consumer="dbnull")
        records = plan.group_count * plan.nant
        statistics = {
            "receiver": {
                "accepted": records - 1,
                "published": records - 1,
                "wrong_length": 0,
                "cq_errors": 0,
            },
            "unpack": {
                "records": records - 1,
                "accepted": records - 1,
                "bad_header": 0,
                "invalid_data": 0,
                "unknown_station": 0,
                "duplicate": 0,
                "late": 0,
                "out_of_range": 0,
                "complete_groups": plan.group_count - 1,
                "incomplete_groups": 1,
                "fully_missing_groups": 0,
                "missing_station": 1,
            },
            "compute": {
                "consumer": "dada_dbnull",
                "exit_code": 0,
                "zero_copy": True,
                "single_transfer": True,
            },
            "gpu": {"completed": True},
        }
        with self.assertRaises(MODULE.StageError) as raised:
            MODULE._validate_statistics(statistics, plan, "65667071")
        self.assertEqual(raised.exception.classification, "PERFORMANCE_FAIL")

    def test_live_backend_builds_direct_host_commands_and_local_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "format_id": "project-vdif-v1",
                        "record": {},
                        "application_header": {"fields": []},
                        "payload": {"axes": []},
                    }
                )
            )
            transport = RecordingTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            plan = make_plan(1.0, 10.0)
            run_dir = root / "results" / "run"
            run_dir.mkdir(parents=True)
            preparation = backend.prepare(plan, run_dir)
            config_evidence = backend.prepare_configs(plan, run_dir, preparation)
            commands = [call[2] for call in transport.calls]
            self.assertTrue((run_dir / "bundle" / "qths" / "prepare.sh").exists())
            self.assertTrue((run_dir / "bundle" / "sender101.json").exists())
            self.assertTrue((run_dir / "bundle" / "sender102.json").exists())
        self.assertTrue(any("qths1" in command for command in commands))
        self.assertFalse(any("HF" in command for command in commands))
        self.assertEqual(
            sorted(config_evidence["config_sha"]),
            [
                "qths1:MANIFEST.sha256",
                "qths1:capture_nic_counters.py",
                "qths1:cleanup.sh",
                "qths1:finish.sh",
                "qths1:output.header",
                "qths1:prepare.sh",
                "qths1:raw.header",
                "qths1:resolved_observation.json",
                "qths1:ring_plan.json",
                "qths1:start.sh",
                "qths1:summarize.py",
                "qths1:supervise.py",
                "qths1:unpacked.header",
                "qths1:validation_report.json",
                "qtpulsar1:101.json",
                "qtpulsar1:probe_sender_endpoint.py",
                "qtpulsar2:102.json",
                "qtpulsar2:probe_sender_endpoint.py",
            ],
        )
        self.assertEqual(
            sorted(config_evidence["binary_sha"]),
            [
                "qths1:dada_db",
                "qths1:dada_dbdisk",
                "qths1:ethtool",
                "qths1:rdma2dada",
                "qths1:vdif_unpack_worker",
                "qtpulsar1:fpga_sender_sim",
                "qtpulsar2:fpga_sender_sim",
            ],
        )

    def test_prepare_configs_preflights_each_sender_source_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            transport = RecordingTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            plan = make_plan(0.1, 10.0)
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
        probes = [
            call
            for call in transport.calls
            if call[0] == "run"
            and call[1] == "CONFIG_READY"
            and "/usr/bin/python3" in call[2]
            and any("probe_sender_endpoint.py" in item for item in call[2])
        ]
        self.assertEqual(len(probes), 2)
        self.assertTrue(all("-c" not in probe[2] for probe in probes))
        self.assertEqual(len(evidence["sender_endpoints"]), 2)
        self.assertEqual(len(evidence["source_ports"]), 2)

    def test_busy_sender_endpoint_is_environment_blocker_before_rings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            backend = MODULE.SshBackend(
                transport=BusySenderEndpointTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            plan = make_plan(0.1, 10.0)
            preparation = backend.prepare(plan, run_dir)
            with self.assertRaises(MODULE.StageError) as raised:
                backend.prepare_configs(plan, run_dir, preparation)
        self.assertEqual(raised.exception.classification, "ENV_BLOCKED")
        self.assertEqual(raised.exception.stage, "CONFIG_READY")

    def test_remote_run_directory_includes_suite_and_run_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            run_dir = root / "suite-20260807" / "measured-01"
            run_dir.mkdir(parents=True)
            backend = MODULE.SshBackend(
                transport=RecordingTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            preparation = backend.prepare(
                make_plan(1.0, 10.0), run_dir
            )
        self.assertEqual(
            preparation["remote_run_dir"],
            "/tmp/task8c-suite-20260807-measured-01",
        )
        self.assertTrue(str(backend.known_hosts).endswith("suite-20260807-measured-01"))

    def test_future_start_is_derived_from_remote_epoch_without_spaced_date_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            transport = EpochTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            preparation = backend.prepare(
                make_plan(1.0, 10.0), run_dir
            )
        self.assertEqual(preparation["start_utc"], "2030-01-01-00:03:00")
        date_commands = [
            call[2] for call in transport.calls if "date" in call[2]
        ]
        self.assertEqual(date_commands[0][-3:], ["date", "-u", "+%s"])
        self.assertFalse(any(" " in argument for argument in date_commands[0][-3:]))

    def test_dbnull_binary_is_included_in_remote_manifest_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            backend = MODULE.SshBackend(
                transport=RecordingTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            plan = make_plan(
                5.0, 10.0, compute_consumer="dbnull"
            )
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
        self.assertEqual(evidence["binary_sha"]["qths1:dada_dbnull"], "abc123")

    def test_dbnull_falls_back_to_known_absolute_install_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            backend = MODULE.SshBackend(
                transport=DbnullFallbackTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            plan = make_plan(
                5.0, 10.0, compute_consumer="dbnull"
            )
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
            start_script = (run_dir / "bundle" / "qths" / "start.sh").read_text()
        self.assertIn(
            "/home/user/psrdada/bin/dada_dbnull -k 00d6 -s -z -q",
            start_script,
        )
        self.assertEqual(evidence["binary_sha"]["qths1:dada_dbnull"], "abc123")

    def test_dbdisk_bundle_uses_preflighted_absolute_psrdada_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            backend = MODULE.SshBackend(
                transport=PsrdadaFallbackTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            plan = make_plan(
                1.0, 10.0, compute_consumer="dbdisk"
            )
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
            qths = run_dir / "bundle" / "qths"
            prepare_script = (qths / "prepare.sh").read_text()
            start_script = (qths / "start.sh").read_text()
            cleanup_script = (qths / "cleanup.sh").read_text()
        self.assertIn(
            "/home/user/psrdada/bin/dada_db -k 00d2", prepare_script
        )
        self.assertIn(
            "/home/user/psrdada/bin/dada_dbdisk -k 00d4", start_script
        )
        self.assertIn(
            "/home/user/psrdada/bin/dada_db -d -k 00d2", cleanup_script
        )
        self.assertEqual(evidence["binary_sha"]["qths1:dada_db"], "abc123")
        self.assertEqual(
            evidence["binary_sha"]["qths1:dada_dbdisk"], "abc123"
        )

    def test_cleanup_skips_missing_runtime_logs_when_prepare_created_no_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            transport = RecordingTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.remote_run_dir = "/tmp/task8c-prepare-failed"
            cleanup = backend.cleanup(MODULE.RunResources(), root)
        diagnostic_copies = [
            call for call in transport.calls
            if call[0] == "run"
            and call[1] == "CLEANUP"
            and call[2][0] == "scp"
        ]
        self.assertEqual(diagnostic_copies, [])
        self.assertEqual(cleanup["CLEANUP_RESULT"], "PASS")

    def test_automatically_generated_run_ids_do_not_collide_within_one_second(self):
        first = MODULE.RatePointController(FakeBackend(), pathlib.Path("/tmp"))
        second = MODULE.RatePointController(FakeBackend(), pathlib.Path("/tmp"))
        self.assertNotEqual(first.run_id, second.run_id)

    def test_success_persists_result_before_cleanup_state(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = FakeBackend()
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="success"
            )
            result = controller.run(make_plan(1.0, 10.0))
            saved = json.loads(
                (pathlib.Path(directory) / "success" / "result.json").read_text()
            )
            state = json.loads(
                (pathlib.Path(directory) / "success" / "state.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(saved["TEST_RESULT"], "PASS")
        self.assertEqual(state["state"], "CLEANED")
        self.assertEqual(state["test_result"], "PASS")
        self.assertEqual(saved["cleanup"]["rings_destroyed"], ["00d2", "00d4"])

    def test_failure_keeps_command_diagnostics_after_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = FakeBackend(fail_stage="CONFIG_READY")
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="failure"
            )
            result = controller.run(make_plan(1.0, 10.0))
            saved = json.loads(
                (pathlib.Path(directory) / "failure" / "result.json").read_text()
            )
            state = json.loads(
                (pathlib.Path(directory) / "failure" / "state.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(saved["failure"]["stage"], "CONFIG_READY")
        self.assertEqual(saved["failure"]["exit_code"], 17)
        self.assertEqual(saved["failure"]["stderr"], "diagnostic stderr")
        self.assertEqual(state["state"], "CLEANED")
        self.assertEqual(state["test_result"], "HARNESS_FAIL")
        self.assertEqual(saved["cleanup"]["rings_destroyed"], [])
        self.assertFalse(saved["cleanup"]["capability_removed"])

    def test_sender_failure_is_not_reported_as_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = FakeBackend()
            backend.processes[1].output = (
                '{"sent_packets":9,"scheduled_packets":10,'
                '"failed_packets":1,"backend":"SENDMMSG",'
                '"actual_payload_gbps":0.45}'
            )
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="sender-failure"
            )
            result = controller.run(make_plan(1.0, 10.0))
        self.assertEqual(result["TEST_RESULT"], "PRODUCT_FAIL")
        self.assertEqual(result["failure"]["stage"], "SENDERS_RUNNING")

    def test_missing_pipeline_statistics_is_not_reported_as_pass(self):
        class MissingStatisticsBackend(FakeBackend):
            def collect(self, plan, run_dir):
                return {"receiver": {}, "unpack": {}, "compute": {}}

        with tempfile.TemporaryDirectory() as directory:
            controller = MODULE.RatePointController(
                MissingStatisticsBackend(),
                pathlib.Path(directory),
                run_id="missing-statistics",
            )
            result = controller.run(make_plan(1.0, 10.0))
        self.assertEqual(result["TEST_RESULT"], "PRODUCT_FAIL")
        self.assertEqual(result["failure"]["stage"], "COLLECTING")

    def test_expired_start_time_is_regenerated_for_both_senders(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            run_dir = root / "results" / "run"
            run_dir.mkdir(parents=True)
            transport = ExpiringStartTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.local_run_dir = run_dir
            backend.remote_run_dir = "/tmp/task8c-run"
            backend.preparation = {
                "start_utc": "2030-01-01-00:03:00",
                "remote_run_dir": backend.remote_run_dir,
            }
            backend._write_bundle(
                make_plan(1.0, 10.0),
                backend.preparation["start_utc"],
            )
            backend.start_senders(make_plan(1.0, 10.0), run_dir)
            first = json.loads((run_dir / "bundle" / "sender101.json").read_text())
            second = json.loads((run_dir / "bundle" / "sender102.json").read_text())
        self.assertEqual(first["time"]["start_utc"], "2030-01-01-00:05:50")
        self.assertEqual(second["time"]["start_utc"], "2030-01-01-00:05:50")
        sender_transfers = [
            call
            for call in transport.calls
            if call[0] == "run"
            and call[1] == "CONFIG_READY"
            and call[2][0] == "scp"
            and ("101.json" in call[2][-1] or "102.json" in call[2][-1])
        ]
        self.assertEqual(len(sender_transfers), 2)

    def test_pipeline_start_failure_still_cleans_acquired_rings_and_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            transport = PipelineStartFailureTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.remote_run_dir = "/tmp/task8c-partial"
            run_dir = root / "results"
            run_dir.mkdir()
            with self.assertRaises(MODULE.StageError) as raised:
                backend.start_pipeline(make_plan(1.0, 10.0), run_dir)
            cleanup = backend.cleanup(MODULE.RunResources(), run_dir)
        cleanup_commands = [
            call[2]
            for call in transport.calls
            if call[0] == "run" and call[1] == "CLEANUP"
        ]
        self.assertEqual(cleanup["rings_destroyed"], ["00d2", "00d4"])
        self.assertTrue(cleanup["capability_removed"])
        self.assertTrue(
            any("/tmp/task8c-partial/cleanup.sh" in command for command in cleanup_commands)
        )
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")

    def test_cleanup_nonzero_exit_is_persisted_as_cleanup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            backend = MODULE.SshBackend(
                transport=CleanupFailureTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.remote_run_dir = "/tmp/task8c-cleanup-failure"
            backend._rings_acquired = ["00d2", "00d4"]
            cleanup = backend.cleanup(MODULE.RunResources(), root)
        self.assertEqual(cleanup["CLEANUP_RESULT"], "FAIL")
        self.assertTrue(
            any("ring 00d4 still exists" in error for error in cleanup["errors"])
        )

    def test_missing_early_failure_diagnostic_does_not_fail_resource_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            backend = MODULE.SshBackend(
                transport=MissingDiagnosticTransport(),
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.remote_run_dir = "/tmp/task8c-missing-diagnostic"
            backend._rings_acquired = ["00d2", "00d4"]
            cleanup = backend.cleanup(MODULE.RunResources(), root)
        self.assertEqual(cleanup["CLEANUP_RESULT"], "PASS")
        self.assertEqual(cleanup["errors"], [])
        self.assertEqual(len(cleanup["diagnostic_errors"]), 1)
        self.assertIn("compute-summary.json", cleanup["diagnostic_errors"][0])

    def test_second_sender_start_failure_terminates_first_sender(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packet_dir = root / "config" / "packet_formats"
            packet_dir.mkdir(parents=True)
            (packet_dir / "frontend.example-v1.json").write_text(
                json.dumps({"schema_version": 1, "format_id": "project-vdif-v1"})
            )
            run_dir = root / "suite" / "measured-01"
            run_dir.mkdir(parents=True)
            transport = SecondSenderStartFailureTransport()
            backend = MODULE.SshBackend(
                transport=transport,
                project_root=root,
                known_hosts=root / "known-hosts",
            )
            backend.local_run_dir = run_dir
            backend.remote_run_dir = "/tmp/task8c-partial-senders"
            backend.preparation = {"start_utc": "2030-01-01-00:06:00"}
            backend._write_bundle(
                make_plan(1.0, 10.0),
                backend.preparation["start_utc"],
            )
            with self.assertRaises(MODULE.StageError):
                backend.start_senders(make_plan(1.0, 10.0), run_dir)
            backend.cleanup(MODULE.RunResources(), run_dir)
        self.assertTrue(transport.started_process.terminated)

    def test_one_sender_early_exit_immediately_aborts_the_other_sender(self):
        backend = MODULE.SshBackend(transport=RecordingTransport())
        running = PolledProcess(None)
        failed = PolledProcess(7, "SEND_ERROR: source stream unavailable")
        with self.assertRaises(MODULE.StageError) as raised:
            backend.wait_senders(
                make_plan(1.0, 10.0), [running, failed]
            )
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")
        self.assertTrue(running.terminated)

    def test_sender_bind_collision_is_environment_blocker_and_aborts_peer(self):
        failed = PolledProcess(
            1, "SEND_ERROR: bind source endpoint: Address already in use"
        )
        running = FakeProcess()
        backend = MODULE.SshBackend(transport=RecordingTransport())
        with self.assertRaises(MODULE.StageError) as raised:
            backend.wait_senders(
                make_plan(0.1, 10.0), [failed, running]
            )
        self.assertEqual(raised.exception.classification, "ENV_BLOCKED")
        self.assertTrue(running.terminated)

    def test_finish_script_fails_when_owned_process_does_not_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ("receiver", "worker", "reader"):
                (root / f"{name}.pid").write_text("12345\n")
            bundle = MODULE.build_qths_bundle(
                make_plan(1.0, 10.0),
                str(root),
            )
            finish = root / "finish.sh"
            finish.write_text(bundle["finish.sh"])
            finish.chmod(0o755)
            capture = root / "capture_nic_counters.py"
            capture.write_text(
                "import pathlib, sys\npathlib.Path(sys.argv[3]).write_text('{}')\n"
            )
            bash_env = root / "bash-env"
            bash_env.write_text("kill() { return 0; }\n")
            environment = os.environ.copy()
            environment.update(
                {
                    "BASH_ENV": str(bash_env),
                    "TASK8C_WAIT_ATTEMPTS": "1",
                    "TASK8C_WAIT_SLEEP": "0",
                }
            )
            completed = subprocess.run(
                ["bash", str(finish)],
                text=True,
                capture_output=True,
                timeout=2,
                env=environment,
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("did not exit", completed.stderr)

    def test_process_supervisor_forwards_term_and_records_child_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            bundle = MODULE.build_qths_bundle(
                make_plan(1.0, 10.0),
                str(root),
            )
            supervisor = root / "supervise.py"
            supervisor.write_text(bundle["supervise.py"])
            child = root / "child.sh"
            child.write_text(
                "#!/usr/bin/env bash\n"
                "trap 'printf terminated >\"$1\"; exit 0' TERM\n"
                "printf ready >\"$2\"\n"
                "while true; do sleep 0.05; done\n"
            )
            child.chmod(0o755)
            marker = root / "child.terminated"
            ready = root / "child.ready"
            exit_path = root / "receiver.exit"
            process = subprocess.Popen(
                [
                    sys.executable,
                    str(supervisor),
                    str(exit_path),
                    str(child),
                    str(marker),
                    str(ready),
                ]
            )
            try:
                for _ in range(500):
                    if ready.exists():
                        break
                    time.sleep(0.01)
                self.assertTrue(ready.exists())
                process.terminate()
                process.wait(timeout=2)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=2)

            self.assertEqual(process.returncode, 0)
            self.assertEqual(marker.read_text(), "terminated")
            self.assertEqual(exit_path.read_text().strip(), "0")

    def test_qths_logs_are_parsed_into_reconciled_statistics(self):
        plan = make_plan(1.0, 10.0)
        records = plan.group_count * 2
        receiver = (
            f"[RDMA] Receive summary: accepted={records}, wrong_length=0, "
            f"published={records}, blocks=149, partial_blocks=1, "
            "cq_tail_records=12, wrong_length_ratio=0.000000000\n"
        )
        worker = (
            "VDIF unpack statistics: "
            f"records={records} accepted={records} bad_header=0 invalid_data=0 "
            "unknown_station=0 duplicate=0 late=0 out_of_range=0 "
            f"complete_groups={plan.group_count} incomplete_groups=0 "
            "fully_missing_groups=0 "
            "missing_station=0/302816 (0.000000%) "
            "large_gap_advances=0/0 max_station_ordinal_skew=3 "
            "raw_blocks_single=1 raw_blocks_mixed=148 "
            "max_station_records_per_raw_block=1024 "
            "max_consecutive_station_records=32\n"
            f"VDIF unpack station statistics: antenna=0 station=101 "
            f"observed={plan.group_count} accepted={plan.group_count} "
            f"late=0 highest_ordinal={plan.group_count - 1}\n"
            f"VDIF unpack station statistics: antenna=1 station=102 "
            f"observed={plan.group_count} accepted={plan.group_count} "
            f"late=0 highest_ordinal={plan.group_count - 1}\n"
            "VDIF unpack transfer completed\n"
        )
        compute = {
            "data_bytes": plan.group_count * 8192,
            "header": {
                "DATA_STAGE": "UNPACKED",
                "ORDER": "ATFP",
                "NANT": "2",
                "NCHAN": "2",
                "NPOL": "2",
                "CONFIG_ID": plan.config_id,
                "GEOMETRY_ID": plan.geometry_id,
            },
            "sample_hex": "65667071",
        }
        statistics = MODULE.parse_qths_statistics(receiver, worker, compute)
        MODULE._validate_statistics(statistics, plan, "65667071")
        self.assertEqual(statistics["receiver"]["accepted"], records)
        self.assertEqual(statistics["unpack"]["complete_groups"], plan.group_count)
        self.assertEqual(statistics["unpack"]["max_station_ordinal_skew"], 3)
        self.assertEqual(statistics["unpack"]["station"][0]["station"], 101)
        self.assertEqual(statistics["compute"]["sample_hex"], "65667071")

    def test_dbdisk_statistics_reject_mixed_configuration_identity(self):
        plan = make_plan(0.1, 10.0)
        backend = FakeBackend()
        statistics = backend.collect(plan, pathlib.Path("unused"))
        statistics["compute"]["CONFIG_ID"] = "c" * 64
        with self.assertRaises(MODULE.StageError) as raised:
            MODULE._validate_statistics(statistics, plan, "65667071")
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")

    def test_preflight_only_runs_configuration_gate_without_starting_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = FakeBackend()
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="preflight"
            )
            result = controller.preflight(make_plan(0.1, 10.0))
            saved = json.loads(
                (pathlib.Path(directory) / "preflight" / "result.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(saved["mode"], "preflight-only")
        self.assertEqual(
            backend.calls,
            ["PREPARE", "CONFIG_READY", "CLEANUP"],
        )

    def test_request_uses_backend_remote_observation_compiler(self):
        class RemoteCompilerBackend(FakeBackend):
            def __init__(self):
                super().__init__()
                self.remote_compiler = mock.Mock()
                self.compiler_requests = []

            def observation_compiler(self, compiler, run_dir):
                self.compiler_requests.append((compiler, run_dir))
                return self.remote_compiler

        with tempfile.TemporaryDirectory() as directory:
            backend = RemoteCompilerBackend()
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="remote-compiler"
            )
            original_compile = MODULE.compile_rate_plan

            def compile_with_remote_executor(request, *_args, **kwargs):
                self.assertIs(
                    kwargs.get("compiler_executor"), backend.remote_compiler
                )
                return make_plan(
                    request.aggregate_gbps,
                    request.duration_seconds,
                    request.batch_packets,
                    request.compute_consumer,
                )

            MODULE.compile_rate_plan = compile_with_remote_executor
            try:
                result = controller.preflight_request(
                    MODULE.RateRequest(0.1, 10.0),
                    pathlib.Path("observation.json"),
                    pathlib.Path("/remote/observation_config_compile"),
                )
            finally:
                MODULE.compile_rate_plan = original_compile

        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(len(backend.compiler_requests), 1)

    def test_execute_cli_routes_through_real_controller_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            original_compile = MODULE.compile_rate_plan
            MODULE.SshBackend = lambda **_kwargs: FakeBackend()
            MODULE.compile_rate_plan = lambda request, *_args: make_plan(
                request.aggregate_gbps, request.duration_seconds,
                request.batch_packets, request.compute_consumer
            )
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "1",
                        "--duration-seconds",
                        "10",
                        "--result-root",
                        directory,
                        "--qths-binary-dir",
                        "/tmp/task8c-release",
                        "--sender-binary-dir",
                        "/tmp/task8c-sender-release",
                        "--observation-config",
                        "/tmp/observation.json",
                        "--config-compiler",
                        "/tmp/observation_config_compile",
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
                MODULE.compile_rate_plan = original_compile
            results = list(pathlib.Path(directory).glob("*/result.json"))
            self.assertEqual(len(results), 1)
            self.assertRegex(
                results[0].parent.name,
                r"^full-1Gbps-10s-\d{8}T\d{6}Z$",
            )
            result = json.loads(results[0].read_text())
        self.assertEqual(return_code, 0)
        self.assertEqual(result["TEST_RESULT"], "PASS")

    def test_preflight_cli_runs_same_gate_without_runtime_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            original_compile = MODULE.compile_rate_plan
            backend = FakeBackend()
            MODULE.SshBackend = lambda **_kwargs: backend
            MODULE.compile_rate_plan = lambda request, *_args: make_plan(
                request.aggregate_gbps,
                request.duration_seconds,
                request.batch_packets,
                request.compute_consumer,
            )
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "0.1",
                        "--result-root",
                        directory,
                        "--qths-binary-dir",
                        "/tmp/task8c-release",
                        "--sender-binary-dir",
                        "/tmp/task8c-sender-release",
                        "--observation-config",
                        "/tmp/observation.json",
                        "--config-compiler",
                        "/tmp/observation_config_compile",
                        "--preflight-only",
                    ]
                )
            finally:
                MODULE.SshBackend = original
                MODULE.compile_rate_plan = original_compile
            results = list(pathlib.Path(directory).glob("*/result.json"))
            result = json.loads(results[0].read_text())
        self.assertEqual(return_code, 0)
        self.assertEqual(result["mode"], "preflight-only")
        self.assertEqual(backend.calls, ["PREPARE", "CONFIG_READY", "CLEANUP"])

    def test_execute_cli_returns_nonzero_when_cleanup_fails(self):
        class CleanupFailedBackend(FakeBackend):
            def cleanup(self, resources, run_dir):
                return {
                    "CLEANUP_RESULT": "FAIL",
                    "rings_destroyed": [],
                    "capability_removed": False,
                    "errors": ["ring remains"],
                }

        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            original_compile = MODULE.compile_rate_plan
            MODULE.SshBackend = lambda **_kwargs: CleanupFailedBackend()
            MODULE.compile_rate_plan = lambda request, *_args: make_plan(
                request.aggregate_gbps, request.duration_seconds,
                request.batch_packets, request.compute_consumer
            )
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "1",
                        "--result-root",
                        directory,
                        "--qths-binary-dir",
                        "/tmp/task8c-release",
                        "--sender-binary-dir",
                        "/tmp/task8c-sender-release",
                        "--observation-config",
                        "/tmp/observation.json",
                        "--config-compiler",
                        "/tmp/observation_config_compile",
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
                MODULE.compile_rate_plan = original_compile
        self.assertEqual(return_code, 1)

    def test_sequence_cli_returns_nonzero_on_harness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            original_compile = MODULE.compile_rate_plan
            MODULE.SshBackend = lambda **_kwargs: FakeBackend(
                fail_stage="PREPARE"
            )
            MODULE.compile_rate_plan = lambda request, *_args: make_plan(
                request.aggregate_gbps, request.duration_seconds,
                request.batch_packets, request.compute_consumer
            )
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "1",
                        "--warmup-runs",
                        "1",
                        "--measured-runs",
                        "1",
                        "--result-root",
                        directory,
                        "--qths-binary-dir",
                        "/tmp/task8c-release",
                        "--sender-binary-dir",
                        "/tmp/task8c-sender-release",
                        "--observation-config",
                        "/tmp/observation.json",
                        "--config-compiler",
                        "/tmp/observation_config_compile",
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
                MODULE.compile_rate_plan = original_compile
        self.assertEqual(return_code, 1)

    def test_request_compile_exception_persists_harness_failure_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = FakeBackend()
            controller = MODULE.RatePointController(
                backend, pathlib.Path(directory), run_id="bad-artifacts"
            )
            original_compile = MODULE.compile_rate_plan
            MODULE.compile_rate_plan = lambda *_args: (_ for _ in ()).throw(
                ValueError("compiler manifest mismatch")
            )
            try:
                result = controller.run_request(
                    MODULE.RateRequest(0.1, 10.0),
                    pathlib.Path("observation.json"),
                    pathlib.Path("observation_config_compile"),
                )
            finally:
                MODULE.compile_rate_plan = original_compile
            saved = json.loads(
                (pathlib.Path(directory) / "bad-artifacts" / "result.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(saved["failure"]["stage"], "CONFIG_READY")
        self.assertIn("compiler manifest mismatch", saved["failure"]["stderr"])
        self.assertEqual(saved["cleanup"]["CLEANUP_RESULT"], "PASS")

    def test_execute_requires_explicit_qths_binary_directory(self):
        class MustNotConstructBackend:
            def __init__(self, **_kwargs):
                raise AssertionError("backend must not use an implicit stale build")

        original = MODULE.SshBackend
        MODULE.SshBackend = MustNotConstructBackend
        try:
            return_code = MODULE.main(
                ["--aggregate-gbps", "0.1", "--execute"]
            )
        finally:
            MODULE.SshBackend = original
        self.assertEqual(return_code, 2)

    def test_acceptance_sequence_runs_one_warmup_and_three_fresh_measurements(self):
        with tempfile.TemporaryDirectory() as directory:
            created = []

            def backend_factory():
                backend = FakeBackend()
                created.append(backend)
                return backend

            summary = MODULE.run_rate_sequence(
                make_plan(1.0, 10.0),
                backend_factory,
                pathlib.Path(directory),
                warmup_runs=1,
                measured_runs=3,
                suite_id="acceptance",
            )
            result_files = sorted(
                (pathlib.Path(directory) / "acceptance").glob("*/result.json")
            )
        self.assertEqual(len(created), 4)
        self.assertEqual(len(result_files), 4)
        self.assertEqual(summary["TEST_RESULT"], "PASS")
        self.assertEqual(summary["warmup_count"], 1)
        self.assertEqual(summary["measured_count"], 3)
        self.assertEqual(summary["actual_aggregate_gbps"]["median"], 1.0)
        self.assertEqual(summary["actual_aggregate_gbps"]["minimum"], 1.0)
        self.assertEqual(summary["actual_aggregate_gbps"]["maximum"], 1.0)
        self.assertEqual(summary["actual_aggregate_gbps"]["spread"], 0.0)

    def test_acceptance_sequence_does_not_start_measurements_after_failed_warmup(self):
        class ProductFailedBackend(FakeBackend):
            def collect(self, plan, run_dir):
                raise MODULE.StageError(
                    "COLLECTING",
                    ["validate", "pipeline-statistics"],
                    1,
                    stderr="receiver accepted count mismatch",
                    classification="PRODUCT_FAIL",
                )

        with tempfile.TemporaryDirectory() as directory:
            created = []

            def backend_factory():
                backend = ProductFailedBackend()
                created.append(backend)
                return backend

            summary = MODULE.run_rate_sequence(
                make_plan(5.0, 10.0),
                backend_factory,
                pathlib.Path(directory),
                warmup_runs=1,
                measured_runs=3,
                suite_id="failed-warmup",
            )
            result_files = sorted(
                (pathlib.Path(directory) / "failed-warmup").glob(
                    "*/result.json"
                )
            )

        self.assertEqual(len(created), 1)
        self.assertEqual(len(result_files), 1)
        self.assertEqual(summary["TEST_RESULT"], "PRODUCT_FAIL")
        self.assertEqual(len(summary["runs"]), 1)
        self.assertEqual(summary["runs"][0]["role"], "warmup")
        self.assertEqual(summary["runs"][0]["CLEANUP_RESULT"], "PASS")

    def test_acceptance_sequence_fails_when_any_run_cleanup_fails(self):
        class CleanupFailedBackend(FakeBackend):
            def cleanup(self, resources, run_dir):
                result = super().cleanup(resources, run_dir)
                result.update(
                    {
                        "CLEANUP_RESULT": "FAIL",
                        "errors": ["compute ring still exists"],
                    }
                )
                return result

        with tempfile.TemporaryDirectory() as directory:
            summary = MODULE.run_rate_sequence(
                make_plan(1.0, 10.0),
                CleanupFailedBackend,
                pathlib.Path(directory),
                warmup_runs=0,
                measured_runs=1,
                suite_id="cleanup-failed",
            )
        self.assertEqual(summary["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(summary["runs"][0]["TEST_RESULT"], "PASS")
        self.assertEqual(summary["runs"][0]["CLEANUP_RESULT"], "FAIL")

    def test_request_sequence_classifies_cleanup_failure_as_harness_failure(self):
        class CleanupFailedBackend(FakeBackend):
            def cleanup(self, resources, run_dir):
                return {
                    "CLEANUP_RESULT": "FAIL",
                    "rings_destroyed": [],
                    "capability_removed": False,
                    "errors": ["ring remains"],
                }

        with tempfile.TemporaryDirectory() as directory:
            original_compile = MODULE.compile_rate_plan
            MODULE.compile_rate_plan = lambda request, *_args: make_plan(
                request.aggregate_gbps,
                request.duration_seconds,
                request.batch_packets,
                request.compute_consumer,
            )
            try:
                summary = MODULE.run_rate_request_sequence(
                    MODULE.RateRequest(0.1, 10.0),
                    pathlib.Path("observation.json"),
                    pathlib.Path("observation_config_compile"),
                    CleanupFailedBackend,
                    pathlib.Path(directory),
                    warmup_runs=0,
                    measured_runs=3,
                    suite_id="cleanup-failure",
                )
            finally:
                MODULE.compile_rate_plan = original_compile
        self.assertEqual(summary["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(len(summary["runs"]), 1)
        self.assertEqual(summary["runs"][0]["TEST_RESULT"], "PASS")
        self.assertEqual(summary["runs"][0]["CLEANUP_RESULT"], "FAIL")

    def test_every_run_persists_a_manifest_before_remote_work(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = MODULE.RatePointController(
                FakeBackend(), pathlib.Path(directory), run_id="manifest"
            )
            controller.run(make_plan(1.0, 10.0))
            manifest = json.loads(
                (pathlib.Path(directory) / "manifest" / "manifest.json").read_text()
            )
        self.assertEqual(manifest["run_id"], "manifest")
        self.assertEqual(manifest["hosts"], ["HF", "qths1", "qtpulsar1", "qtpulsar2"])
        self.assertEqual(manifest["plan"]["aggregate_gbps"], 1.0)
        self.assertIn("controller_sha256", manifest)
        self.assertEqual(manifest["config"]["config_sha_match"], True)

    def test_test_host_manifest_does_not_execute_or_require_git(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = MODULE.RatePointController(
                FakeBackend(), pathlib.Path(directory), run_id="without-git"
            )
            with mock.patch.object(
                MODULE.subprocess,
                "run",
                side_effect=FileNotFoundError("git executable is unavailable"),
            ) as git_probe:
                result = controller.run(make_plan(1.0, 10.0))
            manifest = json.loads(
                (pathlib.Path(directory) / "without-git" / "manifest.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "PASS")
        git_probe.assert_not_called()
        self.assertIsNone(manifest["git_commit"])
        self.assertFalse(manifest["git_required_on_test_host"])

    def test_cleanup_exception_still_persists_original_result(self):
        class CleanupRaisesBackend(FakeBackend):
            def cleanup(self, resources, run_dir):
                raise RuntimeError("cleanup transport disconnected")

        with tempfile.TemporaryDirectory() as directory:
            controller = MODULE.RatePointController(
                CleanupRaisesBackend(), pathlib.Path(directory), run_id="cleanup-raises"
            )
            result = controller.run(make_plan(1.0, 10.0))
            saved = json.loads(
                (pathlib.Path(directory) / "cleanup-raises" / "result.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(saved["TEST_RESULT"], "PASS")
        self.assertEqual(saved["cleanup"]["CLEANUP_RESULT"], "FAIL")
        self.assertIn("cleanup transport disconnected", saved["cleanup"]["errors"][0])


if __name__ == "__main__":
    unittest.main()
