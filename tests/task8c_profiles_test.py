#!/usr/bin/env python3

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import task8c_profiles as profiles  # noqa: E402


def valid_profile():
    return {
        "schema_version": 1,
        "profile_id": "qths1-unpack-30gbps-v1",
        "target_host": "qths1",
        "pipeline_stage": "unpack",
        "source_result": "/results/unpack-pass",
        "runtime": {
            "receiver_poll_cpu": 13,
            "worker_cpu_list": "14,15,16,17,18,19",
            "sink_cpu_list": "20",
            "sender_source_port_101": 45871,
            "sender_source_port_102": 55871,
            "numa_node": 1,
            "receiver_poll_batch": 32,
            "receiver_wr_num": 1024,
            "unpack_start_delay_seconds": 1,
            "missing_wait_ms": 200.0,
            "station_skew_reserve_ms": 200.0,
        },
        "geometry": {
            "target_payload_gbps": 30.0,
            "duration_seconds": 60.0,
            "raw_block_bytes": 52838400,
            "raw_ring_blocks": 16,
            "compute_block_bytes": 52428800,
            "compute_ring_blocks": 8,
            "window_blocks": 31,
            "reorder_horizon_groups": 96000,
        },
    }


def valid_gpu_profile():
    return {
        "schema_version": 1,
        "profile_id": "qths1-gpu-bootstrap-v1",
        "target_host": "qths1",
        "pipeline_stage": "gpu",
        "source_result": "/results/gpu-pass",
        "runtime": {
            "gpu_worker_cpu": 21,
            "sink_cpu_list": "20",
            "numa_node": 1,
        },
        "geometry": {
            "target_payload_gbps": 12.5,
            "duration_seconds": 10.0,
            "compute_block_bytes": 52428800,
            "compute_ring_blocks": 8,
            "output_block_bytes": 209715200,
            "output_ring_blocks": 8,
        },
    }


def valid_split_full_profile():
    value = valid_profile()
    value["profile_id"] = "qths1-full-30gbps-split-numa-v1"
    value["pipeline_stage"] = "full"
    value["runtime"].pop("numa_node")
    value["runtime"].update({
        "worker_cpu_list": "24,25,26,27,28,29",
        "gpu_worker_cpu": 30,
        "sink_cpu_list": "31",
        "ingress_numa_node": 1,
        "processing_numa_node": 0,
    })
    value["geometry"].update({
        "output_block_bytes": 819200,
        "output_ring_blocks": 8,
    })
    return value


@dataclasses.dataclass(frozen=True)
class FakeRequest:
    receiver_poll_cpu: int | None = None
    worker_cpu_list: str | None = None
    gpu_worker_cpu: int | None = None
    sink_cpu_list: str | None = None
    sender_source_port_101: int | None = None
    sender_source_port_102: int | None = None
    numa_node: int | None = None
    ingress_numa_node: int | None = None
    processing_numa_node: int | None = None
    receiver_poll_batch: int = 32
    receiver_wr_num: int = 1024
    unpack_start_delay_seconds: int = 0
    missing_wait_ms: float = 200.0
    station_skew_reserve_ms: float = 200.0


@dataclasses.dataclass(frozen=True)
class FakePlan:
    aggregate_gbps: float = 30.0
    duration_seconds: float = 60.0
    raw_block_bytes: int = 52838400
    raw_ring_blocks: int = 16
    compute_block_bytes: int = 52428800
    compute_ring_blocks: int = 8
    window_bytes: int = 31 * 52428800
    reorder_horizon_groups: int = 96000
    receiver_poll_cpu: int = 13
    worker_cpu_list: str = "14,15,16,17,18,19"
    gpu_worker_cpu: int | None = None
    sink_cpu_list: str = "20"
    sender_source_port_101: int = 45871
    sender_source_port_102: int = 55871
    numa_node: int = 1
    ingress_numa_node: int | None = None
    processing_numa_node: int | None = None
    receiver_poll_batch: int = 32
    receiver_wr_num: int = 1024
    unpack_start_delay_seconds: int = 1
    missing_wait_ms: float = 200.0
    station_skew_reserve_ms: float = 200.0


class Task8cProfilesTest(unittest.TestCase):
    def write_profile(self, root, value=None):
        path = pathlib.Path(root) / "profile.json"
        path.write_text(json.dumps(value or valid_profile(), indent=2) + "\n")
        return path

    def test_load_profile_validates_and_records_source_bytes_sha(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_profile(directory)
            expected_sha = hashlib.sha256(path.read_bytes()).hexdigest()
            profile = profiles.load_profile(path)

        self.assertEqual(profile.profile_id, "qths1-unpack-30gbps-v1")
        self.assertEqual(profile.pipeline_stage, "unpack")
        self.assertEqual(profile.sha256, expected_sha)
        self.assertEqual(profile.runtime["receiver_poll_cpu"], 13)

    def test_checked_in_unpack_profile_locks_authoritative_pass(self):
        profile = profiles.load_profile(
            ROOT / "config" / "testing" / "profiles"
            / "qths1-unpack-30gbps-60s-v1.json"
        )

        self.assertEqual(profile.pipeline_stage, "unpack")
        self.assertEqual(profile.runtime["unpack_start_delay_seconds"], 1)
        self.assertEqual(profile.runtime["sender_source_port_101"], 45871)
        self.assertEqual(profile.runtime["sender_source_port_102"], 55871)
        self.assertEqual(profile.geometry["target_payload_gbps"], 30.0)
        self.assertEqual(profile.geometry["duration_seconds"], 60.0)

    def test_checked_in_a469_unpack_profile_is_production_scale(self):
        profile = profiles.load_profile(
            ROOT / "config" / "testing" / "profiles"
            / "qths1-unpack-30p2505gbps-60s-a469-v1.json"
        )

        self.assertEqual(profile.pipeline_stage, "unpack")
        self.assertEqual(
            profile.runtime["worker_cpu_list"], "14,15,16,17,18,19"
        )
        self.assertEqual(profile.runtime["unpack_start_delay_seconds"], 1)
        self.assertEqual(profile.geometry["target_payload_gbps"], 30.2505)
        self.assertEqual(profile.geometry["duration_seconds"], 60.0)
        self.assertEqual(profile.geometry["raw_block_bytes"], 50336832)
        self.assertEqual(profile.geometry["compute_block_bytes"], 49946624)
        self.assertEqual(profile.geometry["window_blocks"], 33)
        self.assertEqual(profile.geometry["reorder_horizon_groups"], 416)

    def test_load_profile_rejects_unknown_top_level_field(self):
        value = valid_profile()
        value["unexpected"] = True
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_profile(directory, value)
            with self.assertRaisesRegex(ValueError, "unknown top-level"):
                profiles.load_profile(path)

    def test_load_profile_rejects_duplicate_cpu_roles(self):
        value = valid_profile()
        value["runtime"]["sink_cpu_list"] = "19"
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_profile(directory, value)
            with self.assertRaisesRegex(ValueError, "CPU roles must be distinct"):
                profiles.load_profile(path)

    def test_gpu_profile_requires_explicit_worker_sink_and_numa(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = profiles.load_profile(
                self.write_profile(directory, valid_gpu_profile())
            )
        self.assertEqual(profile.runtime["gpu_worker_cpu"], 21)

        value = valid_gpu_profile()
        del value["runtime"]["gpu_worker_cpu"]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "missing fields"):
                profiles.load_profile(self.write_profile(directory, value))

    def test_full_profile_accepts_split_ingress_and_processing_numa(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = profiles.load_profile(
                self.write_profile(directory, valid_split_full_profile())
            )

        self.assertNotIn("numa_node", profile.runtime)
        self.assertEqual(profile.runtime["ingress_numa_node"], 1)
        self.assertEqual(profile.runtime["processing_numa_node"], 0)

    def test_apply_profile_fills_only_non_explicit_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = profiles.load_profile(self.write_profile(directory))
        request = FakeRequest(receiver_poll_cpu=12)

        resolved = profiles.apply_profile(
            request, profile, explicit_fields={"receiver_poll_cpu"}
        )

        self.assertEqual(resolved.receiver_poll_cpu, 12)
        self.assertEqual(resolved.worker_cpu_list, "14,15,16,17,18,19")
        self.assertEqual(resolved.sink_cpu_list, "20")
        self.assertEqual(resolved.sender_source_port_101, 45871)
        self.assertEqual(resolved.sender_source_port_102, 55871)
        self.assertEqual(resolved.numa_node, 1)
        self.assertEqual(resolved.unpack_start_delay_seconds, 1)

    def test_compare_profile_returns_field_level_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = profiles.load_profile(self.write_profile(directory))
        plan = dataclasses.replace(
            FakePlan(), receiver_poll_cpu=12, compute_ring_blocks=16
        )

        differences = profiles.compare_profile(plan, profile)

        self.assertIn(
            {
                "field": "runtime.receiver_poll_cpu",
                "baseline": 13,
                "effective": 12,
            },
            differences,
        )
        self.assertIn(
            {
                "field": "geometry.compute_ring_blocks",
                "baseline": 8,
                "effective": 16,
            },
            differences,
        )

    def test_compare_profile_accepts_exact_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = profiles.load_profile(self.write_profile(directory))
        self.assertEqual(profiles.compare_profile(FakePlan(), profile), [])

    def test_profile_rejects_only_one_station_source_port(self):
        value = valid_profile()
        del value["runtime"]["sender_source_port_102"]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "source ports must be supplied together"):
                profiles.load_profile(self.write_profile(directory, value))

    def test_unpack_profile_requires_fixed_station_source_ports(self):
        value = valid_profile()
        del value["runtime"]["sender_source_port_101"]
        del value["runtime"]["sender_source_port_102"]
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "missing fields"):
                profiles.load_profile(self.write_profile(directory, value))


if __name__ == "__main__":
    unittest.main()
