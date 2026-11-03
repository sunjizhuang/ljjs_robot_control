# Agora 音视频观看与对讲指引（操作员侧, Web）

机器人端把 H.264 视频 + PCM(→Opus) 音频推到声网频道，操作员用浏览器即可观看/对讲。

## 1. 前置条件

- 机器人端已按 `docs/AGORA_INTEGRATION.md` 启动，且 `av_join` 已入频
  （或 `agora.rtsa.auto_join: true`）；
- 你持有同一个 App ID；
- 浏览器: Chrome / Edge / Safari 均可（H.264 解码为浏览器标配能力）。

## 2. 观看音视频（WebRTC Demo 页面）

1. 打开声网官方 Web 示例:
   <https://webdemo.agora.io/example/quickStart/videoAndVoiceCalling/index.html>
2. 填写:
   - **App ID**: 与机器人配置一致的 App ID；
   - **Channel**: `robot-av`（即 `agora.rtsa.channel`）；
   - **User ID**: 随意（如 2001），不与机器人 uid(1001) 冲突；
   - Token 鉴权项目还需填 RTSA Token（官方页面对应 "RTC Token"）。
3. 点击 **Join**，稍候即可看到机器人摄像头画面、听到麦克风声音。
4. 对讲: 页面开启你的麦克风，机器人端 `{"cmd":"av_listen","enabled":true}`
   后即可从机器人扬声器听到你的声音。

## 3. 发送控制指令

音视频频道内走 RTCM 低延迟路径（推荐，需与机器人同频道）:

```bash
python3 test/test_rtcm_cmd.py -i <APP_ID> -c robot-av -u 2001 --cmd '{"cmd":"axis","axis":"x","value":3000,"device":"lite3"}'
```

或走 RTM（无需入频道，机器人启动后即常驻可用）:

```bash
python3 test/test_rtm_cmd.py -i <APP_ID> -p robot-1 --cmd '{"cmd":"mode","mode":"stand","device":"lite3"}'
```

指令格式与 TCP/MQTT 完全相同（见项目 README「JSON 协议」一节），
外加音视频专用的 `av_*` 指令:

| 指令 | 说明 |
|------|------|
| `{"cmd":"av_status"}` | 查询 Agora 状态（回执经 RTCM/RTM 推回） |
| `{"cmd":"av_join","channel":"robot-av","uid":1001}` | 入频推流 |
| `{"cmd":"av_leave"}` | 退频 |
| `{"cmd":"av_video","enabled":false}` | 关闭视频发送 |
| `{"cmd":"av_audio","enabled":false}` | 关闭音频上行 |
| `{"cmd":"av_listen","enabled":true}` | 开启下行对讲播放 |

## 4. 常见问题

| 现象 | 排查 |
|------|------|
| 加入频道看不到画面 | 确认机器人 `av_status` state=IN_CHANNEL 且 video_enabled=true；确认频道名/App ID 一致 |
| 有画面没声音 | 确认 `av_audio` enabled；ALSA 采集设备名正确（`arecord -l` 查看） |
| 对讲无声 | 确认 `av_listen` enabled；`aplay -l` 查看播放设备；若音调异常调 `agora.audio.play_sample_rate` |
| Token 报错 109/110 | 项目启用了 Token 鉴权但配置未填/过期——填入 `rtsa.token`/`rtm.token`（两者不可混用） |
| Error 105 | App ID 鉴权方式不匹配（项目为 App ID 模式时 Web 端也要选 App ID） |
