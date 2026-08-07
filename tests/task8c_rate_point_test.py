#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "task8c_rate_point.py"
SPEC = importlib.util.spec_from_file_location("task8c_rate_point", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


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
            FakeProcess(output='{"sent_packets":10,"scheduled_packets":10,"failed_packets":0,"backend":"SENDMMSG","actual_payload_gbps":0.5}'),
            FakeProcess(output='{"sent_packets":10,"scheduled_packets":10,"failed_packets":0,"backend":"SENDMMSG","actual_payload_gbps":0.5}'),
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
                    "complete_groups": plan.group_count,
                    "incomplete_groups": 0,
                    "missing_station": 0,
                },
                "compute": {
                    "data_bytes": plan.group_count * 8192,
                    "DATA_STAGE": "UNPACKED",
                    "ORDER": "TFPA",
                    "NANT": 2,
                    "NCHAN": 2,
                    "NPOL": 2,
                    "sample_prefix_hex": "65666667",
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
    def test_hf_controller_does_not_ssh_back_through_hf(self):
        argv = MODULE.build_ssh_argv(
            "qths1", ["date", "-u", "+%s"], "/tmp/task8c-known-hosts"
        )
        self.assertEqual(argv[0], "ssh")
        self.assertIn("qths1", argv)
        self.assertNotIn("HF", argv)
        self.assertEqual(argv[-3:], ["date", "-u", "+%s"])

    def test_rate_plan_rounds_groups_to_complete_sender_batches(self):
        plan = MODULE.RatePlan.create(1.0, 10.0, batch_packets=16)
        self.assertEqual(plan.per_station_gbps, 0.5)
        self.assertEqual(plan.record_bytes, 4128)
        self.assertEqual(plan.group_count % 16, 0)
        self.assertEqual(plan.group_count % (plan.records_per_block // 2), 0)
        self.assertEqual((plan.group_count * 2) % plan.records_per_block, 0)
        self.assertGreaterEqual(plan.group_count * plan.record_bytes * 8, 5_000_000_000)

    def test_sender_configs_share_geometry_and_start_time(self):
        plan = MODULE.RatePlan.create(1.0, 10.0, batch_packets=16)
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

    def test_qths_bundle_uses_exact_pid_files_and_no_wildcard_kill(self):
        plan = MODULE.RatePlan.create(1.0, 10.0)
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-run-abc", "2030-01-01-00:03:00"
        )
        combined = "\n".join(bundle.values())
        self.assertIn("receiver.pid", combined)
        self.assertIn("worker.pid", combined)
        self.assertIn("reader.pid", combined)
        self.assertIn("raw-ring.pid", combined)
        self.assertIn("compute-ring.pid", combined)
        self.assertNotIn("pkill", combined)
        self.assertNotIn("killall", combined)
        self.assertIn(str(plan.raw_block_bytes), bundle["prepare.sh"])
        self.assertIn(str(plan.compute_block_bytes), bundle["prepare.sh"])

    def test_qths_bundle_can_select_an_isolated_release_binary_directory(self):
        plan = MODULE.RatePlan.create(0.1, 10.0)
        bundle = MODULE.build_qths_bundle(
            plan,
            "/tmp/task8c-release",
            "2030-01-01-00:03:00",
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

    def test_dbnull_consumer_uses_single_transfer_zero_copy_without_disk_output(self):
        plan = MODULE.RatePlan.create(
            5.0, 10.0, compute_consumer="dbnull"
        )
        bundle = MODULE.build_qths_bundle(
            plan, "/tmp/task8c-dbnull", "2030-01-01-00:03:00"
        )
        self.assertIn("dada_dbnull -k 00d4 -s -z -q", bundle["start.sh"])
        self.assertIn(
            ') >"$run_dir/reader.log" 2>&1 &', bundle["start.sh"]
        )
        self.assertNotIn("dada_dbdisk", bundle["start.sh"])
        self.assertIn('"consumer": "dada_dbnull"', bundle["summarize.py"])

    def test_dbnull_statistics_require_consumer_eod_and_exact_unpack_counts(self):
        plan = MODULE.RatePlan.create(
            5.0, 10.0, compute_consumer="dbnull"
        )
        records = plan.group_count * 2
        receiver = (
            f"[RDMA] Receive summary: accepted={records}, wrong_length=0 "
            "(0.000000%)\n"
        )
        worker = (
            "VDIF unpack statistics: "
            f"records={records} accepted={records} bad_header=0 invalid_data=0 "
            "unknown_station=0 duplicate=0 late=0 "
            f"complete_groups={plan.group_count} incomplete_groups=0 "
            f"missing_station=0/{records} (0.000000%)\n"
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
        )
        MODULE._validate_statistics(statistics, plan)
        statistics["compute"]["exit_code"] = 1
        with self.assertRaises(MODULE.StageError) as raised:
            MODULE._validate_statistics(statistics, plan)
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")

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
            plan = MODULE.RatePlan.create(1.0, 10.0)
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
                "qths1:cleanup.sh",
                "qths1:finish.sh",
                "qths1:packet.json",
                "qths1:pipeline.json",
                "qths1:prepare.sh",
                "qths1:start.sh",
                "qths1:summarize.py",
                "qths1:worker.json",
                "qtpulsar1:101.json",
                "qtpulsar2:102.json",
            ],
        )
        self.assertEqual(
            sorted(config_evidence["binary_sha"]),
            [
                "qths1:dada_db",
                "qths1:dada_dbdisk",
                "qths1:rdma2dada",
                "qths1:vdif_unpack_worker",
                "qtpulsar1:fpga_sender_sim",
                "qtpulsar2:fpga_sender_sim",
            ],
        )

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
                MODULE.RatePlan.create(1.0, 10.0), run_dir
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
                MODULE.RatePlan.create(1.0, 10.0), run_dir
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
            plan = MODULE.RatePlan.create(
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
            plan = MODULE.RatePlan.create(
                5.0, 10.0, compute_consumer="dbnull"
            )
            preparation = backend.prepare(plan, run_dir)
            evidence = backend.prepare_configs(plan, run_dir, preparation)
            start_script = (run_dir / "bundle" / "qths" / "start.sh").read_text()
        self.assertIn(
            "/home/user/psrdada/bin/dada_dbnull -k 00d4 -s -z -q",
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
            plan = MODULE.RatePlan.create(
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
            result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
            result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
            result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
            result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
                MODULE.RatePlan.create(1.0, 10.0),
                backend.preparation["start_utc"],
            )
            backend.start_senders(MODULE.RatePlan.create(1.0, 10.0), run_dir)
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
                backend.start_pipeline(MODULE.RatePlan.create(1.0, 10.0), run_dir)
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
                MODULE.RatePlan.create(1.0, 10.0),
                backend.preparation["start_utc"],
            )
            with self.assertRaises(MODULE.StageError):
                backend.start_senders(MODULE.RatePlan.create(1.0, 10.0), run_dir)
            backend.cleanup(MODULE.RunResources(), run_dir)
        self.assertTrue(transport.started_process.terminated)

    def test_one_sender_early_exit_immediately_aborts_the_other_sender(self):
        backend = MODULE.SshBackend(transport=RecordingTransport())
        running = PolledProcess(None)
        failed = PolledProcess(7, "SEND_ERROR: source stream unavailable")
        with self.assertRaises(MODULE.StageError) as raised:
            backend.wait_senders(
                MODULE.RatePlan.create(1.0, 10.0), [running, failed]
            )
        self.assertEqual(raised.exception.classification, "PRODUCT_FAIL")
        self.assertTrue(running.terminated)

    def test_finish_script_fails_when_owned_process_does_not_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ("receiver", "worker", "reader"):
                (root / f"{name}.pid").write_text("12345\n")
            bundle = MODULE.build_qths_bundle(
                MODULE.RatePlan.create(1.0, 10.0),
                str(root),
                "2030-01-01-00:03:00",
            )
            finish = root / "finish.sh"
            finish.write_text(bundle["finish.sh"])
            finish.chmod(0o755)
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

    def test_qths_logs_are_parsed_into_reconciled_statistics(self):
        plan = MODULE.RatePlan.create(1.0, 10.0)
        records = plan.group_count * 2
        receiver = (
            f"[RDMA] Receive summary: accepted={records}, wrong_length=0 "
            "(0.000000%)\n"
        )
        worker = (
            "VDIF unpack statistics: "
            f"records={records} accepted={records} bad_header=0 invalid_data=0 "
            "unknown_station=0 duplicate=0 late=0 "
            f"complete_groups={plan.group_count} incomplete_groups=0 "
            "missing_station=0/302816 (0.000000%)\n"
            "VDIF unpack transfer completed\n"
        )
        compute = {
            "data_bytes": plan.group_count * 8192,
            "header": {
                "DATA_STAGE": "UNPACKED",
                "ORDER": "TFPA",
                "NANT": "2",
                "NCHAN": "2",
                "NPOL": "2",
            },
            "sample_hex": "65666667",
        }
        statistics = MODULE.parse_qths_statistics(receiver, worker, compute)
        MODULE._validate_statistics(statistics, plan)
        self.assertEqual(statistics["receiver"]["accepted"], records)
        self.assertEqual(statistics["unpack"]["complete_groups"], plan.group_count)
        self.assertEqual(statistics["compute"]["sample_hex"], "65666667")

    def test_execute_cli_routes_through_real_controller_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            MODULE.SshBackend = lambda **_kwargs: FakeBackend()
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "1",
                        "--duration-seconds",
                        "10",
                        "--result-root",
                        directory,
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
            results = list(pathlib.Path(directory).glob("*/result.json"))
            self.assertEqual(len(results), 1)
            result = json.loads(results[0].read_text())
        self.assertEqual(return_code, 0)
        self.assertEqual(result["TEST_RESULT"], "PASS")

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
            MODULE.SshBackend = lambda **_kwargs: CleanupFailedBackend()
            try:
                return_code = MODULE.main(
                    [
                        "--aggregate-gbps",
                        "1",
                        "--result-root",
                        directory,
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
        self.assertEqual(return_code, 1)

    def test_sequence_cli_returns_nonzero_on_harness_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            original = MODULE.SshBackend
            MODULE.SshBackend = lambda **_kwargs: FakeBackend(
                fail_stage="PREPARE"
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
                        "--execute",
                    ]
                )
            finally:
                MODULE.SshBackend = original
        self.assertEqual(return_code, 1)

    def test_acceptance_sequence_runs_one_warmup_and_three_fresh_measurements(self):
        with tempfile.TemporaryDirectory() as directory:
            created = []

            def backend_factory():
                backend = FakeBackend()
                created.append(backend)
                return backend

            summary = MODULE.run_rate_sequence(
                MODULE.RatePlan.create(1.0, 10.0),
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
                MODULE.RatePlan.create(5.0, 10.0),
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
                MODULE.RatePlan.create(1.0, 10.0),
                CleanupFailedBackend,
                pathlib.Path(directory),
                warmup_runs=0,
                measured_runs=1,
                suite_id="cleanup-failed",
            )
        self.assertEqual(summary["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(summary["runs"][0]["TEST_RESULT"], "PASS")
        self.assertEqual(summary["runs"][0]["CLEANUP_RESULT"], "FAIL")

    def test_every_run_persists_a_manifest_before_remote_work(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = MODULE.RatePointController(
                FakeBackend(), pathlib.Path(directory), run_id="manifest"
            )
            controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
                result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
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
            result = controller.run(MODULE.RatePlan.create(1.0, 10.0))
            saved = json.loads(
                (pathlib.Path(directory) / "cleanup-raises" / "result.json").read_text()
            )
        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(saved["TEST_RESULT"], "PASS")
        self.assertEqual(saved["cleanup"]["CLEANUP_RESULT"], "FAIL")
        self.assertIn("cleanup transport disconnected", saved["cleanup"]["errors"][0])


if __name__ == "__main__":
    unittest.main()
