#!/usr/bin/env python3
"""RP2040 GWP1 MCP4922->MCP3202 loopback test.

Expected wiring for the project:
  DAC VOUTA (MCP4922 channel A) -> ADC CH1 (MCP3202 channel 1), or choose --adc-channel.
  RP2040 firmware must be modern GWP1 firmware from mcu-adc/src/main.c.

The script writes several DAC raw codes and reports ADC averages. It refuses to
silently pass legacy ADC2-only firmware because that firmware cannot accept DAC
GWP1 commands.
"""
import argparse, binascii, os, select, statistics, struct, sys, termios, time

GW_MAGIC = 0x31505747
GW_VERSION = 1
GW_HEADER_LEN = 16
GW_MSG_DATA = 1
GW_MSG_CTRL_REQ = 2
GW_MSG_CTRL_RESP = 3
GW_MSG_STATUS = 4
GW_MSG_EVENT = 5
GW_MSG_CAPS = 7
GW_STREAM_CONTROL = 0
GW_STREAM_ADC0 = 1
GW_OP_DAC_WRITE_FRAME = 0x0102
GW_OP_DAC_FLUSH = 0x0105

class Tester:
    def __init__(self, dev):
        self.dev = dev
        self.fd = None
        self.seq = 1
        self.req = 1
        self.buf = b''
        self.saw_gwp = False
        self.saw_adc2 = False

    def open(self):
        self.fd = os.open(self.dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    @staticmethod
    def crc(data):
        return binascii.crc32(data) & 0xffffffff

    def packet(self, typ, stream, payload):
        h = struct.pack('<IBBBBHHI', GW_MAGIC, GW_VERSION, GW_HEADER_LEN, typ, 0, stream, len(payload), self.seq)
        self.seq += 1
        return h + payload + struct.pack('<I', self.crc(h + payload))

    def ctrl(self, op, args=b''):
        payload = struct.pack('<HHHH', op, self.req, len(args), 0) + args
        self.req += 1
        return self.packet(GW_MSG_CTRL_REQ, GW_STREAM_CONTROL, payload)

    def write_dac(self, a, b):
        os.write(self.fd, self.ctrl(GW_OP_DAC_WRITE_FRAME, struct.pack('<HH', a & 0xfff, b & 0xfff)))

    def flush(self):
        os.write(self.fd, self.ctrl(GW_OP_DAC_FLUSH))

    def read_some(self, seconds):
        end = time.time() + seconds
        adc = []
        responses = []
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                data = os.read(self.fd, 8192)
            except BlockingIOError:
                continue
            if not data:
                continue
            self.buf += data
            while True:
                gi = self.buf.find(b'GWP1')
                ai = self.buf.find(b'ADC2')
                if ai >= 0 and (gi < 0 or ai < gi):
                    self.saw_adc2 = True
                    # Skip legacy ADC2 packet if complete, otherwise preserve sync bytes.
                    if len(self.buf) < ai + 32:
                        self.buf = self.buf[ai:]
                        break
                    self.buf = self.buf[ai+4:]
                    continue
                if gi < 0:
                    self.buf = self.buf[-3:]
                    break
                if gi > 0:
                    self.buf = self.buf[gi:]
                if len(self.buf) < 16:
                    break
                try:
                    magic, ver, hlen, typ, flags, stream, plen, pseq = struct.unpack('<IBBBBHHI', self.buf[:16])
                except struct.error:
                    break
                if magic != GW_MAGIC or ver != GW_VERSION or hlen != GW_HEADER_LEN or plen > 4096:
                    self.buf = self.buf[1:]
                    continue
                total = 16 + plen + 4
                if len(self.buf) < total:
                    break
                pay = self.buf[16:16+plen]
                rxcrc = struct.unpack('<I', self.buf[16+plen:total])[0]
                raw = self.buf[:16+plen]
                self.buf = self.buf[total:]
                if self.crc(raw) != rxcrc:
                    continue
                self.saw_gwp = True
                if typ == GW_MSG_CTRL_RESP and len(pay) >= 8:
                    responses.append(struct.unpack('<HHHH', pay[:8]))
                elif typ == GW_MSG_DATA and stream == GW_STREAM_ADC0 and len(pay) >= 8:
                    frame_start, frame_count, ch_count, fmt = struct.unpack('<IHBB', pay[:8])
                    if ch_count == 2 and fmt == 3 and len(pay) >= 8 + frame_count * 4:
                        for k in range(frame_count):
                            adc.append(struct.unpack_from('<HH', pay, 8 + k * 4))
        return adc, responses

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dev', default='/dev/ttyACM1')
    ap.add_argument('--adc-channel', type=int, default=1, choices=[0,1])
    ap.add_argument('--dac-channel', choices=['a','b'], default='a')
    ap.add_argument('--codes', default='0,1024,2048,3072,4095,0')
    ap.add_argument('--settle-ms', type=int, default=50)
    ap.add_argument('--sample-ms', type=int, default=300)
    args = ap.parse_args()
    codes = [int(x.strip(), 0) for x in args.codes.split(',') if x.strip()]
    t = Tester(args.dev)
    t.open()
    try:
        # Probe stream first.
        t.read_some(1.0)
        if not t.saw_gwp:
            msg = 'No GWP1 packets observed.'
            if t.saw_adc2:
                msg += ' Device appears to be legacy ADC2 firmware; reflash mcu_adc.uf2.'
            print('FAIL:', msg, file=sys.stderr)
            return 2
        for code in codes:
            if args.dac_channel == 'a':
                t.write_dac(code, 0)
            else:
                t.write_dac(0, code)
            time.sleep(args.settle_ms / 1000.0)
            adc, resp = t.read_some(args.sample_ms / 1000.0)
            if not adc:
                print(f'DAC_{args.dac_channel.upper()}={code:4d}: no ADC samples')
                continue
            tail = adc[-min(1000, len(adc)):]
            vals = [v[args.adc_channel] for v in tail]
            print(f'DAC_{args.dac_channel.upper()}={code:4d}: samples={len(adc):5d} ADC_CH{args.adc_channel} avg={statistics.mean(vals):7.1f} min={min(vals):4d} max={max(vals):4d}')
        return 0
    finally:
        t.close()

if __name__ == '__main__':
    raise SystemExit(main())
