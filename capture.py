#!/usr/bin/env python3
"""
capture.py - record real PQR device exchanges to finalise the protocol.

Run this once the PQR D50 is connected (e.g. via a USB-serial adapter). It
sends each known command, saves the exact raw reply to disk, and prints a hex +
ASCII dump so the streamed report record layout can be nailed down and the
parsers in pqr_d50/parsers.py tightened.

Examples
--------
    # auto-detect baud, probe every command, write captures to ./captures/
    python3 capture.py --port /dev/ttyUSB0 --autobaud --all

    # just the identity handshake at a fixed rate
    python3 capture.py --port /dev/ttyUSB0 --baud 9600 --connect

    # passive sniff: dump whatever the device sends unprompted
    python3 capture.py --port /dev/ttyUSB0 --baud 9600 --sniff 30
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
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial device, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=P.DEFAULT_BAUD_RATE)
    ap.add_argument("--autobaud", action="store_true",
                    help="sweep supported baud rates until the unit answers")
    ap.add_argument("--outdir", default="captures")
    ap.add_argument("--connect", action="store_true", help="probe C1 (identity)")
    ap.add_argument("--summary", action="store_true", help="probe C2 (.srp)")
    ap.add_argument("--detail", action="store_true", help="probe C3 (.drp)")
    ap.add_argument("--datalog", action="store_true", help="probe C4 (.dlg)")
    ap.add_argument("--calib", type=float, metavar="SECONDS", default=0,
                    help="run C5 readings stream for N seconds")
    ap.add_argument("--all", action="store_true", help="probe C1..C4")
    ap.add_argument("--sniff", type=float, metavar="SECONDS", default=0,
                    help="passively read for N seconds, no command sent")
    args = ap.parse_args()

    dev = PQRClient(args.port, baudrate=args.baud, open_now=True)

    if args.autobaud:
        rate = dev.autobaud()
        if rate is None:
            print("!! autobaud failed - device did not answer at any rate",
                  file=sys.stderr)
            return 2
        print(f"== locked at {rate} baud")

    stamp = time.strftime("%Y%m%d-%H%M%S")

    if args.sniff:
        print(f"== sniffing for {args.sniff:g}s ...")
        t0 = time.monotonic()
        buf = bytearray()
        while time.monotonic() - t0 < args.sniff:
            buf += dev.ser.read(dev.ser.in_waiting or 1)
        p = save(args.outdir, f"sniff-{stamp}.bin", bytes(buf))
        print(hexdump(bytes(buf)))
        print(f"-> {p} ({len(buf)} bytes)")

    if args.connect or args.all:
        info = dev.connect()
        p = save(args.outdir, f"C1-connect-{stamp}.bin", info.raw)
        print(f"\n== C1 connect -> model={info.model} vendor={info.vendor}")
        print(hexdump(info.raw))
        print(f"-> {p}")

    probes = []
    if args.summary or args.all:
        probes.append(("C2-summary", dev.summary_report, P.SUMMARY_REPORT_EXT))
    if args.detail or args.all:
        probes.append(("C3-detail", dev.detail_report, P.DETAIL_REPORT_EXT))
    if args.datalog or args.all:
        probes.append(("C4-datalog", dev.data_log, P.DATA_LOG_EXT))

    for name, fn, ext in probes:
        print(f"\n== {name} ...")
        xfer = fn()
        p = save(args.outdir, f"{name}-{stamp}{ext}", xfer.raw)
        print(f"   {len(xfer.raw)} bytes in {xfer.blocks} reads, "
              f"{xfer.elapsed_s:.1f}s, aborted={xfer.aborted}")
        print(hexdump(xfer.raw[:512]))
        if len(xfer.raw) > 512:
            print(f"   ... ({len(xfer.raw) - 512} more bytes)")
        print(f"-> {p}")

    if args.calib:
        print(f"\n== C5 calibration stream for {args.calib:g}s ...")
        path = save(args.outdir, f"C5-calib-{stamp}.bin", b"")
        with open(path, "wb") as fh:
            for line in dev.calibration_stream(duration_s=args.calib):
                fh.write(line)
                sys.stdout.write(line.decode("latin1", "replace"))
                sys.stdout.flush()
        print(f"\n-> {path}")

    dev.close()
    print("\n== done. Attach the captures/ files when refining parsers.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
