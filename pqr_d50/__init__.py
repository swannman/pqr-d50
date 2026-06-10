"""
pqr_d50 - a Linux/Raspberry-Pi driver for Powertronics PQR-series power-line
monitors (PQR D50 and relatives), reverse-engineered from the Windows-only
PQRHost.exe v2.1.5.

Quick start
-----------
    from pqr_d50 import PQRClient

    with PQRClient("/dev/ttyUSB0", baudrate=9600) as dev:
        print(dev.connect())
        dev.detail_report().save("events.drp")
"""

from .client import PQRClient, PQRError, RawTransfer, DeviceInfo
from . import protocol, parsers

__all__ = [
    "PQRClient",
    "PQRError",
    "RawTransfer",
    "DeviceInfo",
    "protocol",
    "parsers",
]

__version__ = "0.1.0"
