#!/usr/bin/env python3

import argparse
import socket


def parse_args():
    parser = argparse.ArgumentParser(
        description="Send deterministic UDP records for the RDMA receiver test"
    )
    parser.add_argument("--destination-ip", required=True)
    parser.add_argument("--destination-port", required=True, type=int)
    parser.add_argument("--source-ip", required=True)
    parser.add_argument("--source-port", required=True, type=int)
    parser.add_argument("--record-bytes", required=True, type=int)
    parser.add_argument("--valid-count", required=True, type=int)
    parser.add_argument("--fill", required=True, type=int)
    parser.add_argument("--wrong-after", type=int, default=-1)
    return parser.parse_args()


def main():
    args = parse_args()
    if not 1 <= args.destination_port <= 65535:
        raise SystemExit("destination port must be in [1, 65535]")
    if not 1 <= args.source_port <= 65535:
        raise SystemExit("source port must be in [1, 65535]")
    if not 2 <= args.record_bytes <= 65507:
        raise SystemExit("record bytes must be in [2, 65507]")
    if args.valid_count <= 0:
        raise SystemExit("valid count must be positive")
    if not 0 <= args.fill <= 255:
        raise SystemExit("fill must be in [0, 255]")
    if args.wrong_after < -1 or args.wrong_after > args.valid_count:
        raise SystemExit("wrong-after must be -1 or in [0, valid-count]")

    destination = (args.destination_ip, args.destination_port)
    valid_record = bytes([args.fill]) * args.record_bytes
    wrong_record = bytes([0x7E]) * (args.record_bytes - 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.bind((args.source_ip, args.source_port))
        for index in range(args.valid_count + 1):
            if index == args.wrong_after:
                sender.sendto(wrong_record, destination)
            if index < args.valid_count:
                sender.sendto(valid_record, destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
