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

ADC2_MAGIC = b'ADC2'
GWP1_MAGIC = b'GWP1'
ADC2_HEADER_FMT = '<IHHIIIIII'
ADC2_HEADER_SIZE = struct.calcsize(ADC2_HEADER_FMT)
ADC2_FRAME_FMT = '<IHH'
ADC2_FRAME_SIZE = struct.calcsize(ADC2_FRAME_FMT)
GWP_HEADER_FMT = '<IBBBBHHI'
GWP_HEADER_SIZE = struct.calcsize(GWP_HEADER_FMT)
GWP_ADC_META_FMT = '<IHBB'
GWP_ADC_META_SIZE = struct.calcsize(GWP_ADC_META_FMT)
GWP_STATUS_FMT = '<IIIIIIIIIHHBBBB'
GWP_CAPS_FMT = '<IIII I HH BBBB'.replace(' ', '')
CRC_SIZE = 4
GW_MSG_DATA = 1
GW_MSG_STATUS = 4
GW_MSG_CAPS = 7
GW_SAMPLE_U16_LE = 3


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
        if len(window) > 4:
            del window[0]
        if len(window) == 4 and (bytes(window) == ADC2_MAGIC or bytes(window) == GWP1_MAGIC):
            return bytes(window)


def main():
    ap = argparse.ArgumentParser(description='Read/test MCU ADC binary stream (ADC2 or GWP1)')
    ap.add_argument('-d', '--dev', default='/dev/ttyACM0')
    ap.add_argument('-b', '--baud', type=int, default=115200, help='ignored by USB CDC but needed by pyserial')
    ap.add_argument('-s', '--seconds', type=float, default=10.0)
    ap.add_argument('--raw-file', help='optional path to save raw packets')
    args = ap.parse_args()

    if serial:
        f = serial.Serial(args.dev, args.baud, timeout=2)
    else:
        fd = os.open(args.dev, os.O_RDONLY | os.O_NOCTTY)
        f = os.fdopen(fd, 'rb', buffering=0)

    raw = open(args.raw_file, 'wb') if args.raw_file else None
    start = time.monotonic()
    last_print = start
    packets = frames = crc_bad = gaps = flags_seen = 0
    last_seq = None
    sample_rate = None
    proto = None

    try:
        while time.monotonic() - start < args.seconds:
            prefix = sync_to_magic(f)
            if prefix == ADC2_MAGIC:
                rest = read_exact(f, ADC2_HEADER_SIZE - 4)
                hdr_bytes = prefix + rest
                vals = struct.unpack(ADC2_HEADER_FMT, hdr_bytes)
                magic, version, header_bytes, sample_rate_hz, frame_count, sequence_start, flags, lost_frames, reserved = vals
                if header_bytes != ADC2_HEADER_SIZE or frame_count <= 0 or frame_count > 4096:
                    continue
                payload = read_exact(f, frame_count * ADC2_FRAME_SIZE)
                crc_bytes = read_exact(f, CRC_SIZE)
                pkt = hdr_bytes + payload + crc_bytes
                got_crc = struct.unpack('<I', crc_bytes)[0]
                calc_crc = binascii.crc32(hdr_bytes)
                calc_crc = binascii.crc32(payload, calc_crc) & 0xffffffff
                proto = 'ADC2'
                sample_rate = sample_rate_hz
            else:
                rest = read_exact(f, GWP_HEADER_SIZE - 4)
                hdr_bytes = prefix + rest
                magic, version, header_len, msg_type, hdr_flags, stream_id, payload_len, pkt_seq = struct.unpack(GWP_HEADER_FMT, hdr_bytes)
                if header_len != GWP_HEADER_SIZE or payload_len > 4096:
                    continue
                payload = read_exact(f, payload_len)
                crc_bytes = read_exact(f, CRC_SIZE)
                pkt = hdr_bytes + payload + crc_bytes
                got_crc = struct.unpack('<I', crc_bytes)[0]
                calc_crc = binascii.crc32(hdr_bytes)
                calc_crc = binascii.crc32(payload, calc_crc) & 0xffffffff
                proto = 'GWP1'
                if got_crc == calc_crc and msg_type == GW_MSG_DATA and payload_len >= GWP_ADC_META_SIZE:
                    sequence_start, frame_count, channels, fmt = struct.unpack(GWP_ADC_META_FMT, payload[:GWP_ADC_META_SIZE])
                    lost_frames = 0
                    flags = 0
                elif got_crc == calc_crc and msg_type == GW_MSG_STATUS:
                    packets += 1
                    continue
                else:
                    packets += 1
                    continue

            if raw:
                raw.write(pkt)

            if got_crc != calc_crc:
                crc_bad += 1
                continue

            packets += 1
            frames += frame_count
            flags_seen |= flags
            if last_seq is not None and sequence_start != ((last_seq + 1) & 0xffffffff):
                gaps += 1
            last_seq = (sequence_start + frame_count - 1) & 0xffffffff

            now = time.monotonic()
            if now - last_print >= 1.0:
                elapsed = now - start
                print(f't={elapsed:6.2f}s proto={proto:4s} packets={packets} frames={frames} avg_fps={frames/elapsed:9.2f} declared_rate={sample_rate} crc_bad={crc_bad} gaps={gaps} flags=0x{flags_seen:08x}')
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
