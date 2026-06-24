#!/bin/bash

cd /root/mcp-adc/gpio-webui/

/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server --adc-hw-spi --adc-rate 8000 --spi-speed 1800000
