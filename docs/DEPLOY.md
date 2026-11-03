# RK3588 部署指南

适用：编译成功后，将 lljs_ctrl 部署到一台**全新的** RK3588 设备（Ubuntu 20.04）。

## 1. 部署布局

```
/opt/lljs/
├── lljs_ctrl                 aarch64 可执行
├── config/config.yaml
└── lib/
    ├── libagora-rtc-sdk.so   ← 必须随包（声网 SDK）
    ├── libmosquitto.so.1     ← 必须随包（板端可能无 1.6.9）
    ├── libssl.so.1.1 / libcrypto.so.1.1 ← 必须随包（板端若为 OpenSSL 3.x 则必须）
    ├── libasound.so.2        ← 板上有系统库时优先用系统的
    └── librockchip_mpp.so.1  ← 板上有系统库时优先用系统的
```

> 库搜索顺序：`LD_LIBRARY_PATH`（指向随包 lib/）→ 板端系统库。若希望强制用板端
> 系统库（版本与硬件镜像严格一致），把对应 .so 从随包 lib/ 删除即可。

## 2. 一键部署（构建机执行）

```bash
cd ljjs_robot_control
./scripts/deploy.sh root@<板端IP>          # 默认部署到 /opt/lljs
./scripts/deploy.sh root@<板端IP> /data/lljs
```

脚本自动完成：打包产物 + 随包库 → **单连接 tar 管道上传（只需输 2 次密码）** → 板端依赖/硬件检查。
全新设备执行一次：`mv /opt/lljs/config/config.yaml.new /opt/lljs/config/config.yaml`。

> **免密建议**：调试期间频繁部署，先在构建机执行 `ssh-copy-id root@<板端IP>` 配置密钥登录，
> 之后 deploy.sh 全程零密码。也可用 `apt install sshpass` 后
> `sshpass -p <密码> ./scripts/deploy.sh ...` 自动填密码（仅测试环境）。

## 3. 新设备检查清单（板端手动确认）

| 检查项 | 命令 | 期望 |
|---|---|---|
| 架构 | `uname -m` | aarch64 |
| glibc | `ldd --version` | ≤ 2.31（Ubuntu 20.04；与工具链一致） |
| MPP 运行库 | `ls /usr/lib/aarch64-linux-gnu/librockchip_mpp.so*` | 有（RK3588 出厂镜像一般自带 librockchip-mpp1） |
| ALSA 运行库 | `ls /usr/lib/aarch64-linux-gnu/libasound.so.2*` | 有 |
| 摄像头 | `v4l2-ctl --list-devices`（无则 `ls /dev/video*`） | 存在 UVC 设备节点，确认 config.yaml 中 `video.device` 对应 |
| 麦克风 | `arecord -l` | 存在采集卡，确认 `audio.device_in` |
| 扬声器（对讲） | `aplay -l` | 需要 `av_listen` 时确认 `audio.device_out` |
| MQTT broker（可选） | `which mosquitto` | 仅 `mqtt.broker_enabled: true` 时需要（`apt install mosquitto`） |

## 4. 新设备必须修改的配置（config.yaml）

1. `agora.enabled` → `true`（联调期）；`agora.app_id` 填声网 App ID
2. `agora.rtsa.uid` — 每台设备唯一（如 `1001`、`1002`），避免频道内冲突
3. `agora.video.device` — 该设备实际的摄像头节点（不一定还是 `/dev/video11`）
4. `agora.audio.device_in/device_out` — `arecord -l`/`aplay -l` 查到的卡，或保持 `default`
5. Token 鉴权模式才需要 `agora.rtsa.token` / `agora.rtm.token`

## 5. 运行与验证

```bash
# 前台运行（首次调试）
LD_LIBRARY_PATH=/opt/lljs/lib /opt/lljs/lljs_ctrl -c /opt/lljs/config/config.yaml

# 检查日志关键行
#   [agora] RTM 登录成功            → 信令通道就绪
#   [v4l2] 已打开 /dev/videoX ...   → 摄像头 OK
#   [mpp] 打开: 1280x720@25 ...     → 硬编码 OK
#   [alsa-capture] 已打开 ...       → 麦克风 OK
```

**遥控验证（PC 端）**：

```bash
# RTM 指令（状态查询 + 控制）
python3 test/test_rtm_cmd.py --app-id <APPID> --uid pc-1 --peer 1001

# RTCM 指令（入频后低延迟通道）
python3 test/test_rtcm_cmd.py --app-id <APPID> --uid pc-2 --channel robot-av

# 观看画面：浏览器打开 test/AGORA_WATCH_GUIDE.md 中的 Web demo 页 https://webdemo.agora.io/index.html，
# 频道 robot-av、uid 自取、模式选 H.264
```

## 6. systemd 开机自启（可选）

`/etc/systemd/system/lljs.service`：

```ini
[Unit]
Description=LLJS Robot Control
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/opt/lljs/lljs_ctrl -c /opt/lljs/config/config.yaml
Environment=LD_LIBRARY_PATH=/opt/lljs/lib
Restart=always
RestartSec=3
User=root

[Install]
WantedBy=multi-user.target
```

```bash
systemctl daemon-reload && systemctl enable --now lljs
journalctl -u lljs -f        # 看日志
```

## 7. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `GLIBC_2.3x not found` | 工具链 glibc 新于板端 | 用 Ubuntu 20.04 主机工具链重编 |
| 启动即 `error while loading shared libraries` | LD_LIBRARY_PATH 未设或缺库 | `LD_LIBRARY_PATH=/opt/lljs/lib ldd /opt/lljs/lljs_ctrl` 逐个核对 |
| 视频 `/dev/videoX` 打不开 | 节点与 config 不符 / 被 fuser 占用 | `v4l2-ctl --list-devices` 重新确认；`fuser /dev/video*` |
| MPP 打开失败 | 板端无 librockchip_mpp 或与随包版本冲突 | 删除随包 lib 中的 librockchip_mpp.so.1，强制用板上系统库 |
| RTM 登录失败 | App ID 错误 / License 问题 / 网络出不去 | 核对 app_id；`on_license_validation_failure` 时检查 license 配置 |
| 无线网卡设备时钟漂移导致音频卡顿 | NTP 未同步 | `timedatectl set-ntp true` |
