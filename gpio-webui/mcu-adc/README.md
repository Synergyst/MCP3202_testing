# RP2040-Zero MCP3202 ADC Frontend

This directory contains firmware for a Waveshare RP2040-Zero to act as a deterministic MCP3202 ADC frontend for `cm4_gpio_server`.

The goal is to move audio-rate sample timing off Linux/spidev and onto the RP2040. The RP2040 samples both MCP3202 channels at a fixed frame rate and streams binary frames to the CM4 over USB CDC (`/dev/ttyACM0`).

## Why this design

The MCP3202 is a 12-bit SPI ADC with two single-ended analog channels. It supports SPI modes 0,0 and 1,1, operates from 2.7 V to 5.5 V, and can convert up to 100 ksps at 5 V or 50 ksps at 2.7 V [1]. It also requires `CS/SHDN` to be pulled high between conversions [1].

At 16 kHz two-channel frames, the ADC must perform 32 k conversions/s, which is within the MCP3202 limit at 3.3 V. The RP2040 handles that sample clock much more deterministically than Linux userspace.

## Default wiring: RP2040-Zero to MCP3202

Default firmware pins use `spi0` on the top-right pins shown in the Waveshare RP2040-Zero pinout:

| MCP3202 pin | Function | RP2040-Zero default |
|---|---|---|
| 1 CS/SHDN | chip select | GP1 |
| 2 CH0 | analog input 0 | your signal |
| 3 CH1 | analog input 1 | your signal |
| 4 VSS | ground | GND |
| 5 DIN | SPI MOSI into ADC | GP3 / SPI0 TX |
| 6 DOUT | SPI MISO from ADC | GP0 / SPI0 RX |
| 7 CLK | SPI clock | GP2 / SPI0 SCK |
| 8 VDD/VREF | power/reference | 3V3 |

Also connect CM4/RP2040/MCP3202 grounds together.

Recommended analog-side basics:

- Put a 0.1 uF bypass capacitor close to MCP3202 VDD/VSS.
- Keep SPI clock wiring away from analog input wiring.
- Use a low impedance signal source or buffer/filter the analog inputs. The datasheet notes source impedance affects acquisition of the internal sample capacitor and recommends buffering/filtering for higher impedance sources [1].
- Use an anti-aliasing low-pass filter appropriate for your chosen sample rate.

## Default firmware settings

In `CMakeLists.txt`:

```cmake
MCU_ADC_SAMPLE_RATE_HZ=16000
MCU_ADC_SPI_BAUD=1600000
MCU_ADC_PIN_MISO=0
MCU_ADC_PIN_CS=1
MCU_ADC_PIN_SCK=2
MCU_ADC_PIN_MOSI=3
MCU_ADC_PACKET_FRAMES=128
```

Each output frame is:

```c
struct sample_frame {
    uint32_t seq;
    uint16_t ch0; // low 12 bits valid
    uint16_t ch1; // low 12 bits valid
};
```

Packets are binary:

```c
struct packet_header {
    uint32_t magic;          // 'ADC2' little endian, bytes 41 44 43 32
    uint16_t version;        // 1
    uint16_t header_bytes;   // sizeof(packet_header)
    uint32_t sample_rate_hz; // e.g. 16000
    uint32_t frame_count;    // usually 128
    uint32_t sequence_start;
    uint32_t flags;
    uint32_t lost_frames;
    uint32_t reserved;
};
struct sample_frame frames[frame_count];
uint32_t crc32;              // CRC32 over header+frames
```

Flags:

- bit 0: RP2040 ring overflow occurred
- bit 1: sequence gap detected before transmission

## Build

Install Pico SDK and ARM embedded toolchain if not already installed. On this CM4 they are not currently installed.

Typical build:

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc
export PICO_SDK_PATH=/path/to/pico-sdk
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Output:

```text
build/mcu_adc.uf2
```

## Flash RP2040-Zero

1. Hold BOOT on the RP2040-Zero.
2. Tap/reset or plug in USB while holding BOOT.
3. It should mount as `RPI-RP2`.
4. Copy UF2:

```bash
cp build/mcu_adc.uf2 /media/*/RPI-RP2/
```

After reboot it should appear as `/dev/ttyACM0`.

## Quick stream test

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc
python3 tools/read_mcu_adc.py --dev /dev/ttyACM0 --seconds 10
```

If `pyserial` is missing, the script falls back to a raw file read.

Expected after firmware is flashed and MCP3202 is wired:

```text
avg_fps ~= 16000
crc_bad=0
gaps=0
flags=0x00000000
```

## Next host integration

Next step in `cm4_gpio_server` is adding a new ADC backend/source, e.g.:

```bash
--adc-source rp2040
--rp2040-dev /dev/ttyACM0
--adc-rate 16000
```

The host should read this binary stream and fill the existing ADC ring buffer using `sample_rate_hz` from the RP2040 packet header. WAV export, preview, FFT, filters, and FSK should then use this fixed RP2040-provided sample rate.
