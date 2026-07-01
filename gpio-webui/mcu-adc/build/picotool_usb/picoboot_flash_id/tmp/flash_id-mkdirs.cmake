# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-src/picoboot_flash_id"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/tmp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/src/flash_id-stamp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/src/flash_id-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/src/flash_id-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/picoboot_flash_id/src/flash_id-stamp${cfgdir}") # cfgdir has leading slash
endif()
