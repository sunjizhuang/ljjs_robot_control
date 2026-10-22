# 灵境具身机器人控制服务端
# Ubuntu 20.04 / GCC / C++11
# PLATFORM=x86 (default) or PLATFORM=arm

PLATFORM ?= x86
PYTHON   ?= python3

ifeq ($(PLATFORM),arm)
CXX ?= aarch64-linux-gnu-g++
MOSQUITTO_ARCH_DIR := aarch64
else ifeq ($(PLATFORM),x86)
CXX ?= g++
MOSQUITTO_ARCH_DIR := x86_64
else
$(error Unsupported PLATFORM='$(PLATFORM)'. Use PLATFORM=x86 or PLATFORM=arm)
endif

CXXFLAGS ?= -Wall -Wextra -O2 -std=c++11 -pthread -Isrc -Iinclude
LDFLAGS  ?= -pthread

TARGET = lljs_ctrl
BUILD_DIR = build
OPENSSL_LIBS = -lssl -lcrypto
MOSQUITTO_LIB_DIR = lib/$(MOSQUITTO_ARCH_DIR)
MOSQUITTO_LIB = $(MOSQUITTO_LIB_DIR)/libmosquitto.so

NETWORK_MQTT_SRC = src/network/mqtt_client.cpp src/network/mqtt_broker.cpp
NETWORK_MQTT_OBJ = $(NETWORK_MQTT_SRC:.cpp=.o)

SRCS = src/main.cpp \
       src/util/log.cpp src/util/json.cpp \
       src/network/tcp_client.cpp src/network/tcp_control_client.cpp src/network/tcp_server.cpp \
       src/config/yaml_config.cpp \
       src/device/yunji_driver.cpp src/device/lite3_driver.cpp \
       src/control/input_adapter.cpp src/control/cmd_translator.cpp \
       src/keyboard_control.cpp

OBJS = $(SRCS:.cpp=.o)
ALL_OBJS = $(OBJS) $(NETWORK_MQTT_OBJ)

.PHONY: all clean rebuild check python-check check-all \
        cmake-x86 cmake-arm cmake-build \
        run dashboard keyboard external-server mqtt-pub

all: $(TARGET)

$(TARGET): $(ALL_OBJS) $(MOSQUITTO_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(ALL_OBJS) -L$(MOSQUITTO_LIB_DIR) -lmosquitto $(OPENSSL_LIBS) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN/$(MOSQUITTO_LIB_DIR)' -Wl,-rpath-link,$(MOSQUITTO_LIB_DIR)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

check: $(TARGET)
	./$(TARGET) --check-config -c config/config.yaml

python-check:
	$(PYTHON) -m py_compile test/test_client.py test/test_external_server.py test/test_mqtt_publish.py

check-all: check python-check

rebuild: clean all

cmake-x86:
	cmake -S . -B $(BUILD_DIR) -DBUILD_PLATFORM=x86
	cmake --build $(BUILD_DIR) -j$$(nproc)

cmake-arm:
	cmake -S . -B $(BUILD_DIR) -DBUILD_PLATFORM=arm
	cmake --build $(BUILD_DIR) -j$$(nproc)

cmake-build: cmake-x86

run: $(TARGET)
	./$(TARGET) -c config/config.yaml

HOST ?= 127.0.0.1
PORT ?= 9000

external-server:
	$(PYTHON) test/test_external_server.py 0.0.0.0 $(PORT)

dashboard:
	$(PYTHON) test/test_client.py $(HOST) $(PORT) --dashboard

keyboard:
	$(PYTHON) test/test_client.py $(HOST) $(PORT) --keyboard

clean:
	rm -f $(ALL_OBJS) $(TARGET)
	rm -rf $(BUILD_DIR)
	rm -rf test/__pycache__

TOPIC ?= device/command
PAYLOAD ?= {"cmd":"mode","mode":"stand","device":"lite3"}
mqtt-pub:
	$(PYTHON) test/test_mqtt_publish.py $(HOST) $(PORT) $(TOPIC) '$(PAYLOAD)'
