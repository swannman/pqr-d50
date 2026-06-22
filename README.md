# pqr-d50

A Linux / Raspberry-Pi driver for **Powertronics PQR-series** power-line
monitors — an independent, pure-Python implementation of the device's serial
protocol, **validated live, byte-for-byte, against a PQR D50 (firmware V20.55).**

Full protocol reference: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Hardware

* PQR D50 (relatives D52/D200/PST/C90 share the protocol; field shapes may vary).
* A **USB-to-RS-232** adapter (true RS-232 levels, e.g. FTDI) on the Pi →
  `/dev/ttyUSB0` (`/dev/cu.usbserial-*` on macOS). A 3.3 V TTL cable will *not*
  work.

## Install

```bash
pip install pyserial
git clone <this-repo> && cd pqr-d50 && pip install -e .
```

## Usage

```python
from pqr_d50 import PQRClient

with PQRClient("/dev/ttyUSB0", baudrate=19200) as dev:
    dev.reset()                         # sync to a known state
    info = dev.identify()               # C1
    print(info.model, info.firmware, info.unit_id)

    for ev in dev.detail_report().rows:        # C3
        print(ev.date, ev.time, ev.channel, ev.event_type, ev.magnitude)

    for s in dev.data_log().rows:              # C4 (Hot + Neutral volts)
        print(s.date, s.time, s.ch1_value, s.ch2_value)

    print(dev.summary_report().rows)           # C2 (event counts)
```

Settings (the `C6` menu):

```python
from datetime import datetime

dev.get_settings()                 # -> Settings(datetime, baud, sample_rate_s)
dev.set_datetime(datetime.now())   # sync the clock
dev.set_sample_rate(1)             # 1s logging  *** erases the data log ***
dev.set_thresholds({("CH1","Sag"): 100, ("CH1","Surge"): 0})  # volts / 0=default
dev.set_baud(9600)                 # switches the unit AND reopens the port
```

Destructive — guarded:

```python
dev.clear_data(confirm=True)       # C5: erases all events + data log
```

Unknown baud? `dev.autobaud()` sweeps the supported rates.

### Near-real-time

The D50 has no live-readings command. Set the sample rate to 1 second and poll
the data log:

```python
dev.set_sample_rate(1)
while True:
    for s in dev.data_log().rows[-5:]:
        print(s.time, s.ch1_value, s.ch2_value)
    time.sleep(5)
```

## Protocol at a glance

| Cmd | Function                         |
|-----|----------------------------------|
| C1  | identify                         |
| C2  | summary report (event counts)    |
| C3  | detail report (event listing)    |
| C4  | data log (Hot/Neutral voltages)  |
| C5  | **clear all data** (destructive) |
| C6  | setup menu (date/baud/rate/thresholds) |

Link: **8N1**, baud ∈ {2400, 4800, 9600, 14400, 19200, 115200}, default 19200.

## Tools

* `capture.py` — dump raw exchanges (diagnostics / other firmware).
* `tools/pqrdev.py` — low-level live-exploration helper used during RE.

## ESP32-S3 bridge

[`esp32/pqr-d50-bridge/`](esp32/pqr-d50-bridge/) is standalone firmware that
reads the D50 over the ESP32-S3's native USB (host mode — the D50 has an
internal FT232) and pushes readings to Grafana Cloud (InfluxDB) over WiFi. The
parsing / line-protocol cores are host-tested; the USB-host + WiFi paths need
on-device bring-up. See its README for wiring and flashing.

## License

MIT — see [LICENSE](LICENSE). Powertronics and PQR are trademarks of their
owner; this is an independent, interoperability-focused implementation that
ships no third-party code.
