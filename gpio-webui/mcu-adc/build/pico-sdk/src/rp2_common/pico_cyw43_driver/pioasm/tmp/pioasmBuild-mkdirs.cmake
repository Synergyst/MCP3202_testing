# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/pico/pico-sdk/tools/pioasm"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pioasm"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pioasm-install"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/tmp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
