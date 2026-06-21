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
  bus-powered). Meter the native port's VBUS while powered from #1 — if ~5 V,
  you're done; if not, inject 5 V via the OTG adapter, or set `USB_VBUS_EN_GPIO`
  in `config.h` if your board has a VBUS load-switch.

## Build & flash (ESP-IDF ≥ 5.0)

```bash
. $IDF_PATH/export.sh
cd esp32/pqr-d50-bridge
# edit main/config.h: WiFi creds + Grafana Cloud Influx URL/user/token
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # flash via the UART port
```

`idf.py` pulls the managed USB-host components (`usb_host_vcp`,
`usb_host_ftdi`, `usb_host_cdc_acm`) automatically.

## How it works

Each `D50_POLL_SEC` the firmware:
1. `d50_reset()` — spams CR + ESC + verifies `C1` (resync to command mode).
2. Sends `C4`, accumulates the ASCII data log until the line is idle.
3. Parses rows (`d50_parse.c`) → `date,time,Hot,v,Neu,v`.
4. Dedupes by sample epoch, formats Influx line protocol (`influx.c`):
   `pqr_d50,unit=086101 Hot=121.1,Neu=0.0 <ns>`
5. POSTs the batch to Grafana Cloud (HTTP Basic: instance-id : token).

Set the D50's log sample rate (e.g. 60 s, or 1 s for near-real-time) once from
the Python tool, or extend the firmware to use the `C6` menu.

## Tested vs. needs-hardware

* `d50_parse.c`, `influx.c` — **host-tested** against real captured `.dlg`
  (see `test/host_test.c`; `cc -I../main host_test.c ../main/*.c -o t && ./t`).
* `main.cpp` USB-host + WiFi + HTTP — needs on-device bring-up (no IDF in CI).
  The USB VCP setup follows Espressif's `cdc_acm_vcp` example.

## Fallback

If USB-host bring-up is fussy, a Raspberry Pi Zero 2 W hosts the D50's FT232
natively (Linux `ftdi_sio`) and runs the parent `pqr_d50` Python package +
a tiny Influx push loop with zero hardware uncertainty.
