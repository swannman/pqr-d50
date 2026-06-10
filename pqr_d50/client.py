"""
PQRClient - a Linux/Raspberry-Pi friendly driver for Powertronics PQR-series
power-line monitors, reimplementing the serial protocol of the Windows-only
``PQRHost.exe`` v2.1.5.

Transport: RS-232 (typically via a USB-serial adapter, e.g. /dev/ttyUSB0).
Dependencies: pyserial  (``pip install pyserial``).

Design notes
------------
The original host speaks a simple request/response protocol: it writes a short
ASCII command ("C1".."C6") and then reads a reply.  The bulk transfers
(C2/C3/C4) stream a sequence of data blocks until the device falls silent; the
host shows a running "blocks written" counter and lets the user abort with ESC.
The exact on-wire record layout of those blocks could only be partially
recovered statically (the device was not present during reverse engineering),
so the bulk reads here return the *raw* payload plus a best-effort parse, and
``capture.py`` is provided to record real exchanges and finalise the parsers.

Everything that drives the link (port settings, command bytes, abort/terminator
framing, identity handshake) is confirmed from the binary and should work as-is.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Iterator, Optional

try:
    import serial  # pyserial
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "pqr_d50 requires pyserial. Install it with:  pip install pyserial"
    ) from exc

from . import protocol as P

log = logging.getLogger("pqr_d50")


@dataclass
class RawTransfer:
    """Result of a bulk download (Detail/Summary/Data Log)."""
    command: bytes
    raw: bytes
    blocks: int = 0
    elapsed_s: float = 0.0
    aborted: bool = False

    def save(self, path: str) -> None:
        with open(path, "wb") as fh:
            fh.write(self.raw)


@dataclass
class DeviceInfo:
    """Whatever the unit reports in response to the C1 connect command."""
    raw: bytes
    model: Optional[str] = None
    vendor: Optional[str] = None
    extra: dict = field(default_factory=dict)

    @property
    def identified(self) -> bool:
        return self.model is not None or self.vendor is not None


class PQRError(Exception):
    pass


class PQRClient:
    """
    Synchronous driver over a single serial port.

    Example
    -------
        from pqr_d50 import PQRClient
        with PQRClient("/dev/ttyUSB0", baudrate=9600) as dev:
            info = dev.connect()
            print("model:", info.model)
            xfer = dev.detail_report()
            xfer.save("events.drp")
    """

    def __init__(
        self,
        port: str,
        baudrate: int = P.DEFAULT_BAUD_RATE,
        *,
        timeout: float = 1.0,
        inter_block_idle: float = 2.0,
        open_now: bool = True,
    ):
        if baudrate not in P.SUPPORTED_BAUD_RATES:
            log.warning(
                "baudrate %s is not one of the device's supported rates %s",
                baudrate, P.SUPPORTED_BAUD_RATES,
            )
        self.port = port
        self.baudrate = baudrate
        # `timeout` is the per-read byte timeout; `inter_block_idle` is how long
        # the line may stay silent before we consider a bulk transfer complete.
        self.timeout = timeout
        self.inter_block_idle = inter_block_idle
        self._ser: Optional[serial.Serial] = None
        if open_now:
            self.open()

    # -- lifecycle ---------------------------------------------------------
    def open(self) -> None:
        if self._ser and self._ser.is_open:
            return
        # [CONFIRMED] 8N1 framing; mirror the host's "<baud>,N,8,1".
        self._ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=self.timeout,
            write_timeout=2.0,
        )
        log.info("opened %s @ %d 8N1", self.port, self.baudrate)

    def close(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()
            log.info("closed %s", self.port)

    def __enter__(self) -> "PQRClient":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- low-level I/O -----------------------------------------------------
    @property
    def ser(self) -> serial.Serial:
        if not self._ser or not self._ser.is_open:
            raise PQRError("serial port is not open")
        return self._ser

    def _write(self, data: bytes) -> None:
        log.debug("TX %r", data)
        self.ser.reset_input_buffer()
        self.ser.write(data)
        self.ser.flush()

    def send_command(self, cmd: bytes, *, terminate: bool = False) -> None:
        """Write a raw command. Set ``terminate=True`` to append CR."""
        if isinstance(cmd, P.Command):
            cmd = cmd.value
        self._write(cmd + (P.CR if terminate else b""))

    def abort(self) -> None:
        """[CONFIRMED] Send ESC to cancel an in-progress transfer / stream."""
        self._write(P.ESC)

    def _read_reply(self, settle: Optional[float] = None) -> bytes:
        """
        Read a short reply: accumulate bytes until the line goes idle for
        ``settle`` seconds (defaults to the configured byte timeout).
        """
        settle = self.inter_block_idle if settle is None else settle
        buf = bytearray()
        last = time.monotonic()
        while True:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf += chunk
                last = time.monotonic()
            elif time.monotonic() - last >= settle:
                break
        log.debug("RX %r", bytes(buf))
        return bytes(buf)

    # -- high-level operations --------------------------------------------
    def connect(self, retries: int = 2) -> DeviceInfo:
        """
        [CONFIRMED command, VALIDATE parse] Send C1 and parse the identity reply.
        """
        for attempt in range(retries + 1):
            self.send_command(P.Command.CONNECT)
            reply = self._read_reply(settle=0.5)
            if reply:
                return self._parse_identity(reply)
            log.warning("no reply to C1 (attempt %d/%d)", attempt + 1, retries + 1)
            time.sleep(0.2)
        raise PQRError("device did not respond to connect (C1)")

    @staticmethod
    def _parse_identity(reply: bytes) -> DeviceInfo:
        text = reply.decode("latin1", "replace")
        info = DeviceInfo(raw=reply)
        for m in P.KNOWN_MODELS:
            if m in text:
                info.model = m
                break
        for v in P.VENDOR_TOKENS:
            if v in text:
                info.vendor = v
                break
        return info

    def _bulk_download(self, cmd: P.Command) -> RawTransfer:
        """
        Shared logic for the three streaming dumps (C2/C3/C4).  Reads blocks
        until the line stays idle for ``inter_block_idle`` seconds.
        """
        t0 = time.monotonic()
        self.send_command(cmd)
        raw = bytearray()
        blocks = 0
        last = time.monotonic()
        aborted = False
        try:
            while True:
                chunk = self.ser.read(self.ser.in_waiting or 1)
                if chunk:
                    raw += chunk
                    blocks += 1
                    last = time.monotonic()
                elif time.monotonic() - last >= self.inter_block_idle:
                    break
        except KeyboardInterrupt:
            self.abort()
            aborted = True
        return RawTransfer(
            command=cmd.value,
            raw=bytes(raw),
            blocks=blocks,
            elapsed_s=time.monotonic() - t0,
            aborted=aborted,
        )

    def summary_report(self) -> RawTransfer:
        """[CONFIRMED] C2 - download event-count summary (host saves as .srp)."""
        return self._bulk_download(P.Command.SUMMARY_REPORT)

    def detail_report(self) -> RawTransfer:
        """[CONFIRMED] C3 - download full event listing (host saves as .drp)."""
        return self._bulk_download(P.Command.DETAIL_REPORT)

    def data_log(self) -> RawTransfer:
        """[CONFIRMED] C4 - download voltage time-history (host saves as .dlg)."""
        return self._bulk_download(P.Command.DATA_LOG)

    def calibration_stream(
        self, duration_s: Optional[float] = None
    ) -> Iterator[bytes]:
        """
        [CONFIRMED] C5 - begin the real-time readings stream.

        Yields raw line/records as they arrive (~1/sec).  The device auto-stops
        after ~2 minutes; pass ``duration_s`` to stop sooner.  ESC is sent on
        exit to halt the stream cleanly.
        """
        self.send_command(P.Command.CALIBRATION)
        t0 = time.monotonic()
        try:
            while True:
                if duration_s and time.monotonic() - t0 >= duration_s:
                    break
                if time.monotonic() - t0 >= P.CALIBRATION_AUTO_STOP_S:
                    break
                line = self.ser.readline()
                if line:
                    yield line
        finally:
            self.abort()

    # -- settings/programming (C6 family) ---------------------------------
    # [VALIDATE] The C6 path enters the device's programming mode and then
    # streams parameter digits terminated by CR. The exact sub-command codes
    # (set time / set threshold / set baud / set transducer / clear) were not
    # fully recoverable statically. Use program_raw() + capture.py to map them,
    # then add typed helpers here.
    def program_raw(self, payload: bytes, *, read_reply: bool = True) -> bytes:
        """
        Enter the C6 programming path and send a raw payload (you supply any
        sub-code and parameters, CR-terminated as needed). Returns the reply.
        """
        self.send_command(P.Command.PROGRAM)
        time.sleep(0.05)
        self._write(payload)
        return self._read_reply() if read_reply else b""

    def autobaud(self) -> Optional[int]:
        """
        Try each supported baud rate, sending C1, until the unit identifies
        itself. Returns the working rate (and leaves the port set to it).
        """
        original = self.baudrate
        for rate in P.SUPPORTED_BAUD_RATES:
            self.close()
            self.baudrate = rate
            self.open()
            try:
                info = self.connect(retries=1)
                if info.identified or info.raw:
                    log.info("autobaud locked at %d", rate)
                    return rate
            except PQRError:
                continue
        self.close()
        self.baudrate = original
        self.open()
        return None
