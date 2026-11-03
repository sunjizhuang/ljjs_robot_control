#ifndef AGORA_BRIDGE_H
#define AGORA_BRIDGE_H

#include <string>
#include <functional>
#include "agora_service.h"

/*
 * AgoraBridge —— 信令指令桥
 *
 * 职责:
 *   1. 指令拦截: 从指令链入口尝试解析 av_* JSON 指令并执行 (返回 true 表示已处理)。
 *      非 av_* 指令原样放行, 进入原有 InputAdapter → CmdTranslator 链路。
 *   2. 消息分发: RTM/RTCM 收到的非 av_* JSON → handleRemoteCommandFrom
 *      (通过 sink 注入, 由 main 组装, 避免依赖 KeyboardController 类型)。
 *   3. 遥测出口: publishTelemetry() — RTCM 优先(≤1024B), 未入频回退 RTM。
 *   4. av_* 指令回执 (av_ack), 通道选择同遥测。
 *
 * av_* 指令集 (与原 JSON 协议同风格, 三路信令 + TCP/MQTT 均可下发):
 *   {"cmd":"av_join","channel":"robot-av","uid":1001,"token":"..."}
 *   {"cmd":"av_leave"}
 *   {"cmd":"av_video","enabled":true}
 *   {"cmd":"av_audio","enabled":true}
 *   {"cmd":"av_listen","enabled":true}
 *   {"cmd":"av_status"}
 */
class AgoraBridge {
public:
    /* 非 av_* 消息的下游处理 (注入 KeyboardController::handleRemoteCommandFrom) */
    typedef std::function<void(const std::string& payload, const std::string& source)>
        CommandSink;

    explicit AgoraBridge(AgoraService* svc);

    void setCommandSink(CommandSink sink) { cmd_sink_ = std::move(sink); }
    void setMessageSink(AgoraService::MessageSink sink) { msg_sink_ = std::move(sink); }

    /* 指令链入口前置调用: 返回 true = 本条指令已被处理 (av_*), 调用方直接返回 */
    bool tryHandleCommand(const std::string& text, const std::string& source);

    /* RTM/RTCM 收到消息 → av_* 自处理, 其余转 CommandSink */
    void onServiceMessage(const AgoraMessage& msg);

    /* 遥测发布 (main.cpp 遥测线程每 200ms 调用) */
    void publishTelemetry(const std::string& json);

private:
    void sendAck(const std::string& req, bool ok, const std::string& state);

    AgoraService* svc_;
    CommandSink   cmd_sink_;
    AgoraService::MessageSink msg_sink_;
};

#endif /* AGORA_BRIDGE_H */
