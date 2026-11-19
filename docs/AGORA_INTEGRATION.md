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

## 9. 能力查看和配置联动
9.1摄像头能力确认：
     v4l2-ctl --list-devices  -------------查看系统中所有摄像头及其对应设备号
  用 v4l2-ctl -d /dev/video21 --list-formats-ext 看一下，绿联摄像头在 YUYV 或 NV12
*********************************************************************************
video.width/height
	1K 分辨率 1080p (1920 x 1080)---目前机器视觉和控制流媒体最标准、最稳妥的黄金分辨率。
								码率联动：如果改成 1920x1080，把下面的 bitrate_kbps: 1改为 2500 或 3000。
								在 H.264 编码下，1080p@25fps 的画面，2Mbps (2000kbps) 的码率能提供非常清晰、没有马赛克的画质。
	2K 分辨率 (2560 x 1440)-----码率 (bitrate_kbps)：建议设为 4000 (4 Mbps),
								帧率 (fps)：建议保持在 25。如果画质要求极高，可以降到 15,
								GOP (gop)：建议设为 50（约 2 秒一个 I 帧）
	4K 分辨率 (3840 x 2160)-----码率 (bitrate_kbps)：至少需要 8000,甚至 10000 (10 Mbps)。
								帧率 (fps)：强建议降到 15。4K 30fps 的实时硬编+推流对系统的瞬时冲击极大
								建议设为 30（2秒一个 I 帧）。因为 4K 的 I 帧非常大（可能高达 100KB+），如果 GOP 太小，网络会被 I 帧瞬间塞满；
		
	要 1080p 的高清：必须去改代码，让 V4l2Capture 接收 MJPG 格式，然后写一个 MJPEG 转 NV12 的函数（或者用 MPP 硬解），然后再给 H.264 编码器。
	只有这样，你才能在这个摄像头上跑出 1080p @ 25fps 的丝滑画面。
	
	想要 1080p 分辨率 + 25 帧 + H.264 编码，你必须去修改 v4l2_capture.cpp，让它支持请求 V4L2_PIX_FMT_MJPEG，并在回调里引入 JPEG 解码功能（无论是软件 libjpeg 还是 RK 的 MPP 硬解），
	把收到的 JPEG 解成 NV12，再送进 MPP。这是做实时音视频推流必须迈过的一道坎。

*********************************************************************************
video.width.bitrate_kbps
	1. 启动阶段：告诉 MPP 目标大小（CBR 码控）
		当程序初始化视频管线时，bitrate_kbps 被传给 MppEncoder::open()：

		mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR); // 恒定码率模式
		mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate_kbps_ * 1000);
		mpp_enc_cfg_set_s32(cfg, "rc:bps_max",    bitrate_kbps_ * 1000 * 4 / 3); // 最大允许 1.33 倍突发
		mpp_enc_cfg_set_s32(cfg, "rc:bps_min",    bitrate_kbps_ * 1000 * 2 / 3); // 最小允许 0.66 倍
		作用：MPP 硬件编码器会开启 CBR（恒定码率） 模式。这意味着无论画面是静止的（比如对着白墙）还是剧烈运动的，它都会尽量把输出的 H.264 数据量控制在 1800kbps 左右。
		为什么要有最大和最小？ 如果画面全黑，为了保持 1800kbps，MPP 会填充无用数据；如果有剧烈运动，最多允许飙升至 2400kbps（1800 * 4/3），避免画面瞬间糊成马赛克。

	2. 入频阶段：告诉声网服务器带宽预期（BWE）
		在 agora_service.cpp 的 joinChannel 中，你的代码用这个值设置了 BWE（带宽预估）的初始参数：

		const uint32_t max_bps = (uint32_t)cfg.video.bitrate_kbps * 1000 + 400000; // 视频码率 + 音频/重传余量
		agora_rtc_set_bwe_param(conn_id_, 200000, max_bps, max_bps / 2);
		作用：这段代码告诉声网 SDK：“我的视频要发 1800kbps，加上音频差不多 2000kbps 左右，请把带宽上限设为 2200kbps”。
		为什么这么干？ 因为声网 SDK 默认的初始带宽上限可能比较低（比如 1Mbps）。如果不手动调高，你的 MPP 编出来 1.8M 的数据，声网 SDK 会因为“觉得带宽不够”而大量丢弃视频帧，导致严重卡顿。

		3. 网络波动阶段：动态自适应调整（最核心的灵魂）
		这是声网 SDK 最强大的地方。在实际运行中，如果网络变差（比如 Wi-Fi 信号弱），声网服务器接收数据超时，会向你的设备下发一个回调 on_target_bitrate_changed。

		在你的 agora_service.cpp 中捕获了这个回调：
		void AgoraService::cb_target_bitrate(connection_id_t, uint32_t target_bps) {
		    s->target_bps_.store(target_bps);
		    if (g_config.agora.video.mode == "h264") {
		        // 动态下发新的码率给 MPP 编码器
		        s->encoder_.setBitrateKbps((int)(target_bps / 1000));
		    }
		}
		联动机制：假设当前网络很差，声网估算当前只能跑 800kbps，它会回调告诉你 target_bps = 800000。
		MPP 响应：你的代码会把 800 传给 MppEncoder::setBitrateKbps。在下一帧编码时（MppEncoder::encode 里的 lazy 处理），MPP 会修改 rc:bps_target，瞬间降低编码码率。画面可能会变糊一点，但视频绝对不会卡顿断开。
		当网络恢复，声网又会通知你调高码率，MPP 随之提升画质。

		4. 代码里的隐形限制
		在你的 mpp_encoder.cpp 里，有一层保护机制：
		bool MppEncoder::applyBitrate(int kbps) {
		    if (kbps < 200)  kbps = 200;
		    if (kbps > 4000) kbps = 4000;
		    // ...
		}
		无论声网下发多低的码率，MPP 最低只会降到 200kbps（再低就没法看了）。

		无论网络多好，最高只允许跑到 4000kbps
*********************************************************************************
**注意：严格约束（16 对齐）：代码注释里特意写了“需 16 对齐”。这是因为 RK3588 的 MPP 硬件编码器对内存跨度（Stride）有严格限制。


## 10. RTM 和 RTCP
RTM（独立信令通道）：
	生命周期：只要程序启动，且 rtm.enabled: true，它就会常驻登录（即使你还没加入音视频频道）。它的连接是稳定可靠的（类似 TCP）。
	容量：单包最大 31KB，支持点对点（peer）和频道广播（channel）。
	配置：使用 rtm.uid 和 rtm.token。

RTCM（依附于 RTC 通道）：
	生命周期：它必须在你成功加入音视频频道（av_join 成功，状态为 IN_CHANNEL）之后才能发送。如果没入频，发 RTCM 直接失败。
	容量：单包极小，最大 1024 字节。它是跟在音视频数据流里一起走的，延迟极低，但不保证 100% 到达（弱网下可能丢包）。
	配置：没有独立的账号密码，直接复用 RTSA 的频道连接。
	rtsa.rtcm_signaling: true   # 把这里改成 false，就彻底不用 RTCM 了

rtm.mode: "both"			# peer / channel / both
rtm.channel: "testChannel"         	# channel/both 模式的指令+遥测频道
rtm.operator_uid: "operator-1"   	# peer/both 模式的默认对端（远端控制端）
    
	1. mode: "peer"（点对点单播模式）
	含义：机器人只会把消息发给指定的某一个特定用户，并且只接收这个用户发来的消息。
	依赖配置：必须在下面配置 operator_uid（比如 "operator-1"）。
	工作原理：
	机器人上线后，调用 agora_rtm_send_data 将遥测数据（如电量、位置）精准发送给 operator_uid。
	如果有其他人想控制这台机器人，即使连上了同一个 App ID，机器人也不理他。
	适用场景：一对一专属遥控。比如一个操作员拿着手柄/Web 端，专门控制这台特定的机器狗，不需要其他旁观者干预。

	2. mode: "channel"（频道广播模式）
	含义：机器人会加入一个公共频道（Channel），在这个频道里发消息，频道内的所有人都能收到。
	依赖配置：必须在下面配置 channel（比如 "testChannel"）。
	工作原理：
	机器人登录后，会调用 agora_rtm_subscribe 订阅这个频道。
	发送遥测时，调用 agora_rtm_publish 把数据广播到频道。任何订阅了该频道的终端（比如监控中心的多个大屏、多个操作员的 Web 页面）
		都能同步看到机器人的状态。
	接收指令时，频道内任何人发的指令，机器人都能收到。
	适用场景：多方协同监控。比如一个机器人巡检，多个指挥中心的人都在看它的实时状态，且有权限的人都可以下发指令。

	3. mode: "both"（混合双工模式 - 你当前配置的）
	含义：同时开启以上两种模式。既进行点对点通信，又参与频道广播。
	工作原理：
	发送遥测：机器人会先把消息 publish 到 channel 频道里，同时也会把消息通过 send_data 发给 operator_uid 这个人。
	接收指令：机器人既监听频道里的指令，也监听 operator_uid 发来的点对点指令。
	适用场景：主从控制 + 数据上云。比如，机器人的底层安全指令由 operator_uid（主操作员）点对点直达控制；同时，机器人的运行状态
		通过 channel 广播，让云端后台和监控大屏都能实时接收。

## 10. 已知限制 / 后续项

1. **YUV420 直传模式**（`video.mode: yuv`）依赖 SDK 对 `VIDEO_DATA_TYPE_YUV420` 的编码支持，未经实测；默认且推荐 `h264` 模式。首次使用 yuv 模式请先用 hello_rtsa 验证。
2. **MJPEG 摄像头**本期不支持（需 MPP JPEG 硬解前置级, 见 v4l2_capture.cpp TODO）。
3. **VAD** 仅预留接口与配置（`agora.vad.enabled`），本期恒直通。
4. 下行播放采样率默认跟随上行配置（16k）；若对讲音调异常，设置 `agora.audio.play_sample_rate`（SDK 解码输出采样率可能与上行不同）。
5. RTM 无自动重登（进程级常驻）; RTC 断线由 SDK 自动重连, 重连成功回调会重启媒体管线。
