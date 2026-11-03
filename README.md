# 灵境具身机器人控制服务端

本项目是运行在边缘计算设备（如云迹 SC668s、云深处 Lite3、RK3588 等）上的机器人综合控制服务端。系统采用高度模块化的 C++ 架构，不仅实现了对多品牌机器人的底层运动控制、
多协议网络通信（TCP/MQTT），还深度集成了声网 (Agora) RTSA SDK，提供低延迟的实时音视频推流与双向信令能力。
**V1.0.7** 的指定功能，Lite3 控制采用 UDP、小端字节序，运动主机控制端口为 43893；运动主机上报信息需要将目标地址配置到开发主机后，由本机 UDP 接收端口 43897 接收。

## 1. 核心架构设计

系统采用**五维解耦**架构（网络层、信令桥接层、指令解析层、设备驱动层、音视频媒体层），支持多网络协议接入与多设备并发控制。

### 1.1 架构总览

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                         外部控制端 / 上位机 / 云端                            │
├──────────────────────────────────────────────────────────────────────────────┤
│   TCP Client          TCP Server          MQTT Client          Agora Web端  	│
│   (外部服务器)             (本地监听)              (发布/订阅)              (浏览器)			│
│       │ JSON              │ JSON              │ JSON               │ RTC/RTM  │
│       ▼                   ▼                   ▼                    ▼          │
└───────┬───────────────────┬───────────────────┬────────────────────┬─────────┘
        │                   │                   │                    │
        ▼                   ▼                   ▼                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                    SC668S / RK3588 / Ubuntu 20.04  lljs_ctrl                 │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                         网络通信与信令桥接层                           │  │
│  │  ┌───────────────┐   ┌───────────────┐   ┌──────────────────────────┐  │  │
│  │  │ Tcp Client    │   │ TcpServer     │   │ MQTT (Mosquitto)         │  │  │
│  │  │               │   │ listen :9000  │   │ Broker + Client listen :1883│  │  │
│  │  └───────┬───────┘   └───────┬───────┘   └──────────┬───────────────┘  │  │
│  │          │                   │                      │                  │  │
│  │  ┌───────┴───────────────────┴──────────────────────┴───────────────┐  │  │
│  │  │                        Agora Bridge                              │  │  │
│  │  │  (负责拦截 av_* 指令，转发 RTM/RTCM 数据，管理音视频生命周期)    │  │  │
│  │  └───────────────────────────────┬──────────────────────────────────┘  │  │
│  └──────────────────────────────────┼─────────────────────────────────────┘  │
│                                     │ 统一 JSON 指令                         │
│                                     ▼                                        │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                     指令解析与分发中间层                               │  │
│  │  ┌─────────────────────┐        ┌───────────────────────────────────┐  │  │
│  │  │ InputAdapter        │        │          CmdTranslator            │  │  │
│  │  │ (统一解析 JSON/WASD)│───────►│ (设备路由、限幅、状态同步)        │  │  │
│  │  └─────────────────────┘        └────────────────┬──────────────────┘  │  │
│  └──────────────────────────────────────────────────┼─────────────────────┘  │
│                                                     │ 设备底层指令           │
│  ┌──────────────────────────────────────────────────┼─────────────────────┐  │
│  │                         设备适配层               ▼                     │  │
│  │       ┌────────────────────────┐       ┌────────────────────────────┐  │  │
│  │       │      Lite3Driver       │       │       Yunji Driver         │  │  │
│  │       │ UDP (V1.0.7 二进制)    │       │ TCP (JSON/URL)             │  │  │
│  │       └────────────────────────┘       └────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                        音视频媒体层 (Agora)                            │  │
│  │  [V4L2 采集]──(YUYV/NV12)──►[MPP 硬编码]──(H.264)──┐                   │  │
│  │                                                    ├─►[RTSA SDK]──►云端 │  │
│  │  [ALSA 采集]──(PCM 16k)────────────────────────────┘                   │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
└──────────────────────┬─────────────────────────────────┬─────────────────────┘
                       │ UDP                             │ TCP
                       ▼                                 ▼
              ┌──────────────────────┐              ┌──────────────────────┐
              │       Lite3          │              │    Yunji运动主机     │
              │    绝影运动主机      │              │  接收控制指令        │
              │  发送机器人状态      │              │  发送机器人状态      │
              └──────────┬───────────┘              └──────────────────────┘
                         │ UDP Telemetry (0901:Robot State/0902:Joint Angle/0903:Joint Velocity )
                         ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                         lljs_ctrl 状态分发处理                               │
│                              状态解析 / 缓存                                 │
│                         ┌────────────┴────────────┐                          │
│                         ▼                         ▼                          │
│                    TCP Telemetry            MQTT PUB / Agora RTM             │
│                                               ▼                              │
│                                    device/telemetry                          │
└──────────────────────────────────────────────────────────────────────────────┘

核心架构特点：

1.四路网络并行：TCP Client、TCP Server、MQTT、Agora RTM 可同时工作，互不干扰。

2.Agora 双重身份：
	信令通道：作为第四路网络接入，接收业务 JSON 和 av_* 专属控制指令。
	媒体通道：独立运作的音视频推流管线（采集 -> 硬编 -> 推流）。

3. 底层状态回传：机器人通过 UDP 拿到的实时状态（0109等），最终由系统统一切片，通过 TCP、MQTT 和 Agora RTM 实时广播给所有连接的终端。

### 1.2 统一指令处理链
	无论是通过 TCP、MQTT 还是声网的 RTM 通道，所有外部控制指令最终都会统一进入同一套处理链路，确保业务行为一致。

             TCP Server
                 │
             TCP Client
                 │
          MQTT (Broker/Client)
                 │
             Agora RTM
                 │
                 ▼
        ┌─────────────────┐
        │   JSON Message  │
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ AgoraBridge     │ ── 若为 av_* 指令，就地拦截并执行
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ Input Adapter   │ (解析设备类型与动作)
        └────────┬────────┘
                 ▼
        ┌─────────────────┐
        │ CmdTranslator   │ (多设备路由与限幅处理)
        └────────┬────────┘
                 ▼
          Device Manager
            /         \
           /           \
          ▼             ▼
     Lite3Driver    SC668S Driver


## 2. 工程结构
ljjs_robot_control/
├── CMakeLists.txt                  # 主构建脚本 (LLJS_WITH_AGORA 开关)
├── Makefile                        # 快捷构建 (make AGORA=1)
├── README.md
├── config/
│   └── config.yaml                 # 全量 YAML 配置文件模板
├── docs/
│   ├── AGORA_INTEGRATION.md        # Agora 集成方案与验证记录
│   └── DEPLOY.md                   # 部署说明
├── scripts/
│   ├── build.sh                    # 编译脚本
│   ├── deploy.sh                   # 打包产物并部署到板端
│   ├── run.sh                      # 运行脚本
│   └── prepare_agora_env.sh        # 准备 Agora/MPP 编译环境
├── include/
│   └── mosquitto.h                 # Mosquitto 头文件
├── lib/                            # 第三方动态库（按架构划分）
│   └── aarch64/
│        ├── libmosquitto.so
│        └── libagora-rtc-sdk.so
├── bin/
│   └── mosquitto                   # 官方 Mosquitto Broker 可执行文件
├── third_party/                    # 第三方依赖头文件（按需拷入）
│   ├── agora_sdk/                  # 声网 RTSA-Lite SDK
│   ├── alsa/                       # ALSA 音频头文件
│   └── rockchip_mpp/               # RK MPP 硬编头文件
├── src/
│   ├── main.cpp                    # 程序入口，管理生命周期
│   ├── keyboard_control.h/.cpp     # 应用控制层 (Tab切换设备+双设备HUD)
│   ├── network/                    # ── 网络通信层 ──
│   │   ├── tcp_client.h/.cpp       # 云迹设备 TCP 客户端
│   │   ├── tcp_server.h/.cpp       # 外部控制端接入 TCP Server
│   │   ├── tcp_control_client.h/.cpp # 本机作为客户端连接外部控制端
│   │   ├── mqtt_client.h/.cpp      # libmosquitto MQTT Client
│   │   └── mqtt_broker.h/.cpp      # 官方 Mosquitto Broker 进程管理
│   ├── control/                    # ── 指令解析中间层 ──
│   │   ├── input_adapter.h/.cpp    # 输入适配器 (WASD+JSON)
│   │   └── cmd_translator.h/.cpp   # 指令翻译引擎 (多设备管理)
│   ├── device/                     # ── 负责具体机器人协议 ──
│   │   ├── device_driver.h         # 设备适配层抽象接口
│   │   ├── yunji_driver.h/.cpp     # 云迹机器人驱动
│   │   └── lite3_driver.h/.cpp     # 云深处 Lite3 驱动
│   ├── agora/                      # ── Agora 音视频对讲 ──
│   │   ├── agora_service.h/.cpp    # 核心生命周期与媒体管线
│   │   ├── agora_bridge.h/.cpp     # av_* 指令拦截与遥测转发
│   │   ├── v4l2_capture.h/.cpp     # UVC 摄像头采集
│   │   ├── yuyv_convert.h/.cpp     # YUYV→NV12 软件转换
│   │   ├── mpp_encoder.h/.cpp      # RK MPP H.264 硬编
│   │   ├── alsa_capture.h/.cpp     # ALSA 麦克风采集
│   │   ├── alsa_player.h/.cpp      # ALSA 下行对讲播放
│   │   ├── av_pacer.h              # 视频发送节拍器
│   │   └── av_source.h             # 媒体源抽象接口
│   ├── config/                     # ── 配置管理 ──
│   │   ├── yaml_config.h/.cpp      # YAML 解析器与结构体
│   │   └── config.h                # 编译期协议常量 (Lite3 指令码)
│   └── util/                       # ── 工具库 ──
│       ├── log.h / log.cpp         # 日志模块
│       └── json.h / json.cpp       # JSON 解析器
└── test/
    ├── web_demo_h264.html          # Web 拉流与双向对讲测试页
    ├── agora_ab/                   # Agora 最小排障验证工具
    ├── test_client.py              # TCP Server 测试客户端
    ├── test_external_server.py     # TCP Client 测试服务端
    ├── test_mqtt_publish.py        # MQTT 指令测试发布器
    └── AGORA_WATCH_GUIDE.md        # 日志观测指南



> **构建依赖**：`third_party/agora_sdk`（声网 SDK）、`third_party/alsa`、`third_party/rockchip_mpp`，
> 由 `scripts/prepare_agora_env.sh` 从板端/官方包准备；Mosquitto 依赖见 `lib/README_MOSQUITTO.md`。

2. 核心模块说明
device/ (设备驱动层)：
	IDeviceDriver: 统一抽象接口，屏蔽不同厂商机器人的硬件差异。
	Lite3Driver: 云深处四足机器狗驱动，基于 UDP 二进制指令码，内置 20ms 心跳保活和状态接收多线程。
	YunjiDriver: 云迹服务机器人驱动，基于 TCP URL 字符串指令。

control/ (指令翻译层)：
	InputAdapter: 负责将外部输入的文本、单字符（WASD）或 JSON 解析为统一的 InputCommand。
	CmdTranslator: 多设备管理器，根据指令目标动态分发给对应的设备驱动；包含 WASD 速度积分逻辑。

network/ (网络通信层)：
	TcpServer: 监听外部控制端连接，支持多客户端并发接入。
	TcpControlClient: 作为客户端连接外部中心服务器，断线自动重连。
	MqttClient / MqttBroker: 支持接入外部 MQTT 或自带内置 Mosquitto 进程。统一通过 device/command/# 和 device/telemetry 收发业务。

agora/ (音视频与信令层)：
	AgoraService: 封装声网 RTSA 生命周期，管理 V4L2 (采集) + MPP (硬编) + ALSA (音频)，处理 SDK 关键帧请求、带宽自适应等回调。
	AgoraBridge: 拦截并处理 av_* 专属指令（如入会、开关视频），同时转发业务遥测数据至 RTM/RTCM 通道。
	视频：自动适配 V4L2 格式（若摄像头输出 YUYV 则自动软转 NV12），随后送入 RK3588 的 MPP 硬件编码器输出 H.264。支持动态码率调整与强制 I 帧。
	音频：通过 ALSA 采集 16kHz/单声道 PCM，交由声网 SDK 内置 Opus 编码推流。支持下行播放（对讲），内置环形缓冲区消除网络抖动。
---

## 3. 支持设备
| 设备 | 厂家 | 通信方式 | 默认地址 | 特点 |
|------|------|---------|---------|------|
| SC668s | 云迹 | TCP (URL指令+JSON返回) | 192.168.10.10:31001 | 导航/遥控/急停/状态订阅 |
| Lite3 | 云深处 | UDP (二进制指令码) | 192.168.2.1:43893 | 模式切换/离散移动/特殊动作/20ms心跳 |

## 设备感知按键映射

### 通用按键 (两个设备一致)

| 键 | 云迹 SC668s | Lite3 |
|----|------------|-------|
| W | 前进 (速度递增) | 前进 (离散动作) |
| S | 后退 (速度递减) | 后退 (离散动作) |
| Space | 停止 | 停止 |
| H | 帮助 | 帮助 |
| Q | 退出 | 退出 |
| Tab | 切换设备 | 切换设备 |

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

###3. JSON 协议与操作指令全集
外部控制端可通过任意网络通道（TCP / MQTT / Agora RTM）下发以下 JSON 指令。若未填写 device 字段，默认发往当前“激活设备”；
   指定 "device":"lite3" 或 "device":"yunji" 可强制发往特定设备；指定 "device":"all" 发往所有已连接设备
###3.1 通用控制与设备切换
// 切换当前激活设备 (在 yunji 和 lite3 之间切换)
{"cmd":"switch_device"}

###3.2 云深处 (Lite3) 专属指令
// 模式切换 (stand:站立, move:移动, inplace:原地, auto:自主, manual:手动)
{"cmd":"mode","mode":"stand","device":"lite3"}

// 离散动作控制 (底层通过语音指令码实现)
{"cmd":"action","action":"forward","device":"lite3"}       // 前进
{"cmd":"action","action":"backward","device":"lite3"}      // 后退
{"cmd":"action","action":"strafe_left","device":"lite3"}   // 左平移
{"cmd":"action","action":"strafe_right","device":"lite3"}  // 右平移
{"cmd":"action","action":"stop","device":"lite3"}          // 停止
{"cmd":"action","action":"greet","device":"lite3"}         // 打招呼
{"cmd":"action","action":"jump","device":"lite3"}          // 跳跃
{"cmd":"action","action":"twist","device":"lite3"}         // 扭身体
{"cmd":"action","action":"turn_left_90","device":"lite3"}  // 左转90度
{"cmd":"action","action":"turn_right_90","device":"lite3"} // 右转90度
{"cmd":"action","action":"turn_180","device":"lite3"}      // 后转180度

// 轴连续控制 (支持 20Hz 持续发送，死区外生效)
value 取值范围为[-32767,32767]， 当轴指令取值在死区范围内时，视其值为 0，机器人停止移动。轴指令值的正负确定速度方向
{"cmd":"axis","axis":"x","value":30000,"device":"lite3"}    // 前后平移 (正前负后，死区±6553) 指定机器人 x 轴上的期望线速度，正值向前
{"cmd":"axis","axis":"y","value":20000,"device":"lite3"}    // 左右平移 (正右负左，死区±12553) 指定机器人 y 轴上的期望线速度，正值向右
{"cmd":"axis","axis":"yaw","value":10000,"device":"lite3"}  // 左右旋转 (正右负左，死区±9553) 指定机器人的期望角速度，正值向右转
{"cmd":"axis","axis":"height","value":1000,"device":"lite3"}// 身体高度 (仅原地模式，死区±20000) 取正值时抬高身体

**注意：轴指令由 Lite3Driver 独立线程以 **50ms / 20Hz** 周期持续发送当前轴值。停止/急停时清零轴值并停止### 3.2 步态

// 步态切换
{"cmd":"gait","gait":"low","device":"lite3"}       // 慢速
{"cmd":"gait","gait":"medium","device":"lite3"}    // 中速
{"cmd":"gait","gait":"high","device":"lite3"}      // 高速
{"cmd":"gait","gait":"crawl","device":"lite3"}     // 爬行
{"cmd":"gait","gait":"grip","device":"lite3"}      // 抓地
{"cmd":"gait","gait":"general","device":"lite3"}   // 通用
{"cmd":"gait","gait":"high_step","device":"lite3"} // 高抬腿

// 自主模式速度控制 (范围: vx[-1,1]m/s, vy[-0.5,0.5]m/s, yaw[-1.5,1.5]rad/s)
{"cmd":"velocity","vx":0.3,"vy":0.0,"yaw":0.1,"device":"lite3"}

**注意：速度指令要求机器人处于自主模式。测试工具在速度测试前会发送 `mode=auto`。

// 高级动作
{"cmd":"continuous_motion","enabled":true,"device":"lite3"}  // 持续运动 (未受指令时原地踏步)
**注意：持续运动开启后，机器人在未收到轴指令时也将保持原地踏步。

{"cmd":"save_data","device":"lite3"}                         // ⚠️ 保存故障数据并关闭运动程序 (高风险)
**注意：保存数据 动作 这是高风险测试项。** 按 V1.0.7 接口语义，该指令用于保存故障数据并关闭运动程序。测试工具默认要求输入 `YES` 二次确认。

###3.3 声网 (Agora) 音视频专属指令
// 加入音视频频道
{"cmd":"av_join","channel":"testChannel","uid":1001,"token":"你的Token(可选)"}

// 退出频道
{"cmd":"av_leave"}

// 流开关控制
{"cmd":"av_video","enabled":true}   // 开/关视频推流
{"cmd":"av_audio","enabled":true}   // 开/关麦克风推流
{"cmd":"av_listen","enabled":true}  // 开/关下行对讲播放

// 状态查询
{"cmd":"av_status"}
   

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

### MQTT Telemetry

Lite3 的 `0x0901/0x0902/0x0903` 最新状态由 lljs_ctrl 发布到：

device/telemetry
```

其它 MQTT Client 可以：

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

### 5. 配置

所有运行时参数通过 YAML 配置文件设定，模板见 `config/config.yaml`：
# ── 全局配置 ──
device: lite3            # 连接设备: yunji / lite3 / both
log_level: info          # 日志级别: debug / info / warn / error
default_active: ""       # 默认激活设备 (留空自动)

# ── 外部控制端网络通道 (可同时开启) ──
network:
  mode: client           # 兼容旧配置，建议使用下方独立开关
  server_enabled: false  # 开启本机 TCP Server
  client_enabled: true   # 开启本机 TCP Client 主动连接
  mqtt_enabled: false    # 开启 MQTT 通道

# TCP Server 配置 (本机监听)
server:
  bind: "0.0.0.0"
  port: 9000

# TCP Client 配置 (主动连外部)
client:
  server_ip: "175.24.177.82"
  server_port: 20001
  reconnect_interval_ms: 5000
  edge_id: "edge-1"
  robot_id: "lite3-1"
  device: "lite3"

# MQTT 通信配置
mqtt:
  broker_enabled: false        # 启用内置 Mosquitto Broker
  broker_bind: "0.0.0.0"
  broker_port: 1883
  broker_executable: ""        # 留空自动搜索
  broker_config: "/tmp/lljs_ctrl_mosquitto.conf"
  broker_allow_anonymous: true
  client_enabled: false        # 启用 MQTT Client (连接本地或外部 Broker)
  host: "127.0.0.1"
  port: 1883
  client_id: "lljs_ctrl"
  username: ""
  password: ""
  reconnect_interval_ms: 5000

# ── 云迹 SC668s 配置 ──
yunji:
  ip: "192.168.10.10"
  port: 31001

# ── 云深处 Lite3 配置 ──
lite3:
  ip: "192.168.1.120"          # 连接目标 IP
  port: 43893                  # 控制端口
  receive_port: 43897          # 状态接收端口
lite3_heartbeat:
  interval_ms: 20              # 心跳间隔 (20ms)

# ── 遥控参数限幅 ──
control:
  joy_hz: 5                    # 遥控发送频率 (云迹使用)
  max_linear: 0.5              # 最大线速度 (m/s)
  max_angular: 1.0             # 最大角速度 (rad/s)
  linear_step: 0.05            # 键盘调节步长
  angular_step: 0.1

# ── 状态与日志 ──
status:
  poll_hz: 2                   # 机器人状态轮询频率
log:
  queue_size: 20               # 终端 HUD 显示日志条数

# ── 声网 (Agora) 音视频集成 ──
agora:
  enabled: true                # ★ 总开关
  app_id: "your_app_id"
  license: ""
  area_code: "CN"              # CN / GLOB / NA / EU / AS / JP
  log_level: 7                 # -1(关), 4(ERROR), 7(INFO)
  
  rtm:
    enabled: true
    uid: "robot-1"
    token: ""
    mode: "both"               # peer / channel / both
    channel: "testChannel"
    operator_uid: "operator-1"
    ack_enabled: true

  rtsa:
    channel: "testChannel"
    uid: 1001
    token: ""
    token_renew: true
    rtcm_signaling: true
    auto_join: true
    subscribe: false

  video:
    enabled: true
    device: "/dev/video21"     # 摄像头节点
    mode: "h264"               # h264:硬编 / yuv:直传
    width: 1280
    height: 720
    fps: 25
    bitrate_kbps: 1800
    gop: 20

  audio:
    enabled: true
    device_in: "plughw:1,0"    # 麦克风节点
    device_out: "default"      # 扬声器节点
    sample_rate: 16000
    channels: 1
    frame_ms: 20
    play_sample_rate: 0
    listen: true               # 启动即开启下行对讲

  vad:
    enabled: false             # 语音活动检测 (预留)

---

## 6.  编译与依赖
##6.1 环境依赖
操作系统: Linux (推荐 Ubuntu 20.04/22.04 ARM64)
编译工具: CMake >= 3.10, GCC/G++ (支持 C++14)
第三方库:
	libasound2-dev (ALSA 音频)
	libmosquitto-dev (MQTT)
	声网 RTSA SDK (libagora-rtc-sdk.so)
	瑞芯微 MPP 库 (针对 RK3588)

项目同时支持 Makefile 和 CMake 两种构建方式，并支持 x86_64 (PC) 和 ARM64 (RK3588/SC668s) 双架构编译。
在编译前，请确保执行了环境准备脚本（拉取声网 SDK、MPP 库等）：
./scripts/prepare_agora_env.sh

### 6.2 Make
# 查看支持的指令
make help

# 默认纯控制端编译 (x86 架构)
make

# ARM 架构编译纯控制端
make PLATFORM=arm

# 编译包含声网音视频的完整版 (x86 架构)
make AGORA=1

# 编译包含声网音视频的完整版 (ARM 架构)
make PLATFORM=arm AGORA=1

# 清理产物
make clean

### 6.2  使用 CMake (标准构建)
注意：开启 Agora 需显式传入 -DLLJS_WITH_AGORA=ON。请确保 third_party/ 下已准备好对应的头文件和库。

x86_64 (PC) 编译：(真实部署未使用)

	mkdir build && cd build
	make .. -DBUILD_PLATFORM=x86 -DLLJS_WITH_AGORA=ON
	make -j$(nproc)
		```

ARM64 (板端) 编译：

```bash
cmake -S . -B build-arm -DBUILD_PLATFORM=arm -DLLJS_WITH_AGORA=ON
cmake --build build-arm -j1
```
或：
	- Compile for ARM hosts:

		mkdir build && cd build
		cmake .. -DBUILD_PLATFORM=arm -DLLJS_WITH_AGORA=ON
		make -j$(nproc)
		```

## 7. 启动服务

# 指定配置文件启动
./lljs_ctrl -c config/config.yaml

# 校验配置文件格式是否正确
./lljs_ctrl --check-config -c config/config.yaml

不指定 `-c` 时按以下顺序搜索默认配置：
`./config.yaml` → `./config/config.yaml` → `/etc/lljs_ctrl/config.yaml`

### server 模式

配置：

```yaml
network:
  mode: server

server:
  bind: "0.0.0.0"
  port: 9000
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

## 8. Python 测试工具
Web 拉流测试：用浏览器打开 test/web_demo_h264.html，输入对应的 AppID 和 Channel 即可验证视频画面与语音对讲。
Python 控制台测试：python3 test/test_client.py 127.0.0.1 9000 --dashboard

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



