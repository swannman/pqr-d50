# pqr-d50-bridge (ESP32-S3)

Reads a PQR D50 over the ESP32-S3's **native USB (host mode)** and pushes
readings to **Grafana Cloud** (InfluxDB line protocol) over WiFi.

The D50 contains an internal FT232 USB-serial chip, so to the ESP32-S3 it is
just an FTDI VCP device — the same protocol validated from Linux/macOS, driven
here over USB host instead of a PC serial port.

## Wiring / power

```
  ┌──────────── ESP32-S3 (S3-N16R8) ────────────┐
  │  USB-C #1 (UART bridge)  ← 5V power in        │
  │  USB-C #2 (native USB)   → USB HOST → D50     │
  │  WiFi → Grafana Cloud (InfluxDB)              │
  └───────────────────────────────────────────────┘
```

* **Power:** USB-C #1 (the CH343/CP2102 port) from any 5 V brick.
* **D50:** USB-C #2 (native) → a **USB-C OTG adapter** → cable to the D50's USB
  port. The OTG adapter puts the port in host (DFP) role.
* **VBUS:** in host mode the S3 must supply 5 V to the D50 (its FT232 is
  bus-powered). Most dual-USB S3 boards don't route 5 V to the native port by
  default, so the D50 won't enumerate (`USB devices on bus: 0`). **Fix:** these
  boards have a **`USB-OTG` solder jumper** that connects 5 V to the host port's
  VBUS — bridge it and the D50 powers directly off the board (verified on this
  hardware). Alternatively use a powered USB hub, or set `USB_VBUS_EN_GPIO` if
  your board has a VBUS load-switch.
  After bridging, don't plug that port into a PC (it now sources 5 V).

## Build & flash

Built and verified with **ESP-IDF v5.3.2** (use Python ≤ 3.12 — IDF doesn't
support 3.13+ yet). The build pulls the managed components automatically:
`espressif/usb_host_vcp` and `espressif/usb_host_ftdi_vcp` (which brings
`usb_host_cdc_acm` 2.x). The VCP layer uses C++ exceptions, so
`CONFIG_COMPILER_CXX_EXCEPTIONS=y` is set in `sdkconfig.defaults`.

```bash
. $IDF_PATH/export.sh
cd esp32/pqr-d50-bridge
cp main/secrets.h.example main/secrets.h   # then fill in WiFi + Grafana creds
# non-secret settings (TZ, poll interval, measurement names) are in main/config.h
idf.py set-target esp32s3
idf.py build
idf.py -p <COM-port> flash monitor
```

Secrets (WiFi SSID/pass, Grafana Cloud Influx URL/user/token) live in
`main/secrets.h`, which is **gitignored** — never commit it. The Grafana Cloud
Influx endpoint + numeric user/instance ID + a `metrics:write` Cloud Access
Policy token come from your stack's "InfluxDB → Send Metrics" page.

Set `PQR_USB_SELFTEST 1` in `config.h` for a USB-only bring-up test (logs the
D50 over COM, skips WiFi/Grafana); set it back to `0` for normal operation.

**Flash via the "COM" port, not "USB".** On the S3, the native "USB" port
(USB-Serial-JTAG) shares GPIO19/20 with the USB-OTG controller — once the
firmware enables USB host, a console on that port drops. So flash/monitor over
the **"COM"** (UART-bridge) port and leave **"USB"** for the D50. You can keep
both plugged in: COM = power + console, USB = D50 (via OTG adapter).

## How it works

On boot: connect WiFi → SNTP time → sync the D50's RTC (`C6`→1) so its
timestamps match your stack → **prime watermarks** from the current logs (record
the latest sample/event times and push nothing, so we don't backfill old data —
Grafana Cloud/Mimir rejects samples older than ~1 h).

Then every `D50_POLL_SEC`:
1. `d50_reset()` — CR-spam + ESC + verify `C1` (resync to command mode).
2. **Data log (`C4`)** → voltage **metrics** to Grafana Cloud (Influx line
   protocol → Prometheus): `pqr_d50_hot`, `pqr_d50_neu`. Watermark-deduped.
3. **Detail report (`C3`)** → disturbances to Grafana Cloud **Loki** as one
   structured line per event:
   `type=impulse channel=Hot magnitude=32.0 plot_v=154.8` (Loki de-dupes
   identical entries, so the whole report is re-sent each poll — reboot-proof,
   no watermark). `plot_v` is the value to draw on the voltage axis: impulse =
   live volts + spike (peak); sag = the dip volts (start+complete share the dip
   level so they form a line across the sag).
4. Re-sync the RTC every `CLOCK_SYNC_HOURS`.

Why two backends: voltage is a regularly-sampled series (a natural metric),
while disturbances are discrete events with magnitudes — Loki stores those
faithfully (no series collisions) and LogQL derives metrics from them
(`max_over_time(... | unwrap plot_v)`), which is how the dashboard overlays
impulse points and the sag line on the voltage chart.

Set the D50's log sample rate (60 s default, or 1 s for near-real-time) once with
the Python tool (`set_sample_rate`), or extend the firmware via `C6`→3.

## Grafana dashboard

Import `grafana/pqr-d50-dashboard.json` (needs a Prometheus *and* a Loki
datasource). The voltage panel overlays three series on one axis: the **line
voltage**, **impulse peaks** as points (live volts + spike height), and **sag
dips** as a line (spanning start→complete). Plus neutral-to-ground voltage with
its bands, latest-voltage + power-failure stats, a recent-disturbances log, and
disturbance **annotations**.

## Data model (Grafana Cloud)

Metrics (Prometheus / Influx push):

| Metric | Labels | Meaning |
|---|---|---|
| `pqr_d50_hot` | `unit` | line voltage (V) |
| `pqr_d50_neu` | `unit` | neutral-to-ground voltage (V) |

Events (Loki, stream `{service_name="pqr_d50", unit=...}`), one logfmt line each:

```
type=<impulse|sag_start|sag_complete|surge|power_failure|...> channel=Hot magnitude=<V> plot_v=<V>
```

Derive metrics with LogQL, e.g. impulse peaks:
`max by (unit) (max_over_time({service_name="pqr_d50"} | logfmt | type="impulse" | unwrap plot_v [$__interval]))`

## Tested vs. needs-hardware

* `d50_parse.c`, `influx.c` — **host-tested** against real captured `.dlg`
  (see `test/host_test.c`; `cc -I../main host_test.c ../main/*.c -o t && ./t`).
* `main.cpp` USB-host + WiFi + HTTP — needs on-device bring-up (no IDF in CI).
  The USB VCP setup follows Espressif's `cdc_acm_vcp` example.

## Fallback

If USB-host bring-up is fussy, a Raspberry Pi Zero 2 W hosts the D50's FT232
natively (Linux `ftdi_sio`) and runs the parent `pqr_d50` Python package +
a tiny Influx push loop with zero hardware uncertainty.
