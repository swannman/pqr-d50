"""
Wire-protocol constants for the Powertronics PQR-series power-line monitors,
validated live against a **PQR D50, firmware V20.55**.

Determined and confirmed byte-for-byte by communicating with real hardware.
See ``docs/PROTOCOL.md`` for the full protocol reference and transcripts.

Confidence legend:
    [CONFIRMED]  - seen on the wire against a real PQR D50 V20.55
    [INFERRED]   - derived but not exercised live on this unit
    [MODEL-DEP]  - applies to other models in the family, not the D50 V20.55
"""

from enum import Enum

# --- Serial line settings  [CONFIRMED] -----------------------------------
# 8 data bits, no parity, 1 stop bit (8N1).
BYTESIZE = 8
PARITY = "N"
STOPBITS = 1

# [CONFIRMED] The D50 V20.55 came up at 19200. Its own baud menu (C6 option 2)
# offers these six (this firmware supports 115200):
SUPPORTED_BAUD_RATES = (2400, 4800, 9600, 14400, 19200, 115200)
DEFAULT_BAUD_RATE = 19200


# --- Framing bytes  [CONFIRMED] ------------------------------------------
ESC = b"\x1b"   # exits the *main* setup menu; IGNORED inside sub-prompts
CR = b"\x0d"    # terminates command-mode commands and menu field entries


# --- Command set  [CONFIRMED] --------------------------------------------
# Two-character ASCII commands written while the device is in "command mode".
# Each was confirmed against the real unit:
#
#   C1 -> identity banner
#   C2 -> Summary Report  (event counts)     ASCII (.srp)
#   C3 -> Detail Report   (event listing)    ASCII (.drp)
#   C4 -> Data Log        (voltage history)  ASCII (.dlg)
#   C5 -> CLEAR ALL DATA  (destructive!)     prompts "Are You Sure ... ?"  -> "Y"
#   C6 -> Setup Menu      (interactive)      options 1..4, ESC to exit
#
# There is no C0/C7+; every other byte is ignored in command mode.
class Command(bytes, Enum):
    IDENTIFY       = b"C1"   # identity / connection check
    SUMMARY_REPORT = b"C2"   # event-count summary
    DETAIL_REPORT  = b"C3"   # full event listing
    DATA_LOG       = b"C4"   # voltage time-history at the logging sample rate
    CLEAR_DATA     = b"C5"   # *** ERASES all events + data log *** (confirm "Y")
    SETUP_MENU     = b"C6"   # interactive settings menu


# --- Setup menu (C6) option codes  [CONFIRMED] ---------------------------
class SetupOption(bytes, Enum):
    DATE_TIME    = b"1"   # prompt: MM/DD/YY,HH:MM:SS  -> "Command OK"
    BAUD_RATE    = b"2"   # pick 1..6 from the baud list below
    SAMPLE_RATE  = b"3"   # pick 1..6 from the sample-rate list (ERASES log!)
    THRESHOLDS   = b"4"   # 6 sequential prompts CH1/CH2 x {Surge,Sag,PowerFail}


# [CONFIRMED] C6 option 2 baud picker: digit -> rate.
BAUD_MENU = {
    b"1": 115200, b"2": 2400, b"3": 4800,
    b"4": 9600,   b"5": 14400, b"6": 19200,
}
BAUD_MENU_INV = {v: k for k, v in BAUD_MENU.items()}

# [CONFIRMED] C6 option 3 data-log sample-rate picker: digit -> seconds.
# NOTE: changing the sample rate erases the FLASH data-log banks.
SAMPLE_RATE_MENU = {
    b"1": 1, b"2": 5, b"3": 10,
    b"4": 30, b"5": 60, b"6": 240,
}
SAMPLE_RATE_MENU_INV = {v: k for k, v in SAMPLE_RATE_MENU.items()}

# [CONFIRMED] Threshold entry order (option 4) and value semantics:
#   blank -> keep current ; "0" -> default (5%,10%) ; "N" -> trip at N volts.
THRESHOLD_SEQUENCE = (
    ("CH1", "Surge"), ("CH1", "Sag"), ("CH1", "PowerFail"),
    ("CH2", "Surge"), ("CH2", "Sag"), ("CH2", "PowerFail"),
)


# --- Response / status tokens  [CONFIRMED] -------------------------------
class Status(bytes, Enum):
    COMMAND_OK    = b"Command OK"          # date/time set acknowledged
    OK            = b"OK"                  # generic ack (e.g. "Y OK" on clear)
    RAM_CLEARED   = b"Ram has been cleared !"
    SETUP_DONE    = b"Setup Completed"
    ARE_YOU_SURE  = b"Are You Sure you want to CLEAR ALL DATA on this board ?"


# [CONFIRMED] Device/model + vendor tokens in the C1 banner.
KNOWN_MODELS = ("D50", "D52", "D200", "PST", "C90", "PDL", "PQR1010")
VENDOR_TOKENS = ("PowerTronics", "Omega")

# [CONFIRMED] Channel labels the D50 reports (single-phase Hot/Neutral; Ground
# and Phase 1-3 exist for poly-phase models).  [MODEL-DEP] for PH1-3.
CHANNELS = ("Hot", "Neu", "Gnd")
POLYPHASE_CHANNELS = ("Phase 1", "Phase 2", "Phase 3")

# [CONFIRMED] Event types seen in the device's reports.
# "Sag Start"/"Sag Complete" are emitted as a pair (dip begins / recovers).
# "Impulse" is an automatic sub-cycle transient detection (magnitude = spike V),
# independent of the configurable sag/surge RMS thresholds.
EVENT_TYPES = (
    "Sag Start", "Sag Complete", "Surge", "Impulse",
    "Power Failure", "Power Restore", "Signal Failure",
    # [MODEL-DEP] poly-phase current/voltage variants:
    "Current Sag", "Current Swell", "Current Drop", "Power Fail",
)


# --- Conventional report file extensions ----------------------------------
DETAIL_REPORT_EXT = ".drp"
SUMMARY_REPORT_EXT = ".srp"
DATA_LOG_EXT = ".dlg"


# --- Notes ----------------------------------------------------------------
# [MODEL-DEP] Other models in the family have a "Calibration Mode" that streams
# real-time readings; the D50 V20.55 does not (no live-readings command exists).
# For near-real-time data on the D50, set the sample rate to 1 second (C6 opt 3)
# and poll the Data Log (C4).
