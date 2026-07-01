# RP2040 ADC host integration

The RP2040 ADC source is now integrated into `cm4_gpio_server`.

This file is kept as a concise host-side reference for the active integration. See `README.md` for firmware build, wiring, flashing, and test details.

## Server CLI

RP2040 USB CDC source:

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
./cm4_gpio_server \
  --adc-source rp2040 \
  --adc-rp2040-dev /dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00 \
  --adc-rate 16000 \
  --port 8080
```

Linux SPI source remains available:

```bash
./cm4_gpio_server --adc-source mcp3202-spidev
```

Persisted config keys:

```json
{
  "adc_source": "rp2040",
  "rp2040_dev": "/dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00",
  "adc_sample_rate_hz": "16000"
}
```

RP2040 peripheral pins and source selection are persisted under top-level `mcu_peripherals`, separate from CM4 physical-header `pins`:

```json
{
  "mcu_peripherals": {
    "enabled": true,
    "dtmf_decoder": {
      "enabled": true,
      "source": "mcu_mt8870",
      "pins": { "stq": 12, "q1": 27, "q2": 26, "q3": 10, "q4": 11 },
      "polarity": { "stq_active_high": true, "q_active_high": true },
      "debounce_ms": 2,
      "event_holdoff_ms": 25,
      "history_limit": 64
    },
    "ch1817_signals": {
      "ri": { "source": "mcu", "gpio": 8, "active_high": false },
      "oh": { "source": "mcu", "gpio": 7, "active_high": true }
    }
  }
}
```

## Runtime API

Update ADC source/device/rate:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/adc/config' \
  -H 'Content-Type: application/json' \
  -d '{
        "adc_source": "rp2040",
        "rp2040_dev": "/dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00",
        "sample_rate_hz": 16000
      }'
```

The endpoint preserves the existing sampler configuration and persists:

- `adc_source`
- `rp2040_dev`
- `adc_sample_rate_hz`

## `/api/adc` RP2040 diagnostics

RP2040-specific fields exposed by `/api/adc` include:

```json
{
  "adc_source": "rp2040",
  "rp2040_connected": true,
  "rp2040_packets_ok": 12345,
  "rp2040_packets_crc_bad": 0,
  "rp2040_sequence_gaps": 0,
  "rp2040_firmware_lost_frames": 0,
  "rp2040_firmware_flags": 0,
  "rp2040_dev": "/dev/serial/by-id/...",
  "rp2040_declared_rate_hz": 16000
}
```

Healthy RP2040 operation should have:

```text
healthy=true
rp2040_connected=true
rp2040_packets_crc_bad=0
rp2040_sequence_gaps=0
rp2040_firmware_lost_frames=0
rp2040_firmware_flags=0
last_error=""
```

## Protocol summary

All numeric fields are little-endian.

Packet magic bytes:

```text
41 44 43 32   # 'ADC2'
```

Header:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;          // 0x32434441
    uint16_t version;        // 1
    uint16_t header_bytes;   // 32
    uint32_t sample_rate_hz;
    uint32_t frame_count;
    uint32_t sequence_start;
    uint32_t flags;
    uint32_t lost_frames;
    uint32_t reserved;
} packet_header_t;
```

Frame:

```c
typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t ch0;
    uint16_t ch1;
} sample_frame_t;
```

Packet tail:

```c
uint32_t crc32; // CRC32 over header + frames
```

## MCU peripheral APIs

The server programs the MCU at startup, reconnect, and WebUI config changes using GWP1. Useful endpoints:

```text
GET  /api/mcu/peripherals/status
POST /api/mcu/peripherals/config
GET  /api/dac/dtmf/decoder/status
POST /api/dac/dtmf/decoder/history/clear
```

`/api/status` also includes `mcu_peripherals` so browser polling can update live raw pin state and config-send status.

## Web UI

The Scope tab now includes:

- ADC source selector.
- RP2040 device-path input.
- sample-rate input with practical presets.
- RP2040 diagnostics line.
- measured/requested/firmware rate display.
- warning when measured rate differs significantly from requested rate.

WAV exports use source-aware filenames such as:

```text
rp2040_ch0_dry_250ms_16149hz.wav
```

The DAC/DTMF area includes:

- MCU peripheral config editor for MT8870 `StQ/Q1-Q4`, CH1817 RI, and CH1817 OH/OFFHK.
- DTMF decoder live raw pin monitor.
- Last-N decoded DTMF history with clear/copy controls.
- Guided expected-sequence validation and Q-pin mapping suggestion.

The Telephony tab uses CH1817 state from the selected source. With `ri.source=mcu`, RI/ringing is derived from MCU GP8 active-low by default. With `oh.source=mcu`, off-hook requests drive MCU GP7 active-high by default.

## Recommended rates

Use:

```text
8000, 16000, 24000
```

Recommended default:

```text
16000
```

Rates above 24000 may be accepted by the firmware command parser, but the current tested implementation plateaus around ~24 kframes/s.
