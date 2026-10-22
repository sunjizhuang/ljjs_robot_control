#include "keyboard_control.h"
#include "config/config.h"
#include "config/yaml_config.h"
#include "control/input_adapter.h"

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <poll.h>

/* ════ HUD 排版工具 (UTF-8 显示宽度感知) ════ */

static const int BOX_W = 62;

/* 计算 UTF-8 字符串的终端显示宽度 (CJK=2列, ASCII=1列, ANSI转义=0列) */
static int utf8_display_width(const std::string& s)
{
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        /* 跳过 ANSI 转义序列 \033[...m */
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') i++;
            if (i < s.size()) i++;
            continue;
        }
        if      (c < 0x80) { w += 1; i += 1; }
        else if (c < 0xE0) { w += 1; i += 2; }
        else if (c < 0xF0) { w += 2; i += 3; }
        else               { w += 2; i += 4; }
    }
    return w;
}

/* 右填充空格到指定显示宽度 */
static std::string pad_to_width(const std::string& s, int target)
{
    int w = utf8_display_width(s);
    if (w >= target) return s;
    return s + std::string(target - w, ' ');
}

/* 按显示宽度截断 (保留 ANSI 转义序列) */
static std::string utf8_truncate(const std::string& s, int max_w)
{
    int w = 0;
    std::string r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            while (i < s.size() && s[i] != 'm') r += s[i++];
            if (i < s.size()) r += s[i++];
            continue;
        }
        int cw, cl;
        if      (c < 0x80) { cw = 1; cl = 1; }
        else if (c < 0xE0) { cw = 1; cl = 2; }
        else if (c < 0xF0) { cw = 2; cl = 3; }
        else               { cw = 2; cl = 4; }
        if (w + cw > max_w) break;
        for (int j = 0; j < cl && i < s.size(); j++) r += s[i++];
        w += cw;
    }
    return r;
}

/* 打印一行边框内容 */
static void box_line(const std::string& content)
{
    printf("\033[36m║\033[0m%s\033[36m║\033[0m\n", pad_to_width(content, BOX_W).c_str());
}

/* 打印分隔线 */
static void box_sep()
{
    printf("\033[36m╠");
    for (int i = 0; i < BOX_W; i++) fputs("═", stdout);
    printf("╣\033[0m\n");
}

/* 打印顶/底边框 */
static void box_top()
{
    printf("\033[36m╔");
    for (int i = 0; i < BOX_W; i++) fputs("═", stdout);
    printf("╗\033[0m\n");
}
static void box_bottom()
{
    printf("\033[36m╚");
    for (int i = 0; i < BOX_W; i++) fputs("═", stdout);
    printf("╝\033[0m\n");
}

KeyboardController::KeyboardController(CmdTranslator* translator, TcpServer* server, TcpControlClient* client)
    : translator_(translator), server_(server), client_(client) {}

KeyboardController::~KeyboardController()
{
    stop();
    disableRawMode();
}

void KeyboardController::enableRawMode()
{
    if (raw_mode_) return;

    tcgetattr(STDIN_FILENO, &orig_termios_);
    struct termios raw = orig_termios_;

    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    /* 保留 OPOST：让终端自动 \n→\r\n，避免 HUD 边框错位 */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_ = true;
}

void KeyboardController::disableRawMode()
{
    if (!raw_mode_) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
    raw_mode_ = false;
}

void KeyboardController::run()
{
    running_ = true;

    /* 启动 joy_control 发送线程 */
    joy_thread_ = std::thread(&KeyboardController::joySendThread, this);

    /* 启动状态显示线程 */
    display_thread_ = std::thread(&KeyboardController::statusDisplayThread, this);

    enableRawMode();

    LOG_INFO("键盘控制已启动，按 Tab 切换设备，按 H 查看帮助，Q 退出");

    char c;
    while (running_) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n == 1) {
                handleLocalKey(c);
            }
        }
    }

    /* 退出时停止机器人 */
    translator_->resetVelocity();
    IDeviceDriver* dev = translator_->getActiveDevice();
    if (dev && dev->isConnected()) {
        dev->joyControl(0, 0);
    }

    disableRawMode();

    if (joy_thread_.joinable()) joy_thread_.join();
    if (display_thread_.joinable()) display_thread_.join();
}

void KeyboardController::stop()
{
    running_ = false;
}

void KeyboardController::handleLocalKey(char c)
{
    /* Tab 键 → 切换设备 */
    if (c == '\t') {
        std::string next = translator_->switchToNextDevice();
        return;
    }

    /* 获取当前激活设备类型，用于按键映射 */
    std::string dev_type = translator_->getActiveDeviceType();

    /* 本地按键解析 (设备感知) */
    InputCommand cmd = InputAdapter::parse(std::string(1, c), "local", dev_type);

    /* M 键需要交互输入点位名称 (仅云迹) */
    if (cmd.type == InputCommand::KEY_MOVE) {
        disableRawMode();
        printf("\n请输入目标点位名称: ");
        fflush(stdout);
        std::string name;
        std::getline(std::cin, name);
        if (!name.empty()) {
            cmd.marker = name;
            translator_->execute(cmd);
        }
        enableRawMode();
        return;
    }

    /* Q 键退出 */
    if (cmd.type == InputCommand::KEY_QUIT) {
        LOG_INFO("正在退出...");
        translator_->execute(cmd);
        running_ = false;
        return;
    }

    translator_->execute(cmd);
}

void KeyboardController::handleRemoteCommand(const std::string& cmd_text, int client_fd)
{
    (void)client_fd;
    std::string source = "remote";
    if (server_) {
        auto clients = server_->getClientList();
        if (!clients.empty()) source = clients[0];
    } else if (client_) {
        source = "server:" + client_->getServerAddress();
    }
    handleRemoteCommandFrom(cmd_text, source);
}

void KeyboardController::handleRemoteCommandFrom(const std::string& cmd_text,
                                                   const std::string& source)
{
    /* 所有网络入口最终都走同一条 InputAdapter → CmdTranslator 链路。 */
    std::string dev_type = translator_->getActiveDeviceType();
    InputCommand cmd = InputAdapter::parse(cmd_text, source, dev_type);

    if (cmd.type == InputCommand::UNKNOWN) {
        LOG_WARN("[%s] 未知指令: %s", source.c_str(), cmd_text.c_str());
        return;
    }

    LOG_INFO("[%s] 指令: %s", source.c_str(), InputAdapter::toString(cmd).c_str());

    /* 远程退出指令不做处理（仅本地 Q 键退出） */
    if (cmd.type == InputCommand::KEY_QUIT) {
        LOG_INFO("[%s] 远程退出指令已忽略（仅本地可退出）", source.c_str());
        return;
    }

    translator_->execute(cmd);
}

void KeyboardController::joySendThread()
{
    while (running_) {
        if (translator_->isJoyActive()) {
            IDeviceDriver* dev = translator_->getActiveDevice();
            if (dev && dev->isConnected()) {
                /* 仅云迹需要周期发送 joy_control */
                if (dev->getDeviceType() == "yunji") {
                    double lin = translator_->getLinear();
                    double ang = translator_->getAngular();
                    dev->joyControl(lin, ang);
                }
            }
        }
        usleep(g_config.joySendIntervalMs() * 1000);
    }
}

void KeyboardController::statusDisplayThread()
{
    /* 订阅所有已连接的云迹设备状态 */
    for (auto& name : translator_->getDeviceNames()) {
        IDeviceDriver* dev = translator_->getDevice(name);
        if (dev && dev->isConnected() && dev->getDeviceType() == "yunji") {
            dev->subscribeStatus(g_config.status_poll_hz);
        }
    }

    while (running_) {
        refreshDisplay();
        usleep(500000);
    }
}

void KeyboardController::refreshDisplay()
{
    std::vector<std::string> client_list;
    if (server_) {
        client_list = server_->getClientList();
    } else if (client_) {
        if (client_->isConnected()) client_list.push_back("Server:" + client_->getServerAddress());
        else client_list.push_back("Server:" + client_->getServerAddress() + " (重连中)");
    }
    auto device_names = translator_->getDeviceNames();
    std::string active_name = translator_->getActiveDeviceName();
    std::string active_type = translator_->getActiveDeviceType();
    IDeviceDriver* active_dev = translator_->getActiveDevice();

    /* 清屏 + 光标归位 */
    printf("\033[2J\033[H");

    /* ── 标题 ── */
    box_top();
    box_line("          \033[1m灵境具身机器人控制服务端 (双设备)\033[0m");
    box_sep();

    /* ── 设备状态 ── */
    if (active_dev) {
        bool conn = active_dev->isConnected();
        std::string conn_str = conn ? "\033[32m● 已连接\033[0m" : "\033[31m○ 未连接\033[0m";
        std::string active_marker = " \033[33m◀ 激活\033[0m";

        box_line(" \033[1m设备:\033[0m " + active_dev->getDeviceName() + "  " + conn_str + active_marker);

        if (active_type == "yunji") {
            const char* joy_state = translator_->isJoyActive() ? "\033[33m遥控中\033[0m" : "空闲";
            char buf[128];
            snprintf(buf, sizeof(buf),
                     " 遥控: %s  线速度: %+.2f m/s  角速度: %+.2f rad/s",
                     joy_state, translator_->getLinear(), translator_->getAngular());
            box_line(buf);
        } else if (active_type == "lite3") {
            DeviceStatus s = active_dev->getStatusSnapshot();
            box_line(" 模式: " + std::string(s.mode.empty() ? "未知" : s.mode) +
                     "  心跳: \033[32m运行中\033[0m");
        }
    }

    /* ── 设备列表 ── */
    std::string dev_list = " 设备列表: ";
    for (size_t i = 0; i < device_names.size(); i++) {
        if (device_names[i] == active_name)
            dev_list += "\033[33m[" + device_names[i] + "]\033[0m ";
        else
            dev_list += device_names[i] + " ";
    }
    dev_list += " Tab 切换";
    box_line(dev_list);

    /* ── 远程客户端 ── */
    if (client_list.empty()) {
        box_line(" 远程: 无连接");
    } else {
        std::string clients_str = " 远程: ";
        for (size_t i = 0; i < client_list.size(); i++) {
            if (i > 0) clients_str += ", ";
            clients_str += client_list[i];
        }
        if (utf8_display_width(clients_str) > BOX_W)
            clients_str = utf8_truncate(clients_str, BOX_W - 3) + "...";
        box_line(clients_str);
    }

    /* ── 云迹详细状态 ── */
    if (active_dev && active_type == "yunji") {
        DeviceStatus s = active_dev->getStatusSnapshot();
        if (s.valid) {
            int bar_len = 10;
            int filled = s.power * bar_len / 100;
            if (filled > bar_len) filled = bar_len;
            if (filled < 0) filled = 0;
            char bar[32];
            int p = 0;
            for (int i = 0; i < filled; i++)       bar[p++] = '#';
            for (int i = filled; i < bar_len; i++) bar[p++] = '-';
            bar[p] = '\0';

            std::string estop_str = s.estop ? "\033[31m急停\033[0m" : "正常";
            std::string charge_str = s.charging ? "\033[32m充电中\033[0m" : "未充电";

            box_sep();
            char buf[256];
            snprintf(buf, sizeof(buf),
                     " 位置: (%7.2f, %7.2f, %6.3f)  楼层: %d",
                     s.pos_x, s.pos_y, s.pos_theta, s.floor);
            box_line(buf);
            snprintf(buf, sizeof(buf),
                     " 电量: %3d%% [%s]  %s  急停: %s",
                     s.power, bar, charge_str.c_str(), estop_str.c_str());
            box_line(buf);
            snprintf(buf, sizeof(buf),
                     " 任务: %s  目标: %s  重试: %d",
                     s.move_status.c_str(), s.move_target.c_str(), s.move_retry);
            box_line(buf);
        }
    }

    /* ── 按键提示 ── */
    box_sep();
    if (active_type == "lite3") {
        box_line(" \033[1m── Lite3 遥控 ──\033[0m");
        box_line("  W: 前进  S: 后退  A: 左平移  D: 右平移  Space/F: 停止");
        box_line("  Z: 站立  Y: 移动  U: 原地  X: 自主  C: 手动  2: 急停");
        box_line("  G: 打招呼  J: 跳  1: 扭身体  N: 左转90  E: 右转90  R: 后转");
    } else {
        box_line(" \033[1m── 云迹遥控 (WASD) ──\033[0m  \033[1m── 功能键 ──\033[0m");
        box_line("  W: 前进  S: 后退       Space: 停止    X: 急停   Z: 解除");
        box_line("  A: 左转  D: 右转       C: 取消移动  M: 导航  L: 点位列表");
        box_line("                        R: 刷新状态  P: 电源  V: 版本");
    }
    box_line("  Tab: 切换设备  Q: 退出  H: 帮助");

    /* ── 日志 ── */
    box_sep();
    box_line(" \033[1m日志:\033[0m");

    auto logs = Logger::instance().getRecentLogs(5);
    int log_max = BOX_W - 1;  /* 1 列留给前导空格 */
    for (auto& log : logs) {
        std::string line = " " + log;
        if (utf8_display_width(line) > log_max)
            line = utf8_truncate(line, log_max - 3) + "...";
        box_line(line);
    }
    for (int i = (int)logs.size(); i < 5; i++) {
        box_line("");
    }

    box_bottom();
    printf("\n> ");
    fflush(stdout);
}

void KeyboardController::printHelp()
{
    /* 帮助已在 CmdTranslator::execute() 中处理 */
}
