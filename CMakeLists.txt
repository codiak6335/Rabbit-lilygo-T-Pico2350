cmake_minimum_required(VERSION 3.20)

project(rabbit_firmware LANGUAGES CXX)

option(RABBIT_BUILD_HOST "Build the desktop simulator and native tests" ON)

include(CTest)

if(RABBIT_BUILD_HOST)
    add_subdirectory(firmware/core)
    add_subdirectory(firmware/protocol)
    add_subdirectory(firmware/host)
    add_subdirectory(tests/native)
endif()
