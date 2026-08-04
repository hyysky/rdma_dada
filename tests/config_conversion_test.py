#!/usr/bin/env python3
"""Regression test for legacy-to-JSON pipeline config conversion."""

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 5:
        print(
            "usage: config_conversion_test.py CONVERTER INSPECTOR LEGACY EXPECTED",
            file=sys.stderr,
        )
        return 2

    converter = pathlib.Path(sys.argv[1])
    inspector = pathlib.Path(sys.argv[2])
    legacy = pathlib.Path(sys.argv[3])
    expected = pathlib.Path(sys.argv[4])
    with tempfile.TemporaryDirectory(prefix="rdma-dada-config-") as directory:
        directory_path = pathlib.Path(directory)
        actual = directory_path / "pipeline.json"
        subprocess.run(
            [sys.executable, str(converter), str(legacy), str(actual)],
            check=True,
        )
        subprocess.run([str(inspector), str(actual)], check=True, capture_output=True)
        with actual.open("r", encoding="utf-8") as source:
            actual_document = json.load(source)
        with expected.open("r", encoding="utf-8") as source:
            expected_document = json.load(source)

        if actual_document != expected_document:
            print("converted JSON differs from the checked-in example", file=sys.stderr)
            return 1

        invalid_documents = []
        unknown_field = dict(actual_document)
        unknown_field["misspelled_field"] = 1
        invalid_documents.append(unknown_field)

        missing_field = json.loads(json.dumps(actual_document))
        del missing_field["packet"]["nbit"]
        invalid_documents.append(missing_field)

        non_integer = json.loads(json.dumps(actual_document))
        non_integer["observation"]["nant"] = 4.0
        invalid_documents.append(non_integer)

        for index, document in enumerate(invalid_documents):
            invalid_path = directory_path / f"invalid-{index}.json"
            with invalid_path.open("w", encoding="utf-8") as destination:
                json.dump(document, destination)
            result = subprocess.run(
                [str(inspector), str(invalid_path)], capture_output=True, text=True
            )
            if result.returncode == 0:
                print(f"invalid config {index} was accepted", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
