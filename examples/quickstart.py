#!/usr/bin/env python3
"""Minimal end-to-end example against a PQR D50."""

import sys
from pqr_d50 import PQRClient


def main(port: str = "/dev/ttyUSB0") -> int:
    with PQRClient(port, baudrate=19200) as dev:
        dev.reset()

        info = dev.identify()
        print(f"connected: {info.model} fw{info.firmware} "
              f"(id {info.unit_id}, clock {info.date})")

        s = dev.get_settings()
        print(f"settings: clock={s.datetime}  baud={s.baud}  "
              f"sample_rate={s.sample_rate_s}s")

        print("\n-- recent events (detail report) --")
        for ev in dev.detail_report().rows[:20]:
            print(f"  {ev.date} {ev.time}  {ev.channel:>3}  "
                  f"{ev.event_type:<14} {ev.magnitude:>6}")

        print("\n-- data log (voltage history) --")
        for s in dev.data_log().rows[:10]:
            print(f"  {s.date} {s.time}  {s.ch1_name} {s.ch1_value:>6}  "
                  f"{s.ch2_name} {s.ch2_value:>5}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(*sys.argv[1:]))
