#!/usr/bin/env python3

import importlib.util
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
from decimal import Decimal
from unittest import mock


PROJECT_ROOT = pathlib.Path(__file__).parents[1]
SCRIPT = PROJECT_ROOT / "scripts" / "atfp_throughput_campaign.py"
CAMPAIGN_CONFIG = PROJECT_ROOT / "config/testing/atfp-throughput-campaign.json"
OBSERVATION_CONFIG = PROJECT_ROOT / "config/testing/atfp-throughput-observation.json"
SPEC = importlib.util.spec_from_file_location("atfp_throughput_campaign", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class WireModelTest(unittest.TestCase):
    def test_converts_4128_byte_record_rate_to_untagged_ipv4_wire_rate(self):
        model = MODULE.WireModel.untagged_ipv4()
        self.assertEqual(model.overhead_bytes, 66)
        self.assertEqual(model.physical_bytes(4128), 4194)
        aggregate, per_station = MODULE.wire_gbps_to_record_gbps(
            40.0, 4128, 2, model
        )
        self.assertEqual(per_station, 19.685264664)
        self.assertEqual(aggregate, 39.370529328)
        self.assertEqual(len(str(per_station).split(".")[1]), 9)

    def test_rejects_invalid_wire_conversion_inputs(self):
        model = MODULE.WireModel.untagged_ipv4()
        for wire_rate, record_bytes, stations in (
            (0.0, 4128, 2), (1.0, 0, 2), (1.0, 4128, 0)
        ):
            with self.subTest(
                wire_rate=wire_rate, record_bytes=record_bytes, stations=stations
            ):
                with self.assertRaises(ValueError):
                    MODULE.wire_gbps_to_record_gbps(
                        wire_rate, record_bytes, stations, model
                    )


class CampaignConfigTest(unittest.TestCase):
    def setUp(self):
        self.valid = {
            "schema_version": 1,
            "target_wire_gbps": [1, 5, 10, 20, 30, 35, 40],
            "duration_seconds": 30,
            "warmup_runs": 1,
            "measured_runs": 3,
            "bisection_tolerance_gbps": 0.5,
            "compute_consumer": "dbnull",
            "wire_model": "UNTAGGED_IPV4_ETHERNET",
        }

    def load(self, value):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "campaign.json"
            path.write_text(json.dumps(value))
            return MODULE.load_campaign_config(path)

    def test_loads_the_formal_campaign_contract(self):
        config = self.load(self.valid)
        self.assertEqual(config.target_wire_gbps, (1, 5, 10, 20, 30, 35, 40))
        self.assertEqual(config.duration_seconds, 30)
        self.assertEqual(config.warmup_runs, 1)
        self.assertEqual(config.measured_runs, 3)
        self.assertEqual(config.bisection_tolerance_gbps, 0.5)
        self.assertEqual(config.compute_consumer, "dbnull")

    def test_rejects_unknown_or_non_formal_values(self):
        mutations = {
            "unknown": lambda value: value.update({"extra": 1}),
            "duplicate_rate": lambda value: value.update(
                {"target_wire_gbps": [1, 5, 5, 40]}
            ),
            "descending_rate": lambda value: value.update(
                {"target_wire_gbps": [1, 10, 5, 40]}
            ),
            "duration": lambda value: value.update({"duration_seconds": 10}),
            "warmup": lambda value: value.update({"warmup_runs": 0}),
            "measured": lambda value: value.update({"measured_runs": 1}),
            "tolerance": lambda value: value.update(
                {"bisection_tolerance_gbps": 1.0}
            ),
            "consumer": lambda value: value.update({"compute_consumer": "dbdisk"}),
            "wire_model": lambda value: value.update({"wire_model": "VLAN_IPV4"}),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                value = dict(self.valid)
                mutate(value)
                with self.assertRaises(ValueError):
                    self.load(value)


class CampaignScheduleTest(unittest.TestCase):
    def config(self):
        return MODULE.CampaignConfig(
            target_wire_gbps=(1, 5, 10, 20, 30, 35, 40),
            duration_seconds=30,
            warmup_runs=1,
            measured_runs=3,
            bisection_tolerance_gbps=0.5,
            compute_consumer="dbnull",
            wire_model="UNTAGGED_IPV4_ETHERNET",
        )

    def test_all_fixed_points_pass_without_claiming_a_measured_ceiling(self):
        calls = []

        def run_point(rate, warmups, measured):
            calls.append((rate, warmups, measured))
            return {"TEST_RESULT": "PASS", "CLEANUP_RESULT": "PASS"}

        result = MODULE.run_campaign_schedule(self.config(), run_point)
        self.assertEqual(
            [str(rate) for rate, _, _ in calls],
            ["1", "5", "10", "20", "30", "35", "40"],
        )
        self.assertTrue(all(item[1:] == (1, 3) for item in calls))
        self.assertEqual(result["TEST_RESULT"], "PASS")
        self.assertEqual(result["stable_lower_gbps"], "40")
        self.assertIsNone(result["failing_upper_gbps"])
        self.assertFalse(result["bottleneck_reached"])

    def test_first_failure_stops_higher_fixed_points_and_bisects_to_half_gbps(self):
        calls = []

        def run_point(rate, warmups, measured):
            calls.append(rate)
            passed = rate <= Decimal("30.75")
            return {
                "TEST_RESULT": "PASS" if passed else "PERFORMANCE_FAIL",
                "CLEANUP_RESULT": "PASS",
            }

        result = MODULE.run_campaign_schedule(self.config(), run_point)
        self.assertEqual(
            [str(rate) for rate in calls],
            [
                "1", "5", "10", "20", "30", "35", "32.5", "31.25",
                "30.625", "30.9375",
            ],
        )
        self.assertNotIn(Decimal("40"), calls)
        self.assertEqual(result["stable_lower_gbps"], "30.625")
        self.assertEqual(result["failing_upper_gbps"], "30.9375")
        self.assertEqual(result["boundary_width_gbps"], "0.3125")
        self.assertTrue(result["bottleneck_reached"])

    def test_failure_at_minimum_has_no_stable_lower_bound(self):
        result = MODULE.run_campaign_schedule(
            self.config(),
            lambda rate, warmups, measured: {
                "TEST_RESULT": "PERFORMANCE_FAIL",
                "CLEANUP_RESULT": "PASS",
            },
        )
        self.assertIsNone(result["stable_lower_gbps"])
        self.assertEqual(result["failing_upper_gbps"], "1")

    def test_cleanup_failure_stops_as_harness_failure(self):
        result = MODULE.run_campaign_schedule(
            self.config(),
            lambda rate, warmups, measured: {
                "TEST_RESULT": "PASS",
                "CLEANUP_RESULT": "FAIL",
            },
        )
        self.assertEqual(result["TEST_RESULT"], "HARNESS_FAIL")
        self.assertEqual(len(result["points"]), 1)

    def test_product_failure_stops_without_treating_it_as_a_rate_boundary(self):
        calls = []

        def run_point(rate, warmups, measured):
            calls.append(rate)
            return {
                "TEST_RESULT": "PASS" if rate < 10 else "PRODUCT_FAIL",
                "CLEANUP_RESULT": "PASS",
            }

        result = MODULE.run_campaign_schedule(self.config(), run_point)
        self.assertEqual(calls, [Decimal("1"), Decimal("5"), Decimal("10")])
        self.assertEqual(result["TEST_RESULT"], "PRODUCT_FAIL")
        self.assertFalse(result["bottleneck_reached"])


class CampaignCliTest(unittest.TestCase):
    def test_bootstrap_plan_uses_qths_remote_compiler(self):
        executor = mock.Mock()
        backend = mock.Mock()
        backend.observation_compiler.return_value = executor
        rate_point = mock.Mock()
        expected = object()
        rate_point.compile_rate_plan.return_value = expected
        request = object()
        run_directory = pathlib.Path("/tmp/campaign/bootstrap")

        actual = MODULE.compile_plan_on_backend(
            rate_point,
            backend,
            request,
            pathlib.Path("/home/user/wy/rdma_dada/config/observation.json"),
            pathlib.Path("/home/user/wy/rdma_dada/build/observation_config_compile"),
            run_directory,
        )

        self.assertIs(actual, expected)
        backend.observation_compiler.assert_called_once()
        self.assertIs(
            rate_point.compile_rate_plan.call_args.kwargs["compiler_executor"],
            executor,
        )
        executor.close.assert_called_once_with()

    def test_campaign_lock_rejects_a_second_formal_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "campaign.lock"
            with MODULE.CampaignLock(path, "first"):
                with self.assertRaises(RuntimeError):
                    with MODULE.CampaignLock(path, "second"):
                        pass
            with MODULE.CampaignLock(path, "third"):
                self.assertTrue(path.exists())
            self.assertFalse(path.exists())

    def test_execute_requires_explicit_release_paths_and_source_manifest(self):
        with mock.patch("sys.stderr"):
            code = MODULE.main(
                [
                    "--campaign-config", str(CAMPAIGN_CONFIG),
                    "--observation-config", str(OBSERVATION_CONFIG),
                    "--config-compiler", "/tmp/compiler",
                    "--execute",
                ]
            )
        self.assertEqual(code, 2)

    def test_dry_run_returns_only_the_fixed_formal_contract(self):
        with mock.patch("builtins.print") as output:
            code = MODULE.main(
                [
                    "--campaign-config", str(CAMPAIGN_CONFIG),
                    "--dry-run",
                ]
            )
        self.assertEqual(code, 0)
        rendered = json.loads(output.call_args.args[0])
        self.assertEqual(rendered["target_wire_gbps"], [1, 5, 10, 20, 30, 35, 40])
        self.assertEqual(rendered["duration_seconds"], 30)
        self.assertEqual(rendered["compute_consumer"], "dbnull")

    def test_source_manifest_rejects_changed_or_missing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "one.txt").write_text("one\n")
            digest = hashlib.sha256(b"one\n").hexdigest()
            manifest = root / "SOURCE_MANIFEST.sha256"
            manifest.write_text(f"{digest}  one.txt\n")
            verified = MODULE.verify_source_manifest(root, manifest)
            self.assertEqual(verified["one.txt"], digest)
            (root / "one.txt").write_text("changed\n")
            with self.assertRaises(ValueError):
                MODULE.verify_source_manifest(root, manifest)


if __name__ == "__main__":
    unittest.main()
