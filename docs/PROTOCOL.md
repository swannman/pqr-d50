# Powertronics PQR serial protocol — reverse-engineering notes

Target: **PQR D50** (and relatives D52 / D200 / PST / C90).
Source: static analysis of `PQRHost.exe` v2.1.5 — a native-compiled Visual
Basic 6 application that talks to the device through the **MSComm32** OCX, i.e.
over an **RS-232 serial port**.

> The physical device was **not** available during this analysis, so items are
> tagged with a confidence level. Run `capture.py` against a real unit to
> confirm the `[VALIDATE]` items (chiefly the streamed report record layouts).

## How this was recovered

* `PQRHost.CAB` (InstallShield) → extracted `PQRHost.exe` (512 KB, VB6 native).
* Dependency `MSCOMM32.OCX` in `SETUP.LST` ⇒ the app uses a serial port.
* `PQRHost.hlp` documents the operator-facing commands and behaviour.
* Disassembly (rizin) located the `MSComm.Output` send sites (a vtable call at
  object-offset `0x3a0`) and the ASCII string constants pushed into them.

## Link settings  `[CONFIRMED]`

* Framing: **8 data bits, No parity, 1 stop bit (8N1)**. The host builds its
  MSComm `Settings` string as `"<baud>,N,8,1"` (literal `,N,8,1` at va `0x40d8f0`).
* Baud rates offered by the "Set Baud Rate" dialog:
  **1200, 2400, 4800, 9600, 14400, 19200**. Host and *unit* baud are settable
  separately (`mnuSettingsSetHostBaudRate`, `mnuSettingsSetUnitBaudRate`).
* No hardware-handshake lines are configured by the host beyond the defaults.

## Command set  `[CONFIRMED]`

Commands are **two ASCII characters** written to `MSComm.Output`. Each maps to a
distinct handler (cross-referenced by the report-file extension / UI strings in
the same routine):

| Command | Meaning                          | Notes / evidence                                  |
|--------:|----------------------------------|---------------------------------------------------|
| `C1`    | Connect / identify               | handler references `PowerTronics`, `Omega`        |
| `C2`    | Summary Report (event counts)    | handler writes a `.srp` file                      |
| `C3`    | Detail Report (event listing)    | handler writes a `.drp` file                      |
| `C4`    | Data Log (voltage time-history)  | handler writes a `.dlg` file                      |
| `C5`    | Calibration / real-time readings | streams ~1/s, auto-stops ~120 s                   |
| `C6`    | Settings / programming           | used by every Set-* handler; then streams params  |

No `C0`/`C7+` commands exist in the binary.

### Framing bytes  `[CONFIRMED]`

* **ESC (`0x1B`)** — sent alone to **abort** an in-progress transfer or stop the
  readings stream (built next to the `"Cancelling download..."` string).
* **CR (`0x0D`)** — terminator used when streaming **parameter values** in the
  `C6` programming paths (set time / threshold / baud / transducer).

### Response tokens  `[CONFIRMED]`

The reply-parsing code references the status tokens **`OK`, `BUSY`, `DIAL`,
`NONE`**, and recognises model strings **`D50`, `D52`, `D200`, `PST`, `C90`**.

## Bulk transfers (C2 / C3 / C4)  `[CONFIRMED behaviour, VALIDATE layout]`

The host sends the command, then reads a sequence of **data blocks** until the
line goes idle, displaying a running block counter; the operator can cancel with
ESC. Documented record contents (from the help file):

* **Detail Report** — one row per disturbance: `date, time, phase, event type, magnitude`
* **Summary Report** — one row per group: `phase, event type, magnitude, count`
* **Data Log** — voltage samples over time

The exact byte encoding of these rows (delimited ASCII vs packed binary) needs a
real capture to confirm — that is what `capture.py` is for. `pqr_d50/parsers.py`
currently assumes delimited ASCII and always preserves the raw bytes.

## Calibration / readings (C5)  `[CONFIRMED]`

Sends `C5`; the device transmits current readings about once per second and
auto-stops after ~2 minutes. Send ESC to stop early.

## Settings / programming (C6)  `[VALIDATE]`

`C6` enters the device's programming path; the host then streams parameter
digits terminated by CR. The individual sub-codes (set time, set threshold,
set baud, set transducer ratio, clear data) were not fully recovered statically.
Use `PQRClient.program_raw()` + `capture.py` to map them, then add typed helpers.

Threshold UI hint from the binary: *"Entering 0 will set the unit to test at 5%
and 10% limits. Entering a value between 1 and 999 will set the actual voltage
for the unit to trip at. Leaving a field blank will keep the threshold."*

## Open questions for hardware validation

1. Is each `C1..C5` command CR-terminated on the wire, or sent bare? (parsers
   handle both; `send_command(..., terminate=True)` adds CR.)
2. Exact record layout of `.drp` / `.srp` / `.dlg` block streams.
3. Full `C6` sub-command map and parameter encodings.
4. Whether a `C1` handshake/identify must precede other commands each session.
