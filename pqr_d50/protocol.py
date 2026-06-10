"""
Wire-protocol constants for the Powertronics PQR-series power-line monitors
(PQR D50 / D52 / D200 / PST / C90 ...).

Everything in this module was recovered by static reverse-engineering of the
Windows host program ``PQRHost.exe`` v2.1.5 (a native-compiled Visual Basic 6
application that drives the device over an RS-232 serial port via the
MSComm32 OCX).  See ``docs/PROTOCOL.md`` for the full analysis and the
confidence level of each item.

Confidence legend used in comments:
    [CONFIRMED] - extracted directly from the binary, high confidence
    [LIKELY]    - strongly implied by the binary + help file, not yet seen on the wire
    [VALIDATE]  - placeholder / hypothesis; confirm against a real device with capture.py
"""

from enum import Enum

# --- Serial line settings -------------------------------------------------
# [CONFIRMED] The host builds its MSComm "Settings" string as
#   "<baud>,N,8,1"  (the literal ",N,8,1" lives at .text va 0x40d8f0).
# => No parity, 8 data bits, 1 stop bit.
BYTESIZE = 8
PARITY = "N"          # none
STOPBITS = 1

# [CONFIRMED] The "Set Baud Rate" dialog (PQRHost.hlp) offers exactly these six.
SUPPORTED_BAUD_RATES = (1200, 2400, 4800, 9600, 14400, 19200)

# [LIKELY] 9600 is the conventional default for this generation of meters and
# is the value the host preselects.  If a fresh unit does not answer at 9600,
# sweep SUPPORTED_BAUD_RATES (see PQRClient.autobaud()).
DEFAULT_BAUD_RATE = 9600


# --- Framing bytes --------------------------------------------------------
# [CONFIRMED] Chr(27)/ESC is sent on its own to abort an in-progress transfer
# (it is built next to the "Cancelling download..." string).
ESC = b"\x1b"

# [CONFIRMED] Chr(13)/CR is used as the terminator when the host streams
# parameter values to the device (set-time / set-threshold / set-baud paths).
CR = b"\x0d"


# --- Command set ----------------------------------------------------------
# [CONFIRMED] The host writes two-character ASCII commands "C1".."C6" to
# MSComm.Output.  Each was located at a distinct .text string constant and
# cross-referenced to its handler:
#
#   C1  -> connect / identify   (handler also references "PowerTronics","Omega")
#   C2  -> Summary Report dump   (handler writes a ".srp" file)
#   C3  -> Detail Report dump    (handler writes a ".drp" file)
#   C4  -> Data Log dump         (handler writes a ".dlg" file)
#   C5  -> Calibration / real-time readings stream
#   C6  -> settings/programming  (used by every Set-* handler, which then
#                                 streams parameter digits terminated by CR)
class Command(bytes, Enum):
    CONNECT       = b"C1"   # [CONFIRMED] handshake / identify the unit
    SUMMARY_REPORT = b"C2"  # [CONFIRMED] download event counts  -> .srp
    DETAIL_REPORT = b"C3"   # [CONFIRMED] download event listing -> .drp
    DATA_LOG      = b"C4"   # [CONFIRMED] download voltage time-history -> .dlg
    CALIBRATION   = b"C5"   # [CONFIRMED] begin real-time readings stream
    PROGRAM       = b"C6"   # [CONFIRMED] enter settings/programming path


# --- Device status / response tokens -------------------------------------
# [CONFIRMED] These ASCII tokens are referenced by the response-parsing code.
# They are the device's short status replies.
class Status(bytes, Enum):
    OK   = b"OK"
    BUSY = b"BUSY"
    DIAL = b"DIAL"     # modem / dial-up linked variants
    NONE = b"NONE"


# [CONFIRMED] Device/model identifiers the host recognises in a C1 reply.
KNOWN_MODELS = ("D50", "D52", "D200", "PST", "C90")
VENDOR_TOKENS = ("PowerTronics", "Omega")


# --- Report file extensions the host writes (for reference / compatibility)
DETAIL_REPORT_EXT = ".drp"
SUMMARY_REPORT_EXT = ".srp"
DATA_LOG_EXT = ".dlg"


# --- Behavioural constants from PQRHost.hlp -------------------------------
# [CONFIRMED] The calibration/readings stream updates ~once per second and the
# device auto-stops after ~2 minutes if not told to stop sooner (send ESC).
CALIBRATION_UPDATE_PERIOD_S = 1.0
CALIBRATION_AUTO_STOP_S = 120
