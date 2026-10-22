# 灵境具身机器人控制服务端

双设备机器人控制服务端：支持 **云迹 SC668s** 与 **云深处绝影 Lite3**，并在现有 TCP 控制架构上完整接入 Lite3 运动主机通讯接口 **V1.0.7** 的指定功能，
             Lite3 控制采用 UDP、小端字节序，运动主机控制端口为 43893；运动主机上报信息需要将目标地址配置到开发主机后，由本机 UDP 接收端口 43897 接收。

## 1. 架构总览

┌──────────────────────────────────────────────────────────────────────────────┐
│                         外部控制端 / 上位机 / 云端                           │
├──────────────────────────────────────────────────────────────────────────────┤
│   TCP Client                 TCP Client                  MQTT Client         │
│   / WebSocket                控制端                      控制端              │
│       │ JSON                    │ JSON                    │ PUB JSON         │
│       ▼                         ▼                         ▼                  │
└───────┬─────────────────────────┬─────────────────────────┬──────────────────┘
        ▼                         ▼                         ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                    SC668S / Ubuntu 20.04  lljs_ctrl                          │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                         网络通信层                                     │  │
│  │  ┌────────────────┐   ┌────────────────┐   ┌────────────────────────┐  │  │
│  │  │   TCP Server   │   │   TCP Client   │   │    MQTT Server/Broker  │  │  │
│  │  │                │   │                │   │    Mosquitto           │  │  │
│  │  │ listen :9000   │   │ 主动连接外部   │   │ listen :1883           │  │  │
│  │  │ 接收 JSON      │   │ TCP Server     │   │ 接收其它 MQTT Client   │  │  │
│  │  └───────┬────────┘   └───────┬────────┘   └──────────┬─────────────┘  │  │
│  │          │ JSON               │ JSON                  │ MQTT PUB       │  │
│  │          └────────────────────┼───────────────────────┘                │  │
│  │                               ▼                                        │  │
│  │                    ┌──────────────────────┐                            │  │
│  │                    │ MQTT Client          │                            │  │
│  │                    │ mqtt_client.cpp      │                            │  │
│  │                    │ libmosquitto.so      │                            │  │
│  │                    │                      │                            │  │
│  │                    │ SUB / PUB            │                            │  │
│  │                    └──────────┬───────────┘                            │  │
│  └───────────────────────────────┼────────────────────────────────────────┘  │
│                                  │ 统一 JSON 指令                            │
│                                  ▼                                           │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                     指令解析与中间层                                   │  │
│  │  ┌─────────────────────┐        ┌───────────────────────────────────┐  │  │
│  │  │    输入适配器       │        │          CmdTranslator            │  │  │
│  │  │                     │───────►│                                   │  │  │
│  │  │ JSON / WASD         │        │ JSON → 标准设备指令               │  │  │
│  │  │ 键盘控制            │        │ 设备选择 / 参数转换 / 指令分发    │  │  │
│  │  │ 协议兼容            │        │ Lite3 / SC668S                    │  │  │
│  │  └─────────────────────┘        └────────────────┬──────────────────┘  │  │
│  └──────────────────────────────────────────────────┼─────────────────────┘  │
│                                                     │                        │
│                                                     │ 设备指令               │
│                                                     ▼                        │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                         设备适配层                                     │  │
│  │       ┌────────────────────────┐       ┌────────────────────────────┐  │  │
│  │       │      Lite3Driver       │       │       Yunji Driver         │  │  │
│  │       │                        │       │                            │  │  │
│  │       │ UDP                    │       │ TCP / 设备 API             │  │  │
│  │       │ Lite3 V1.0.7           │       │ 云迹机器人                 │  │  │
│  │       │ 指令码                 │       │                            │  │  │
│  │       └───────────┬────────────┘       └────────────────────────────┘  │  │
│  └───────────────────┼─────────────────────────────────┼──────────────────┘  │
└──────────────────────┼─────────────────────────────────┼─────────────────────┘
                       │ UDP                             │ TCP
                       ▼                                 ▼
              ┌──────────────────────┐              ┌──────────────────────┐
              │       Lite3          │              │                      │
              │    绝影运动主机      │              │    Yunji运动主机     │
              │                      │              │  接收控制指令        │
              │  接收控制指令        │              │  发送机器人状态      │
              │  发送机器人状态      │              └──────────────────────┘
              └──────────┬───────────┘
                         │ UDP Telemetry
                         ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                         lljs_ctrl 状态处理                                       │
│                                                                              │
│       0901 Robot State       0902 Joint Angle       0903 Joint Velocity      │
│              │                       │                       │               │
│              └───────────────────────┼───────────────────────┘               │
│                                      ▼                                       │
│                              状态解析 / 缓存                                       │
│                         ┌────────────┴────────────┐                          │
│                         │                         │                          │
│                         ▼                         ▼                          │
│                    TCP Telemetry            MQTT PUB                         │
│                                               │                              │
│                                               ▼                              │
│                                    device/telemetry                          │
└──────────────────────────────────────────────────────────────────────────────┘

### 网络通道
三种外部网络方式

目前最大的架构特点是：TCP Server、TCP Client、MQTT(Broker/Client) 可以同时存在，而且通过配置独立控制
它们最终都进入同一套指令处理链路，不会因为网络协议不同而产生三套业务逻辑
MQTT 部分采用“双角色”， MQTT Broker/Server + MQTT Client

### 统一指令处理链
             TCP Server
                 │
             TCP Client
                 │
          MQTT Broker
                 │
          MQTT Client
                 │
                 ▼
        ┌─────────────────┐
        │   JSON Message  │
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ Input Adapter   │
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ CmdTranslator   │
        └────────┬────────┘
                 ▼
          Device Manager
            /         \
           /           \
          ▼             ▼
     Lite3Driver    SC668S Driver

## 2. 工程结构

```text
ljjs_robot_control/
├── CMakeLists.txt
├── Makefile
├── README.md
├── LITE3_V1.0.7_CHANGELOG.md
├── config/
│   └── config.yaml				# YAML 配置文件模板
├── scripts/
│   ├── build.sh				# 编译脚本
│   ├── run.sh					# 运行脚本
│   └── fetch_mosquitto_ubuntu20.sh		# 获取 Ubuntu 20.04 对应 Mosquitto Broker + libmosquitto
├── include/
│   └── mosquitto.h			# Mosquitto libmosquitto API 头文件
├── lib/				# Eclipse Mosquitto 官方动态库（目标平台对应架构）
├   └──x86_64/
│        ├── libmosquitto.so -> libmosquitto.so.1.6.9
│        ├── libmosquitto.so.1 -> libmosquitto.so.1.6.9
│        └── libmosquitto.so.1.6.9
│   └── aarch64/
│        ├── libmosquitto.so -> libmosquitto.so.1.6.9
│        ├── libmosquitto.so.1 -> libmosquitto.so.1.6.9
│        └── libmosquitto.so.1.6.9
│   ├── README_MOSQUITTO.md			# Mosquitto 依赖说明
│   └── MOSQUITTO_BINARY_NOTE.txt
├── bin/
│   └── mosquitto				# 官方 Mosquitto Broker 可执行文件（脚本获取/目标机安装）
├── src/
│   ├── main.cpp				# 程序入口
│   ├── keyboard_control.h/.cpp			# 应用控制层 (Tab切换设备+双设备HUD)
│   ├── network/				# ── 网络通信层 ──
│   │   ├── tcp_client.h/.cpp           	# 云迹设备 TCP 客户端
│   │   ├── tcp_server.h/.cpp           	# 外部控制端接入 TCP Server
│   │   ├── tcp_control_client.h/.cpp   	# 本机作为客户端连接外部控制端 Server
│   │   ├── mqtt_client.h/.cpp           	# libmosquitto MQTT Client：SUB/PUB
│   │   └── mqtt_broker.h/.cpp           	# 官方 Mosquitto Broker 进程管理
│   ├── control/                 		# ── 指令解析中间层 ──
│   │   ├── input_adapter.h/.cpp 		# 输入适配器 (设备感知 WASD+JSON 解析)
│   │   └── cmd_translator.h/.cpp 		# 指令翻译引擎 (多设备管理+设备感知执行)
│   ├── device/					# ── 负责具体机器人协议 ── 
│   │   ├── device_driver.h			# 设备适配层 —— 抽象接口 (多厂家兼容)
│   │   ├── yunji_driver.h/.cpp
│   │   └── lite3_driver.h/.cpp
│   ├── config/                  		# ── 配置管理 ──
│   │   ├── yaml_config.h/.cpp   		# YAML 解析器 + AppConfig 结构体
│   │   └── config.h             		# 编译期协议常量 (Lite3 指令码)
│   └── util/                    		# ── 工具库：负责协议和设备测试 ──
│       ├── log.h / log.cpp      		# 日志模块
│       └── json.h / json.cpp    		# JSON 解析器
└── test/
    ├── test_client.py				# 测试客户端
    ├── test_external_server.py			# 测试服务端
    └── test_mqtt_publish.py			# MQTT QoS0 测试发布器（仅标准库）
```

---

## 3. 支持设备
| 设备 | 厂家 | 通信方式 | 默认地址 | 特点 |
|------|------|---------|---------|------|
| SC668s | 云迹 | TCP (URL指令+JSON返回) | 192.168.10.10:31001 | 导航/遥控/急停/状态订阅 |
| Lite3 | 云深处 | UDP (二进制指令码) | 192.168.2.1:43893 | 模式切换/离散移动/特殊动作/20ms心跳 |

## 设备感知按键映射

同一按键在不同设备上含义不同，**Tab 键切换激活设备**：

### 通用按键 (两个设备一致)

| 键 | 云迹 SC668s | Lite3 |
|----|------------|-------|
| W | 前进 (速度递增) | 前进 (离散动作) |
| S | 后退 (速度递减) | 后退 (离散动作) |
| Space | 停止 | 停止 |
| H | 帮助 | 帮助 |
| Q | 退出 | 退出 |
| Tab | 切换设备 | 切换设备 |

### 云迹专用按键 (激活云迹时)

| 键 | 功能 |
|----|------|
| A / D | 左转 / 右转 (角速度递增) |
| X | 急停 |
| Z | 解除急停 |
| C | 取消移动 |
| M | 导航到点位 |
| L | 查询点位列表 |
| R | 刷新状态 |
| P | 查询电源 |
| V | 查询版本 |

### Lite3 专用按键 (激活 Lite3 时)

| 键 | 功能 | 指令码 |
|----|------|--------|
| A / D | 左平移 / 右平移 | voice 5 / 6 |
| Z | 站立模式 | 0x21010202 |
| 2 | 软急停 | 0x21020C0E |
| Y | 移动模式 | 0x21010D06 |
| U | 原地模式 | 0x21010D05 |
| X | 自主模式 | 0x21010C03 |  			#自主模式下机器人响应由感知主机下发的速度指令
| C | 手动模式 | 0x21010C02 |      	#手动模式下机器人响应由手柄下发的速度指令
| G | 打招呼 | 0x21010507 |
| J | 跳 | 0x2101050B |
| 1 | 扭身体 | 0x21010204 |
| N | 左转90° | voice 13 |
| E | 右转90° | voice 14 |
| R | 后转180° | voice 15 |
| F | 停止 (同空格) | voice 7 |

## 4. MQTT 通信

### MQTT 角色

`lljs_ctrl` 支持两个独立的 MQTT 角色：

1. **MQTT Broker / Server**：由 `MqttBroker` 管理官方 Mosquitto Broker 进程，允许其它 MQTT Client 连接、PUB、SUB。
2. **MQTT Client**：由 `MqttClient` 使用官方 `libmosquitto.so` 连接 Broker，支持 SUB / PUB / 自动重连。

当 `mqtt.broker_enabled=true` 时，即使 `mqtt.client_enabled=false`，`lljs_ctrl` 仍会自动建立内部 MQTT Client 连接本机 Broker，用于接收其它客户端发送的控制 JSON。

### Topic 设计

MQTT Topic 统一由 `src/network/mqtt_client.cpp` 管理，所有业务 Topic 均以 `device/` 开头。

默认 Topic：

```text
SUB: device/command/#
PUB: device/telemetry
```

其中 `device/command/#` 是 MQTT 多级通配订阅过滤器，可以接收：

```text
device/command
device/command/lite3
device/command/lite3/axis
device/command/lite3/gait
device/command/yunji
```

### 配置

```yaml
network:
  server_enabled: false
  client_enabled: true
  mqtt_enabled: false

mqtt:
  broker_enabled: false
  broker_bind: "0.0.0.0"
  broker_port: 1883
  broker_executable: ""
  broker_config: "/tmp/lljs_ctrl_mosquitto.conf"
  broker_allow_anonymous: true

  client_enabled: false
  host: "127.0.0.1"
  port: 1883
  client_id: "lljs_ctrl"
  username: ""
  password: ""
  reconnect_interval_ms: 5000
```

注意：配置文件中已经**没有 `subscribe_topic` / `publish_topic`**。

### MQTT 控制消息

其它 MQTT Client 连接 `lljs_ctrl` Broker 后，向：

```text
device/command
```

PUB 与 TCP 完全相同的 JSON，例如：

```json
{"cmd":"axis","axis":"x","value":3000,"device":"lite3"}
```

也可以使用扩展 Topic：

```text
device/command/lite3
device/command/lite3/axis
```

消息仍然统一进入：

```text
Mosquitto Broker
      ↓
lljs_ctrl MQTT Client (SUB device/command/#)
      ↓
InputAdapter
      ↓
CmdTranslator
      ↓
DeviceDriver
```

### MQTT Telemetry

Lite3 的 `0x0901/0x0902/0x0903` 最新状态由 lljs_ctrl 发布到：

```text
device/telemetry
```

其它 MQTT Client 可以：

```text
SUB device/telemetry
```

获取机器人状态。

### 测试

使用内置 Python 测试发布器：

```bash
python3 test/test_mqtt_publish.py 127.0.0.1 1883 device/command '{"cmd":"mode","mode":"stand","device":"lite3"}'
```

或：

```bash
make mqtt-pub HOST=127.0.0.1 PORT=1883 TOPIC=device/command PAYLOAD='{"cmd":"mode","mode":"stand","device":"lite3"}'
```

## JSON 协议

### 设备指定
```json
{"cmd":"switch_device"}
{"cmd":"mode","mode":"stand","device":"lite3"}
{"cmd":"action","action":"greet","device":"lite3"}
```

`device` 字段可选: `"yunji"` / `"lite3"` / `"all"` / 不填(=激活设备)

### 模式切换 (Lite3)
```json
{"cmd":"mode","mode":"stand"}
{"cmd":"mode","mode":"move"}
{"cmd":"mode","mode":"manual"}
{"cmd":"mode","mode":"auto"}
{"cmd":"mode","mode":"inplace"}
```

### 执行动作 (Lite3)
```json
{"cmd":"action","action":"forward"}
{"cmd":"action","action":"backward"}
{"cmd":"action","action":"strafe_left"}
{"cmd":"action","action":"strafe_right"}
{"cmd":"action","action":"stop"}
{"cmd":"action","action":"greet"}
{"cmd":"action","action":"jump"}
{"cmd":"action","action":"twist"}
{"cmd":"action","action":"turn_left_90"}
{"cmd":"action","action":"turn_right_90"}
{"cmd":"action","action":"turn_180"}
```
Lite3 V1.0.7 已实现功能
| 文档章节 | 功能 | TCP JSON 命令 | Lite3 UDP |
|---|---|---|---|
| 1.2.3 | 轴指令 | `axis` | `0x21010130/31/35` 等 |
| 1.2.5 | 步态切换 | `gait` | `0x21010300` 等 |
| 1.2.8 | 保存数据 | `save_data` | `0x21010C01` |
| 1.2.9 | 持续运动 | `continuous_motion` | `0x21010C06` |
| 1.2.12 | 导航速度 | `velocity` | `0x0141/0140/0145` |
| 1.3.1 | 机器人状态 | `telemetry` | `0x0901` |
| 1.3.2 | 关节信息 | `telemetry` | `0x0902/0903` |

### 3.1 轴指令
value 取值范围为[-32767,32767]， 当轴指令取值在死区范围内时，视其值为 0，机器人停止移动。轴指令值的正负确定速度方向
```json
{"cmd":"axis","axis":"x","value":30000,"device":"lite3"}		#前后平移 死区[-6553,6553] 指定机器人 x 轴上的期望线速度，正值向前
{"cmd":"axis","axis":"y","value":20000,"device":"lite3"}		#左右平移 死区[-12553,12553] 指定机器人 y 轴上的期望线速度，正值向右
{"cmd":"axis","axis":"yaw","value":10000,"device":"lite3"}		#左右转弯 死区[-9553,9553] 指定机器人的期望角速度，正值向右转
{"cmd":"axis","axis":"height","value":1000,"device":"lite3"}	#【原地模式】身体高度 死区[-20000,20000] 取正值时抬高身体

```
轴指令由 Lite3Driver 独立线程以 **50ms / 20Hz** 周期持续发送当前轴值。停止/急停时清零轴值并停止轴控制。

### 3.2 步态

```json
{"cmd":"gait","gait":"low","device":"lite3"}
{"cmd":"gait","gait":"medium","device":"lite3"}
{"cmd":"gait","gait":"high","device":"lite3"}
{"cmd":"gait","gait":"crawl","device":"lite3"}
{"cmd":"gait","gait":"grip","device":"lite3"}
{"cmd":"gait","gait":"general","device":"lite3"}
{"cmd":"gait","gait":"high_step","device":"lite3"}
```

### 3.3 保存数据

```json
{"cmd":"save_data","device":"lite3"}
```

**注意：这是高风险测试项。** 按 V1.0.7 接口语义，该指令用于保存故障数据并关闭运动程序。测试工具默认要求输入 `YES` 二次确认。

### 3.4 持续运动
持续运动开启后，机器人在未收到轴指令时也将保持原地踏步。
```json
{"cmd":"continuous_motion","enabled":true,"device":"lite3"}
{"cmd":"continuous_motion","enabled":false,"device":"lite3"}
```

### 3.5 导航速度

```json
{"cmd":"velocity","vx":0.3,"vy":0.0,"yaw":0.1,"device":"lite3"}
```

对应关系：

```text
vx  → 0x0140，范围 [-1.0, 1.0] m/s
vy  → 0x0145，范围 [-0.5, 0.5] m/s
yaw → 0x0141，范围 [-1.5, 1.5] rad/s
```

速度指令要求机器人处于自主模式。测试工具在速度测试前会发送 `mode=auto`。

---

## 4. Lite3 状态接收

Lite3Driver 接收运动主机 UDP 上报：

```text
UDP 43897
   │
   ├── 0x0901 → RobotStateUpload
   ├── 0x0902 → 12 个 joint_angle
   └── 0x0903 → 12 个 joint_vel
```

驱动内部保存最新 `Lite3State` 快照。

### TCP Telemetry

为了让远程测试工具能够观察状态，`lljs_ctrl` 每 **200ms** 向 TCP 对端发送最新快照：

```json
{"cmd":"telemetry","device":"lite3","code":"0x0901", ...}
{"cmd":"telemetry","device":"lite3","code":"0x0902", ...}
{"cmd":"telemetry","device":"lite3","code":"0x0903", ...}
```

这不是修改 Lite3 原始协议，而是本项目 TCP 测试/监控层的转发。

### 0901

包含机器人基本状态、步态状态、运动状态、电量、RPY、RPY 速度、位置、世界系速度、机体系速度、超声波等。

### 0902

12 个关节角度。

### 0903

12 个关节速度。

---

## 5. 配置

所有运行时参数通过 YAML 配置文件设定，模板见 `config/config.yaml`：

```yaml
device: lite3           			# 连接设备: yunji / lite3 / both
log_level: info         			# 日志级别

default_active: ""

network:
  mode: client			 			# server / client

client:
  server_ip: "175.24.177.82"		#本机 127.0.0.1
  server_port: 20001				#本机 9000
  reconnect_interval_ms: 5000
  edge_id: "edge-1"
  robot_id: "lite3-1"
  device: "lite3"

lite3:
  ip: "192.168.1.120"				#wifi mode： 192.168.2.1
  port: 43893
  receive_port: 43897

lite3_heartbeat:
  interval_ms: 20

server:
  bind: "0.0.0.0"
  port: 9000

control:
  joy_hz: 5
  max_linear: 0.5
  max_angular: 1.0
  linear_step: 0.05
  angular_step: 0.1

lite3_heartbeat:
  interval_ms: 20

status:
  poll_hz: 2

log:
  queue_size: 20
```

实际部署时请根据现场网络修改 IP、端口和 client/server 模式。

---

## 6. 编译

### 6.1 Make

```bash
make
```
编译结果：
```text
./lljs_ctrl
```
清理：
```bash
make clean
```
重新编译：
```bash
make rebuild
```
配置检查：
```bash
make check
```
全部检查：
```bash
make check-all
```

### 6.2 CMake

x86：

- mkdir build && cd build
- Navigate to the ***build*** directory and then compile:

   - Compile for x86 hosts:

		```bash
		cd build
		cmake .. -DBUILD_PLATFORM=x86     # cmake <path to where the CMakeLists.txt is>
		make -j
		```

ARM：

```bash
cmake -S . -B build -DBUILD_PLATFORM=arm
cmake --build build -j$(nproc)
```
或：
	- Compile for ARM hosts:

		```bash
		cd build
		cmake .. -DBUILD_PLATFORM=arm     # cmake <path to where the CMakeLists.txt is>
		make -j
		```
---

## 7. 启动服务

### server 模式

配置：

```yaml
network:
  mode: server

server:
  bind: "0.0.0.0"
  port: 9000
```
启动：

```bash
./lljs_ctrl -c config/config.yaml
```
不指定 `-c` 时按以下顺序搜索默认配置：
`./config.yaml` → `./config/config.yaml` → `/etc/lljs_ctrl/config.yaml`

或者：

```bash
make run
```

### client 模式

配置：

```yaml
network:
  mode: client

client:
  server_ip: "192.168.1.100"
  server_port: 9000
  reconnect_interval_ms: 5000
```

启动后 `lljs_ctrl` 主动连接外部 TCP Server，并在连接成功后发送注册信息。

---

## 8. Python 测试工具

两个工具分别用于两种网络角色。

### 8.1 test_client.py

用于连接 `lljs_ctrl` 的 TCP Server。

```bash
python3 test/test_client.py 192.168.1.120 9000
```

菜单：

```text
========== Lite3 V1.0.7 Test Client ==========
1  轴指令       1.2.3
2  步态切换     1.2.5
3  保存数据     1.2.8
4  持续运动     1.2.9
5  速度指令     1.2.12
6  接收信息     1.3 / 0901 / 0902 / 0903
7  全部测试
8  实时仪表盘
9  键盘实时控制
0  退出
```

### 8.2 test_external_server.py

用于模拟外部控制端 Server，配合 `network.mode=client`。
然后将 `config/config.yaml` 设置为：

```yaml
network:
  mode: client
client:
  server_ip: "127.0.0.1"
  server_port: 9000
  reconnect_interval_ms: 5000
```

```bash
python3 test/test_external_server.py 0.0.0.0 9000
```

然后启动 `lljs_ctrl` client 模式。

服务端会显示客户端注册信息，例如：

```text
edgeId=edge-1
robotId=lite3-1
device=lite3
```

---

## 9. 实时仪表盘

### test_client

```bash
python3 test/test_client.py 127.0.0.1 9000 --dashboard
```

仪表盘固定刷新显示：

```text
TCP 连接状态
0901 / 0902 / 0903 接收频率
0901 总包数
0902 总包数
0903 总包数
机器人基本状态
步态状态
运动状态
电量
RPY / RPY velocity
位置 / 世界系速度 / 机体系速度
超声波
12 个关节角度
12 个关节速度
```

按 `Q` 或 `ESC` 退出。

### test_external_server

```bash
python3 test/test_external_server.py 127.0.0.1 9000 --dashboard
```

---

## 10. 键盘实时控制

键盘模式直接通过 TCP JSON 控制 Lite3 的自主速度。

### 启动

```bash
python3 test/test_client.py 192.168.1.120 9000 --keyboard
```

### 按键

```text
W / S       前进 / 后退
A / D       左平移 / 右平移
J / L       左旋 / 右旋
F / SPACE   立即停止
X           切换自主模式
Z           停止速度并站立
Q / ESC     停止并退出
```

默认速度：

```text
vx  = ±0.30 m/s
vy  = ±0.20 m/s
yaw = ±0.50 rad/s
```

### Watchdog

Linux 终端输入通常不能可靠提供独立的物理 `key-up` 事件，因此测试程序采用：

```text
键盘事件
   ↓
刷新当前速度
   ↓
50ms 周期继续发送
   ↓
超过 250ms 没有新的有效按键事件
   ↓
自动发送 vx=0, vy=0, yaw=0
```

这样可以避免终端失焦或按键重复停止后机器人继续保持最后速度。

> **实机测试必须确认机器人周围安全、具备急停条件。** 键盘模式只用于联调，不应替代正式安全控制系统。

---

## 11. 自动测试

```bash
python3 test/test_client.py 192.168.1.120 9000 --suite
```

或者外部 Server：

```bash
python3 test/test_external_server.py 0.0.0.0 9000 --suite
```

自动测试包含：

```text
步态
持续运动
接收信息
轴指令
速度指令
保存数据
```

涉及机器人运动的测试会要求：

```text
输入 YES 确认
```

`save_data` 也会单独要求确认。

---

## 12. 常用命令
## 远程控制端连接

```bash
# 单字符指令
echo "w" | nc <HOST_IP> 9000

# JSON 指令 (指定设备)
echo '{"cmd":"mode","mode":"stand","device":"lite3"}' | nc <HOST_IP> 9000
echo '{"cmd":"action","action":"greet","device":"lite3"}' | nc <HOST_IP> 9000

# 切换设备
echo '{"cmd":"switch_device"}' | nc <HOST_IP> 9000

# 持续交互
nc <HOST_IP> 9000
# 然后输入: w s a d x z c ...
```

### 直接 TCP JSON 测试

```bash
echo '{"cmd":"mode","mode":"auto","device":"lite3"}' | nc <HOST_IP> 9000

echo '{"cmd":"gait","gait":"medium","device":"lite3"}' | nc <HOST_IP> 9000

echo '{"cmd":"velocity","vx":0.2,"vy":0,"yaw":0,"device":"lite3"}' | nc <HOST_IP> 9000

echo '{"cmd":"velocity","vx":0,"vy":0,"yaw":0,"device":"lite3"}' | nc <HOST_IP> 9000
```

### 配置检查
```bash
# 启动（指定配置文件）
lljs_ctrl -c /etc/lljs_ctrl/config.yaml

# 校验配置（不启动服务）
lljs_ctrl --check-config -c config.yaml

# 查看最终生效配置（默认值填充后的结果）
lljs_ctrl --dump-config -c config.yaml

# 帮助 / 版本
lljs_ctrl --help
lljs_ctrl --version
```

不指定 `-c` 时按以下顺序搜索默认配置：
`./config.yaml` → `./config/config.yaml` → `/etc/lljs_ctrl/config.yaml`

```
-c,  --config <file>       YAML 配置文件路径
     --check-config        校验配置后退出（配合 -c）
     --dump-config         输出最终生效配置后退出（配合 -c）
-V,  --version             显示版本
-h,  --help                显示帮助
```
本机启动后主动连接外部 Server；首次连接失败、运行中断线均会自动重连。外部 Server 发送协议与原 TCP Server 接收协议保持一致：**每行一条指令**，既支持单字符也支持 JSON。

## 13. 实机联调建议

建议按以下顺序进行：

```text
1. make check-all
        ↓
2. 检查 Lite3 UDP 43893 / 43897 网络连通
        ↓
3. 启动 lljs_ctrl
        ↓
4. 先确认 0901/0902/0903 是否有数据
        ↓
5. 进入实时仪表盘
        ↓
6. 测试步态 / 模式
        ↓
7. 小速度测试 velocity
        ↓
8. 测试轴指令
        ↓
9. 最后再测试保存数据
```

首先确认状态链路：

```text
Lite3 → UDP 43897 → Lite3Driver → TCP telemetry → test_client
```

如果仪表盘中：

```text
0901 ≈ 5Hz
0902 ≈ 5Hz
0903 ≈ 5Hz
```

说明 TCP Telemetry 快照转发正常；这里显示的是项目层每 200ms 的快照发送频率，并不代表 Lite3 原始 UDP 上报频率。

---

## 14. 设计要点与注意事项

| 设计 | 说明 |
|------|------|
| 双设备兼容 | 云迹 (TCP) + Lite3 (UDP) 同时连接，统一管理 |
| 设备感知按键 | 同一按键根据激活设备映射为不同指令 |
| Tab 切换 | 终端按 Tab 快速切换激活设备 |
| JSON 设备指定 | 远程指令可通过 `device` 字段指定目标设备 |
| 三层分离 | 网络通信、指令解析、设备驱动完全解耦 |
| 多厂家接口 | IDeviceDriver 抽象接口，新增厂家只需实现接口 |
| Lite3 心跳 | 内置 20ms 心跳线程，保持控制权 |
| 速度模式差异 | 云迹: 连续速度递增遥控; Lite3: 离散动作指令 |

1. Lite3 原始 UDP 协议使用小端字节序。
2. Lite3 控制端口默认为 `43893`。
3. Lite3 状态接收端口默认为 `43897`。
4. 轴指令需要持续发送，驱动内部使用 20Hz 发送循环。
5. 速度指令的范围必须符合 V1.0.7。
6. 速度控制前应进入自主模式。
7. `save_data` 是高风险指令，可能关闭运动程序，实机慎用。
8. 键盘控制具备 250ms watchdog，但仍必须准备硬件/现场急停。
9. TCP Telemetry 是本项目增加的监控通道，不改变 Lite3 UDP 原始协议。
10. 不要把 `test/test_external_server.py` 暴露到不可信公网环境。

---

## 15. 版本信息

```text
Application: lljs_ctrl
Lite3 API:  V1.0.7
Build:      C++11
Platform:   Ubuntu 20.04 x86 / ARM
```


## 网络开关的推荐配置

### 默认：仅 TCP Client

```yaml
network:
  server_enabled: false
  client_enabled: true
  mqtt_enabled: false
```

### TCP Server + TCP Client + MQTT 同时开启

```yaml
network:
  server_enabled: true
  client_enabled: true
  mqtt_enabled: true
```

三路收到的业务 JSON 不重复实现解析逻辑，统一调用：

```text
TCP Server ─┐
TCP Client ─┼→ handleRemoteCommandFrom() → InputAdapter → CmdTranslator
MQTT      ──┘
```

这样可以保证同一个 JSON 指令通过 TCP 或 MQTT 到达时，设备侧行为一致。


## MQTT Topic

MQTT Topic 不再放在配置文件中，由 `src/network/mqtt_client.cpp` 统一定义。

- 默认控制订阅过滤器：`device/command/#`
- 默认遥测发布 Topic：`device/telemetry`

`device/command/#` 支持 Topic 扩展，例如：

```text
device/command
device/command/lite3
device/command/lite3/axis
device/command/lite3/gait
```

收到 `device/command/#` 下任意 Topic 的 JSON 消息后，统一进入现有指令处理链。后续如需增加其它 Topic，可在 `MqttClient` 中增加独立订阅过滤器，不需要修改配置文件。

## Mosquitto 多架构库

工程使用官方 Mosquitto `libmosquitto.so` 动态库，

第三方库按 CPU 架构分别放置：

```text
lib/
├── x86_64/
│   ├── libmosquitto.so -> libmosquitto.so.1.6.9
│   ├── libmosquitto.so.1 -> libmosquitto.so.1.6.9
│   └── libmosquitto.so.1.6.9
└── aarch64/
    ├── libmosquitto.so -> libmosquitto.so.1.6.9
    ├── libmosquitto.so.1 -> libmosquitto.so.1.6.9
    └── libmosquitto.so.1.6.9
```

PC x86_64 构建：

```bash
cmake -S . -B build -DBUILD_PLATFORM=x86
cmake --build build -j$(nproc)
```

ARM64 交叉构建：

```bash
cmake -S . -B build -DBUILD_PLATFORM=arm
cmake --build build -j$(nproc)
```

Makefile 也支持：

```bash
make PLATFORM=x86
make PLATFORM=arm
```

构建时会检查 `libmosquitto.so` 的 ELF 架构，避免 ARM 构建误链接 x86_64 Mosquitto 动态库。
