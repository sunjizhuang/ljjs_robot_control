#include "yaml_config.h"
#include "util/log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <sstream>
#include <map>

/* 全局配置实例定义 */
AppConfig g_config;

/* ══════════════════════════════════════════════════════════
 * 极简 YAML 解析器
 *
 * 解析结果为 "section.key" → 原始字符串值 的扁平 map。
 * 只支持两层结构（顶层键 + 一层嵌套区块），足够本项目使用。
 * ══════════════════════════════════════════════════════════ */

namespace {

/* 去掉行尾注释和首尾空白 */
std::string stripComment(const std::string& line)
{
    std::string out;
    bool in_squote = false, in_dquote = false;

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\'' && !in_dquote) in_squote = !in_squote;
        else if (c == '"' && !in_squote) in_dquote = !in_dquote;
        else if (c == '#' && !in_squote && !in_dquote) {
            /* '#' 前必须是空白或行首，才算注释 */
            if (i == 0 || std::isspace((unsigned char)line[i - 1])) break;
        }
        out += c;
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* 去掉字符串值的引号 */
std::string unquote(const std::string& s)
{
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/* 将扁平 map 解析为 YAML 行集 */
bool parseYamlFile(const std::string& path,
                   std::map<std::string, std::string>& kv,
                   std::string& err_msg)
{
    std::ifstream fin(path.c_str());
    if (!fin.is_open()) {
        err_msg = "无法打开配置文件: " + path;
        return false;
    }

    std::string line;
    std::string current_section;
    int line_no = 0;

    while (std::getline(fin, line)) {
        line_no++;

        std::string content = stripComment(line);

        /* 空行 */
        if (trim(content).empty()) continue;

        /* 计算缩进（空格数，tab 按 1 计） */
        size_t indent = 0;
        while (indent < content.size() &&
               (content[indent] == ' ' || content[indent] == '\t')) indent++;

        std::string body = trim(content);
        if (body.empty()) continue;

        /* 找 "key: value" 或 "key:" */
        size_t colon = std::string::npos;
        bool in_sq = false, in_dq = false;
        for (size_t i = 0; i < body.size(); i++) {
            char c = body[i];
            if (c == '\'' && !in_dq) in_sq = !in_sq;
            else if (c == '"' && !in_sq) in_dq = !in_dq;
            else if (c == ':' && !in_sq && !in_dq) {
                /* "key:" 必须在结尾或后面跟空白 */
                if (i + 1 == body.size() || std::isspace((unsigned char)body[i + 1])) {
                    colon = i;
                    break;
                }
            }
        }

        if (colon == std::string::npos) {
            char buf[256];
            snprintf(buf, sizeof(buf), "第 %d 行: 无法解析 (缺少 ':'): %.60s",
                     line_no, body.c_str());
            err_msg = buf;
            return false;
        }

        std::string key   = trim(body.substr(0, colon));
        std::string value = trim(body.substr(colon + 1));

        if (key.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "第 %d 行: 键为空", line_no);
            err_msg = buf;
            return false;
        }

        if (indent == 0) {
            /* 顶层 */
            if (value.empty()) {
                /* 新区块开始 */
                current_section = key;
            } else {
                current_section.clear();
                if (kv.count(key)) {
                    char buf[192];
                    snprintf(buf, sizeof(buf), "第 %d 行: 重复的顶层键: %s",
                             line_no, key.c_str());
                    err_msg = buf;
                    return false;
                }
                kv[key] = unquote(value);
            }
        } else {
            /* 嵌套键 */
            if (current_section.empty()) {
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "第 %d 行: 缩进的键 '%s' 不属于任何区块 (顶层无对应键)",
                         line_no, key.c_str());
                err_msg = buf;
                return false;
            }
            if (value.empty()) {
                /* 不支持更深层嵌套 */
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "第 %d 行: 不支持两层以上嵌套: %s.%s",
                         line_no, current_section.c_str(), key.c_str());
                err_msg = buf;
                return false;
            }
            std::string full_key = current_section + "." + key;
            if (kv.count(full_key)) {
                char buf[192];
                snprintf(buf, sizeof(buf), "第 %d 行: 重复的键: %s",
                         line_no, full_key.c_str());
                err_msg = buf;
                return false;
            }
            kv[full_key] = unquote(value);
        }
    }

    return true;
}

/* ── 类型转换辅助 ── */

bool toInt(const std::string& s, int& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end && *end != '\0') return false;
    out = (int)v;
    return true;
}

bool toBool(const std::string& s, bool& out)
{
    std::string v = s;
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(v[i])));
    if (v == "true" || v == "yes" || v == "1" || v == "on") { out = true; return true; }
    if (v == "false" || v == "no" || v == "0" || v == "off") { out = false; return true; }
    return false;
}

bool toDouble(const std::string& s, double& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (end && *end != '\0') return false;
    out = v;
    return true;
}

} /* anonymous namespace */

/* ══════════════════════════════════════════════════════════
 * AppConfig 实现
 * ══════════════════════════════════════════════════════════ */

bool AppConfig::loadFromFile(const std::string& path, std::string& err_msg)
{
    std::map<std::string, std::string> kv;
    if (!parseYamlFile(path, kv, err_msg)) {
        return false;
    }

    /* 逐键取值，未知键报错（避免拼写错误静默失效） */
    std::string key, val;
    bool has_network_switch = false;
    auto failType = [&](void*) -> bool {
        err_msg = "配置项 " + key + " 的值类型不正确: \"" + val + "\"";
        return false;
    };

    (void)failType; /* 在下方按需展开 */

    for (auto it = kv.begin(); it != kv.end(); ++it) {
        key = it->first;
        val = it->second;

        if      (key == "device")             device = val;
        else if (key == "log_level")          log_level = val;
        else if (key == "default_active")     default_active = val;
        else if (key == "network.mode")       network_mode = val;
        else if (key == "network.server_enabled") { has_network_switch = true; if (!toBool(val, network_server_enabled)) return failType(nullptr); }
        else if (key == "network.client_enabled") { has_network_switch = true; if (!toBool(val, network_client_enabled)) return failType(nullptr); }
        else if (key == "network.mqtt_enabled") { has_network_switch = true; if (!toBool(val, network_mqtt_enabled)) return failType(nullptr); }
        else if (key == "client.server_ip")   client_server_ip = val;
        else if (key == "client.server_port") { if (!toInt(val, client_server_port)) return failType(nullptr); }
        else if (key == "client.reconnect_interval_ms") { if (!toInt(val, client_reconnect_interval_ms)) return failType(nullptr); }
        else if (key == "client.edge_id")       client_edge_id = val;
        else if (key == "client.robot_id")      client_robot_id = val;
        else if (key == "client.device")        client_device = val;
        else if (key == "mqtt.broker_enabled") { if (!toBool(val, mqtt_broker_enabled)) return failType(nullptr); }
        else if (key == "mqtt.broker_bind")      mqtt_broker_bind = val;
        else if (key == "mqtt.broker_port")      { if (!toInt(val, mqtt_broker_port)) return failType(nullptr); }
        else if (key == "mqtt.broker_executable") mqtt_broker_executable = val;
        else if (key == "mqtt.broker_config")    mqtt_broker_config = val;
        else if (key == "mqtt.broker_allow_anonymous") { if (!toBool(val, mqtt_broker_allow_anonymous)) return failType(nullptr); }
        else if (key == "mqtt.client_enabled") { if (!toBool(val, mqtt_client_enabled)) return failType(nullptr); }
        else if (key == "mqtt.host")             mqtt_host = val;
        else if (key == "mqtt.port")             { if (!toInt(val, mqtt_port)) return failType(nullptr); }
        else if (key == "mqtt.client_id")        mqtt_client_id = val;
        else if (key == "mqtt.username")         mqtt_username = val;
        else if (key == "mqtt.password")         mqtt_password = val;
        else if (key == "mqtt.reconnect_interval_ms") { if (!toInt(val, mqtt_reconnect_interval_ms)) return failType(nullptr); }
        else if (key == "yunji.ip")           yunji_ip = val;
        else if (key == "yunji.port")         { if (!toInt(val, yunji_port)) return failType(nullptr); }
        else if (key == "lite3.ip")           lite3_ip = val;
        else if (key == "lite3.port")         { if (!toInt(val, lite3_port)) return failType(nullptr); }
        else if (key == "lite3.receive_port") { if (!toInt(val, lite3_receive_port)) return failType(nullptr); }
        else if (key == "server.bind")        server_bind = val;
        else if (key == "server.port")        { if (!toInt(val, server_port)) return failType(nullptr); }
        else if (key == "control.joy_hz")     { if (!toInt(val, joy_hz)) return failType(nullptr); }
        else if (key == "control.max_linear")     { if (!toDouble(val, max_linear)) return failType(nullptr); }
        else if (key == "control.max_angular")    { if (!toDouble(val, max_angular)) return failType(nullptr); }
        else if (key == "control.linear_step")    { if (!toDouble(val, linear_step)) return failType(nullptr); }
        else if (key == "control.angular_step")   { if (!toDouble(val, angular_step)) return failType(nullptr); }
        else if (key == "lite3_heartbeat.interval_ms") { if (!toInt(val, lite3_heartbeat_ms)) return failType(nullptr); }
        else if (key == "status.poll_hz")     { if (!toInt(val, status_poll_hz)) return failType(nullptr); }
        else if (key == "log.queue_size")     { if (!toInt(val, log_queue_size)) return failType(nullptr); }
        else {
            err_msg = "未知的配置项: " + key;
            return false;
        }
    }

    /* 旧版 network.mode 兼容：只有完全没有新开关时才生效。 */
    if (!has_network_switch) {
        if (network_mode == "server") {
            network_server_enabled = true;
            network_client_enabled = false;
        } else if (network_mode == "client") {
            network_server_enabled = false;
            network_client_enabled = true;
        }
    }

    loaded_from = path;
    return true;
}

bool AppConfig::validate(std::string& err_msg) const
{
    if (device != "yunji" && device != "lite3" && device != "both") {
        err_msg = "device 必须是 yunji / lite3 / both，当前: " + device;
        return false;
    }
    if (log_level != "debug" && log_level != "info" &&
        log_level != "warn" && log_level != "error") {
        err_msg = "log_level 必须是 debug / info / warn / error，当前: " + log_level;
        return false;
    }
    if (network_mode != "server" && network_mode != "client") {
        err_msg = "network.mode 必须是 server / client，当前: " + network_mode;
        return false;
    }
    if (!network_server_enabled && !network_client_enabled && !network_mqtt_enabled) {
        err_msg = "至少需要开启 network.server_enabled / network.client_enabled / network.mqtt_enabled 其中一个网络通道";
        return false;
    }
    if (mqtt_broker_port <= 0 || mqtt_broker_port > 65535) {
        err_msg = "mqtt.broker_port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (mqtt_port <= 0 || mqtt_port > 65535) {
        err_msg = "mqtt.port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (mqtt_host.empty()) {
        err_msg = "mqtt.host 不能为空";
        return false;
    }
    if (mqtt_client_id.empty()) {
        err_msg = "mqtt.client_id 不能为空";
        return false;
    }
    if (mqtt_reconnect_interval_ms < 100 || mqtt_reconnect_interval_ms > 600000) {
        err_msg = "mqtt.reconnect_interval_ms 必须在 100-600000 ms";
        return false;
    }
    if (client_server_ip.empty()) {
        err_msg = "client.server_ip 不能为空";
        return false;
    }
    if (client_server_port <= 0 || client_server_port > 65535) {
        err_msg = "client.server_port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (network_mode == "client") {
        if (client_edge_id.empty()) {
            err_msg = "client.edge_id 不能为空";
            return false;
        }
        if (client_robot_id.empty()) {
            err_msg = "client.robot_id 不能为空";
            return false;
        }
        if (!client_device.empty() && client_device != "yunji" && client_device != "lite3") {
            err_msg = "client.device 必须是 yunji / lite3，或留空自动推断，当前: " + client_device;
            return false;
        }
    }
    if (client_reconnect_interval_ms < 100 || client_reconnect_interval_ms > 600000) {
        err_msg = "client.reconnect_interval_ms 必须在 100-600000 ms";
        return false;
    }
    if (!default_active.empty() &&
        default_active != "yunji" && default_active != "lite3") {
        err_msg = "default_active 必须是 yunji / lite3 或留空，当前: " + default_active;
        return false;
    }
    if (yunji_port <= 0 || yunji_port > 65535) {
        err_msg = "yunji.port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (lite3_receive_port <= 0 || lite3_receive_port > 65535) {
        err_msg = "lite3.receive_port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (lite3_port <= 0 || lite3_port > 65535) {
        err_msg = "lite3.port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (server_port <= 0 || server_port > 65535) {
        err_msg = "server.port 超出有效端口范围 (1-65535)";
        return false;
    }
    if (joy_hz < 3 || joy_hz > 50) {
        err_msg = "control.joy_hz 必须在 3-50 Hz（云迹 API 要求 >2）";
        return false;
    }
    if (max_linear <= 0 || max_linear > 0.5) {
        err_msg = "control.max_linear 必须在 (0, 0.5] m/s（云迹 API 限定）";
        return false;
    }
    if (max_angular <= 0 || max_angular > 1.0) {
        err_msg = "control.max_angular 必须在 (0, 1.0] rad/s（云迹 API 限定）";
        return false;
    }
    if (linear_step <= 0 || linear_step > max_linear) {
        err_msg = "control.linear_step 必须在 (0, max_linear] 范围内";
        return false;
    }
    if (angular_step <= 0 || angular_step > max_angular) {
        err_msg = "control.angular_step 必须在 (0, max_angular] 范围内";
        return false;
    }
    if (lite3_heartbeat_ms < 5 || lite3_heartbeat_ms > 1000) {
        err_msg = "lite3_heartbeat.interval_ms 必须在 5-1000 ms";
        return false;
    }
    if (status_poll_hz < 1 || status_poll_hz > 10) {
        err_msg = "status.poll_hz 必须在 1-10 Hz";
        return false;
    }
    if (log_queue_size < 5 || log_queue_size > 100) {
        err_msg = "log.queue_size 必须在 5-100";
        return false;
    }
    return true;
}

std::string AppConfig::dump() const
{
    std::ostringstream os;
    char buf[256];

    os << "# ══ 最终生效配置";
    if (!loaded_from.empty()) os << " (来源: " << loaded_from << ")";
    os << " ══\n";

    os << "\n[全局]\n";
    os << "device           = " << device << "\n";
    os << "log_level        = " << log_level << "\n";
    os << "default_active   = " << (default_active.empty() ? "(自动)" : default_active) << "\n";
    os << "network.mode     = " << network_mode << " (兼容旧配置)\n";
    os << "network.server_enabled = " << (network_server_enabled ? "true" : "false") << "\n";
    os << "network.client_enabled = " << (network_client_enabled ? "true" : "false") << "\n";
    os << "network.mqtt_enabled   = " << (network_mqtt_enabled ? "true" : "false") << "\n";

    os << "\n[云迹 SC668s]\n";
    os << "yunji.ip         = " << yunji_ip << "\n";
    os << "yunji.port       = " << yunji_port << "\n";

    os << "\n[云深处 Lite3]\n";
    os << "lite3.ip         = " << lite3_ip << "\n";
    os << "lite3.port       = " << lite3_port << "\n";
    os << "lite3.receive_port = " << lite3_receive_port << "\n";
    os << "lite3_heartbeat.interval_ms = " << lite3_heartbeat_ms << "\n";

    os << "\n[TCP Server]\n";
    os << "server.bind      = " << server_bind << "\n";
    os << "server.port      = " << server_port << "\n";
    os << "\n[TCP Client]\n";
    os << "client.server_ip = " << client_server_ip << "\n";
    os << "client.server_port = " << client_server_port << "\n";
    os << "client.reconnect_interval_ms = " << client_reconnect_interval_ms << "\n";
    os << "client.edge_id = " << client_edge_id << "\n";
    os << "client.robot_id = " << client_robot_id << "\n";
    os << "client.device = " << client_device << "\n";

    os << "\n[MQTT]\n";
    os << "mqtt.broker_enabled = " << (mqtt_broker_enabled ? "true" : "false") << "\n";
    os << "mqtt.broker_bind = " << mqtt_broker_bind << "\n";
    os << "mqtt.broker_port = " << mqtt_broker_port << "\n";
    os << "mqtt.broker_executable = " << (mqtt_broker_executable.empty() ? "(auto)" : mqtt_broker_executable) << "\n";
    os << "mqtt.broker_config = " << mqtt_broker_config << "\n";
    os << "mqtt.broker_allow_anonymous = " << (mqtt_broker_allow_anonymous ? "true" : "false") << "\n";
    os << "mqtt.client_enabled = " << (mqtt_client_enabled ? "true" : "false") << "\n";
    os << "mqtt.host = " << mqtt_host << "\n";
    os << "mqtt.port = " << mqtt_port << "\n";
    os << "mqtt.client_id = " << mqtt_client_id << "\n";
    os << "mqtt.username = " << mqtt_username << "\n";
    os << "mqtt.password = " << (mqtt_password.empty() ? "(empty)" : "******") << "\n";
    os << "mqtt.reconnect_interval_ms = " << mqtt_reconnect_interval_ms << "\n";

    os << "\n[遥控参数]\n";
    os << "control.joy_hz        = " << joy_hz << " (发送间隔 " << joySendIntervalMs() << " ms)\n";
    snprintf(buf, sizeof(buf), "%.3f", max_linear);
    os << "control.max_linear    = " << buf << "\n";
    snprintf(buf, sizeof(buf), "%.3f", max_angular);
    os << "control.max_angular   = " << buf << "\n";
    snprintf(buf, sizeof(buf), "%.3f", linear_step);
    os << "control.linear_step   = " << buf << "\n";
    snprintf(buf, sizeof(buf), "%.3f", angular_step);
    os << "control.angular_step  = " << buf << "\n";

    os << "\n[状态/日志]\n";
    os << "status.poll_hz   = " << status_poll_hz << "\n";
    os << "log.queue_size   = " << log_queue_size << "\n";

    return os.str();
}
