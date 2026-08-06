#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: fpga_sender_sim_loopback_test.py EXECUTABLE CONFIG", file=sys.stderr)
        return 2

    executable = Path(sys.argv[1])
    source_config = Path(sys.argv[2])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(2.0)
        port = receiver.getsockname()[1]

        config = json.loads(source_config.read_text(encoding="utf-8"))
        config["destination"]["port"] = port
        config["station"]["station_id"] = 345
        config["time"]["group_count"] = 4
        config["time"]["mode"] = "BURST"
        config["time"]["start_utc"] = ""
        config["faults"] = {
            "drop_groups": [1],
            "duplicate_groups": [2],
            "invalid_header_groups": [3],
        }

        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "sender.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(executable), str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            datagrams = []
            try:
                while len(datagrams) < 4:
                    datagrams.append(receiver.recv(65535))
            except socket.timeout:
                pass
            stdout, stderr = process.communicate(timeout=5)

    if process.returncode != 0:
        fail(f"sender exited {process.returncode}: stdout={stdout!r} stderr={stderr!r}")
    if len(datagrams) != 4:
        fail(f"expected four datagrams after drop/duplicate faults, got {len(datagrams)}")
    if any(len(packet) != 4128 for packet in datagrams):
        fail(f"unexpected record lengths: {[len(packet) for packet in datagrams]}")

    stations = [struct.unpack_from("<I", packet, 12)[0] & 0xFFFF
                for packet in datagrams]
    if stations != [345, 345, 345, 345]:
        fail(f"Station IDs were not isolated to this sender: {stations}")
    frames = [struct.unpack_from("<I", packet, 4)[0] & 0xFFFFFF
              for packet in datagrams]
    if frames != [0, 2, 2, 3]:
        fail(f"drop/duplicate group sequence mismatch: {frames}")
    if datagrams[1] != datagrams[2]:
        fail("duplicate group datagrams are not byte-identical")
    if struct.unpack_from("<I", datagrams[3], 28)[0] == 0:
        fail("invalid-header group did not corrupt reserved Word 7")
    if struct.unpack_from("<I", datagrams[0], 28)[0] != 0:
        fail("normal group unexpectedly has an invalid Word 7")

    print("fpga_sender_sim_loopback_test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
