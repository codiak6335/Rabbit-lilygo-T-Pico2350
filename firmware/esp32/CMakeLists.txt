cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "Source ESP-IDF first, then run idf.py build from firmware/esp32.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rabbit_esp32)
