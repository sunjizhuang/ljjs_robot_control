#include "config/config.h"
#include "config/yaml_config.h"
#include "util/log.h"
#include "device/yunji_driver.h"
#include "device/lite3_driver.h"
#include "control/cmd_translator.h"
#include "keyboard_control.h"
#include "network/tcp_server.h"
#include "network/tcp_control_client.h"
#include "network/mqtt_client.h"
#include "network/mqtt_broker.h"
#include "control/input_adapter.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <getopt.h>
#include <unistd.h>

/* Lite3 telemetry → TCP 测试通道：每 200ms 推送最新 0x0901/0902/0903 快照。 */
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>

#define APP_NAME    "lljs_ctrl"
#define APP_VERSION "1.3.0"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ══════════════════ 帮助 / 版本 ══════════════════ */

static void print_usage(const char* prog)
{
    fprintf(stderr,
        "\n"
        "灵境具身机器人控制服务端 (双设备: 云迹 SC668s + 云深处 Lite3)\n"
        "================================================================\n"
        "\n"
        "架构: TCP Server / TCP Client / MQTT 三种外部控制通道可独立同时启用\n"
        "      所有通道收到的 JSON 最终统一进入 InputAdapter → CmdTranslator → DeviceDriver\n"
        "\n"
        "用法:\n"
        "  %s -c <config.yaml>              以配置文件启动\n"
        "  %s --check-config -c <file>      校验配置文件（不启动服务）\n"
        "  %s --dump-config  -c <file>      查看最终生效配置（不启动服务）\n"
        "  %s --help                        显示本帮助\n"
        "  %s --version                     显示版本\n"
        "\n"
        "选项:\n"
        "  -c, --config <file>     YAML 配置文件路径\n"
        "                          (默认搜索: ./config.yaml → ./config/config.yaml\n"
        "                           → /etc/lljs_ctrl/config.yaml)\n"
        "\n"
        "配置文件说明:\n"
        "  所有运行时参数（设备选择/IP/端口/速度限幅/频率）均在 YAML 中配置，\n"
        "  详见 config/config.yaml 模板。\n"
        "\n"
        "控制说明:\n"
        "  Tab: 切换激活设备 (云迹 ↔ Lite3)\n"
        "  激活云迹时: WASD=遥控  X=急停  Z=解除  M=导航  C=取消\n"
        "  激活Lite3时: WASD=移动  Z=站立  Y=移动模式  X=自主  C=手动\n"
        "                G=打招呼  J=跳  1=扭身体  N=左转90  E=右转90\n"
        "  通用: Space=停止  H=帮助  Q=退出\n"
        "\n"
        "远程指令格式:\n"
        "  单字符: w s a d x z c q ...\n"
        "  JSON:   {\"cmd\":\"mode\",\"mode\":\"stand\",\"device\":\"lite3\"}\n"
        "          {\"cmd\":\"action\",\"action\":\"greet\",\"device\":\"lite3\"}\n"
        "          {\"cmd\":\"joy\",\"linear\":0.3,\"angular\":0.1}\n"
        "          {\"cmd\":\"switch_device\"}\n"
        "\n",
        prog, prog, prog, prog, prog);
}

static void print_version(void)
{
    printf("%s %s\n", APP_NAME, APP_VERSION);
    printf("双设备机器人控制服务端 (云迹 SC668s + 云深处 Lite3)\n");
    printf("协议: 云迹 TCP URL / Lite3 UDP 指令码\n");
}

/* ══════════════════ 配置文件查找 ══════════════════ */

static bool file_exists(const char* path)
{
    return access(path, R_OK) == 0;
}

/* 按优先级搜索默认配置文件，找到返回路径 */
static std::string findDefaultConfig()
{
    static const char* candidates[] = {
        "./config.yaml",
        "./config/config.yaml",
        "/etc/lljs_ctrl/config.yaml",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) return candidates[i];
    }
    return "";
}

/* ══════════════════ main ══════════════════ */

/* Lite3 telemetry → TCP 测试通道：每 200ms 推送最新 0x0901/0902/0903 快照。 */
static std::string lite3TelemetryJson(const Lite3State& s, const char* code)
{
    std::ostringstream os;
    os << std::setprecision(10);
    os << "{\"cmd\":\"telemetry\",\"device\":\"lite3\",\"code\":\"" << code << "\"";
    if (std::string(code) == "0x0901") {
        os << ",\"valid\":" << (s.valid ? "true" : "false")
           << ",\"robot_basic_state\":" << s.robot_basic_state
           << ",\"robot_gait_state\":" << s.robot_gait_state
           << ",\"robot_motion_state\":" << s.robot_motion_state
           << ",\"battery_level\":" << s.battery_level
           << ",\"is_robot_need_move\":" << (s.is_robot_need_move ? "true" : "false")
           << ",\"zero_position_flag\":" << (s.zero_position_flag ? "true" : "false")
           << ",\"rpy\":[" << s.rpy[0] << ',' << s.rpy[1] << ',' << s.rpy[2] << ']'
           << ",\"rpy_vel\":[" << s.rpy_vel[0] << ',' << s.rpy_vel[1] << ',' << s.rpy_vel[2] << ']'
           << ",\"pos_world\":[" << s.pos_world[0] << ',' << s.pos_world[1] << ',' << s.pos_world[2] << ']'
           << ",\"vel_world\":[" << s.vel_world[0] << ',' << s.vel_world[1] << ',' << s.vel_world[2] << ']'
           << ",\"vel_body\":[" << s.vel_body[0] << ',' << s.vel_body[1] << ',' << s.vel_body[2] << ']'
           << ",\"ultrasound\":[" << s.ultrasound[0] << ',' << s.ultrasound[1] << ']'
           << ",\"update_count\":" << s.state_update_count;
    } else if (std::string(code) == "0x0902") {
        os << ",\"joint_angle\":[";
        for (int i = 0; i < 12; ++i) { if (i) os << ','; os << s.joint_angle[i]; }
        os << "],\"update_count\":" << s.joint_angle_update_count;
    } else if (std::string(code) == "0x0903") {
        os << ",\"joint_vel\":[";
        for (int i = 0; i < 12; ++i) { if (i) os << ','; os << s.joint_vel[i]; }
        os << "],\"update_count\":" << s.joint_vel_update_count;
    }
    os << '}';
    return os.str();
}

static void lite3TelemetryThread(Lite3Driver* driver,
                                 TcpServer* server,
                                 TcpControlClient* client,
                                 MqttClient* mqtt,
                                 bool server_enabled,
                                 bool client_enabled,
                                 bool mqtt_enabled,
                                 std::atomic<bool>* running)
{
    while (running->load()) {
        if (driver && driver->isConnected()) {
            Lite3State s = driver->getLite3StateSnapshot();
            if (s.valid) {
                const std::string t1 = lite3TelemetryJson(s, "0x0901");
                const std::string t2 = lite3TelemetryJson(s, "0x0902");
                const std::string t3 = lite3TelemetryJson(s, "0x0903");
                if (server_enabled && server) {
                    server->broadcast(t1);
                    server->broadcast(t2);
                    server->broadcast(t3);
                }
                if (client_enabled && client && client->isConnected()) {
                    client->send(t1);
                    client->send(t2);
                    client->send(t3);
                }
                if (mqtt_enabled && mqtt && mqtt->isConnected()) {
                    mqtt->publish(MqttClient::telemetryTopic(), t1);
                    mqtt->publish(MqttClient::telemetryTopic(), t2);
                    mqtt->publish(MqttClient::telemetryTopic(), t3);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

int main(int argc, char* argv[])
{
    std::string config_path;
    bool opt_check_config = false;
    bool opt_dump_config  = false;
    bool opt_version      = false;
    bool opt_help         = false;

    static struct option long_opts[] = {
        {"config",       required_argument, 0, 'c'},
        {"check-config", no_argument,       0, 'C'},
        {"dump-config",  no_argument,       0, 'D'},
        {"version",      no_argument,       0, 'V'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:CDVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'C': opt_check_config = true; break;
        case 'D': opt_dump_config  = true; break;
        case 'V': opt_version      = true; break;
        case 'h': opt_help         = true; break;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (opt_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (opt_version) {
        print_version();
        return 0;
    }

    /* 定位配置文件 */
    if (config_path.empty()) {
        config_path = findDefaultConfig();
        if (config_path.empty()) {
            if (opt_check_config || opt_dump_config) {
                fprintf(stderr, "错误: 未指定配置文件 (-c)，且默认路径均不存在\n");
                return 1;
            }
            fprintf(stderr,
                "警告: 未找到配置文件，使用内置默认值\n"
                "      (可通过 -c 指定，参考 config/config.yaml 模板)\n\n");
        }
    }

    /* 加载配置 */
    if (!config_path.empty()) {
        std::string err;
        if (!g_config.loadFromFile(config_path, err)) {
            fprintf(stderr, "配置文件错误: %s\n%s\n", config_path.c_str(), err.c_str());
            return 1;
        }
    }

    /* --check-config: 校验后退出 */
    if (opt_check_config) {
        std::string err;
        if (!g_config.validate(err)) {
            fprintf(stderr, "配置校验失败: %s\n%s\n", config_path.c_str(), err.c_str());
            return 1;
        }
        printf("配置校验通过: %s\n", config_path.c_str());
        return 0;
    }

    /* --dump-config: 输出最终配置后退出 */
    if (opt_dump_config) {
        std::string err;
        if (!g_config.validate(err)) {
            fprintf(stderr, "配置校验失败: %s\n%s\n", config_path.c_str(), err.c_str());
            return 1;
        }
        printf("%s\n", g_config.dump().c_str());
        return 0;
    }

    /* 正常启动: 必须通过校验 */
    {
        std::string err;
        if (!g_config.validate(err)) {
            fprintf(stderr, "配置无效，拒绝启动: %s\n", err.c_str());
            fprintf(stderr, "提示: 运行 %s --check-config -c <file> 检查配置\n", argv[0]);
            return 1;
        }
    }

    /* 日志级别 */
    if      (g_config.log_level == "debug") Logger::instance().setLevel(LOG_DEBUG);
    else if (g_config.log_level == "warn")  Logger::instance().setLevel(LOG_WARN);
    else if (g_config.log_level == "error") Logger::instance().setLevel(LOG_ERROR);
    else                                    Logger::instance().setLevel(LOG_INFO);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    const std::string& device_sel = g_config.device;

    LOG_INFO("=== %s %s (灵境具身机器人控制服务端) ===", APP_NAME, APP_VERSION);
    if (!g_config.loaded_from.empty()) {
        LOG_INFO("配置文件: %s", g_config.loaded_from.c_str());
    } else {
        LOG_INFO("配置文件: (无，使用内置默认值)");
    }
    LOG_INFO("设备选择: %s", device_sel.c_str());
    if (device_sel == "yunji" || device_sel == "both") {
        LOG_INFO("云迹 SC668s: %s:%d", g_config.yunji_ip.c_str(), g_config.yunji_port);
    }
    if (device_sel == "lite3" || device_sel == "both") {
        LOG_INFO("云深处 Lite3: %s:%d (UDP)", g_config.lite3_ip.c_str(), g_config.lite3_port);
    }
    LOG_INFO("网络模式: %s", g_config.network_mode.c_str());
    if (g_config.network_mode == "server") {
        LOG_INFO("TCP Server 监听: %s:%d", g_config.server_bind.c_str(), g_config.server_port);
    } else {
        LOG_INFO("TCP Client 目标: %s:%d", g_config.client_server_ip.c_str(), g_config.client_server_port);
    }

    /* ════ 1. 设备适配层: 创建驱动 ════ */
    YunjiDriver  yunji_driver;
    Lite3Driver  lite3_driver;

    /* ════ 2. 指令翻译引擎 (多设备管理) ════ */
    CmdTranslator translator;

    bool yunji_ok  = false;
    bool lite3_ok  = false;

    if (device_sel == "yunji" || device_sel == "both") {
        /* 连接云迹 */
        if (yunji_driver.connect(g_config.yunji_ip, g_config.yunji_port)) {
            translator.addDevice("yunji", &yunji_driver);
            yunji_ok = true;
            yunji_driver.requestInfo();
            yunji_driver.requestVersion();
            yunji_driver.requestStatus();
        } else {
            LOG_ERR("无法连接云迹 SC668s %s:%d", g_config.yunji_ip.c_str(), g_config.yunji_port);
            LOG_ERR("  请检查: 1.机器人是否已开机 2.网络是否在同一局域网 3.配置文件 IP/端口是否正确");
        }
    }

    if (device_sel == "lite3" || device_sel == "both") {
        /* 连接 Lite3 */
        if (lite3_driver.connect(g_config.lite3_ip, g_config.lite3_port)) {
            translator.addDevice("lite3", &lite3_driver);
            lite3_ok = true;
        } else {
            LOG_ERR("无法连接 Lite3 %s:%d", g_config.lite3_ip.c_str(), g_config.lite3_port);
            LOG_ERR("  请检查: 1.机器狗是否已开机 2.网络是否在同一局域网 3.配置文件 IP/端口是否正确");
        }
    }

    if (!yunji_ok && !lite3_ok) {
        LOG_ERR("没有设备连接成功，退出");
        return 1;
    }

    /* 设置默认激活设备 */
    {
        std::string prefer = g_config.default_active;
        if (prefer == "yunji" && yunji_ok)        translator.setActiveDevice("yunji");
        else if (prefer == "lite3" && lite3_ok)   translator.setActiveDevice("lite3");
        else if (yunji_ok)                        translator.setActiveDevice("yunji");
        else                                      translator.setActiveDevice("lite3");
    }

    /* 设置设备回调 */
    yunji_driver.setNotifCallback([](const DeviceNotification& n) {
        (void)n;
    });

    /* ════ 3. 网络通信层：Server / Client / MQTT 可同时启用 ════ */
    TcpServer server;
    TcpControlClient client;
    MqttClient mqtt;
    MqttBroker mqtt_broker;

    const bool server_enabled = g_config.network_server_enabled;
    const bool client_enabled = g_config.network_client_enabled;
    const bool mqtt_enabled = g_config.network_mqtt_enabled;
    const bool mqtt_broker_enabled = mqtt_enabled && g_config.mqtt_broker_enabled;
    const bool mqtt_client_enabled = mqtt_enabled && (g_config.mqtt_client_enabled || mqtt_broker_enabled);

    LOG_INFO("网络通道: TCP Server=%s, TCP Client=%s, MQTT=%s",
             server_enabled ? "开启" : "关闭",
             client_enabled ? "开启" : "关闭",
             mqtt_enabled ? "开启" : "关闭");
    LOG_INFO("MQTT 角色: Broker(Server)=%s, Client=%s",
             mqtt_broker_enabled ? "开启" : "关闭",
             mqtt_client_enabled ? "开启" : "关闭");

    /* ════ 4. 键盘控制器 ════ */
    KeyboardController controller(&translator,
                                  server_enabled ? &server : nullptr,
                                  client_enabled ? &client : nullptr);

    if (server_enabled) {
        server.setCommandCallback([&controller](const std::string& cmd, int client_fd) {
            controller.handleRemoteCommand(cmd, client_fd);
        });
        server.setClientCallback([](int fd, const std::string& addr, bool connected) {
            (void)fd;
            if (connected) LOG_INFO("外部控制端上线: %s", addr.c_str());
            else LOG_INFO("外部控制端离线: %s", addr.c_str());
        });
        if (!server.start(g_config.server_port, g_config.server_bind)) {
            LOG_ERR("无法启动 TCP Server (%s:%d)", g_config.server_bind.c_str(), g_config.server_port);
            return 1;
        }
    }

    if (client_enabled) {
        std::string register_device = g_config.client_device;
        if (register_device.empty()) register_device = translator.getActiveDeviceType();
        client.setRegistration(g_config.client_edge_id,
                               g_config.client_robot_id,
                               register_device);
        LOG_INFO("TCP Client 注册配置: edgeId=%s robotId=%s device=%s",
                 g_config.client_edge_id.c_str(),
                 g_config.client_robot_id.c_str(),
                 register_device.c_str());
        client.setCommandCallback([&controller](const std::string& cmd) {
            controller.handleRemoteCommand(cmd, -1);
        });
        client.setStatusCallback([](bool connected) {
            if (connected) LOG_INFO("TCP Client: 已连接外部控制端");
            else LOG_WARN("TCP Client: 与外部控制端断开，准备自动重连");
        });
        if (!client.start(g_config.client_server_ip,
                          g_config.client_server_port,
                          g_config.client_reconnect_interval_ms)) {
            LOG_ERR("无法启动 TCP Client");
            return 1;
        }
    }

    if (mqtt_broker_enabled) {
        if (!mqtt_broker.start(g_config.mqtt_broker_executable,
                               g_config.mqtt_broker_bind,
                               g_config.mqtt_broker_port,
                               g_config.mqtt_broker_allow_anonymous,
                               g_config.mqtt_broker_config)) {
            LOG_ERR("无法启动 MQTT Broker(Server)");
            return 1;
        }
    }

    if (mqtt_client_enabled) {
        mqtt.setMessageCallback([&controller](const std::string& topic,
                                               const std::string& payload) {
            LOG_INFO("MQTT RX topic=%s payload=%s", topic.c_str(), payload.c_str());
            controller.handleRemoteCommandFrom(payload, "mqtt:" + topic);
        });
        mqtt.setStatusCallback([](bool connected) {
            if (connected) LOG_INFO("MQTT: 已连接 Broker");
            else LOG_WARN("MQTT: 与 Broker 断开，准备自动重连");
        });
        const std::string mqtt_host = mqtt_broker_enabled ?
            (g_config.mqtt_broker_bind == "0.0.0.0" ? "127.0.0.1" : g_config.mqtt_broker_bind) :
            g_config.mqtt_host;
        const int mqtt_port = mqtt_broker_enabled ? g_config.mqtt_broker_port : g_config.mqtt_port;
        if (!mqtt.start(mqtt_host,
                        mqtt_port,
                        g_config.mqtt_client_id,
                        g_config.mqtt_username,
                        g_config.mqtt_password,
                        g_config.mqtt_reconnect_interval_ms)) {
            LOG_ERR("无法启动 MQTT Client");
            return 1;
        }
    }

    /* Lite3 telemetry → TCP/MQTT：每 200ms 推送最新快照。 */
    std::atomic<bool> telemetry_running{true};
    std::thread telemetry_thread(lite3TelemetryThread,
                                 &lite3_driver, &server, &client, &mqtt,
                                 server_enabled, client_enabled, mqtt_enabled,
                                 &telemetry_running);

    /* 启动键盘控制主循环（阻塞） */
    controller.run();

    /* 清理 */
	/* Lite3 telemetry → TCP 测试通道：每 200ms 推送最新 0x0901/0902/0903 快照。 */
    telemetry_running = false;
    if (telemetry_thread.joinable()) telemetry_thread.join();

    LOG_INFO("断开连接...");
    if (yunji_ok)  yunji_driver.disconnect();
    if (lite3_ok)  lite3_driver.disconnect();
    server.stop();
    client.stop();
    mqtt.stop();
    mqtt_broker.stop();
    LOG_INFO("已退出");

    return 0;
}
