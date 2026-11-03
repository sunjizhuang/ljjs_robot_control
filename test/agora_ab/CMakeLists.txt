# ══════════════════════════════════════════════════════
# agora_ab — RTSA A/B 对照推流工具 (aarch64 交叉编译)
# 用法:
#   mkdir -p build && cd build && cmake .. && make
# 产物: build/agora_ab
# ══════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.10)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)

project(agora_ab C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -O2 -g -Wall")

get_filename_component(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/agora_sdk" ABSOLUTE)
include_directories(${SDK_DIR}/include)
link_directories(${SDK_DIR}/lib/aarch64)

add_executable(agora_ab agora_ab.c)
target_link_libraries(agora_ab agora-rtc-sdk pthread dl rt m)
