"""Reusable live-exploration helper for the PQR D50.

The device keeps its menu/prompt state across serial open/close, so this uses an
idle-based read (wait until the line is quiet) and always logs exact bytes.
"""
import serial, time, sys

PORT = "/dev/cu.usbserial-A10PAK4J"
BAUD = 19200


class Session:
    def __init__(self, baud=BAUD, quiet=False):
        self.s = serial.Serial(PORT, baud, bytesize=8, parity='N', stopbits=1,
                               timeout=0.2, write_timeout=2)
        self.quiet = quiet

    def read(self, settle=1.0, maxwait=8.0):
        buf = bytearray(); last = time.monotonic(); t0 = last
        while True:
            c = self.s.read(self.s.in_waiting or 1)
            if c:
                buf += c; last = time.monotonic()
            elif time.monotonic() - last >= settle:
                break
            elif time.monotonic() - t0 >= maxwait:
                break
        return bytes(buf)

    def send(self, data, settle=1.0, label=None):
        if isinstance(data, str):
            data = data.encode('latin1')
        self.s.write(data); self.s.flush()
        r = self.read(settle)
        if not self.quiet:
            tag = f" [{label}]" if label else ""
            sys.stdout.write(f"TX {data!r}{tag}\nRX {r!r}\n\n")
        return r

    def esc_flush(self, n=6):
        for _ in range(n):
            self.s.write(b'\x1b'); self.s.flush(); self.read(0.25)
        self.s.reset_input_buffer()

    def reset(self):
        """Reliably return to command mode from ANY menu/sub-prompt state.

        Sub-prompts ignore ESC; only digits/CR advance them. So we spam CR to
        walk any flow to completion, ESC to exit the main menu, then confirm
        command mode with C1. Returns True when the identity banner is seen.
        """
        for _ in range(30):
            self.s.write(b'\r'); self.s.flush(); self.read(0.08)
        self.s.write(b'\x1b'); self.s.flush(); self.read(0.3)
        self.s.reset_input_buffer()
        self.s.write(b'C1'); self.s.flush()
        return b'PowerTronics' in self.read(1.0)

    def menu(self):
        """Enter the C6 setup menu and return its text (caller should ESC after)."""
        self.s.write(b'C6'); self.s.flush()
        return self.read(1.2)

    def close(self):
        try: self.s.close()
        except Exception: pass
