# MCP4922 DAC Integration

The MCP4922 is a dual 12-bit voltage-output DAC using a unidirectional SPI-compatible serial interface. The project driver is implemented in:

```text
gpio-webui/MCP4922.hpp
gpio-webui/MCP4922.cpp
```

The native Pi DAC path is optional and disabled by default. MCU DAC mode is implemented by the RP2040 firmware in `mcu-adc/src/main.c` and uses GWP1 packets over USB CDC.

## Datasheet-derived behavior

The MCP4922 family provides dual voltage-output DACs. MCP4922 is the 12-bit variant and supports an SPI interface with up to a 20 MHz clock [1]. It operates from a 2.7V to 5.5V supply [1].

The serial interface supports SPI mode 0,0 and 1,1. Data is clocked in on the rising edge of SCK, CS must remain low for the full write command, and each write command is 16 bits [1].

MCP4922 16-bit command word:

```text
bit 15    A/B   0 = DACA, 1 = DACB
bit 14    BUF   0 = unbuffered VREF, 1 = buffered VREF
bit 13    GA    1 = 1x gain, 0 = 2x gain
bit 12    SHDN  1 = active, 0 = shutdown selected channel
bits 11:0 DATA  12-bit DAC code
```

The ideal output equation is:

```text
VOUT = VREF * Dn / 2^n * G
```

For MCP4922, `n=12`; `G=1` when GA=1 and `G=2` when GA=0 [1].

LDAC can be used to synchronously transfer input registers to DAC output registers. If LDAC is tied low, outputs update when CS rises after a valid command [1]. SHDN is active-low and can shut down both DAC channels in hardware [1].

## Default host wiring/config

Defaults are chosen to avoid conflicting with the MCP3202 ADC default CE0 wiring:

```text
SPI device: /dev/spidev0.1
CS:         BCM7 / SPI0 CE1 / physical pin 26
SCK:        BCM11 / physical pin 23
MOSI/SDI:   BCM10 / physical pin 19
LDAC:       -1 (not controlled; tie low or wire and configure)
SHDN:       -1 (not controlled; tie high or wire and configure)
```

The MCP4922 has no MISO pin.

## CLI

Enable and persist DAC settings:

```bash
./cm4_gpio_server --dac-enable
```

Disable and persist:

```bash
./cm4_gpio_server --dac-disable
```

Useful options:

```text
--dac-bitbang
--dac-hw-spi
--dac-spi-dev PATH
--dac-spi-speed HZ
--dac-cs-bcm N
--dac-clk-bcm N
--dac-mosi-bcm N
--dac-ldac-bcm N
--dac-shdn-bcm N
--dac-vref VOLTS
--dac-vref-a VOLTS
--dac-vref-b VOLTS
--dac-gain-a 1|2
--dac-gain-b 1|2
--dac-buffered-a 0|1
--dac-buffered-b 0|1
```

When any DAC CLI flag is supplied, the resolved DAC configuration is written to `config.json` using `dac_*` keys.

## Config keys

```json
{
  "dac_enabled": "false",
  "dac_bitbang": "false",
  "dac_spi_dev": "/dev/spidev0.1",
  "dac_spi_speed_hz": "10000000",
  "dac_cs_bcm": "7",
  "dac_clk_bcm": "11",
  "dac_mosi_bcm": "10",
  "dac_ldac_bcm": "-1",
  "dac_shdn_bcm": "-1",
  "dac_vref_a": "3.300000",
  "dac_vref_b": "3.300000",
  "dac_gain_a": "1",
  "dac_gain_b": "1",
  "dac_buffered_a": "false",
  "dac_buffered_b": "false"
}
```

## Startup behavior

- DAC disabled: no hardware is opened and no pins are reserved.
- DAC enabled: pins are reserved from GPIO UI control, but the driver opens hardware lazily on first write.

This allows the software to be configured before the DAC is physically wired.

## Transport modes

### Native Pi SPI

Native mode uses the host `MCP4922` driver directly. This is the lowest-latency path for one-shot output values and future buffered output generated on the CM4. It uses `/dev/spidev*` or GPIO bit-bang according to the `dac_*` config keys above.

### MCU/GWP1 over USB

MCU mode sends DAC commands and data to an RP2040 running the `mcu-adc` firmware. The RP2040 owns the MCP4922 SPI bus and reports DAC capability via GWP1 `CAPS` with `GW_DEVICE_CLASS_DAC` and `dac_streams=1`.

Default RP2040 DAC wiring:

```text
CS:       GPIO13
SCK:      GPIO14 / SPI1 SCK
MOSI/SDI: GPIO15 / SPI1 TX
LDAC:     -1, tie low unless configured
SHDN:     -1, tie high unless configured
```

GWP1 DAC controls are defined in `protocol/gw_protocol.h` and documented in `protocol/PROTOCOL.md`:

```text
GW_OP_DAC_SET_RATE
GW_OP_DAC_SET_FORMAT
GW_OP_DAC_WRITE_FRAME
GW_OP_DAC_WRITE_BLOCK
GW_OP_DAC_STREAM_START
GW_OP_DAC_STREAM_STOP
GW_OP_DAC_FLUSH
GW_OP_DAC_GET_STATUS
```

DAC `DATA` packets use `gw_dac_data_payload_t` followed by interleaved samples. For `GW_SAMPLE_U16_LE`, only the low 12 bits of each channel word are sent to the MCP4922.

## Future work

The remaining higher-level work is to connect web/API DAC controls to a generic server-side DAC sink abstraction that can select either native Pi SPI or MCU/GWP1 transport at runtime.
