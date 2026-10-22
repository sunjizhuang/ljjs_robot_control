cmake_minimum_required(VERSION 3.10)
project(robot_controller CXX)

SET(BUILD_PLATFORM "x86" CACHE STRING "select build cpu type")
if (BUILD_PLATFORM STREQUAL arm)
  message("this is arm platform")
  SET(CMAKE_C_COMPILER "aarch64-linux-gnu-gcc")
  SET(CMAKE_CXX_COMPILER "aarch64-linux-gnu-g++")
  set(MOSQUITTO_ARCH_DIR "aarch64")
else()
  message("this is x86 platform")
  set(MOSQUITTO_ARCH_DIR "x86_64")
endif()

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

include_directories(src include)

# Mosquitto is a third-party shared library. Keep libraries separated by CPU architecture.
set(MOSQUITTO_LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${MOSQUITTO_ARCH_DIR}")
set(MOSQUITTO_LIB_FILE "${MOSQUITTO_LIB_DIR}/libmosquitto.so")

if(NOT EXISTS "${MOSQUITTO_LIB_FILE}")
    message(FATAL_ERROR
        "Mosquitto library not found: ${MOSQUITTO_LIB_FILE}\n"
        "For BUILD_PLATFORM=arm, place an ARM64/aarch64 libmosquitto.so in lib/aarch64/.\n"
        "For BUILD_PLATFORM=x86, place an x86_64 libmosquitto.so in lib/x86_64/.")
endif()

message(STATUS "Mosquitto library: ${MOSQUITTO_LIB_FILE}")

add_executable(lljs_ctrl
    src/main.cpp
    src/util/log.cpp
    src/util/json.cpp
    src/network/tcp_client.cpp
    src/network/tcp_control_client.cpp
    src/network/tcp_server.cpp
    src/network/mqtt_client.cpp
    src/network/mqtt_broker.cpp
    src/config/yaml_config.cpp
    src/device/yunji_driver.cpp
    src/device/lite3_driver.cpp
    src/control/input_adapter.cpp
    src/control/cmd_translator.cpp
    src/keyboard_control.cpp
)

target_link_libraries(lljs_ctrl PRIVATE
    "${MOSQUITTO_LIB_FILE}"
    "${MOSQUITTO_LIB_DIR}/libssl.so"
    "${MOSQUITTO_LIB_DIR}/libcrypto.so"
    pthread
)

# libmosquitto has OpenSSL 1.1 as transitive dependencies.
# rpath-link is needed by the cross linker to locate the target-platform .so files.
set_target_properties(lljs_ctrl PROPERTIES
    LINK_FLAGS "-Wl,-rpath-link,${MOSQUITTO_LIB_DIR}"
)

# Executable is normally in build/, so resolve to ../lib/<arch> at runtime.
set_target_properties(lljs_ctrl PROPERTIES
    BUILD_RPATH "${MOSQUITTO_LIB_DIR}"
    INSTALL_RPATH "\$ORIGIN/../lib/${MOSQUITTO_ARCH_DIR}"
)
