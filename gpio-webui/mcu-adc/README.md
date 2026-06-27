# RP2040 MCP3202 ADC Frontend

This project contains RP2040 firmware that turns a Pico/RP2040-Zero into a deterministic USB ADC frontend for the `gpio-webui` / `cm4_gpio_server` application.

The RP2040 samples both MCP3202 channels using SPI, packetizes the samples with sequence numbers and CRC32, and streams them to the CM4 over USB CDC. The host server can use this stream as an ADC source via `adc_source = rp2040`, replacing Linux userspace SPI timing with MCU-side sampling.

## Current status

The RP2040 ADC source is integrated and tested end-to-end with `cm4_gpio_server`.

Implemented and validated:

- RP2040 dual-channel MCP3202 sampling firmware.
- Binary `ADC2` USB CDC packet protocol.
- CRC32 packet validation on host.
- Sequence-gap and firmware lost-frame diagnostics.
- Runtime sample-rate command from host to RP2040.
- Host ADC backend selected with `--adc-source rp2040` or persisted config.
- Web UI ADC source/rate/device controls.
- Web UI RP2040 diagnostics.
- Source-aware WAV export filenames.
- Stable tested default: `16000` frames/s.
- Practical tested maximum: `24000` frames/s. Higher requested rates are accepted but may not be physically achieved by the current firmware/hardware path.

## Default hardware wiring

Default firmware pins use RP2040 `spi0`:

| MCP3202 pin | Function | RP2040 default |
|---|---|---|
| 1 CS/SHDN | Chip select | GPIO1 |
| 2 CH0 | Analog input 0 | Signal input |
| 3 CH1 | Analog input 1 | Signal input |
| 4 VSS | Ground | GND |
| 5 DIN | SPI MOSI into ADC | GPIO3 / SPI0 TX |
| 6 DOUT | SPI MISO from ADC | GPIO0 / SPI0 RX |
| 7 CLK | SPI clock | GPIO2 / SPI0 SCK |
| 8 VDD/VREF | Power/reference | 3V3 |

Also connect CM4, RP2040, and MCP3202 grounds together.

Recommended analog-side basics:

- Place a 0.1 uF bypass capacitor close to MCP3202 VDD/VSS.
- Keep SPI clock wiring away from analog input wiring.
- Use a low-impedance source or buffer/filter analog inputs where appropriate.
- Add anti-aliasing filtering appropriate for the selected sample rate.

## Firmware defaults

Configured in `CMakeLists.txt`:

```cmake
MCU_ADC_SAMPLE_RATE_HZ=16000
MCU_ADC_SPI_BAUD=1600000
MCU_ADC_SPI_PORT=0
MCU_ADC_PIN_MISO=0
MCU_ADC_PIN_CS=1
MCU_ADC_PIN_SCK=2
MCU_ADC_PIN_MOSI=3
MCU_ADC_PACKET_FRAMES=128
MCU_ADC_RING_FRAMES=4096
```

Notes:

- `MCU_ADC_PACKET_FRAMES=128` is the tested packet size.
- `MCU_ADC_RING_FRAMES=4096` is the tested firmware ring size.
- Runtime sample-rate changes are sent by the host as `S<rate>\n` over the same USB CDC interface.
- Valid firmware command range is `1..100000`, but tested production rates should be limited to `8000`, `16000`, and `24000` unless firmware timing is further optimized.

## USB packet protocol

All numeric fields are little-endian.

Packet header:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;          // 'ADC2' little-endian: 0x32434441
    uint16_t version;        // 1
    uint16_t header_bytes;   // 32
    uint32_t sample_rate_hz; // firmware-declared current rate
    uint32_t frame_count;    // usually 128
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
    uint16_t ch0; // low 12 bits valid
    uint16_t ch1; // low 12 bits valid
} sample_frame_t;
```

Packet layout:

```text
packet_header_t header
sample_frame_t frames[header.frame_count]
uint32_t crc32   # CRC32 over header + frames
```

Firmware flags:

- bit 0: RP2040 ring overflow occurred.
- bit 1: firmware detected a counter gap before transmission.

## Build

On the CM4 used for this project, the Pico SDK is available at:

```bash
/root/pico/pico-sdk
```

Build:

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc
cmake -S . -B build
cmake --build build -j4
```

Output artifacts include:

```text
build/mcu_adc.uf2
build/mcu_adc.elf
build/mcu_adc.bin
```

## Flash

Put the RP2040 into BOOTSEL mode so it appears as `RPI-RP2`, then copy the UF2:

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc
mkdir -p /mnt/rpi-rp2
mount /dev/sda1 /mnt/rpi-rp2   # adjust if the RP2040 appears elsewhere
cp build/mcu_adc.uf2 /mnt/rpi-rp2/
sync
umount /mnt/rpi-rp2
```

After reboot, the device should enumerate as USB CDC, for example:

```text
/dev/ttyACM0
/dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00
```

Prefer the `/dev/serial/by-id/...` path for persisted server config.

## Raw stream test

A low-level packet reader is available:

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc
python3 tools/read_mcu_adc.py --dev /dev/ttyACM0 --seconds 10
```

Expected healthy output:

```text
crc_bad=0
gaps=0
flags=0x00000000
```

## Host/server usage

Start the server in RP2040 mode:

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
./cm4_gpio_server \
  --adc-source rp2040 \
  --adc-rp2040-dev /dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00 \
  --adc-rate 16000 \
  --port 8080
```

The server also reads persisted config keys:

```json
{
  "adc_source": "rp2040",
  "rp2040_dev": "/dev/serial/by-id/usb-Raspberry_Pi_Pico_50355860629A201F-if00",
  "adc_sample_rate_hz": "16000"
}
```

The Web UI Scope tab exposes ADC source, RP2040 device path, and sample-rate controls.

## Host monitor tool

A repeatable host-side health monitor is available:

```bash
python3 /root/mcp-adc/gpio-webui/mcu-adc/tools/monitor_host_adc.py \
  --url http://127.0.0.1:8080/api/adc \
  --seconds 60 \
  --json-summary
```

It reports and fails on:

- RP2040 disconnects.
- ADC unhealthy state.
- CRC error increments.
- sequence-gap increments.
- firmware lost-frame increments.
- nonzero firmware flags.

## Tested rates

Short and stress testing showed:

| Requested rate | Result | Notes |
|---:|---|---|
| 8000 | Pass | Stable |
| 16000 | Pass | Recommended default |
| 24000 | Pass | Practical tested maximum |
| 32000+ | Stream remains healthy | Actual measured throughput plateaus around ~24 kframes/s |

Recommended production default:

```text
16000 frames/s
```

Recommended UI/API presets:

```text
8000, 16000, 24000
```
