# -*- coding: utf-8 -*-
"""Send the agreed reset byte to the board's UART.

The kernel's NativeSd driver runs a thread that reads the UART receive path (package/NativeSd/NativeSd.cpp,
the serial_getc() of pexpert/arm/pe_kprintf.c) and calls panic() on 0x1d, which this port's PSCI restart
turns into a board reset. That is what ends a round without reaching for the power cable.

It only works because board.toml passes serial=1: the INPUT bit is clear, so serial_keyboard_poll in
osfmk/console/serial_general.c never starts and never drains the receive FIFO ahead of that thread. The
console output is unaffected, so the board's log still arrives on the same port.

    py -3 tools/wsl/serial-reset.py
    py -3 tools/wsl/serial-reset.py --port COM3 --byte 0x1d
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=1_500_000)
    ap.add_argument("--byte", default="0x1d", help="the reset byte, decimal or 0x-hex (0x1d is Ctrl-])")
    args = ap.parse_args()

    try:
        value = int(args.byte, 0)
    except ValueError:
        print("byte %r is not a number" % args.byte)
        return 2
    if not 0 <= value <= 0xFF:
        print("byte 0x%x is out of range" % value)
        return 2

    try:
        with serial.Serial(args.port, args.baud, bytesize=8, parity="N", stopbits=1,
                           timeout=0.2, write_timeout=1.0) as sp:
            sp.write(bytes([value]))
            sp.flush()
            # Let the shift register empty before the handle closes under the byte.
            time.sleep(0.05)
    except Exception as exc:  # noqa: BLE001 - the caller needs the reason, not a traceback
        print("%s @ %d: %s: %s" % (args.port, args.baud, type(exc).__name__, exc))
        return 1

    print("%s @ %d: sent 0x%02x, the board should reset" % (args.port, args.baud, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
