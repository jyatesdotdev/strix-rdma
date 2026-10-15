#!/usr/bin/env python3
"""Record netconsole output from the lab hosts.

Every line of every UDP datagram is appended to a log with its receive time and
the sender's address, so both hosts can share one listener, and a reboot that
hangs after journald has stopped still leaves a record. Arm the hosts with
tools/scripts/arm-shutdown-capture.sh.

Usage: netconsole-listen.py [PORT] [LOG_PATH]   (defaults: 6666, ./netconsole.log)
"""
import datetime
import socket
import sys

DEFAULT_PORT = 6666
DEFAULT_LOG_PATH = "netconsole.log"
MAX_DATAGRAM = 65536


def format_datagram(payload: bytes, sender: str, received: datetime.datetime) -> str:
    stamp = received.strftime("%H:%M:%S.%f")[:-3]
    lines = payload.decode("utf-8", "replace").splitlines()
    return "".join(f"{stamp} {sender} {line}\n" for line in lines)


def listen(port: int, log_path: str) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, \
            open(log_path, "a", buffering=1) as log:
        sock.bind(("0.0.0.0", port))
        log.write(f"--- listening on udp/{port} at {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
        while True:
            payload, (sender, _sender_port) = sock.recvfrom(MAX_DATAGRAM)
            log.write(format_datagram(payload, sender, datetime.datetime.now()))


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    log_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_LOG_PATH
    try:
        listen(port, log_path)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
