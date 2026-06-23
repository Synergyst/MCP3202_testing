#!/bin/bash

cd /root/mcp-adc/gpio-webui/

/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --gpio-phys 32,40 --adc-hw-spi --adc-rate 8000 --spi-speed 1800000 --spi-dev /dev/spidev0.0 --adc-cs-bcm 8
