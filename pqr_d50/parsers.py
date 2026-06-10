"""
Best-effort parsers for the report payloads returned by the PQR device.

IMPORTANT: the field *contents* below are confirmed from PQRHost.hlp, but the
exact byte/record encoding of the streamed reports was not fully recoverable by
static analysis alone (the device was not present). These parsers therefore try
both an ASCII/CSV interpretation and expose the raw bytes so you can finalise
them with a real capture (see capture.py). When you confirm the layout, tighten
``_RECORD_*`` below.

From the help file:
  * Detail Report  (.drp): one row per disturbance =
        date, time, phase, event type, magnitude
  * Summary Report (.srp): one row per (phase, event type, magnitude) =
        phase, event type, magnitude, count
  * Data Log       (.dlg): voltage time-history samples
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List


# Event-type vocabulary seen in the binary (phase sag/swell/drop on V and I).
EVENT_TYPES = (
    "PH1 Sag", "PH2 Sag", "PH3 Sag",
    "PH1 Current Sag", "PH2 Current Sag", "PH3 Current Sag",
    "PH1 Current Swell", "PH2 Current Swell", "PH3 Current Swell",
    "PH1 Current Drop", "PH2 Current Drop", "PH3 Current Drop",
)


@dataclass
class DetailEvent:
    date: str
    time: str
    phase: str
    event_type: str
    magnitude: str
    raw: str = ""


@dataclass
class SummaryRow:
    phase: str
    event_type: str
    magnitude: str
    count: str
    raw: str = ""


@dataclass
class DataLogSample:
    timestamp: str
    value: str
    raw: str = ""


def _ascii_rows(raw: bytes) -> List[str]:
    """Split a payload into printable rows, tolerating CR, LF, or CRLF."""
    text = raw.decode("latin1", "replace")
    rows = re.split(r"[\r\n]+", text)
    return [r.strip() for r in rows if r.strip()]


def parse_detail_report(raw: bytes) -> List[DetailEvent]:
    """[VALIDATE] Assumes delimited ASCII rows of 5 fields."""
    out: List[DetailEvent] = []
    for row in _ascii_rows(raw):
        parts = re.split(r"[,\t;|]+|\s{2,}", row)
        if len(parts) >= 5:
            out.append(DetailEvent(*parts[:5], raw=row))
        else:
            out.append(DetailEvent("", "", "", "", "", raw=row))
    return out


def parse_summary_report(raw: bytes) -> List[SummaryRow]:
    """[VALIDATE] Assumes delimited ASCII rows of 4 fields."""
    out: List[SummaryRow] = []
    for row in _ascii_rows(raw):
        parts = re.split(r"[,\t;|]+|\s{2,}", row)
        if len(parts) >= 4:
            out.append(SummaryRow(*parts[:4], raw=row))
        else:
            out.append(SummaryRow("", "", "", "", raw=row))
    return out


def parse_data_log(raw: bytes) -> List[DataLogSample]:
    """[VALIDATE] Assumes delimited ASCII timestamp,value rows."""
    out: List[DataLogSample] = []
    for row in _ascii_rows(raw):
        parts = re.split(r"[,\t;|]+|\s{2,}", row)
        if len(parts) >= 2:
            out.append(DataLogSample(parts[0], parts[1], raw=row))
        else:
            out.append(DataLogSample("", parts[0] if parts else "", raw=row))
    return out


def looks_binary(raw: bytes) -> bool:
    """Heuristic: is this payload binary (not delimited ASCII)?"""
    if not raw:
        return False
    printable = sum(1 for b in raw if 9 <= b <= 13 or 32 <= b <= 126)
    return printable / len(raw) < 0.85
