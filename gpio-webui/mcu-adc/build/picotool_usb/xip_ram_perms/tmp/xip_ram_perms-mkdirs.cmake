# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-src/xip_ram_perms"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/tmp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/src/xip_ram_perms-stamp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/src/xip_ram_perms-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/src/xip_ram_perms-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/xip_ram_perms/src/xip_ram_perms-stamp${cfgdir}") # cfgdir has leading slash
endif()
