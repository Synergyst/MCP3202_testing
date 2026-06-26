#!/usr/bin/env python3
import argparse
import binascii
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    serial = None

MAGIC = b'ADC2'
HEADER_FMT = '<IHHIIIIII'
HEADER_SIZE = struct.calcsize(HEADER_FMT)
FRAME_FMT = '<IHH'
FRAME_SIZE = struct.calcsize(FRAME_FMT)
CRC_SIZE = 4


def read_exact(f, n):
    b = bytearray()
    while len(b) < n:
        chunk = f.read(n - len(b))
        if not chunk:
            raise EOFError('short read')
        b.extend(chunk)
    return bytes(b)


def sync_to_magic(f):
    window = bytearray()
    while True:
        c = f.read(1)
        if not c:
            raise EOFError('no data while syncing')
        window += c
        if len(window) > len(MAGIC):
            del window[0]
        if bytes(window) == MAGIC:
            return bytes(window)


def main():
    ap = argparse.ArgumentParser(description='Read/test RP2040 MCP3202 ADC binary USB stream')
    ap.add_argument('-d', '--dev', default='/dev/ttyACM0')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='ignored by USB CDC but needed by pyserial')
    ap.add_argument('-s', '--seconds', type=float, default=10.0)
    ap.add_argument('--raw-file', help='optional path to save raw packets')
    args = ap.parse_args()

    if serial:
        f = serial.Serial(args.dev, args.baud, timeout=2)
    else:
        # USB CDC ignores termios baud. This raw fallback is enough for quick testing.
        fd = os.open(args.dev, os.O_RDONLY | os.O_NOCTTY)
        f = os.fdopen(fd, 'rb', buffering=0)

    raw = open(args.raw_file, 'wb') if args.raw_file else None
    start = time.monotonic()
    last_print = start
    packets = frames = crc_bad = gaps = flags_seen = 0
    last_seq = None
    sample_rate = None

    try:
        while time.monotonic() - start < args.seconds:
            prefix = sync_to_magic(f)
            rest = read_exact(f, HEADER_SIZE - 4)
            hdr_bytes = prefix + rest
            vals = struct.unpack(HEADER_FMT, hdr_bytes)
            magic, version, header_bytes, sample_rate_hz, frame_count, sequence_start, flags, lost_frames, reserved = vals
            if header_bytes != HEADER_SIZE or frame_count <= 0 or frame_count > 4096:
                continue
            payload = read_exact(f, frame_count * FRAME_SIZE)
            crc_bytes = read_exact(f, CRC_SIZE)
            pkt = hdr_bytes + payload + crc_bytes
            if raw:
                raw.write(pkt)

            got_crc = struct.unpack('<I', crc_bytes)[0]
            calc_crc = binascii.crc32(hdr_bytes)
            calc_crc = binascii.crc32(payload, calc_crc) & 0xffffffff
            if got_crc != calc_crc:
                crc_bad += 1
                continue

            sample_rate = sample_rate_hz
            packets += 1
            frames += frame_count
            flags_seen |= flags
            if last_seq is not None and sequence_start != ((last_seq + 1) & 0xffffffff):
                gaps += 1
            last_seq = (sequence_start + frame_count - 1) & 0xffffffff

            now = time.monotonic()
            if now - last_print >= 1.0:
                elapsed = now - start
                print(f't={elapsed:6.2f}s packets={packets} frames={frames} avg_fps={frames/elapsed:9.2f} declared_rate={sample_rate} crc_bad={crc_bad} gaps={gaps} flags=0x{flags_seen:08x} lost={lost_frames}')
                last_print = now
    finally:
        if raw:
            raw.close()
        try:
            f.close()
        except Exception:
            pass


if __name__ == '__main__':
    main()
