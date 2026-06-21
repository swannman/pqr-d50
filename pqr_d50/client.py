"""
PQRClient - Linux/Raspberry-Pi driver for Powertronics PQR-series power-line
monitors, validated live against a PQR D50 (firmware V20.55).

Transport: RS-232 (USB-serial adapter, e.g. /dev/ttyUSB0 on Linux,
/dev/cu.usbserial-* on macOS).  Requires pyserial.

Device model
------------
The unit has two interaction modes:

* **command mode** - two-char ASCII commands C1..C6 (see protocol.Command).
  C1..C4 return ASCII reports; C5 ERASES all data; C6 opens the setup menu.
* **setup menu (C6)** - an interactive, *stateful* text menu. Sub-prompts
  ignore ESC (only digits/CR advance them), so navigation must be deliberate.
  reset() reliably walks back to command mode from any state.

Reports stream until the line goes idle; reads here are idle-terminated.
"""

from __future__ import annotations

import logging
import re
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, Optional, Tuple

try:
    import serial  # pyserial
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "pqr_d50 requires pyserial. Install it with:  pip install pyserial"
    ) from exc

from . import protocol as P
from . import parsers

log = logging.getLogger("pqr_d50")


class PQRError(Exception):
    pass


@dataclass
class DeviceInfo:
    raw: bytes
    model: Optional[str] = None
    firmware: Optional[str] = None
    vendor: Optional[str] = None
    date: Optional[str] = None
    unit_id: Optional[str] = None

    @property
    def identified(self) -> bool:
        return self.model is not None


@dataclass
class Settings:
    """Current device settings, as shown by the C6 menu."""
    datetime: Optional[str] = None
    baud: Optional[int] = None
    sample_rate_s: Optional[int] = None
    raw: str = ""


class PQRClient:
    """
    Example
    -------
        from pqr_d50 import PQRClient
        with PQRClient("/dev/ttyUSB0", baudrate=19200) as dev:
            print(dev.identify())
            for ev in dev.detail_report().rows:
                print(ev.date, ev.time, ev.event_type, ev.magnitude)
    """

    def __init__(self, port: str, baudrate: int = P.DEFAULT_BAUD_RATE,
                 *, byte_timeout: float = 0.2, idle: float = 1.2,
                 open_now: bool = True):
        self.port = port
        self.baudrate = baudrate
        self.byte_timeout = byte_timeout
        self.idle = idle          # line-quiet seconds = end of a response
        self._ser: Optional[serial.Serial] = None
        if open_now:
            self.open()

    # -- lifecycle ---------------------------------------------------------
    def open(self) -> None:
        if self._ser and self._ser.is_open:
            return
        self._ser = serial.Serial(
            self.port, self.baudrate,
            bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE, timeout=self.byte_timeout,
            write_timeout=2.0)
        log.info("opened %s @ %d 8N1", self.port, self.baudrate)

    def close(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()

    def __enter__(self) -> "PQRClient":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def ser(self) -> serial.Serial:
        if not self._ser or not self._ser.is_open:
            raise PQRError("serial port is not open")
        return self._ser

    # -- low-level I/O -----------------------------------------------------
    def _read_idle(self, idle: Optional[float] = None, maxwait: float = 60.0) -> bytes:
        idle = self.idle if idle is None else idle
        buf = bytearray()
        last = t0 = time.monotonic()
        while True:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf += chunk
                last = time.monotonic()
            elif time.monotonic() - last >= idle:
                break
            elif time.monotonic() - t0 >= maxwait:
                log.warning("read hit maxwait (%.0fs)", maxwait)
                break
        return bytes(buf)

    def _write(self, data: bytes) -> None:
        if isinstance(data, P.Command) or isinstance(data, P.SetupOption):
            data = data.value
        log.debug("TX %r", data)
        self.ser.write(data)
        self.ser.flush()

    def reset(self) -> bool:
        """
        Return to command mode from ANY menu/sub-prompt state and confirm it.

        Sub-prompts ignore ESC, so we first spam CR (advancing/closing any
        flow), then ESC (exits the main menu), then verify with C1.
        """
        for _ in range(30):
            self._write(P.CR)
            self._read_idle(0.06, maxwait=0.2)
        self._write(P.ESC)
        self._read_idle(0.3, maxwait=1.0)
        self.ser.reset_input_buffer()
        self._write(P.Command.IDENTIFY)
        return b"PowerTronics" in self._read_idle(0.6, maxwait=3.0)

    def raw(self, data: bytes, idle: Optional[float] = None) -> bytes:
        """Send arbitrary bytes and return the idle-terminated reply."""
        self.ser.reset_input_buffer()
        self._write(data)
        return self._read_idle(idle)

    def _command(self, cmd: P.Command, idle: Optional[float] = None,
                 maxwait: float = 60.0) -> bytes:
        self.ser.reset_input_buffer()
        self._write(cmd)
        return self._read_idle(idle, maxwait)

    # -- identity ----------------------------------------------------------
    def identify(self, retries: int = 2) -> DeviceInfo:
        """[C1] Read the identity banner."""
        for attempt in range(retries + 1):
            raw = self._command(P.Command.IDENTIFY, idle=0.5, maxwait=3.0)
            if b"PowerTronics" in raw:
                return self._parse_identity(raw)
            time.sleep(0.2)
        raise PQRError("device did not respond to C1 (identify)")

    @staticmethod
    def _parse_identity(raw: bytes) -> DeviceInfo:
        text = raw.decode("latin1", "replace")
        info = DeviceInfo(raw=raw)
        m = re.search(r"PowerTronics\s+PQR\s+(\S+)\s+V([\d.]+)", text)
        if m:
            info.model, info.firmware = m.group(1), m.group(2)
        for v in P.VENDOR_TOKENS:
            if v in text:
                info.vendor = v
                break
        d = re.search(r"([A-Z][a-z]{2}/\d{2}/\d{2})", text)
        if d:
            info.date = d.group(1)
        i = re.search(r"ID:\s*(\d+)", text)
        if i:
            info.unit_id = i.group(1)
        return info

    # -- reports (read-only) ----------------------------------------------
    def summary_report(self) -> parsers.Report:
        """[C2] Event-count summary."""
        return parsers.parse_summary_report(self._command(P.Command.SUMMARY_REPORT))

    def detail_report(self) -> parsers.Report:
        """[C3] Full event listing."""
        return parsers.parse_detail_report(self._command(P.Command.DETAIL_REPORT))

    def data_log(self) -> parsers.Report:
        """[C4] Voltage time-history at the configured sample rate."""
        return parsers.parse_data_log(self._command(P.Command.DATA_LOG))

    # -- destructive -------------------------------------------------------
    def clear_data(self, confirm: bool = False) -> bool:
        """
        [C5] ERASE all events and the data log. Pass ``confirm=True``.

        The device prompts "Are You Sure ... ?"; we answer "Y", it erases the
        FLASH banks and replies "Ram has been cleared !".
        """
        if not confirm:
            raise PQRError(
                "clear_data() is destructive; call clear_data(confirm=True)")
        self.ser.reset_input_buffer()
        self._write(P.Command.CLEAR_DATA)
        prompt = self._read_idle(0.8, maxwait=3.0)
        if P.Status.ARE_YOU_SURE.value not in prompt:
            log.warning("unexpected clear prompt: %r", prompt)
        self._write(b"Y")
        result = self._read_idle(1.5, maxwait=20.0)
        return P.Status.RAM_CLEARED.value in result

    # -- setup menu (C6) ---------------------------------------------------
    def _enter_menu(self) -> bytes:
        self.ser.reset_input_buffer()
        self._write(P.Command.SETUP_MENU)
        return self._read_idle(1.0, maxwait=3.0)

    def get_settings(self) -> Settings:
        """Open the C6 menu, read current settings, and exit."""
        menu = self._enter_menu()
        text = menu.decode("latin1", "replace")
        s = Settings(raw=text)
        m = re.search(r"Set the Date and Time\s+(.+)", text)
        if m:
            s.datetime = m.group(1).strip()
        m = re.search(r"Set the Baud Rate\s+([\d,]+)\s*Baud", text)
        if m:
            s.baud = int(m.group(1).replace(",", ""))
        m = re.search(r"Sample rate\s+(\d+)\s*(Seconds?|Minutes?)", text)
        if m:
            n = int(m.group(1))
            s.sample_rate_s = n * (60 if m.group(2).startswith("Min") else 1)
        self._write(P.ESC)
        self._read_idle(0.4, maxwait=1.0)
        return s

    def set_datetime(self, when: Optional[datetime] = None) -> bool:
        """[C6->1] Set the clock (defaults to now). Format MM/DD/YY,HH:MM:SS."""
        when = when or datetime.now()
        stamp = when.strftime("%m/%d/%y,%H:%M:%S")
        self._enter_menu()
        self._write(P.SetupOption.DATE_TIME.value + P.CR)
        self._read_idle(0.8, maxwait=3.0)              # the prompt
        reply = self.raw(stamp.encode() + P.CR, idle=1.0)
        ok = P.Status.COMMAND_OK.value in reply
        self.reset()
        return ok

    def set_sample_rate(self, seconds: int) -> bool:
        """
        [C6->3] Set the data-log sample interval. WARNING: this ERASES the
        existing data log (the device re-formats its FLASH banks).
        Allowed: 1, 5, 10, 30, 60, 240 seconds.
        """
        if seconds not in P.SAMPLE_RATE_MENU_INV:
            raise PQRError(f"sample rate must be one of "
                           f"{sorted(P.SAMPLE_RATE_MENU_INV)} seconds")
        digit = P.SAMPLE_RATE_MENU_INV[seconds]
        self._enter_menu()
        self._write(P.SetupOption.SAMPLE_RATE.value + P.CR)
        self._read_idle(0.8, maxwait=3.0)              # the rate list
        reply = self.raw(digit + P.CR, idle=2.0)       # erases FLASH banks
        self.reset()
        return b"sample" in reply.lower() or b"Minute" in reply or b"Sec" in reply

    def set_baud(self, rate: int, *, reopen: bool = True) -> bool:
        """
        [C6->2] Change the device baud rate. The unit switches immediately, so
        by default we reopen the local port at the new rate to stay in sync.
        Allowed: 2400, 4800, 9600, 14400, 19200, 115200.
        """
        if rate not in P.BAUD_MENU_INV:
            raise PQRError(f"baud must be one of {sorted(P.BAUD_MENU_INV)}")
        digit = P.BAUD_MENU_INV[rate]
        self._enter_menu()
        self._write(P.SetupOption.BAUD_RATE.value + P.CR)
        self._read_idle(0.8, maxwait=3.0)              # the rate list
        self._write(digit + P.CR)                      # device switches now
        self._read_idle(0.4, maxwait=1.0)
        if reopen:
            self.close()
            self.baudrate = rate
            self.open()
            return self.reset()
        return True

    def set_thresholds(
        self, values: Optional[Dict[Tuple[str, str], Optional[int]]] = None
    ) -> bool:
        """
        [C6->4] Walk the 6-prompt threshold sequence
        (CH1/CH2 x {Surge, Sag, PowerFail}).

        ``values`` maps (channel, kind) -> setting:
            None  -> keep current
            0     -> default (5%,10%) percentage mode
            N     -> trip at N volts (1..999)
        Omitted entries keep their current value. The flow ignores ESC and ends
        with "Setup Completed".
        """
        values = values or {}
        self._enter_menu()
        self._write(P.SetupOption.THRESHOLDS.value + P.CR)
        ok = False
        last = self._read_idle(1.0, maxwait=3.0)
        for ch, kind in P.THRESHOLD_SEQUENCE:
            if b"Setup Completed" in last:
                break
            v = values.get((ch, kind))
            entry = b"" if v is None else str(int(v)).encode()
            last = self.raw(entry + P.CR, idle=0.8)
            if b"Setup Completed" in last:
                ok = True
                break
        self.reset()
        return ok or b"Setup Completed" in last

    # -- convenience -------------------------------------------------------
    def autobaud(self) -> Optional[int]:
        """Sweep supported rates sending C1 until the unit identifies itself."""
        for rate in P.SUPPORTED_BAUD_RATES:
            self.close()
            self.baudrate = rate
            self.open()
            try:
                if self.identify(retries=1).identified:
                    log.info("autobaud locked at %d", rate)
                    return rate
            except PQRError:
                continue
        return None
