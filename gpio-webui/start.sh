#!/bin/bash

cd /root/mcp-adc/gpio-webui/

/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --adc-hw-spi --adc-rate 8000 --spi-speed 1800000 --spi-dev /dev/spidev0.0 --adc-cs-bcm 8 --gpio-phys 11 --adc-realtime --adc-rt-priority 99 --adc-cpu 3
#/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --adc-hw-spi --adc-rate 22050 --spi-speed 1800000 --spi-dev /dev/spidev0.0 --adc-cs-bcm 8 --gpio-phys 11,15,32,36,37,38,40 --adc-realtime --adc-rt-priority 20 --adc-cpu 3
# Experimental only if the ADC CS wire is truly on SPI0 CE0 and hardware CS returns valid non-zero samples:
#/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --adc-hw-spi --adc-rate 3600 --spi-speed 1800000 --spi-dev /dev/spidev0.0 --adc-cs-bcm -1 --gpio-phys 11 --adc-realtime --adc-rt-priority 20 --adc-cpu 3
