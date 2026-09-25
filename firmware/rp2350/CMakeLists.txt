cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a pinned Raspberry Pi Pico SDK checkout.")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(rabbit_rp2350 LANGUAGES C CXX ASM)
pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../protocol ${CMAKE_CURRENT_BINARY_DIR}/protocol)

set(RABBIT_LED_PIN -1 CACHE STRING "External WS2812 data pin; -1 disables output until verified")
set(RABBIT_AUDIO_PIN -1 CACHE STRING "External active-buzzer pin; -1 disables audio until verified")
set(RABBIT_LOCAL_STOP_PIN -1 CACHE STRING "Local stop input pin; -1 disables it until verified")

add_executable(rabbit_rp2350
    src/main.cpp
    src/uart_workout_link.cpp
    src/ws2812_dma.cpp
)
set_source_files_properties(src/ws2812_dma.cpp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(rabbit_rp2350 PRIVATE include)
target_link_libraries(rabbit_rp2350 PRIVATE
    rabbit_core
    rabbit_protocol
    hardware_dma
    hardware_i2c
    hardware_pio
    hardware_uart
    hardware_gpio
    pico_stdlib
)
target_compile_features(rabbit_rp2350 PRIVATE cxx_std_17)
target_compile_definitions(rabbit_rp2350 PRIVATE
    RABBIT_LED_PIN=${RABBIT_LED_PIN}
    RABBIT_AUDIO_PIN=${RABBIT_AUDIO_PIN}
    RABBIT_LOCAL_STOP_PIN=${RABBIT_LOCAL_STOP_PIN}
)
target_compile_options(rabbit_rp2350 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
pico_generate_pio_header(rabbit_rp2350 ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
pico_enable_stdio_usb(rabbit_rp2350 1)
pico_enable_stdio_uart(rabbit_rp2350 0)
pico_add_extra_outputs(rabbit_rp2350)
