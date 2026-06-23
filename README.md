# MCP3202 Testing on Raspberry Pi CM4

A small Raspberry Pi Compute Module 4 test project for reading a Microchip MCP3202 12-bit, 2-channel ADC and viewing live samples through a simple GPIO/Web UI server.

The MCP3202 is a dual-channel 12-bit SAR ADC with an SPI-compatible serial interface. It supports two single-ended inputs or one pseudo-differential pair, operates from 2.7 V to 5.5 V, and supports SPI modes 0,0 and 1,1 [1]. In this project it is used from a 3.3 V Raspberry Pi CM4, normally with SPI mode 0.

## Repository layout

```text
/root/mcp-adc
├── README.md
├── mcp3202_bitbang_test.cpp       # Standalone GPIO bit-banged MCP3202 diagnostic
├── mcp3202_bitbang_test           # Built diagnostic binary
├── mcp3202_spidev_test.cpp        # Standalone Linux spidev MCP3202 diagnostic
├── mcp3202_spidev_test            # Built diagnostic binary
└── gpio-webui
    ├── Makefile                   # Wrapper makefile
    ├── config.json                # Runtime GPIO config used from gpio-webui/
    └── gpio-webui
        ├── Makefile
        ├── cm4_gpio_server         # Built web server binary
        ├── *.cpp / *.hpp           # Server, GPIO, ADC sampler, MCP3202 driver
        ├── httplib.h               # Header-only HTTP server dependency
        └── config.json             # Alternate/local config file
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

The MCP3202 pin functions are: pin 1 `CS/SHDN`, pin 2 `CH0`, pin 3 `CH1`, pin 4 `VSS`, pin 5 `DIN`, pin 6 `DOUT`, pin 7 `CLK`, and pin 8 `VDD/VREF` [1]. `CS/SHDN` is pulled low to initiate communication and must be pulled high between conversions [1]. `DIN` receives channel/configuration bits, `DOUT` shifts out the conversion result, and data changes on the falling edge of the clock [1].

### Raspberry Pi SPI0 wiring

Current working wiring:

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

With `VDD/VREF = 3.3 V` and both inputs tied to 3.3 V, both channels should read near `4095`. The MCP3202 output code is proportional to `4096 * VIN / VDD`, using `VDD` as the reference voltage [1].

> Debug note: a reversed 3-pin CLK/DOUT/DIN header can make every hardware-SPI read return `RX: 00 00 00`. The known-good order is `CLK -> BCM11`, `DOUT -> BCM9`, `DIN -> BCM10`.

## Raspberry Pi SPI setup

Enable SPI0 in `/boot/config.txt`:

```ini
dtparam=spi=on
```

For this project's hardware-SPI mode with software-controlled CS on BCM8, free the kernel-owned CE pins by adding:

```ini
dtoverlay=spi0-0cs
```

Then reboot.

After reboot, verify the expected state:

```bash
ls -l /dev/spidev*
gpioinfo | grep -E 'line +(8|9|10|11)|SPI_CE0'
for p in 8 9 10 11; do raspi-gpio get $p; done
```

Expected highlights:

```text
/dev/spidev0.0 exists
GPIO 8 is free for software CS, then becomes output/high while the server runs
GPIO 9  = SPI0_MISO
GPIO 10 = SPI0_MOSI
GPIO 11 = SPI0_SCLK
```

## Build

### Web UI server

From the repository root:

```bash
cd /root/mcp-adc/gpio-webui
make
```

Or build directly in the source directory:

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
make
```

Clean generated objects/binary:

```bash
cd /root/mcp-adc/gpio-webui
make clean
```

### Standalone diagnostics

```bash
cd /root/mcp-adc

g++ -std=c++17 -O2 -Wall -Wextra -o mcp3202_bitbang_test mcp3202_bitbang_test.cpp

g++ -std=c++17 -O2 -Wall -Wextra -o mcp3202_spidev_test mcp3202_spidev_test.cpp
```

The web server links against `libgpiod`, `libgpiodcxx`, and pthreads.

## Run the web UI server

Known-good hardware-SPI launch command:

```bash
cd /root/mcp-adc/gpio-webui

./gpio-webui/cm4_gpio_server \
  --gpio-phys 32,40 \
  --adc-hw-spi \
  --adc-rate 30 \
  --spi-speed 900000 \
  --spi-dev /dev/spidev0.0 \
  --adc-cs-bcm 8
```

Then open:

```text
http://<cm4-ip-address>:8080/
```

Or query the ADC JSON endpoint locally:

```bash
curl -s http://127.0.0.1:8080/api/adc | python3 -m json.tool
```

Useful endpoints:

```text
GET  /             Web UI
GET  /api/status   GPIO state JSON
GET  /api/adc      ADC scope/sample JSON
POST /api/config   Update GPIO mode/state/timeout from UI
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

### Hardware SPI/spidev diagnostic

This test uses Linux spidev. If the ADC's CS pin is wired to BCM8 and using `dtoverlay=spi0-0cs`, make sure CS is driven low/high externally or use the web server's software-CS path. The source is useful for confirming the MCP3202 command framing.

```bash
cd /root/mcp-adc
./mcp3202_spidev_test --dev /dev/spidev0.0 --speed 900000 --samples 10
```

### GPIO bit-banged diagnostic

This test directly requests GPIO lines and bit-bangs the MCP3202 protocol:

```bash
cd /root/mcp-adc
./mcp3202_bitbang_test --cs 8 --clk 11 --din 10 --dout 9 --samples 10 --delay-us 2
```

For test wiring with `CH0 = 3.3 V` and `CH1 = GND`, the expected result is CH0 near 4095 and CH1 near 0. For both channels tied to 3.3 V, both should be near 4095.

## MCP3202 protocol notes

The MCP3202 communication is SPI-compatible and begins when `CS/SHDN` is pulled low [1]. The first clock received with `CS` low and `DIN` high is the start bit; this is followed by `SGL/DIFF`, `ODD/SIGN`, and `MSBF` configuration bits [1]. The ADC then outputs a null bit followed by the 12-bit result, MSB first [1].

The code uses 24 clocks / three 8-bit transfers:

```text
CH0 single-ended: TX = 0x01 0xA0 0x00
CH1 single-ended: TX = 0x01 0xE0 0x00
```

The result is decoded as:

```c
value = ((rx[1] & 0x0F) << 8) | rx[2];
```

This matches the datasheet's guidance for MCU SPI ports that transfer 8-bit segments, where leading zeros before the start bit are ignored and the returned bytes contain the null bit plus the high and low conversion bits [1].

## Reference

[1] Microchip Technology Inc., `MCP3202.pdf`, MCP3202 2.7V Dual Channel 12-Bit A/D Converter with SPI Serial Interface.
