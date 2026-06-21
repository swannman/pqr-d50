#!/usr/bin/env python3
"""
capture.py - record raw PQR device exchanges (diagnostics / new-firmware checks).

The protocol is already validated against a PQR D50 V20.55; this tool is handy
for capturing raw bytes from a different unit/firmware, or for debugging.

Examples
--------
    python3 capture.py --port /dev/ttyUSB0 --baud 19200 --identify
    python3 capture.py --port /dev/ttyUSB0 --autobaud --reports
    python3 capture.py --port /dev/ttyUSB0 --baud 19200 --raw C4
    python3 capture.py --port /dev/ttyUSB0 --baud 19200 --sniff 30
"""

import argparse
import os
import sys
import time

from pqr_d50 import PQRClient, protocol as P


def hexdump(data: bytes, width: int = 16) -> str:
    out = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        ascii_ = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        out.append(f"{off:08x}  {hexs:<{width*3}}  {ascii_}")
    return "\n".join(out)


def save(outdir: str, name: str, data: bytes) -> str:
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, name)
    with open(path, "wb") as fh:
        fh.write(data)
    return path


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=P.DEFAULT_BAUD_RATE)
    ap.add_argument("--autobaud", action="store_true")
    ap.add_argument("--outdir", default="captures")
    ap.add_argument("--identify", action="store_true")
    ap.add_argument("--reports", action="store_true",
                    help="dump raw C2/C3/C4 report bytes")
    ap.add_argument("--raw", metavar="CMD",
                    help="send an arbitrary command (e.g. C4) and dump the reply")
    ap.add_argument("--sniff", type=float, default=0, metavar="SECONDS")
    args = ap.parse_args()

    dev = PQRClient(args.port, baudrate=args.baud)
    dev.reset()
    if args.autobaud:
        rate = dev.autobaud()
        if rate is None:
            print("!! autobaud failed", file=sys.stderr)
            return 2
        print(f"== locked at {rate} baud")
    stamp = time.strftime("%Y%m%d-%H%M%S")

    if args.sniff:
        print(f"== sniffing {args.sniff:g}s ...")
        t0 = time.monotonic()
        buf = bytearray()
        while time.monotonic() - t0 < args.sniff:
            buf += dev.ser.read(dev.ser.in_waiting or 1)
        save(args.outdir, f"sniff-{stamp}.bin", bytes(buf))
        print(hexdump(bytes(buf)))

    if args.identify:
        info = dev.identify()
        print(f"\n== identify: {info.model} fw{info.firmware} id={info.unit_id}")
        print(hexdump(info.raw))

    if args.reports:
        for cmd, ext in [(P.Command.SUMMARY_REPORT, P.SUMMARY_REPORT_EXT),
                         (P.Command.DETAIL_REPORT, P.DETAIL_REPORT_EXT),
                         (P.Command.DATA_LOG, P.DATA_LOG_EXT)]:
            raw = dev.raw(cmd.value, idle=2.0)
            p = save(args.outdir, f"{cmd.name}-{stamp}{ext}", raw)
            print(f"\n== {cmd.name} ({len(raw)} bytes) -> {p}")
            print(hexdump(raw[:512]))

    if args.raw:
        raw = dev.raw(args.raw.encode(), idle=2.0)
        p = save(args.outdir, f"raw-{args.raw}-{stamp}.bin", raw)
        print(f"\n== raw {args.raw!r} ({len(raw)} bytes) -> {p}")
        print(hexdump(raw))

    dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
