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

# ── 语言标准: C 库 (agora SDK) 显式 C99; C++ 部分 C++11, 互不干扰 ──
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Agora (RTSA) 音视频 + 信令集成开关 ──
# 需要 third_party/{agora_sdk,alsa,rockchip_mpp} 就绪
# (先执行 scripts/prepare_agora_env.sh <板端IP>)
option(LLJS_WITH_AGORA "Enable Agora RTSA audio/video + RTM/RTCM signaling" OFF)

set(CMAKE_BUILD_TYPE Release)
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

# ══════════════════════════════════════════════════════════════
# Agora (RTSA) 音视频 + 信令集成
# 依赖: third_party/{agora_sdk (SDK 头+库), alsa, rockchip_mpp}
# 准备: scripts/prepare_agora_env.sh <板端IP>
# ══════════════════════════════════════════════════════════════
if(LLJS_WITH_AGORA)
    set(AGORA_SDK_DIR     "${CMAKE_SOURCE_DIR}/third_party/agora_sdk")
    set(ALSA_DIR          "${CMAKE_SOURCE_DIR}/third_party/alsa")
    set(MPP_DIR           "${CMAKE_SOURCE_DIR}/third_party/rockchip_mpp")

    # 前置检查
    if(NOT EXISTS "${AGORA_SDK_DIR}/include/agora_rtc_api.h")
        message(FATAL_ERROR "缺少 ${AGORA_SDK_DIR}/include/agora_rtc_api.h — 请先拷贝声网 SDK")
    endif()
    if(NOT EXISTS "${ALSA_DIR}/include/alsa/asoundlib.h")
        message(FATAL_ERROR
            "缺少 ALSA aarch64 头文件 (${ALSA_DIR}/include/alsa/)\n"
            "请在 Linux 构建机执行: scripts/prepare_agora_env.sh <板端IP>")
    endif()
    if(NOT EXISTS "${MPP_DIR}/include/rockchip/rk_mpi.h")
        message(FATAL_ERROR
            "缺少 MPP aarch64 头文件 (${MPP_DIR}/include/rockchip/)\n"
            "请在 Linux 构建机执行: scripts/prepare_agora_env.sh <板端IP>")
    endif()
    if(NOT EXISTS "${MPP_DIR}/include/rockchip/rk_venc_cfg.h")
        message(FATAL_ERROR
            "缺少 ${MPP_DIR}/include/rockchip/rk_venc_cfg.h (MppEncCfg 声明所在头文件)\n"
            "请从板端拷贝: scp -r root@<板端IP>:/usr/include/rockchip "
            "third_party/rockchip_mpp/include/")
    endif()

    # agora 模块源码直接编入主程序 (隔离性由目录/宏保证)
    target_compile_definitions(lljs_ctrl PRIVATE LLJS_WITH_AGORA)
    target_include_directories(lljs_ctrl PRIVATE "${CMAKE_SOURCE_DIR}/src/agora")
    target_sources(lljs_ctrl PRIVATE
        src/agora/agora_service.cpp
        src/agora/agora_bridge.cpp
        src/agora/v4l2_capture.cpp
        src/agora/mpp_encoder.cpp
        src/agora/alsa_capture.cpp
        src/agora/alsa_player.cpp
        src/agora/yuyv_convert.cpp
    )
    target_include_directories(lljs_ctrl PRIVATE
        "${AGORA_SDK_DIR}/include"
        "${ALSA_DIR}/include"
        "${MPP_DIR}/include"
    )

    # 链接: agora 放在 C++ 目标之后; SDK 依赖 pthread/m/dl 一并加上
    target_link_libraries(lljs_ctrl PRIVATE
        "${AGORA_SDK_DIR}/lib/aarch64/libagora-rtc-sdk.so"
        "${MPP_DIR}/lib/aarch64/librockchip_mpp.so"
        "${ALSA_DIR}/lib/aarch64/libasound.so"
        pthread
        m
        dl
    )
    # 传递依赖解析 (libagora-rtc-sdk.so 内部依赖)
    set_property(TARGET lljs_ctrl APPEND PROPERTY LINK_FLAGS
        "-Wl,-rpath-link,${AGORA_SDK_DIR}/lib/aarch64")
    # 运行期 rpath: 产物与库同盘部署时按 $ORIGIN 解析; 部署路径不同则用 LD_LIBRARY_PATH
    set_property(TARGET lljs_ctrl APPEND PROPERTY BUILD_RPATH
        "${AGORA_SDK_DIR}/lib/aarch64;${MPP_DIR}/lib/aarch64;${ALSA_DIR}/lib/aarch64")
endif()
