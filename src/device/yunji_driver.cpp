#include "yunji_driver.h"
#include "config/yaml_config.h"
#include "util/log.h"
#include "config/config.h"

#include <cstdio>
#include <sstream>

YunjiDriver::YunjiDriver()
{
    tcp_.setMessageCallback([this](const JsonValue& msg) { onMessage(msg); });
    tcp_.setStatusCallback([this](bool c) { onConnChanged(c); });
}

YunjiDriver::~YunjiDriver()
{
    disconnect();
}

bool YunjiDriver::connect(const std::string& ip, int port)
{
    bool ok = tcp_.connect(ip, port);
    connected_ = ok;
    return ok;
}

void YunjiDriver::disconnect()
{
    tcp_.disconnect();
    connected_ = false;
}

bool YunjiDriver::isConnected() const
{
    return connected_;
}

bool YunjiDriver::sendApi(const std::string& path)
{
    if (!connected_) {
        LOG_WARN("未连接，无法发送: %s", path.c_str());
        return false;
    }
    LOG_DBG("发送: %s", path.c_str());
    return tcp_.sendCommand(path);
}

/* ── 移动控制 ── */
bool YunjiDriver::moveToMarker(const std::string& marker)
{
    LOG_INFO("移动到点位: %s", marker.c_str());
    return sendApi("/api/move?marker=" + marker);
}

bool YunjiDriver::moveToLocation(double x, double y, double theta)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "/api/move?location=%.6f,%.6f,%.6f", x, y, theta);
    LOG_INFO("移动到坐标: (%.2f, %.2f, %.2f)", x, y, theta);
    return sendApi(buf);
}

bool YunjiDriver::cancelMove()
{
    LOG_INFO("取消当前移动任务");
    return sendApi("/api/move/cancel");
}

/* ── 遥控 ── */
bool YunjiDriver::joyControl(double linear, double angular)
{
    if (linear > g_config.max_linear)   linear = g_config.max_linear;
    if (linear < -g_config.max_linear)  linear = -g_config.max_linear;
    if (angular > g_config.max_angular)   angular = g_config.max_angular;
    if (angular < -g_config.max_angular)  angular = -g_config.max_angular;

    char buf[128];
    snprintf(buf, sizeof(buf), "/api/joy_control?linear_velocity=%.4f&angular_velocity=%.4f",
             linear, angular);
    return sendApi(buf);
}

/* ── 急停 ── */
bool YunjiDriver::estop(bool flag)
{
    LOG_INFO(flag ? "急停" : "解除急停");
    return sendApi(std::string("/api/estop?flag=") + (flag ? "true" : "false"));
}

/* ── 状态查询 ── */
bool YunjiDriver::requestStatus()
{
    return sendApi("/api/robot_status");
}

bool YunjiDriver::requestInfo()
{
    return sendApi("/api/robot_info");
}

bool YunjiDriver::requestPower()
{
    return sendApi("/api/get_power_status");
}

bool YunjiDriver::requestVersion()
{
    return sendApi("/api/software/get_version");
}

bool YunjiDriver::requestMarkers()
{
    return sendApi("/api/markers/query_brief");
}

/* ── 实时数据订阅 ── */
bool YunjiDriver::subscribeStatus(double freq_hz)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "/api/request_data?topic=robot_status&frequency=%.1f", freq_hz);
    LOG_INFO("订阅实时状态 (%.1f Hz)", freq_hz);
    return sendApi(buf);
}

bool YunjiDriver::unsubscribeStatus()
{
    LOG_INFO("取消实时状态订阅");
    return sendApi("/api/request_data?topic=robot_status&switch=off");
}

/* ── 状态解析 ── */
DeviceStatus YunjiDriver::parseStatus(const JsonValue& r)
{
    DeviceStatus s;
    s.valid          = true;
    s.move_target    = r["move_target"].asString();
    s.move_status    = r["move_status"].asString();
    s.running_status = r["running_status"].asString();
    s.move_retry     = r["move_retry_times"].asInt();
    s.charging       = r["charge_state"].asBool();
    s.soft_estop     = r["soft_estop_state"].asBool();
    s.hard_estop     = r["hard_estop_state"].asBool();
    s.estop          = r["estop_state"].asBool();
    s.power          = r["power_percent"].asInt();
    s.floor          = r["current_floor"].asInt();
    s.error_code     = r["error_code"].asString();
    if (r.has("current_pose")) {
        s.pos_x     = r["current_pose"]["x"].asNumber();
        s.pos_y     = r["current_pose"]["y"].asNumber();
        s.pos_theta = r["current_pose"]["theta"].asNumber();
    }
    return s;
}

/* ── 消息处理 ── */
void YunjiDriver::onMessage(const JsonValue& msg)
{
    std::string type = msg["type"].asString();

    if (type == "response") {
        std::string cmd    = msg["command"].asString();
        std::string status = msg["status"].asString();
        std::string err    = msg["error_message"].asString();

        if (status != "OK") {
            LOG_WARN("指令 %s 返回: %s (%s)", cmd.c_str(), status.c_str(), err.c_str());
        } else {
            LOG_DBG("指令 %s 成功", cmd.c_str());
        }

        if (cmd == "/api/robot_status" && msg.has("results")) {
            DeviceStatus s = parseStatus(msg["results"]);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                last_status_ = s;
            }
            if (status_cb_) status_cb_(s);
        }
    }
    else if (type == "callback") {
        std::string topic = msg["topic"].asString();
        if (topic == "robot_status" && msg.has("results")) {
            DeviceStatus s = parseStatus(msg["results"]);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                last_status_ = s;
            }
            if (status_cb_) status_cb_(s);
        }
    }
    else if (type == "notification") {
        DeviceNotification n;
        n.code        = msg["code"].asString();
        n.level       = msg["level"].asString();
        n.description = msg["description"].asString();

        LOG_INFO("[通知 %s] %s", n.code.c_str(), n.description.c_str());

        if (notif_cb_) notif_cb_(n);
    }
}

void YunjiDriver::onConnChanged(bool connected)
{
    connected_ = connected;
    if (conn_cb_) conn_cb_(connected);
}

DeviceStatus YunjiDriver::getStatusSnapshot()
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return last_status_;
}
