#include "agora_bridge.h"
#include "../config/yaml_config.h"
#include "../util/log.h"
#include "../util/json.h"

#define TAG "agora-bridge"

AgoraBridge::AgoraBridge(AgoraService* svc)
    : svc_(svc)
{
    if (svc_) {
        svc_->setMessageSink([this](const AgoraMessage& m) { onServiceMessage(m); });
    }
}

void AgoraBridge::onServiceMessage(const AgoraMessage& msg)
{
    /* 1. av_* 指令: 本地处理 (不进入设备指令链) */
    if (tryHandleCommand(msg.payload, msg.source)) {
        return;
    }
    /* 2. 业务 JSON: 转入现有指令处理链 (KeyboardController::handleRemoteCommandFrom) */
    if (cmd_sink_) {
        cmd_sink_(msg.payload, msg.source);
    } else if (msg_sink_) {
        msg_sink_(msg);
    }
}

bool AgoraBridge::tryHandleCommand(const std::string& text, const std::string& source)
{
    /* 快速预筛: 非裸 JSON / 不含 av_ 前缀的直接放行 */
    if (text.empty() || text.front() != '{') return false;

    JsonValue root;
    if (!json_parse(text, root) || !root.isObject()) return false;

    if (!root.has("cmd")) return false;
    const std::string cmd = root["cmd"].asString();
    if (cmd.compare(0, 3, "av_") != 0) return false;

    LOG_INFO("[%s] av 指令 [%s]: %s", TAG, source.c_str(), text.c_str());

    if (!svc_) {
        LOG_ERR("[%s] AgoraService 未就绪", TAG);
        return true;
    }

    if (cmd == "av_join") {
        std::string channel = root.has("channel") ? root["channel"].asString() : "";
        std::string token   = root.has("token")   ? root["token"].asString()   : "";
        uint32_t uid = root.has("uid") ? (uint32_t)root["uid"].asInt() : 0;
        std::string err;
        bool ok = svc_->joinChannel(channel, uid, token, err);
        if (!ok) LOG_WARN("[%s] av_join 失败: %s", TAG, err.c_str());
        if (g_config.agora.rtm.ack_enabled) {
            sendAck("av_join", ok, ok ? "JOINING" : "ERROR");
        }
        return true;
    }

    if (cmd == "av_leave") {
        std::string err;
        bool ok = svc_->leaveChannel(err);
        if (g_config.agora.rtm.ack_enabled) {
            sendAck("av_leave", ok, "READY");
        }
        return true;
    }

    if (cmd == "av_video" || cmd == "av_audio" || cmd == "av_listen") {
        bool en = root.has("enabled") ? root["enabled"].asBool() : false;
        std::string err;
        bool ok = true;
        if      (cmd == "av_video")  ok = svc_->enableVideo(en, err);
        else if (cmd == "av_audio")  ok = svc_->enableAudio(en, err);
        else                         ok = svc_->enableListen(en, err);
        if (!ok) LOG_WARN("[%s] %s 失败: %s", TAG, cmd.c_str(), err.c_str());
        if (g_config.agora.rtm.ack_enabled) {
            sendAck(cmd, ok, en ? "ENABLED" : "DISABLED");
        }
        return true;
    }

    if (cmd == "av_status") {
        publishTelemetry(svc_->statusJson());
        return true;
    }

    LOG_WARN("[%s] 未知的 av 指令: %s", TAG, cmd.c_str());
    return true;
}

void AgoraBridge::sendAck(const std::string& req, bool ok, const std::string& state)
{
    std::string ack = "{\"cmd\":\"av_ack\",\"req\":\"" + req +
                      "\",\"ok\":" + (ok ? "true" : "false") +
                      ",\"state\":\"" + state + "\"}";
    publishTelemetry(ack);
}

void AgoraBridge::publishTelemetry(const std::string& json)
{
    const AgoraConfig& cfg = g_config.agora;
    bool sent = false;

    /* 1. RTCM 优先: 入频且开启 rtcm_signaling, 单条 ≤1024B */
    if (svc_ && svc_->inChannel() && cfg.rtsa.rtcm_signaling &&
        json.size() <= 1024) {
        sent = svc_->sendMediaCtrlStr(json);   /* remote_uid=0 频道广播 */
    }

    /* 2. RTM 兜底/并行: channel 模式 publish, peer 模式回默认对端 */
    if (cfg.rtm.enabled && (cfg.rtm.mode == "channel" || cfg.rtm.mode == "both")) {
        svc_->rtmPublishChannel(cfg.rtm.channel, json);
        sent = true;
    }
    if (cfg.rtm.enabled && (cfg.rtm.mode == "peer" || cfg.rtm.mode == "both")) {
        svc_->rtmSendPeer(cfg.rtm.operator_uid, json);
        sent = true;
    }

    if (!sent) {
        LOG_DBG("[%s] 遥测无处可发 (未入频且 RTM 未启用)", TAG);
    }
}
