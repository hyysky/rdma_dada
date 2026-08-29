#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def reserve_source_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: fpga_sender_sim_linux_batch_test.py EXECUTABLE V2_CONFIG",
              file=sys.stderr)
        return 2
    executable = Path(sys.argv[1])
    source_config = Path(sys.argv[2])
    source_port = reserve_source_port()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(4.0)
        destination_port = receiver.getsockname()[1]
        config = json.loads(source_config.read_text(encoding="utf-8"))
        config["schema_version"] = 3
        config["source"]["port"] = source_port
        config["destination"]["port"] = destination_port
        config["station"] = {"station_ids": [777, 778, 779, 780]}
        config["time"]["group_count"] = 16
        config["time"]["start_utc"] = (
            datetime.now(timezone.utc) + timedelta(seconds=1)
        ).strftime("%Y-%m-%d-%H:%M:%S")
        config["transmit"]["target_gbps"] = 0.02
        config["transmit"]["batch_packets"] = 16

        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "linux-batch.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(executable), str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            received = []
            try:
                while len(received) < 64:
                    received.append(receiver.recvfrom(65535))
            except socket.timeout:
                pass
            stdout, stderr = process.communicate(timeout=5)

    if process.returncode != 0:
        fail(f"sender exited {process.returncode}: stdout={stdout!r} stderr={stderr!r}")
    if len(received) != 64:
        fail(f"expected 64 datagrams, got {len(received)}")
    if {address[1] for _, address in received} != {source_port}:
        fail("Linux batch sender did not use the configured source port")
    stations = [struct.unpack_from("<I", packet, 12)[0] & 0xFFFF
                for packet, _ in received]
    expected = []
    configured = [777, 778, 779, 780]
    for group in range(16):
        expected.extend(configured[group % 4:] + configured[:group % 4])
    if stations != expected:
        fail(f"Linux multi-Station batch order is incorrect: {stations[:12]}")

    lines = [line for line in stdout.splitlines() if line.strip()]
    try:
        summary = json.loads(lines[-1])
    except (IndexError, json.JSONDecodeError) as exc:
        fail(f"missing final JSON summary: {stdout!r}: {exc}")
    if summary["backend"] != "SENDMMSG":
        fail(f"Linux sender did not use sendmmsg: {summary}")
    if summary["sent_packets"] != 64 or summary["failed_packets"] != 0:
        fail(f"Linux batch counters do not reconcile: {summary}")
    if summary.get("station_ids") != configured:
        fail(f"Linux batch summary lost Station IDs: {summary}")
    if [item["sent_packets"] for item in summary.get("station_counts", [])] != [16] * 4:
        fail(f"Linux batch per-Station counts do not close: {summary}")
    if summary["batches"] != 4 or summary["batch_packets"] != 16:
        fail(f"Linux batch geometry is incorrect: {summary}")

    print("fpga_sender_sim_linux_batch_test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
