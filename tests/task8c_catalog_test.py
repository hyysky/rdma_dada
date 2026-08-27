#!/usr/bin/env python3
"""Tests for the compact Task 8C result catalog."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import task8c_artifacts  # noqa: E402
import task8c_catalog as catalog  # noqa: E402


def make_suite_fixture(
    root,
    suite_id="unpack-30Gbps-60s-20260826T120000Z",
    topology="unpack",
    test_result="PASS",
    cleanup_result="PASS",
    started_utc="2026-08-26T12:00:00+00:00",
    rate_gbps=30.0,
    duration_seconds=60.0,
    profile_id="qths1-unpack-30gbps-v1",
):
    suite = pathlib.Path(root) / suite_id
    (suite / "runs").mkdir(parents=True)
    request = {
        "aggregate_gbps": rate_gbps,
        "duration_seconds": duration_seconds,
        "pipeline_stage": topology,
        "baseline_profile_path": "config/testing/profiles/%s.json" % profile_id,
        "baseline_profile_sha256": "b" * 64,
        "receiver_poll_cpu": 13,
        "worker_cpu_list": (
            "14,15,16,17,18,19" if topology in ("unpack", "full") else None
        ),
        "gpu_worker_cpu": 21 if topology in ("gpu", "full") else None,
        "sink_cpu_list": "20",
        "numa_node": 1,
        "receiver_poll_batch": 32,
        "receiver_wr_num": 1024,
        "unpack_start_delay_seconds": 1,
    }
    plan = {
        "pipeline_stage": topology,
        "aggregate_gbps": rate_gbps,
        "duration_seconds": duration_seconds,
        "config_id": "c" * 64,
        "geometry_id": "d" * 64,
        "raw_block_bytes": 52838400,
        "raw_ring_blocks": 16,
        "compute_block_bytes": 52428800,
        "compute_ring_blocks": 8,
        "output_block_bytes": 209715200,
        "output_ring_blocks": 8,
        "window_groups": 198400,
        "reorder_horizon_groups": 96000,
        "profile_evidence": {"profile_id": profile_id},
    }
    summary = {
        "TEST_RESULT": test_result,
        "suite_id": suite_id,
        "request": request,
        "warmup_count": 0,
        "measured_count": 1,
        "runs": [{
            "role": "measured",
            "index": 1,
            "run_id": "measured-01",
            "TEST_RESULT": test_result,
            "CLEANUP_RESULT": cleanup_result,
            "result_path": "runs/measured-01.json",
        }],
        "actual_aggregate_gbps": ({
            "median": 29.9999,
            "minimum": 29.9998,
            "maximum": 30.0,
            "spread": 0.0002,
        } if test_result == "PASS" else None),
    }
    preflight = {
        "created_utc": started_utc,
        "plan": plan,
        "config": {"binary_sha": {"qths1:rdma2dada": "e" * 64}},
    }
    evidence_text = "receiver: summary\n"
    run = {
        "TEST_RESULT": test_result,
        "cleanup": {"CLEANUP_RESULT": cleanup_result},
        "failure": None if test_result == "PASS" else {
            "stage": "COLLECTING",
            "classification": test_result,
            "exit_code": 1,
            "stderr": "count mismatch",
        },
        "statistics": {},
        "evidence_file": "measured-01.evidence.log",
        "evidence_sha256": hashlib.sha256(evidence_text.encode()).hexdigest(),
    }
    (suite / "observation.json").write_text('{"schema_version": 1}\n')
    (suite / "resolved_observation.json").write_text(
        '{"schema_version": 1}\n'
    )
    (suite / "summary.json").write_text(json.dumps(summary) + "\n")
    (suite / "preflight.json").write_text(json.dumps(preflight) + "\n")
    (suite / "runs" / "measured-01.json").write_text(json.dumps(run) + "\n")
    (suite / "runs" / "measured-01.evidence.log").write_text(evidence_text)
    if test_result != "PASS" or cleanup_result != "PASS":
        debug = suite / "debug" / "measured-01"
        debug.mkdir(parents=True)
        (debug / "failed-command.json").write_text(
            '{"argv": ["fixture"], "exit_code": 1}\n'
        )
        (debug / "failed-process.log").write_text("fixture failure output\n")
        (debug / "resource-snapshot.json").write_text('{"resources": []}\n')
    task8c_artifacts.write_manifest(suite)
    return suite


class CatalogTest(unittest.TestCase):
    def test_catalog_records_split_ingress_and_processing_numa(self):
        with tempfile.TemporaryDirectory() as directory:
            suite = make_suite_fixture(
                directory,
                suite_id="full-30Gbps-split-numa",
                topology="full",
            )
            summary_path = suite / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["request"]["numa_node"] = None
            summary["request"]["ingress_numa_node"] = 1
            summary["request"]["processing_numa_node"] = 0
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(suite)
            (suite / "origin.json").write_text(json.dumps({
                "schema_version": 1,
                "suite_id": suite.name,
                "source_host": "HF",
                "remote_suite_root": f"/results/{suite.name}",
                "imported_utc": "2026-08-27T00:00:00Z",
                "source_manifest_sha256": "a" * 64,
            }) + "\n")

            entry = catalog.derive_catalog_entry(suite)

        self.assertIsNone(entry["configuration"]["numa_node"])
        self.assertEqual(entry["configuration"]["ingress_numa_node"], 1)
        self.assertEqual(entry["configuration"]["processing_numa_node"], 0)

    def test_rebuild_empty_root_creates_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            catalog.rebuild_catalog(
                root, generated_utc="2026-08-26T00:00:00Z"
            )
            value = json.loads((root / "catalog.json").read_text())
            self.assertEqual(value["suite_count"], 0)
            self.assertEqual(value["suites"], [])
            self.assertTrue((root / "catalog.csv").is_file())
            self.assertTrue((root / "MANIFEST.sha256").is_file())

    def test_import_valid_suite_records_origin_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(base / "source")
            results = base / "results"
            target = catalog.import_suite(
                results,
                source,
                "qths1",
                "/home/user/wy/results/%s" % source.name,
                "a" * 64,
            )
            origin = json.loads((target / "origin.json").read_text())
            self.assertEqual(origin["source_host"], "qths1")
            self.assertEqual(origin["source_manifest_sha256"], "a" * 64)
            self.assertEqual(origin["schema_version"], 1)
            self.assertEqual(origin["suite_id"], source.name)
            self.assertEqual(
                catalog.import_suite(
                    results,
                    source,
                    "qths1",
                    "/home/user/wy/results/%s" % source.name,
                    "a" * 64,
                ),
                target,
            )
            self.assertFalse(any(results.glob(".import-*")))

    def test_idempotent_import_rebuilds_a_missing_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(base / "source")
            results = base / "results"
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            (results / "catalog.json").unlink()
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            catalog.verify_catalog(results)
            value = json.loads((results / "catalog.json").read_text())
            self.assertEqual(value["suite_count"], 1)

    def test_import_rejects_conflicting_suite_id(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(base / "source")
            results = base / "results"
            catalog.import_suite(
                results, source, "qths1", "/remote/first", "a" * 64
            )
            conflict_root = base / "conflict"
            conflict = make_suite_fixture(conflict_root, suite_id=source.name)
            summary_path = conflict / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["fixture_variant"] = "conflicting-content"
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(conflict)
            with self.assertRaisesRegex(ValueError, "conflicting suite id"):
                catalog.import_suite(
                    results, conflict, "qths1", "/remote/second", "a" * 64
                )

    def test_import_rejects_manifest_mismatch_and_removes_temporary_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(base / "source")
            (source / "summary.json").write_text("{}\n")
            results = base / "results"
            with self.assertRaisesRegex(ValueError, "manifest"):
                catalog.import_suite(
                    results, source, "qths1", "/remote/suite", "a" * 64
                )
            self.assertFalse(any(results.glob(".import-*")))

    def test_validate_rejects_missing_preflight_and_unsafe_result_path(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            missing = make_suite_fixture(base / "missing")
            (missing / "preflight.json").unlink()
            task8c_artifacts.write_manifest(missing)
            with self.assertRaisesRegex(ValueError, "preflight"):
                catalog.validate_suite(missing)

            missing_observation = make_suite_fixture(base / "missing-observation")
            (missing_observation / "observation.json").unlink()
            task8c_artifacts.write_manifest(missing_observation)
            with self.assertRaisesRegex(ValueError, "observation"):
                catalog.validate_suite(missing_observation)

            unsafe = make_suite_fixture(base / "unsafe")
            summary_path = unsafe / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["runs"][0]["result_path"] = "../outside.json"
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(unsafe)
            with self.assertRaisesRegex(ValueError, "result_path"):
                catalog.validate_suite(unsafe)

    def test_import_rejects_invalid_source_manifest_sha_and_origin_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(base / "source")
            results = base / "results"
            with self.assertRaisesRegex(ValueError, "source manifest SHA256"):
                catalog.import_suite(
                    results, source, "qths1", "/remote/suite", "not-a-sha"
                )
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            with self.assertRaisesRegex(ValueError, "conflicting suite id"):
                catalog.import_suite(
                    results, source, "qths2", "/remote/suite", "a" * 64
                )

    def test_validate_rejects_symlink_in_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            suite = make_suite_fixture(base / "source")
            external = base / "external.log"
            external.write_text("outside\n")
            link = suite / "runs" / "linked.evidence.log"
            link.symlink_to(external)
            task8c_artifacts.write_manifest(suite)
            with self.assertRaisesRegex(ValueError, "symlink"):
                catalog.validate_suite(suite)

    def test_validate_reconciles_run_status_and_evidence_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            suite = make_suite_fixture(base / "status")
            run_path = suite / "runs" / "measured-01.json"
            run = json.loads(run_path.read_text())
            run["TEST_RESULT"] = "PRODUCT_FAIL"
            run_path.write_text(json.dumps(run) + "\n")
            task8c_artifacts.write_manifest(suite)
            with self.assertRaisesRegex(ValueError, "TEST_RESULT"):
                catalog.validate_suite(suite)

            evidence_suite = make_suite_fixture(base / "evidence")
            evidence_path = evidence_suite / "runs" / "measured-01.evidence.log"
            evidence_path.write_text("tampered but remanifested\n")
            task8c_artifacts.write_manifest(evidence_suite)
            with self.assertRaisesRegex(ValueError, "evidence SHA256"):
                catalog.validate_suite(evidence_suite)

            summary_suite = make_suite_fixture(
                base / "summary", test_result="PRODUCT_FAIL"
            )
            summary_path = summary_suite / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["TEST_RESULT"] = "PASS"
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(summary_suite)
            with self.assertRaisesRegex(ValueError, "suite TEST_RESULT"):
                catalog.validate_suite(summary_suite)

    def test_validate_rejects_unreferenced_run_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            suite = make_suite_fixture(pathlib.Path(directory) / "source")
            extra_run = suite / "runs" / "measured-02.json"
            extra_log = suite / "runs" / "measured-02.evidence.log"
            extra_run.write_text('{}\n')
            extra_log.write_text("extra\n")
            task8c_artifacts.write_manifest(suite)
            with self.assertRaisesRegex(ValueError, "unreferenced run"):
                catalog.validate_suite(suite)

    def test_validate_rejects_extra_root_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            suite = make_suite_fixture(pathlib.Path(directory) / "source")
            (suite / "receiver-progress.log").write_text("full progress log\n")
            task8c_artifacts.write_manifest(suite)
            with self.assertRaisesRegex(ValueError, "compact suite file set"):
                catalog.validate_suite(suite)

    def test_failure_requires_structured_failure_and_debug_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            missing_debug = make_suite_fixture(
                base / "missing-debug", test_result="PERFORMANCE_FAIL"
            )
            (missing_debug / "debug" / "measured-01" / "failed-process.log").unlink()
            task8c_artifacts.write_manifest(missing_debug)
            with self.assertRaisesRegex(ValueError, "compact suite file set"):
                catalog.validate_suite(missing_debug)

            missing_failure = make_suite_fixture(
                base / "missing-failure", test_result="PERFORMANCE_FAIL"
            )
            run_path = missing_failure / "runs" / "measured-01.json"
            run = json.loads(run_path.read_text())
            run["failure"] = None
            run_path.write_text(json.dumps(run) + "\n")
            task8c_artifacts.write_manifest(missing_failure)
            with self.assertRaisesRegex(ValueError, "structured failure"):
                catalog.validate_suite(missing_failure)

    def test_pass_requires_all_requested_repetitions(self):
        with tempfile.TemporaryDirectory() as directory:
            suite = make_suite_fixture(pathlib.Path(directory) / "source")
            summary_path = suite / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["measured_count"] = 3
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(suite)
            with self.assertRaisesRegex(ValueError, "PASS repetition"):
                catalog.validate_suite(suite)

    def test_validate_rejects_unlisted_directory_and_broken_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            suite = make_suite_fixture(base / "source")
            external = base / "external"
            external.mkdir()
            (suite / "linked-directory").symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "symlink"):
                catalog.validate_suite(suite)

            (suite / "linked-directory").unlink()
            (suite / "broken-link").symlink_to(base / "missing")
            with self.assertRaisesRegex(ValueError, "symlink"):
                catalog.validate_suite(suite)

    def test_derive_catalog_entry_preserves_identity_geometry_and_modules(self):
        expected_modules = {
            "receive": ["rdma2dada", "dada_dbnull"],
            "unpack": ["rdma2dada", "vdif_unpack_worker", "dada_dbnull"],
            "gpu": ["dada_junkdb", "pipeline_worker", "dada_dbnull"],
            "full": [
                "rdma2dada", "vdif_unpack_worker", "pipeline_worker",
                "dada_dbnull",
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            for topology, modules in expected_modules.items():
                source = make_suite_fixture(
                    base / ("source-" + topology),
                    suite_id=topology + "-suite",
                    topology=topology,
                )
                imported = catalog.import_suite(
                    base / "results",
                    source,
                    "qths1",
                    "/remote/%s" % source.name,
                    "a" * 64,
                )
                entry = catalog.derive_catalog_entry(imported)
                self.assertEqual(entry["modules"], modules)
                self.assertEqual(entry["test_topology"], topology)
                self.assertEqual(entry["target_payload_gbps"], 30.0)
                self.assertEqual(entry["duration_seconds"], 60.0)
                self.assertEqual(entry["cleanup_result"], "PASS")
                self.assertEqual(entry["identity"]["config_id"], "c" * 64)
                self.assertEqual(entry["identity"]["geometry_id"], "d" * 64)
                expected_raw = (
                    {"block_bytes": 52838400, "blocks": 16}
                    if topology in ("receive", "unpack", "full") else None
                )
                self.assertEqual(
                    entry["configuration"]["rings"]["raw"], expected_raw
                )
                self.assertEqual(
                    entry["result_path"], "suites/%s" % source.name
                )
                self.assertRegex(entry["summary_sha256"], r"^[0-9a-f]{64}$")

    def test_rebuild_is_deterministic_and_orders_suites(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            later = make_suite_fixture(
                base / "later",
                suite_id="unpack-later",
                started_utc="2026-08-26T13:00:00+00:00",
            )
            earlier = make_suite_fixture(
                base / "earlier",
                suite_id="receive-earlier",
                topology="receive",
                started_utc="2026-08-26T11:00:00+00:00",
            )
            for source in (later, earlier):
                catalog.import_suite(
                    results, source, "qths1", "/remote/%s" % source.name,
                    "a" * 64,
                )
            catalog.rebuild_catalog(
                results, generated_utc="2026-08-26T14:00:00Z"
            )
            first_json = (results / "catalog.json").read_bytes()
            first_csv = (results / "catalog.csv").read_bytes()
            catalog.rebuild_catalog(
                results, generated_utc="2026-08-26T14:00:00Z"
            )
            self.assertEqual((results / "catalog.json").read_bytes(), first_json)
            self.assertEqual((results / "catalog.csv").read_bytes(), first_csv)
            value = json.loads(first_json)
            self.assertEqual(value["suite_count"], 2)
            self.assertEqual(
                [item["suite_id"] for item in value["suites"]],
                ["receive-earlier", "unpack-later"],
            )
            catalog.verify_catalog(results)

    def test_failed_rebuild_preserves_previous_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            target = catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            catalog.rebuild_catalog(
                results, generated_utc="2026-08-26T14:00:00Z"
            )
            before_json = (results / "catalog.json").read_bytes()
            before_csv = (results / "catalog.csv").read_bytes()
            (target / "summary.json").write_text("{}\n")
            with self.assertRaisesRegex(ValueError, "manifest"):
                catalog.rebuild_catalog(results)
            self.assertEqual((results / "catalog.json").read_bytes(), before_json)
            self.assertEqual((results / "catalog.csv").read_bytes(), before_csv)

    def test_mid_publish_failure_restores_all_catalog_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            catalog.rebuild_catalog(
                results, generated_utc="2026-01-01T00:00:00Z"
            )
            before = {
                name: (results / name).read_bytes()
                for name in ("catalog.json", "catalog.csv", "MANIFEST.sha256")
            }
            real_write = catalog._atomic_write
            calls = {"count": 0}

            def fail_second_write(path, data):
                calls["count"] += 1
                if calls["count"] == 2:
                    raise OSError("injected catalog publish failure")
                return real_write(path, data)

            with mock.patch.object(catalog, "_atomic_write", fail_second_write):
                with self.assertRaisesRegex(OSError, "injected"):
                    catalog.rebuild_catalog(results)
            for name, content in before.items():
                self.assertEqual((results / name).read_bytes(), content)

    def test_verify_rejects_an_unindexed_suite_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            first = make_suite_fixture(base / "first", suite_id="first-suite")
            second = make_suite_fixture(base / "second", suite_id="second-suite")
            catalog.import_suite(
                results, first, "qths1", "/remote/first", "a" * 64
            )
            old_files = {
                name: (results / name).read_bytes()
                for name in ("catalog.json", "catalog.csv", "MANIFEST.sha256")
            }
            catalog.import_suite(
                results, second, "qths1", "/remote/second", "a" * 64
            )
            for name, content in old_files.items():
                (results / name).write_bytes(content)
            with self.assertRaisesRegex(ValueError, "unindexed"):
                catalog.verify_catalog(results)

    def test_catalog_validation_rejects_unknown_and_invalid_identity_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            value = json.loads((results / "catalog.json").read_text())
            unknown = copy.deepcopy(value)
            unknown["suites"][0]["unexpected"] = True
            with self.assertRaisesRegex(ValueError, "fields"):
                catalog.validate_catalog_value(unknown)
            invalid = copy.deepcopy(value)
            invalid["suites"][0]["identity"]["config_id"] = "invalid"
            with self.assertRaisesRegex(ValueError, "config_id"):
                catalog.validate_catalog_value(invalid)
            invalid_result = copy.deepcopy(value)
            invalid_result["suites"][0]["test_result"] = "MADE_UP"
            with self.assertRaisesRegex(ValueError, "test_result"):
                catalog.validate_catalog_value(invalid_result)
            invalid_cpu = copy.deepcopy(value)
            invalid_cpu["suites"][0]["configuration"]["receiver_poll_cpu"] = "13"
            with self.assertRaisesRegex(ValueError, "receiver_poll_cpu"):
                catalog.validate_catalog_value(invalid_cpu)
            invalid_profile = copy.deepcopy(value)
            invalid_profile["suites"][0]["identity"]["baseline_profile_id"] = 7
            with self.assertRaisesRegex(ValueError, "baseline_profile_id"):
                catalog.validate_catalog_value(invalid_profile)

    def test_query_combines_filters_and_latest_is_chronological(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            fixtures = [
                make_suite_fixture(
                    base / "one", suite_id="receive-one", topology="receive",
                    rate_gbps=10.0, started_utc="2026-08-25T10:00:00+00:00",
                    profile_id="receive-v1",
                ),
                make_suite_fixture(
                    base / "two", suite_id="unpack-two", topology="unpack",
                    rate_gbps=30.0, started_utc="2026-08-26T10:00:00+00:00",
                    profile_id="unpack-v1",
                ),
                make_suite_fixture(
                    base / "three", suite_id="unpack-three", topology="unpack",
                    rate_gbps=30.0, started_utc="2026-08-27T10:00:00+00:00",
                    profile_id="unpack-v1",
                ),
            ]
            for source in fixtures:
                catalog.import_suite(
                    results, source, "qths1", "/remote/%s" % source.name,
                    "a" * 64,
                )
            value = json.loads((results / "catalog.json").read_text())
            selected = catalog.query_catalog(
                value,
                topology="unpack",
                rate_gbps=30.0,
                test_result="PASS",
                cleanup_result="PASS",
                started_from="2026-08-26T00:00:00Z",
                started_to="2026-08-28T00:00:00Z",
                baseline_profile_id="unpack-v1",
                latest=1,
            )
            self.assertEqual(
                [entry["suite_id"] for entry in selected], ["unpack-three"]
            )
            self.assertEqual(catalog.query_catalog(value, rate_gbps=99.0), [])
            with self.assertRaisesRegex(ValueError, "latest"):
                catalog.query_catalog(value, latest=0)
            with self.assertRaisesRegex(ValueError, "date"):
                catalog.query_catalog(value, started_from="not-a-date")
            with self.assertRaisesRegex(ValueError, "test result"):
                catalog.query_catalog(value, test_result="MADE_UP")
            with self.assertRaisesRegex(ValueError, "cleanup result"):
                catalog.query_catalog(value, cleanup_result="MADE_UP")
            for invalid_rate in (0.0, -1.0, float("nan"), float("inf")):
                with self.assertRaisesRegex(ValueError, "rate-gbps"):
                    catalog.query_catalog(value, rate_gbps=invalid_rate)

    def test_catalog_uses_completed_repetitions_and_records_cleanup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            source = make_suite_fixture(
                base / "source", test_result="PASS", cleanup_result="FAIL"
            )
            summary_path = source / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["warmup_count"] = 1
            summary["measured_count"] = 3
            summary["runs"][0]["role"] = "warmup"
            summary_path.write_text(json.dumps(summary) + "\n")
            task8c_artifacts.write_manifest(source)
            results = base / "results"
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            entry = json.loads((results / "catalog.json").read_text())["suites"][0]
            self.assertEqual(entry["warmup_count"], 1)
            self.assertEqual(entry["measured_count"], 0)
            self.assertEqual(entry["first_failure"]["stage"], "CLEANUP")
            self.assertEqual(
                entry["first_failure"]["classification"], "CLEANUP_FAIL"
            )

    def test_query_cli_outputs_json_and_absolute_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            command = [
                sys.executable,
                str(ROOT / "scripts" / "task8c_catalog.py"),
                "query",
                "--results-root", str(results),
                "--suite-id", source.name,
            ]
            json_result = subprocess.run(
                command + ["--format", "json"],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(json.loads(json_result.stdout)[0]["suite_id"], source.name)
            path_result = subprocess.run(
                command + ["--format", "paths"],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(
                path_result.stdout.strip(),
                str((results / "suites" / source.name).resolve()),
            )

    def test_promote_verified_failure_boundary_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(
                base / "source", test_result="PERFORMANCE_FAIL"
            )
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            accepted = base / "docs" / "results" / "accepted-results.json"
            accepted.parent.mkdir(parents=True)
            accepted.write_text('{"schema_version": 1, "results": []}\n')
            value = catalog.promote_suite(results, source.name, accepted)
            self.assertEqual(len(value["results"]), 1)
            promoted = value["results"][0]
            self.assertEqual(
                promoted["catalog_entry"]["test_result"], "PERFORMANCE_FAIL"
            )
            self.assertIn("promoted_utc", promoted)
            self.assertEqual(promoted["origin"]["source_host"], "qths1")
            self.assertEqual(promoted["origin"]["remote_suite_root"], "/remote/suite")
            before = accepted.read_bytes()
            catalog.promote_suite(results, source.name, accepted)
            self.assertEqual(accepted.read_bytes(), before)

    def test_promote_rejects_unknown_unverified_and_conflicting_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            target = catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            accepted = base / "accepted-results.json"
            accepted.write_text('{"schema_version": 1, "results": []}\n')
            with self.assertRaisesRegex(ValueError, "unknown suite"):
                catalog.promote_suite(results, "unknown", accepted)

            (target / "summary.json").write_text("{}\n")
            with self.assertRaisesRegex(ValueError, "manifest"):
                catalog.promote_suite(results, source.name, accepted)

            shutil_source = make_suite_fixture(base / "fresh")
            fresh_results = base / "fresh-results"
            catalog.import_suite(
                fresh_results, shutil_source, "qths1", "/remote/fresh", "a" * 64
            )
            entry = json.loads((fresh_results / "catalog.json").read_text())["suites"][0]
            conflicting = json.loads(json.dumps(entry))
            conflicting["identity"]["source_manifest_sha256"] = "f" * 64
            accepted.write_text(json.dumps({
                "schema_version": 1,
                "results": [{
                    "catalog_entry": conflicting,
                    "origin": {
                        "schema_version": 1,
                        "suite_id": conflicting["suite_id"],
                        "source_host": "qths1",
                        "remote_suite_root": "/remote/fresh",
                        "imported_utc": "2026-08-26T00:00:00Z",
                        "source_manifest_sha256": "f" * 64,
                    },
                    "promoted_utc": "2026-08-26T00:00:00Z",
                }],
            }) + "\n")
            with self.assertRaisesRegex(ValueError, "conflicting promoted suite"):
                catalog.promote_suite(
                    fresh_results, shutil_source.name, accepted
                )

    def test_promote_rejects_truncated_accepted_entry_and_cli_accepts_spec_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            accepted = base / "accepted-results.json"
            accepted.write_text(json.dumps({
                "schema_version": 1,
                "results": [{
                    "catalog_entry": {"suite_id": "old"},
                    "origin": {},
                    "promoted_utc": "2026-08-26T00:00:00Z",
                }],
            }) + "\n")
            with self.assertRaisesRegex(ValueError, "catalog"):
                catalog.promote_suite(results, source.name, accepted)

    def test_promote_rejects_origin_source_identity_conflict(self):
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            results = base / "results"
            source = make_suite_fixture(base / "source")
            catalog.import_suite(
                results, source, "qths1", "/remote/suite", "a" * 64
            )
            accepted = base / "accepted-results.json"
            value = catalog.promote_suite(results, source.name, accepted)
            value["results"][0]["origin"]["source_manifest_sha256"] = "f" * 64
            accepted.write_text(json.dumps(value) + "\n")
            with self.assertRaisesRegex(ValueError, "source identity"):
                catalog.promote_suite(results, source.name, accepted)

            help_result = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "task8c_catalog.py"),
                 "promote", "--help"],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("--accepted-output", help_result.stdout)


if __name__ == "__main__":
    unittest.main()
