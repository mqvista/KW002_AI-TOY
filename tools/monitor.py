"""
Serial log monitor for the KW002 board (single USB-TTL on UART0).

The CH343 adapter's RTS line is wired to the T5 module reset (CEN): opening the
port the default pyserial way (RTS asserted) holds the chip in reset, so the
device looks dead. `tos.py monitor` has the same problem with this adapter.
This script keeps RTS/DTR released and can optionally pulse a reset.

Usage (TuyaOpen venv python has pyserial):
    python tools/monitor.py                      # COM10 @ 460800, print everything
    python tools/monitor.py -r                   # pulse reset first, see boot log
    python tools/monitor.py -f "KEY|AUDIO"       # only lines matching a regex
    python tools/monitor.py -l device.log        # also append raw bytes to a file
Ctrl+C to quit.
"""

import argparse
import re
import sys
import time

import serial

ANSI = re.compile(r'\x1b\[[0-9;]*m')


def main():
    ap = argparse.ArgumentParser(description='KW002 serial log monitor')
    ap.add_argument('-p', '--port', default='COM10')
    ap.add_argument('-b', '--baud', type=int, default=460800)
    ap.add_argument('-r', '--reset', action='store_true', help='pulse RTS to reset the module first')
    ap.add_argument('-f', '--filter', help='only print lines matching this regex')
    ap.add_argument('-l', '--log', help='append raw received bytes to this file')
    ap.add_argument('-t', '--time', type=float, default=0, help='stop after N seconds (0 = forever)')
    args = ap.parse_args()

    pat = re.compile(args.filter) if args.filter else None

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = args.port, args.baud, 0.2
    ser.rts = False  # RTS asserted = module held in reset on this adapter
    ser.dtr = False
    ser.open()

    if args.reset:
        ser.rts = True
        time.sleep(0.2)
        ser.rts = False

    logf = open(args.log, 'ab') if args.log else None
    end = time.time() + args.time if args.time > 0 else None
    buf = b''
    try:
        while end is None or time.time() < end:
            data = ser.read(4096)
            if not data:
                continue
            if logf:
                logf.write(data)
                logf.flush()
            buf += data
            *lines, buf = buf.split(b'\n')
            for raw in lines:
                line = ANSI.sub('', raw.decode('utf-8', 'replace')).rstrip('\r')
                if pat and not pat.search(line):
                    continue
                print(line, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.rts = False  # leave CEN released when ending a debug session
            ser.dtr = False
        finally:
            ser.close()
            if logf:
                logf.close()


if __name__ == '__main__':
    sys.exit(main())
