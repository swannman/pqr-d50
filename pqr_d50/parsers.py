"""
Parsers for the PQR D50 ASCII reports, written against real captured output.

All three reports share a banner line:
    PowerTronics PQR D50 V20.55   - <Report Type> as of <Date> - ID: <id>

Then comma-delimited rows (line terminators vary between report types: the
device uses CRLF, LFCR, and CRCRLF in different places, so we split on any run
of CR/LF).  Confirmed row shapes (PQR D50 V20.55):

  Summary  (C2 / .srp):  "<channel>, <event>, <count>"
        e.g.  "   Hot,   Power Failure, 1"
  Detail   (C3 / .drp):  "<date>, <time>, <channel>, <event>, <magnitude>,"
        e.g.  "Jun/10/26, 13:56:40.69, Hot,        Sag Start, 70.1,"
  Data Log (C4 / .dlg):  "<date>, <time>, <ch1>, <v1>, <ch2>, <v2>,"
        e.g.  "Jun/21/26, 11:28:05, Hot, 121.1, Neu, 0.0,"
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import List, Optional


# --- banner ---------------------------------------------------------------
_BANNER_RE = re.compile(
    r"PowerTronics\s+PQR\s+(?P<model>\S+)\s+V(?P<fw>[\d.]+)\s+"
    r".*?(?:as of|\*\*.*?\*\*)\s+(?P<date>\S+)\s*-\s*ID:\s*(?P<id>\d+)"
)


@dataclass
class ReportHeader:
    model: Optional[str] = None
    firmware: Optional[str] = None
    date: Optional[str] = None
    unit_id: Optional[str] = None
    report_type: Optional[str] = None
    raw: str = ""


def parse_header(raw: bytes) -> ReportHeader:
    text = raw.decode("latin1", "replace")
    m = _BANNER_RE.search(text)
    h = ReportHeader(raw=text.splitlines()[0] if text.strip() else "")
    if m:
        h.model = m.group("model")
        h.firmware = m.group("fw")
        h.date = m.group("date")
        h.unit_id = m.group("id")
    rt = re.search(r"-\s*([A-Za-z ]+?)\s+as of", text)
    if rt:
        h.report_type = rt.group(1).strip()
    return h


# --- records --------------------------------------------------------------
@dataclass
class SummaryRow:
    channel: str
    event_type: str
    count: int
    raw: str = ""


@dataclass
class DetailEvent:
    date: str
    time: str
    channel: str
    event_type: str
    magnitude: float
    raw: str = ""


@dataclass
class DataLogSample:
    date: str
    time: str
    ch1_name: str
    ch1_value: float
    ch2_name: str
    ch2_value: float
    raw: str = ""


@dataclass
class Report:
    header: ReportHeader
    rows: list = field(default_factory=list)
    raw: bytes = b""


def _rows(raw: bytes) -> List[str]:
    """Split into trimmed, non-empty rows, tolerating CR/LF/CRLF/LFCR."""
    text = raw.decode("latin1", "replace")
    return [r.strip() for r in re.split(r"[\r\n]+", text) if r.strip()]


def _fields(row: str) -> List[str]:
    return [f.strip() for f in row.split(",")]


def _is_data_row(row: str) -> bool:
    # Skip the banner and any all-letter heading lines.
    return "," in row and "PowerTronics" not in row


def _num(s: str) -> float:
    try:
        return float(s)
    except ValueError:
        return float("nan")


def parse_summary_report(raw: bytes) -> Report:
    rep = Report(header=parse_header(raw), raw=raw)
    for row in _rows(raw):
        if not _is_data_row(row):
            continue
        f = _fields(row)
        if len(f) >= 3:
            try:
                count = int(f[2])
            except ValueError:
                count = 0
            rep.rows.append(SummaryRow(f[0], f[1], count, raw=row))
    return rep


def parse_detail_report(raw: bytes) -> Report:
    rep = Report(header=parse_header(raw), raw=raw)
    for row in _rows(raw):
        if not _is_data_row(row):
            continue
        f = _fields(row)
        # trailing comma yields an empty last field; need at least 5 real ones
        if len(f) >= 5:
            rep.rows.append(DetailEvent(
                date=f[0], time=f[1], channel=f[2],
                event_type=f[3], magnitude=_num(f[4]), raw=row))
    return rep


def parse_data_log(raw: bytes) -> Report:
    rep = Report(header=parse_header(raw), raw=raw)
    for row in _rows(raw):
        if not _is_data_row(row):
            continue
        f = _fields(row)
        if len(f) >= 6:
            rep.rows.append(DataLogSample(
                date=f[0], time=f[1],
                ch1_name=f[2], ch1_value=_num(f[3]),
                ch2_name=f[4], ch2_value=_num(f[5]), raw=row))
    return rep
