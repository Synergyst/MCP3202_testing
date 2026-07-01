# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-src/otp_header_parser"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/tmp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/src/otp_header_parser-stamp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/src/otp_header_parser-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/src/otp_header_parser-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/picotool_usb/otp_header_parser/src/otp_header_parser-stamp${cfgdir}") # cfgdir has leading slash
endif()
