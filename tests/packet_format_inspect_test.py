#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: packet_format_inspect_test.py INSPECT CONFIG")

    inspect = pathlib.Path(sys.argv[1])
    config_path = pathlib.Path(sys.argv[2])
    schema_path = config_path.with_name("packet-format-v2.schema.json")
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    profile = json.loads(config_path.read_text(encoding="utf-8"))
    require(profile["schema_version"] == 2, "wire profile schema must be v2")
    require(
        set(profile["record"]) == {"application_header_bytes"},
        "record must not own observation payload geometry",
    )
    schema_fields = [
        item["const"]
        for item in schema["properties"]["application_header"]
        ["properties"]["fields"]["prefixItems"]
    ]
    schema_axes = [
        item["const"]
        for item in schema["properties"]["payload"]
        ["properties"]["axes"]["prefixItems"]
    ]
    require(
        schema_fields == profile["application_header"]["fields"],
        "Schema must encode the exact 22-field Project VDIF v1 table",
    )
    require(
        schema_axes == profile["payload"]["axes"],
        "Schema must encode the exact wire TFP and Station lookup semantics",
    )
    result = subprocess.run(
        [str(inspect), str(config_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    require(result.returncode == 0, result.stderr)
    lines = set(result.stdout.splitlines())
    expected = {
        "FORMAT_ID=project-vdif-v1",
        "APPLICATION_HEADER_BYTES=32",
        "HEADER_FIELD_COUNT=22",
        "SAMPLE_FORMAT=CI8",
        "SAMPLE_ENCODING=TWOS_COMPLEMENT",
        "COMPONENT_ORDER=IQ",
        "SAMPLE_BYTES=2",
        "PACKED_ORDER=TFP",
        "AXIS_T_EXTENT=HEADER:nsamp_per_packet",
        "AXIS_T_ORIGIN=DERIVED:vdif_frame_time",
        "AXIS_A_ORIGIN=LOOKUP:antenna_map:station_id",
    }
    require(expected.issubset(lines), "inspect output is incomplete")
    require(
        not any(line.startswith("OUTPUT_ORDER=") for line in lines),
        "wire-format inspection must not advertise a downstream output order",
    )
    require(
        not any(line.startswith(("PAYLOAD_BYTES=", "RECORD_BYTES=")) for line in lines),
        "wire-format inspection must not advertise observation byte geometry",
    )

    legacy_schema = json.loads(config_path.read_text(encoding="utf-8"))
    legacy_schema["schema_version"] = 1
    legacy_schema["record"]["payload_bytes"] = 12288
    with tempfile.TemporaryDirectory() as directory:
        legacy_schema_path = pathlib.Path(directory) / "schema-v1.json"
        legacy_schema_path.write_text(json.dumps(legacy_schema), encoding="utf-8")
        legacy = subprocess.run(
            [str(inspect), str(legacy_schema_path)],
            check=False,
            capture_output=True,
            text=True,
        )
    require(legacy.returncode != 0, "schema v1 profile must be rejected")
    require(
        "schema_version 1" in legacy.stderr,
        "schema v1 rejection must identify the migration source",
    )

    document = json.loads(config_path.read_text(encoding="utf-8"))
    document["unexpected"] = True
    with tempfile.TemporaryDirectory() as directory:
        invalid_path = pathlib.Path(directory) / "invalid.json"
        invalid_path.write_text(json.dumps(document), encoding="utf-8")
        invalid = subprocess.run(
            [str(inspect), str(invalid_path)],
            check=False,
            capture_output=True,
            text=True,
        )
    require(invalid.returncode != 0, "unknown JSON field must be rejected")
    require("unknown field" in invalid.stderr, "error identifies unknown field")

    wrong_header_size = json.loads(config_path.read_text(encoding="utf-8"))
    wrong_header_size["record"]["application_header_bytes"] = 64
    with tempfile.TemporaryDirectory() as directory:
        wrong_header_path = pathlib.Path(directory) / "wrong-header-size.json"
        wrong_header_path.write_text(
            json.dumps(wrong_header_size), encoding="utf-8"
        )
        wrong_header = subprocess.run(
            [str(inspect), str(wrong_header_path)],
            check=False,
            capture_output=True,
            text=True,
        )
    require(wrong_header.returncode != 0, "64-byte header must be rejected")
    require(
        "application_header_bytes must be 32" in wrong_header.stderr,
        "fixed-header error must identify the required 32-byte size",
    )

    legacy_header_signed = json.loads(config_path.read_text(encoding="utf-8"))
    legacy_header_signed["application_header"]["fields"][0]["signed"] = False
    legacy_payload_signed = json.loads(config_path.read_text(encoding="utf-8"))
    legacy_payload_signed["payload"]["component_signed"] = True
    with tempfile.TemporaryDirectory() as directory:
        directory_path = pathlib.Path(directory)
        for name, document in (
            ("header-signed.json", legacy_header_signed),
            ("payload-signed.json", legacy_payload_signed),
        ):
            legacy_path = directory_path / name
            legacy_path.write_text(json.dumps(document), encoding="utf-8")
            legacy = subprocess.run(
                [str(inspect), str(legacy_path)],
                check=False,
                capture_output=True,
                text=True,
            )
            require(legacy.returncode != 0, name + " must be rejected")
            require(
                "unknown field" in legacy.stderr,
                name + " must report the obsolete signed field",
            )

    print("packet_format_inspect_test passed")


if __name__ == "__main__":
    main()
