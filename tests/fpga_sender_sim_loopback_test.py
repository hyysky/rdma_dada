#!/usr/bin/env python3

import json
import socket
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


PACED_START_MARGIN_SECONDS = 5


def fail(message: str) -> None:
    raise AssertionError(message)


def reserve_source_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run_v1(executable: Path, source_config: Path) -> None:
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
        fail(f"v1 sender exited {process.returncode}: stdout={stdout!r} stderr={stderr!r}")
    if len(datagrams) != 4:
        fail(f"expected four v1 datagrams after faults, got {len(datagrams)}")
    if any(len(packet) != 4128 for packet in datagrams):
        fail(f"unexpected v1 record lengths: {[len(packet) for packet in datagrams]}")

    stations = [struct.unpack_from("<I", packet, 12)[0] & 0xFFFF
                for packet in datagrams]
    if stations != [345, 345, 345, 345]:
        fail(f"v1 Station IDs were not isolated: {stations}")
    frames = [struct.unpack_from("<I", packet, 4)[0] & 0xFFFFFF
              for packet in datagrams]
    if frames != [0, 2, 2, 3]:
        fail(f"v1 drop/duplicate group sequence mismatch: {frames}")
    if datagrams[1] != datagrams[2]:
        fail("v1 duplicate group datagrams are not byte-identical")
    if struct.unpack_from("<I", datagrams[3], 28)[0] == 0:
        fail("v1 invalid-header group did not corrupt reserved Word 7")


def run_v2(executable: Path, source_config: Path) -> None:
    source_port = reserve_source_port()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(PACED_START_MARGIN_SECONDS + 5.0)
        destination_port = receiver.getsockname()[1]
        config = json.loads(source_config.read_text(encoding="utf-8"))
        config["source"]["port"] = source_port
        config["destination"]["port"] = destination_port
        config["time"]["group_count"] = 320
        config["time"]["start_utc"] = (
            datetime.now(timezone.utc)
            + timedelta(seconds=PACED_START_MARGIN_SECONDS)
        ).strftime("%Y-%m-%d-%H:%M:%S")
        config["transmit"]["target_gbps"] = 0.01

        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "paced.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(executable), str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            received = []
            try:
                while len(received) < 320:
                    received.append(receiver.recvfrom(65535))
            except socket.timeout:
                pass
            stdout, stderr = process.communicate(timeout=5)

    if process.returncode != 0:
        fail(f"v2 sender exited {process.returncode}: stdout={stdout!r} stderr={stderr!r}")
    if len(received) != 320:
        fail(f"expected 320 paced datagrams, got {len(received)}")
    if {address[1] for _, address in received} != {source_port}:
        fail(f"paced sender did not bind source port {source_port}: {received[0][1]}")
    frames = [struct.unpack_from("<I", packet, 4)[0] & 0xFFFFFF
              for packet, _ in received]
    if frames != list(range(320)):
        fail(f"paced VDIF frame sequence mismatch: {frames}")
    payloads = [packet[32:] for packet, _ in received]
    if any(payload != payloads[0] for payload in payloads[1:]):
        fail("REPEAT_TEMPLATE payload changed between paced groups")
    lines = [line for line in stdout.splitlines() if line.strip()]
    try:
        summary = json.loads(lines[-1])
    except (IndexError, json.JSONDecodeError) as exc:
        fail(f"missing final sender JSON summary: {stdout!r}: {exc}")
    if summary["sent_packets"] != 320 or summary["failed_packets"] != 0:
        fail(f"paced sender counters do not reconcile: {summary}")
    if summary["source_port"] != source_port:
        fail(f"summary source port mismatch: {summary}")
    if summary["backend"] not in {"SEND", "SENDMMSG"}:
        fail(f"unexpected UDP backend: {summary}")
    expected_prefix = received[0][0][32:36].hex()
    if summary.get("payload_prefix_hex") != expected_prefix:
        fail(
            "sender summary payload prefix differs from transmitted bytes: "
            f"summary={summary.get('payload_prefix_hex')} "
            f"packet={expected_prefix}"
        )
    if not 0.0098 <= summary["actual_payload_gbps"] <= 0.0102:
        fail(f"paced sender rate is outside 2% tolerance: {summary}")


def run_v3(executable: Path, source_config: Path) -> None:
    source_port = reserve_source_port()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(PACED_START_MARGIN_SECONDS + 5.0)
        config = json.loads(source_config.read_text(encoding="utf-8"))
        config["schema_version"] = 3
        config["source"]["port"] = source_port
        config["destination"]["port"] = receiver.getsockname()[1]
        config["station"] = {"station_ids": [700, 701, 702]}
        config["time"]["group_count"] = 4
        config["time"]["start_utc"] = (
            datetime.now(timezone.utc)
            + timedelta(seconds=PACED_START_MARGIN_SECONDS)
        ).strftime("%Y-%m-%d-%H:%M:%S")
        config["transmit"]["target_gbps"] = 0.01

        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "multi-paced.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(executable), str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            received = []
            try:
                while len(received) < 12:
                    received.append(receiver.recvfrom(65535))
            except socket.timeout:
                pass
            stdout, stderr = process.communicate(timeout=5)

    if process.returncode != 0:
        fail(f"v3 sender exited {process.returncode}: stdout={stdout!r} stderr={stderr!r}")
    if len(received) != 12:
        fail(f"expected twelve v3 datagrams, got {len(received)}")
    if {address[1] for _, address in received} != {source_port}:
        fail("multi-Station sender did not preserve one fixed source port")
    stations = [struct.unpack_from("<I", packet, 12)[0] & 0xFFFF
                for packet, _ in received]
    expected_stations = [
        700, 701, 702,
        701, 702, 700,
        702, 700, 701,
        700, 701, 702,
    ]
    if stations != expected_stations:
        fail(f"multi-Station rotating order mismatch: {stations}")
    frames = [struct.unpack_from("<I", packet, 4)[0] & 0xFFFFFF
              for packet, _ in received]
    if frames != [0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3]:
        fail(f"multi-Station time-group sequence mismatch: {frames}")
    summary = json.loads([line for line in stdout.splitlines() if line.strip()][-1])
    if summary.get("station_ids") != [700, 701, 702]:
        fail(f"multi-Station summary lost Station identities: {summary}")
    if [item["sent_packets"] for item in summary.get("station_counts", [])] != [4, 4, 4]:
        fail(f"multi-Station per-Station closure mismatch: {summary}")


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: fpga_sender_sim_loopback_test.py EXECUTABLE V1_CONFIG V2_CONFIG",
            file=sys.stderr,
        )
        return 2

    executable = Path(sys.argv[1])
    run_v1(executable, Path(sys.argv[2]))
    run_v2(executable, Path(sys.argv[3]))
    run_v3(executable, Path(sys.argv[3]))
    print("fpga_sender_sim_loopback_test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
