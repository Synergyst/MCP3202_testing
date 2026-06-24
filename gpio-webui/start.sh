#!/bin/bash

cd /root/mcp-adc/gpio-webui/

/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --adc-hw-spi --adc-rate 8000 --spi-speed 1800000 --spi-dev /dev/spidev0.0 --adc-cs-bcm 8 --gpio-phys 11,13,15,32,36,37,38,40
