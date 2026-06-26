# Host integration plan: `--adc-source rp2040`

Firmware side is now defined in this directory. The next `cm4_gpio_server` changes should add an RP2040 USB ADC backend.

## Proposed CLI

```bash
cm4_gpio_server \
  --adc-source rp2040 \
  --rp2040-dev /dev/ttyACM0 \
  --adc-rate 16000
```

Keep existing direct SPI path as:

```bash
--adc-source mcp3202-spidev
```

For backwards compatibility, `--adc-hw-spi` should map to `mcp3202-spidev`.

## Protocol summary

Binary packets start with bytes:

```text
41 44 43 32   # 'ADC2'
```

Header, little endian:

```c
struct packet_header {
    uint32_t magic;          // 'ADC2'
    uint16_t version;        // 1
    uint16_t header_bytes;   // 32
    uint32_t sample_rate_hz; // 16000 default
    uint32_t frame_count;    // 128 default
    uint32_t sequence_start;
    uint32_t flags;
    uint32_t lost_frames;
    uint32_t reserved;
};
```

Frame, little endian:

```c
struct sample_frame {
    uint32_t seq;
    uint16_t ch0;
    uint16_t ch1;
};
```

Then `uint32_t crc32` over header + frames.

## Host behavior

- Open `/dev/ttyACM0` raw, nonblocking or blocking reader thread.
- Sync to magic.
- Verify header size/version/frame_count.
- Read payload and CRC.
- Verify CRC.
- Detect sequence gaps.
- Push frames into existing ADC ring at `sample_rate_hz` from packet header.
- Report RP2040 flags/lost frames in `/api/adc`.

## Audio correctness

When RP2040 mode is active, all WAV export, preview, filters, FFT, and Caller ID/FSK logic should use the firmware-declared `sample_rate_hz`, normally 16000. That should fix pitch/timebase errors caused by Linux userspace sampling drift.
