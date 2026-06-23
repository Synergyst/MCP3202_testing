# MCP3202 Testing on Raspberry Pi CM4

Raspberry Pi Compute Module 4 test project for reading a Microchip MCP3202 dual-channel 12-bit ADC, viewing live samples in a browser, controlling selected GPIO pins, and downloading captured ADC history as WAV audio.

The MCP3202 is a 12-bit successive-approximation ADC with onboard sample-and-hold circuitry and an SPI-compatible serial interface. It can be configured as two single-ended inputs or one pseudo-differential pair, operates from 2.7 V to 5.5 V, and supports SPI modes 0,0 and 1,1 [1]. At 5 V it supports up to 100 ksps, and at 2.7 V up to 50 ksps [1]. In this project it is used from a 3.3 V Raspberry Pi CM4, normally via Linux `spidev` plus a manually controlled chip-select GPIO.

## Current features

- MCP3202 CH0 + CH1 continuous sampler.
- Hardware-SPI MCP3202 driver using `/dev/spidev0.0`.
- Optional GPIO bit-banged MCP3202 mode.
- Browser dashboard with:
  - live dual-channel ADC scope,
  - latest raw and voltage values,
  - requested vs measured sample rate,
  - selected GPIO input/output controls,
  - GPIO transition/frequency display.
- WAV download from the ADC ring buffer:
  - mono CH0,
  - mono CH1,
  - stereo CH0 left + CH1 right,
  - mono mixed CH0 + CH1.
- Standalone MCP3202 diagnostic programs.

## Repository layout

```text
mcp-adc/
├── README.md
├── MCP3202.pdf
├── mcp3202_bitbang_test.cpp
├── mcp3202_bitbang_test
├── mcp3202_spidev_test.cpp
├── mcp3202_spidev_test
└── gpio-webui/
    ├── Makefile
    ├── config.json
    ├── start.sh
    └── gpio-webui/
        ├── Makefile
        ├── cm4_gpio_server
        ├── AdcSampler.cpp/.hpp
        ├── ConfigManager.cpp/.hpp
        ├── GpioManager.cpp/.hpp
        ├── MCP3202.cpp/.hpp
        ├── WebServer.cpp/.hpp
        ├── PinConfig.hpp
        ├── SystemContext.hpp
        ├── httplib.h
        └── config.json
```

## Hardware wiring

### MCP3202 pinout

Looking at the chip from above with the notch/dot at the top:

```text
       notch
    .--------.
CS  |1      8| VDD/VREF
CH0 |2      7| CLK
CH1 |3      6| DOUT
GND |4      5| DIN
    '--------'
```

Pin functions:

```text
1 CS/SHDN     Chip select / shutdown
2 CH0         Channel 0 analog input
3 CH1         Channel 1 analog input
4 VSS         Ground
5 DIN         SPI data input
6 DOUT        SPI data output
7 CLK         SPI serial clock
8 VDD/VREF    Power and ADC reference
```

The datasheet states that `CS/SHDN` initiates communication when pulled low, ends a conversion and places the device in low-power standby when pulled high, and must be pulled high between conversions [1]. `DIN` clocks in channel configuration data, while `DOUT` shifts out conversion results; output data changes on the falling edge of the clock [1].

### Raspberry Pi SPI0 wiring

Known-good wiring:

```text
MCP3202 pin 1 CS/SHDN  -> Pi BCM8  / physical pin 24
MCP3202 pin 5 DIN      -> Pi BCM10 / physical pin 19 / SPI0 MOSI
MCP3202 pin 6 DOUT     -> Pi BCM9  / physical pin 21 / SPI0 MISO
MCP3202 pin 7 CLK      -> Pi BCM11 / physical pin 23 / SPI0 SCLK
MCP3202 pin 8 VDD/VREF -> Pi 3.3 V
MCP3202 pin 4 VSS      -> Pi GND
```

For a simple full-scale test:

```text
MCP3202 pin 2 CH0 -> 3.3 V
MCP3202 pin 3 CH1 -> 3.3 V
```

With `VDD/VREF = 3.3 V` and an input tied to 3.3 V, the corresponding channel should read near `4095`. The MCP3202 uses `VDD` as its reference, and its theoretical output code is `4096 * VIN / VDD` [1].

> Debug note: a reversed 3-pin `CLK/DOUT/DIN` header can make every hardware-SPI read return `RX: 00 00 00`. The working order is `CLK -> BCM11`, `DOUT -> BCM9`, `DIN -> BCM10`.

## Raspberry Pi SPI setup

Enable SPI0 in `/boot/config.txt`:

```ini
dtparam=spi=on
```

For this project's recommended hardware-SPI mode with software-controlled CS on BCM8, also free the kernel-owned CE pins:

```ini
dtoverlay=spi0-0cs
```

Then reboot.

Verify after reboot:

```bash
ls -l /dev/spidev*
gpioinfo | grep -E 'line +(8|9|10|11)|SPI_CE0'
for p in 8 9 10 11; do raspi-gpio get $p; done
```

Expected highlights:

```text
/dev/spidev0.0 exists
GPIO 8 is free before the server starts, then output/high while the server owns software CS
GPIO 9  = SPI0_MISO
GPIO 10 = SPI0_MOSI
GPIO 11 = SPI0_SCLK
```

## Build

### Web UI server

From the wrapper directory:

```bash
cd /root/mcp-adc/gpio-webui
make
```

Or directly from the source directory:

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
make
```

Clean generated objects and server binary:

```bash
cd /root/mcp-adc/gpio-webui
make clean
```

The web server links against:

```text
libgpiodcxx
libgpiod
pthread
```

### Standalone diagnostics

```bash
cd /root/mcp-adc

g++ -std=c++17 -O2 -Wall -Wextra -o mcp3202_bitbang_test mcp3202_bitbang_test.cpp

g++ -std=c++17 -O2 -Wall -Wextra -o mcp3202_spidev_test mcp3202_spidev_test.cpp
```

## Run

### Recommended start script

```bash
cd /root/mcp-adc/gpio-webui
./start.sh
```

Current `start.sh` launches:

```bash
/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server \
  --gpio-phys 32,40 \
  --adc-hw-spi \
  --adc-rate 8000 \
  --spi-speed 1800000 \
  --spi-dev /dev/spidev0.0 \
  --adc-cs-bcm 8
```

### Manual launch example

```bash
cd /root/mcp-adc/gpio-webui

./gpio-webui/cm4_gpio_server \
  --gpio-phys 32,40 \
  --adc-hw-spi \
  --adc-rate 8000 \
  --adc-history 32000 \
  --spi-speed 1800000 \
  --spi-dev /dev/spidev0.0 \
  --adc-cs-bcm 8
```

Open the dashboard:

```text
http://<cm4-ip-address>:8080/
```

Local JSON test:

```bash
curl -s http://127.0.0.1:8080/api/adc | python3 -m json.tool
```

## Browser WAV export

The web UI has a WAV capture panel under the ADC scope. Choose:

- duration in milliseconds,
- mono CH0,
- mono CH1,
- stereo CH0+CH1,
- mono mixed CH0+CH1,

then click **Download WAV**.

The server exports the newest requested duration from the ADC ring buffer as signed 16-bit PCM WAV. Raw 12-bit ADC samples are centered around code 2048 and scaled to signed 16-bit audio.

The WAV header uses the **measured** effective sample rate, not just the requested sample rate. This avoids pitch shift if Linux userspace does not manage to hit the requested ADC frame rate exactly.

Direct endpoint examples:

```bash
# Mono CH0, latest 1 second
curl -o ch0.wav 'http://127.0.0.1:8080/api/adc/wav?ms=1000&mode=ch0'

# Mono CH1, latest 2 seconds
curl -o ch1.wav 'http://127.0.0.1:8080/api/adc/wav?ms=2000&mode=ch1'

# Stereo CH0 left / CH1 right, latest 5 seconds
curl -o stereo.wav 'http://127.0.0.1:8080/api/adc/wav?ms=5000&mode=stereo'

# Mono mix of CH0 and CH1, latest 1 second
curl -o mix.wav 'http://127.0.0.1:8080/api/adc/wav?ms=1000&mode=mix'
```

Supported WAV modes:

```text
ch0      mono CH0
ch1      mono CH1
stereo   stereo CH0 left + CH1 right
mix      mono average of CH0 + CH1
```

The `/api/adc` response includes both:

```json
{
  "sample_rate_hz": 8000,
  "measured_sample_rate_hz": 6760
}
```

The dashboard displays this as actual vs requested rate.

## HTTP endpoints

```text
GET  /                 Browser dashboard
GET  /api/status       GPIO state JSON
GET  /api/adc          ADC status, latest values, and decimated scope history
GET  /api/adc/wav      WAV download from ADC ring buffer
POST /api/config       Update GPIO mode/state/timeout from UI
```

`/api/adc/wav` query parameters:

```text
ms=<duration_ms>       Duration to export from recent ring-buffer history
mode=ch0|ch1|stereo|mix
```

## Server options

```text
Modes:
  default ADC graph mode + GPIO controls except reserved ADC/SPI pins
  --gpio-only | --full-gpio | --adc-disable
      Disable ADC/graph and expose GPIO controls only

GPIO selection:
  --gpio-phys LIST       Physical header pins, e.g. 32,40
  --gpio-bcm LIST        BCM GPIO list

ADC:
  --adc-bitbang          Use GPIO bit-banged SPI
  --adc-hw-spi           Use Linux spidev hardware SPI
  --adc-rate HZ          Two-channel frame rate
  --adc-history N        History samples per channel
  --adc-vref VOLTS       Reference voltage used for display
  --spi-dev PATH         Default /dev/spidev0.0
  --spi-speed HZ         SPI clock speed
  --adc-cs-bcm N         Software CS BCM GPIO; use -1 for controller CE
  --adc-clk-bcm N        ADC CLK GPIO, default BCM11
  --adc-mosi-bcm N       ADC DIN/MOSI GPIO, default BCM10
  --adc-miso-bcm N       ADC DOUT/MISO GPIO, default BCM9
  --adc-gpio-chip N      gpiochip number string, default 0

Server/config:
  --config PATH          JSON config path, default config.json
  --host ADDR            Listen address, default 0.0.0.0
  --port PORT            Listen port, default 8080
```

## Diagnostic commands

### Hardware SPI / spidev diagnostic

```bash
cd /root/mcp-adc
./mcp3202_spidev_test --dev /dev/spidev0.0 --speed 900000 --samples 10
```

This diagnostic is useful for confirming the command framing and SPI receive path. If using `dtoverlay=spi0-0cs`, remember that the MCP3202 still needs its CS pin driven low/high by some software-controlled GPIO during conversion.

### GPIO bit-banged diagnostic

```bash
cd /root/mcp-adc
./mcp3202_bitbang_test --cs 8 --clk 11 --din 10 --dout 9 --samples 10 --delay-us 2
```

For test wiring with `CH0 = 3.3 V` and `CH1 = GND`, expected output is CH0 near 4095 and CH1 near 0. For both channels tied to 3.3 V, both should be near 4095.

## MCP3202 protocol notes

Communication starts by bringing `CS` low. The first clock received with `CS` low and `DIN` high is the start bit. The command then sends `SGL/DIFF`, `ODD/SIGN`, and `MSBF`; the ADC outputs a null bit followed by the 12-bit conversion result MSB-first [1].

This code uses the 24-clock / three-byte hardware-SPI framing described for 8-bit MCU SPI ports. The first transmitted byte contains seven leading zeros followed by the start bit; those leading zeros are ignored by the device [1].

Single-ended command bytes:

```text
CH0: 0x01 0xA0 0x00
CH1: 0x01 0xE0 0x00
```

Decode:

```c
value = ((rx[1] & 0x0F) << 8) | rx[2];
```

The MCP3202 requires enough SPI clock speed to avoid sample-capacitor droop during conversion. The datasheet notes that effective clock rates below 10 kHz can affect linearity, especially at elevated temperatures, because the sampled charge can bleed off before all 12 bits are clocked out [1].

## Reference

[1] Microchip Technology Inc., `MCP3202.pdf`, MCP3202 2.7V Dual Channel 12-Bit A/D Converter with SPI Serial Interface.
