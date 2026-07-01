#!/bin/bash
set -euo pipefail

cd /root/mcp-adc/gpio-webui/

# Target hardware wiring:
#   RPi CM4 GPIO: CH1817 RI/OH only (no ADC/DAC on CM4 GPIO)
#   RP2040 MCU: MCP3202 ADC on SPI0 GP0..GP3, MCP4922 DAC on SPI1 GP13..GP15
#   ADC/DAC control/data over the same RP2040 USB CDC device.
#
# Use a stable /dev/serial/by-id path here if available; /dev/ttyACM1 is the
# currently observed device on this host.
RP2040_DEV="${RP2040_DEV:-/dev/ttyACM0}"

exec /root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server \
  --gpio-phys 11 \
  --adc-source rp2040 \
  --adc-rp2040-dev "$RP2040_DEV" \
  --adc-max-buffer-mb 256 \
  --adc-history-ms 60000 \
  --dac-enable \
  --dac-transport rp2040 \
  --dac-rp2040-dev "$RP2040_DEV" \
  --dac-start-raw-a 2048 \
  --dac-start-raw-b 2048
