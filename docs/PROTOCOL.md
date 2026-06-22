# Powertronics PQR D50 serial protocol

This documents the PQR D50's serial command protocol, determined and validated
byte-for-byte by communicating with a real **PQR D50, firmware V20.55**.

Confidence tags: `[CONFIRMED]` = validated against the device; `[INFERRED]` =
derived but not exercised on this unit; `[MODEL-DEP]` = applies to other models
in the family, not this D50.

## Link settings `[CONFIRMED]`

* **8N1** (8 data bits, no parity, 1 stop bit).
* The D50 V20.55 ships at **19200 baud**. Its own baud menu offers
  **2400, 4800, 9600, 14400, 19200, 115200**.
* No hardware flow control.
* Wiring: true RS-232 levels — use an RS-232 USB adapter (not a 3.3 V TTL cable).
  (Some units expose the serial port over an internal USB-serial bridge.)

## Two interaction modes

### 1. Command mode `[CONFIRMED]`

Two-character ASCII commands. Every other byte is ignored; there is no `C0`/`C7+`.

| Cmd  | Action            | Reply                                                    |
|------|-------------------|---------------------------------------------------------|
| `C1` | Identify          | banner (see below)                                      |
| `C2` | Summary Report    | ASCII, event counts                                     |
| `C3` | Detail Report     | ASCII, one row per event                                |
| `C4` | Data Log          | ASCII, voltage samples at the logging rate              |
| `C5` | **CLEAR ALL DATA**| prompts `Are You Sure ... ?` → send `Y` → erases FLASH  |
| `C6` | Setup Menu        | interactive text menu (below)                           |

Identity banner (`C1`):
```
PowerTronics PQR D50 V20.55  ** PQR SERIES **  Jun/21/26 - ID: 086101
```

`C5` clear transcript:
```
TX: C5
RX: Are You Sure you want to CLEAR ALL DATA on this board ?
TX: Y
RX: Y OK  ... 62 61 60 ... 00   Ram has been cleared !
```

### 2. Setup menu (`C6`) `[CONFIRMED]`

```
 These are your Setup Options:                 Current Settings:
   1 - Set the Date and Time                   Jun/21/26 11:35:33
   2 - Set the Baud Rate                       19,200 Baud
   3 - Select a Data Logging Sample rate       60 Seconds
   4 - Set the Sag, Surge, and Power fail thresholds
  ESC - Exit this Menu
Select an Option :
```

The menu is a **stateful state machine**. Important: sub-prompts **ignore ESC**
(only digits/CR advance them); ESC only exits the *main* menu. To recover to
command mode from anywhere: spam CR (walks any flow to completion) then ESC,
then verify with `C1`. This is exactly what `PQRClient.reset()` does.

* **Option 1 — Date/Time.** Prompt `Input the Date and Time in the format
  MM/DD/YY,HH:MM:SS`. Send `MM/DD/YY,HH:MM:SS<CR>` → replies `Command OK`.
* **Option 2 — Baud.** Picker `1) 115,200  2) 2,400  3) 4,800  4) 9,600
  5) 14,400  6) 19,200`. Send digit+CR; the unit switches **immediately**, so
  the client must reopen at the new rate.
* **Option 3 — Sample rate.** Picker `1) 1 Sec  2) 5 Sec  3) 10 Sec
  4) 30 Sec  5) 1 Min  6) 4 Min`. **Changing it ERASES the data log** (the
  device re-formats its FLASH banks — a `62→00` countdown).
* **Option 4 — Thresholds.** Six sequential prompts:
  `CH1 Surge, CH1 Sag, CH1 Power Failure, CH2 Surge, CH2 Sag, CH2 Power Failure`,
  then `Setup Completed`. Each shows its current value as `(5%,10%)`
  (percentage mode) or `(NV)` (absolute volts). Per prompt:
  blank = keep, `0` = default `(5%,10%)`, `N` = trip at N volts (1..999).

## Report formats `[CONFIRMED]`

All reports start with a banner line and then comma-delimited rows. Line
terminators vary by report (CRLF / LFCR / CRCRLF) — split on any CR/LF run.

Banner: `PowerTronics PQR D50 V20.55   - <Report Type> as of <Date> - ID: <id>`

**Summary (`C2`)** — `channel, event, count`:
```
   Hot,   Power Failure, 1
   Hot,   Power Restore, 1
   Hot,       Sag Start, 1
```

**Detail (`C3`)** — `date, time, channel, event, magnitude,`
(time carries hundredths; magnitude is volts):
```
Jun/10/26, 13:56:40.69, Hot,        Sag Start, 70.1,
Jun/10/26, 13:56:40.71, Hot,   Power Failure, 27.8,
Jun/21/26, 11:27:05.05, Hot,   Power Restore, 121.1,
```

**Data Log (`C4`)** — `date, time, ch1, v1, ch2, v2,`
(D50 logs Hot + Neutral voltages):
```
Jun/10/26, 13:56:34, Hot, 108.9, Neu, 0.6,
Jun/21/26, 11:28:05, Hot, 121.1, Neu, 0.0,
```

Channels: **Hot, Neu(tral), Gnd**. Event types observed / supported: *Sag Start,
Sag Complete, Surge, Impulse, Power Failure, Power Restore, Signal Failure*, plus
*Freq Fault* (line-frequency) and *HF Noise* (high-frequency noise). Note that
line frequency and HF/common-mode noise surface as **events** (logged when they
cross a limit) rather than continuously-sampled values — the periodic Data Log
(`C4`) carries only Hot + Neutral voltage. (Poly-phase models add `PHx ...`
current/voltage variants `[MODEL-DEP]`.)

`Sag Start`/`Sag Complete` are emitted as a pair (the dip's onset and recovery,
magnitudes = the low and recovered RMS volts). `Impulse` is an automatic
sub-cycle transient detector (magnitude = spike volts) — *not* gated by the
configurable sag/surge RMS thresholds, so it fires on fast events (e.g. a laser
printer's fuser switching) even when RMS never leaves the normal band.

## Real-time data

There is **no live-readings command** on the D50 V20.55. (Other models in the
family have a "Calibration Mode"; the D50 does not.) For near-real-time data:
set the sample rate to **1 second** (`C6`→3) and poll the **Data Log** (`C4`).

## Response / status tokens `[CONFIRMED]`

`Command OK`, `OK` (`Y OK`), `Ram has been cleared !`, `Setup Completed`,
`Are You Sure you want to CLEAR ALL DATA on this board ?`.

## Characterising the protocol against a unit

`tools/pqrdev.py` and `capture.py` drive the device and log the exact bytes for
each command — handy for confirming the report layouts or `C6` sub-codes on a
different unit or firmware.
