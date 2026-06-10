#!/usr/bin/env python3
"""Minimal end-to-end example: identify the unit and pull a detail report."""

import sys

from pqr_d50 import PQRClient
from pqr_d50.parsers import parse_detail_report, looks_binary


def main(port: str = "/dev/ttyUSB0") -> int:
    with PQRClient(port, baudrate=9600) as dev:
        info = dev.connect()
        print(f"connected: model={info.model!r} vendor={info.vendor!r} "
              f"raw={info.raw!r}")

        xfer = dev.detail_report()
        print(f"detail report: {len(xfer.raw)} bytes "
              f"({xfer.blocks} reads, {xfer.elapsed_s:.1f}s)")
        xfer.save("events.drp")

        if looks_binary(xfer.raw):
            print("payload looks binary — capture it with capture.py to map the "
                  "record layout before parsing.")
        else:
            for ev in parse_detail_report(xfer.raw)[:20]:
                print(f"  {ev.date} {ev.time}  ph{ev.phase}  "
                      f"{ev.event_type}  {ev.magnitude}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(*sys.argv[1:]))
