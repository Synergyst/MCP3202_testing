# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-build"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/tmp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/src"
  "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/mcp-adc/gpio-webui/mcu-adc/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
