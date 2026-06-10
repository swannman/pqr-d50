# pqr-d50

A Linux / Raspberry-Pi driver for **Powertronics PQR-series** power-line
monitors (PQR **D50** and relatives D52 / D200 / PST / C90).

The vendor's only software, `PQRHost.exe`, is Windows-only. This is a clean
reimplementation of its serial protocol in pure Python, reverse-engineered from
that binary so you can pull data off the device from a Pi (or any Linux box)
over a USB-serial adapter. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the
full protocol analysis and confidence levels.

## Status

The serial link, command set, and framing are **confirmed from the binary** and
should work as-is. The exact record layout of the streamed reports could not be
finalised without the physical device, so report parsing is best-effort and the
raw bytes are always preserved. When your D50 arrives, run `capture.py` to record
real exchanges and tighten the parsers — see [Validating](#validating-with-the-real-device).

## Hardware

* PQR D50 with its serial port (DB9 / terminal — RS-232 levels).
* A **USB-to-RS-232 serial adapter** on the Pi (e.g. FTDI), appearing as
  `/dev/ttyUSB0`. (A TTL-only USB-serial cable will **not** work against true
  RS-232 levels — use an RS-232 adapter or a MAX3232 level shifter.)

## Install

```bash
pip install pyserial
git clone <this-repo> && cd pqr-d50
pip install -e .            # optional, installs the `pqr_d50` package
```

## Usage

```python
from pqr_d50 import PQRClient

with PQRClient("/dev/ttyUSB0", baudrate=9600) as dev:
    info = dev.connect()                 # C1 — identify
    print("model:", info.model, "vendor:", info.vendor)

    dev.detail_report().save("events.drp")   # C3
    dev.summary_report().save("counts.srp")  # C2
    dev.data_log().save("voltage.dlg")       # C4

    # live readings (~1/s, auto-stops ~2 min; Ctrl-C to stop early)
    for line in dev.calibration_stream(duration_s=10):   # C5
        print(line)
```

Don't know the baud rate? Let it sweep:

```python
dev = PQRClient("/dev/ttyUSB0")
print("locked at", dev.autobaud(), "baud")
```

## Validating with the real device

```bash
# auto-detect baud and capture every report to ./captures/ as raw + hexdump
python3 capture.py --port /dev/ttyUSB0 --autobaud --all

# 30-second passive sniff (see what the unit emits unprompted)
python3 capture.py --port /dev/ttyUSB0 --baud 9600 --sniff 30
```

Send the resulting `captures/*.drp/.srp/.dlg` files back into
`pqr_d50/parsers.py` to lock down the field encodings.

## Protocol at a glance

| Command | Function                          |
|--------:|-----------------------------------|
| `C1`    | connect / identify                |
| `C2`    | summary report (event counts)     |
| `C3`    | detail report (event listing)     |
| `C4`    | data log (voltage time-history)   |
| `C5`    | calibration / real-time readings  |
| `C6`    | settings / programming            |

Link: **8N1**, baud ∈ {1200, 2400, 4800, 9600, 14400, 19200}.
`ESC` aborts a transfer; `CR` terminates programming parameters.

## License

MIT — see [LICENSE](LICENSE). Powertronics and PQR are trademarks of their
respective owner; this project is an independent, interoperability-only
reimplementation and ships no vendor code.
