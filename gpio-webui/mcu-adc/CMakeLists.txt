cmake_minimum_required(VERSION 3.13)

# Import Pico SDK. Set PICO_SDK_PATH or place pico_sdk_import.cmake next to this file.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)
elseif(DEFINED ENV{PICO_SDK_PATH})
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
    message(FATAL_ERROR "Pico SDK not found. Set PICO_SDK_PATH or copy pico_sdk_import.cmake here.")
endif()

project(mcu_adc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(mcu_adc
    src/main.c
)

target_include_directories(mcu_adc PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(mcu_adc PRIVATE
    # Waveshare RP2040-Zero + MCP3202 default wiring. Override with cmake -D if desired.
    MCU_ADC_SAMPLE_RATE_HZ=16000
    MCU_ADC_SPI_BAUD=1600000
    MCU_ADC_SPI_PORT=0
    MCU_ADC_PIN_MISO=0
    MCU_ADC_PIN_CS=1
    MCU_ADC_PIN_SCK=2
    MCU_ADC_PIN_MOSI=3
    MCU_ADC_PACKET_FRAMES=128
    MCU_ADC_RING_FRAMES=4096
    # Optional MCP4922 DAC on the other RP2040 SPI controller by default.
    MCU_DAC_ENABLE=1
    MCU_DAC_SPI_BAUD=10000000
    MCU_DAC_SPI_PORT=1
    MCU_DAC_PIN_CS=13
    MCU_DAC_PIN_SCK=14
    MCU_DAC_PIN_MOSI=15
    MCU_DAC_PIN_LDAC=-1
    MCU_DAC_PIN_SHDN=-1
    MCU_DAC_DEFAULT_RATE_HZ=48000
    MCU_DAC_MAX_RATE_HZ=100000
)

target_link_libraries(mcu_adc
    pico_stdlib
    pico_multicore
    hardware_spi
)

pico_enable_stdio_usb(mcu_adc 1)
pico_enable_stdio_uart(mcu_adc 0)

pico_add_extra_outputs(mcu_adc)
