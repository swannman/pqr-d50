"""
pqr_d50 - a Linux/Raspberry-Pi driver for Powertronics PQR-series power-line
monitors (PQR D50 and relatives), reverse-engineered from the Windows-only
PQRHost.exe and validated live against a PQR D50 V20.55.

Quick start
-----------
    from pqr_d50 import PQRClient

    with PQRClient("/dev/ttyUSB0", baudrate=19200) as dev:
        print(dev.identify())
        for ev in dev.detail_report().rows:
            print(ev.date, ev.time, ev.event_type, ev.magnitude)
"""

from .client import PQRClient, PQRError, DeviceInfo, Settings
from . import protocol, parsers

__all__ = [
    "PQRClient", "PQRError", "DeviceInfo", "Settings",
    "protocol", "parsers",
]

__version__ = "0.2.0"
