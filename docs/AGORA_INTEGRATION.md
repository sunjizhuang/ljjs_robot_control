# Agora RTSA 音视频 + 信令集成说明

> 对应设计方案: 《ljjs_robot_control × Agora RTSA-Lite 集成方案》v1.2
> SDK: Agora RTSA-Lite v1.10.1 (aarch64 Linux 动态库)
> 平台: RK3588 / Ubuntu 20.04, x86 交叉编译

## 1. 架构概览

```
信令(控制面):  TCP Server ─┐
              TCP Client ──┼─► handleRemoteCommandFrom → InputAdapter → CmdTranslator → 设备
              MQTT       ──┤
              RTM/RTCM  ──┘  (AgoraBridge: av_* 拦截, 其余 JSON 原样进入指令链)

媒体(媒体面):  V4L2(UVC) → MPP 硬编 H.264 → RTSA send_video_data
              ALSA 采集 16k/20ms PCM → SDK 内置 Opus → send_audio_data
              RTSA on_audio_data → RingBuffer → ALSA 播放(下行对讲)
```

- 信令三路并行: **RTM peer**(常驻) + **RTM channel**(广播) + **RTCM**(入频后, 同音视频通道, ≤1024B/条, 延迟极低)。
- 新增 JSON 指令: `av_join / av_leave / av_video / av_audio / av_listen / av_status`。
- 总开关 `agora.enabled=false` 或编译期不开启时, 行为与原版完全一致。

## 2. 新增文件

```
third_party/
├── agora_sdk/                # 声网 SDK (include + lib/aarch64)
├── alsa/                     # 板端拷入: include/alsa + lib/aarch64/libasound.so*
└── rockchip_mpp/             # 板端拷入: include/rockchip + lib/aarch64/librockchip_mpp.so*
src/agora/
├── agora_service.h/.cpp      # 核心: 生命周期/状态机/回调联动/媒体管线/消息分发
├── agora_bridge.h/.cpp       # av_* 指令拦截 + ACK + 遥测出口(RTCM 优先)
├── v4l2_capture.h/.cpp       # UVC 采集 (YU12/NV12 探测, MJPEG 本期不支持)
├── mpp_encoder.h/.cpp        # RK MPP H.264 (IDR/码率动态联动, EACH_IDR 头)
├── alsa_capture.h/.cpp       # 16k/1ch/20ms PCM 采集
├── alsa_player.h/.cpp        # 下行播放 (RingBuffer 防延迟累积)
├── av_pacer.h                # 发送节奏器
└── av_source.h               # 采集抽象 + VAD 预留(本期恒直通)
scripts/prepare_agora_env.sh  # Linux 构建机一次性环境准备
test/test_rtm_cmd.py          # RTM 指令联调 (封装预编译 hello_rtm)
test/test_rtcm_cmd.py         # RTCM 指令联调 (封装预编译 hello_rtcm)
test/AGORA_WATCH_GUIDE.md     # Web 观看/对讲指引
```

现有代码仅 3 处改动: `yaml_config`(agora 配置段+解析器支持多层嵌套)、
`keyboard_control`(指令前置过滤器 setCommandFilter)、`main.cpp`(组装, 全部宏守护)。

## 3. 环境准备（Linux 构建机, 一次性）

```bash
cd ljjs_robot_control
./scripts/prepare_agora_env.sh <板端IP> [root]
# 脚本完成: 1) lib/aarch64 补 mosquitto/ssl/crypto 软链接
#           2) 从板端拷贝 ALSA/MPP 头文件+库到 third_party/
#           3) 补齐 third_party 软链接
```

## 4. 编译

```bash
# CMake (推荐)
cmake -S . -B build-arm -DBUILD_PLATFORM=arm -DLLJS_WITH_AGORA=ON
cmake --build build-arm -j$(nproc)

# Make
make PLATFORM=arm AGORA=1

# 不集成 Agora (行为同原版)
cmake -S . -B build-arm -DBUILD_PLATFORM=arm && cmake --build build-arm -j
# 或: make PLATFORM=arm
```

## 5. 配置

`config/config.yaml` 的 `agora:` 段（模板已含完整注释）:

```yaml
agora:
  enabled: true            # ★ 总开关
  app_id: "你的AppID"
  rtm:      { enabled: true, uid: "robot-1", mode: "both", channel: "robot-cmd", operator_uid: "operator-1" }
  rtsa:     { channel: "robot-av", uid: 1001, auto_join: true }   # Token 鉴权时填 rtsa.token / rtm.token
  video:    { enabled: true, device: "/dev/video11", mode: "h264", width: 1280, height: 720, fps: 25, bitrate_kbps: 1800, gop: 75 }
  audio:    { enabled: true, device_in: "default", device_out: "default" }
```

配置校验: `./lljs_ctrl --check-config -c config/config.yaml`
完整查看: `./lljs_ctrl --dump-config -c config/config.yaml`

## 6. 运行与验证

```bash
# 板端部署: lljs_ctrl + third_party/agora_sdk/lib/aarch64/libagora-rtc-sdk.so (+ third_party 其他 .so 如板端没有)
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/third_party/agora_sdk/lib/aarch64
./lljs_ctrl -c config/config.yaml
```

验证顺序（对应方案里程碑）:

| 步骤 | 操作 | 通过标准 |
|------|------|---------|
| M1 | 用 SDK example 预编译 hello_rtsa 先行验证频道/推流链路 | Web 端看到画面 |
| M2 | `python3 test/test_rtm_cmd.py -i APPID -p robot-1 --cmd '{"cmd":"mode","mode":"stand","device":"lite3"}'` | 机器人执行指令 |
| M2 | `python3 test/test_rtcm_cmd.py -i APPID -c robot-av -u 2001 --cmd '...'` (入频后) | RTCM 路径同样生效 |
| M3 | Web Join 同频道 + `{"cmd":"av_listen","enabled":true}` | 双向对讲 |
| M4 | Web 观察画质; 断网重连 | 720p25 流畅; 恢复 3s 内出画面 |
| M5 | `{"cmd":"av_status"}`; `agora.enabled=false` 回归 | 状态正确; 原功能不受影响 |

## 7. av_* 指令参考

| 指令 | 说明 |
|------|------|
| `{"cmd":"av_join","channel":"x","uid":1001,"token":"..."}` | 入频推流（channel/uid/token 可省略用配置值） |
| `{"cmd":"av_leave"}` | 退频并停采集 |
| `{"cmd":"av_video","enabled":true\|false}` | 单独开关视频 |
| `{"cmd":"av_audio","enabled":true\|false}` | 单独开关音频上行 |
| `{"cmd":"av_listen","enabled":true\|false}` | 单独开关下行对讲播放 |
| `{"cmd":"av_status"}` | 状态查询（回执经 RTCM/RTM 推回） |

所有 av_* 指令可经 TCP/MQTT/RTM/RTCM 任意通道下发; 回执格式
`{"cmd":"av_ack","req":"...","ok":true,"state":"..."}`。


## 8. av_* 视频关键点设计
a.动态带宽自适应（BWE）与码率联动
  声网 SDK 会在网络不好时通过 cb_target_bitrate 回调告诉应用：“网卡了，降点码率”。
  代码捕获该回调后，调用 encoder_.setBitrateKbps()，在下一个编码帧到来时，动态修改 MPP 硬件编码器的目标码率。这样既不卡顿，画面也不会瞬间糊掉。

b.关键帧即时响应（防花屏）
  当有新的 Web 端观众进来，或网络丢包导致画面花屏时，声网会触发 cb_key_frame_req 回调。
  代码捕获后，调用 encoder_.requestKeyframe()。MPP 编码器会在下一帧立刻强制生成一个 IDR 关键帧（I 帧），让接收端瞬间恢复画面。

c.精细的 YUYV 降级转换
  应对了 UVC 摄像头不支持输出 YUV420 的痛点。如果读到的是 YUYV，代码会动态分配一块 conv_buf_ 内存，用纯 C 函数 yuyvToNv12 做无损重排，再送给编码器。

d.环形队列防积压（背压控制）
  video_queue_ 的最大长度被死死限制在 3 帧。
  如果网络严重卡顿，导致发送线程变慢，采集/编码线程依然会疯狂产帧。此时队列满了，就会直接丢弃最旧的帧（pop_front()），绝对不让延迟累积，保证用户看到的永远是“最新”的画面。

e.粘包与防碎包（发送节拍器）
使用 AvPacer 控制发送频率。网络底层绝不会瞬间把一堆帧挤出去导致 UDP 丢包，也不会因为太慢导致画面卡顿。

支持的编码情况:
视频发送层极其灵活，支持 两种视频模式，以及 三种采集格式：

8.2. 两种视频编码模式（cfg.video.mode）
a."h264" 模式（推荐，默认模式）：
摄像头原始帧 -> MPP 硬件编码 -> H.264 码流。
发送给声网时，info.data_type = VIDEO_DATA_TYPE_H264。
优点：极大地节省带宽（720p 仅需 1-2Mbps），不占用 RK3588 CPU，完美适配 Web 端（WebRTC 强依赖 H.264）。

b."yuv" 直传模式：
不经过 MPP 编码，直接发送 YU12 裸流。
发送时，info.data_type = VIDEO_DATA_TYPE_YUV420。
缺点：带宽消耗极大（720p 一帧就需要 1.3MB），但代码里限制了仅当摄像头本身输出 YU12 时才允许开启。

c. 三种采集格式支持（V4l2Capture::PixFormat）
NV12：直接送入 MPP，零转换。             ---(4:2:0),读取时的内存不跳跃,硬件编码（MPP）的完美形态,
YU12 (I420)：直接送入 MPP，零转换。      ---YU12 (I420)高压缩（4:2:0）。每 4 个像素共享一组 UV 颜色信息
YUYV (4:2:2)：软件转 NV12 后送入 MPP。    ---YUYV (4:2:2) ,无压缩,一个像素平均占 2 字节
MJPEG：当前版本不支持（代码里直接报错并建议换摄像头）。

d.视频整体流程（数据链路）
整个视频数据流走的是多线程流水线（Pipeline）模型:
[USB/MIPI 摄像头]
      ↓ (V4L2 / mmap)
[V4l2Capture 采集线程] 
      ↓ (原始帧回调 onCapturedVideoFrame)
[格式转换 (可选)] ── 若是 YUYV 格式 ──> [yuyvToNv12 软件转换]
      ↓ (送入编码器)
[MppEncoder 硬件编码器] ──> 吐出 H.264 裸流
      ↓ (PacketCallback)
[onEncodedPacket 压入队列] (队列长3, 防堆积)
      ↓
[videoSendLoop 发送线程] ── (受 AvPacer 节拍器控制) ──> [agora_rtc_send_video_data]

## 9. 已知限制 / 后续项

1. **YUV420 直传模式**（`video.mode: yuv`）依赖 SDK 对 `VIDEO_DATA_TYPE_YUV420` 的编码支持，未经实测；默认且推荐 `h264` 模式。首次使用 yuv 模式请先用 hello_rtsa 验证。
2. **MJPEG 摄像头**本期不支持（需 MPP JPEG 硬解前置级, 见 v4l2_capture.cpp TODO）。
3. **VAD** 仅预留接口与配置（`agora.vad.enabled`），本期恒直通。
4. 下行播放采样率默认跟随上行配置（16k）；若对讲音调异常，设置 `agora.audio.play_sample_rate`（SDK 解码输出采样率可能与上行不同）。
5. RTM 无自动重登（进程级常驻）; RTC 断线由 SDK 自动重连, 重连成功回调会重启媒体管线。
