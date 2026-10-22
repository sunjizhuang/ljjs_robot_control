#ifndef KEYBOARD_CONTROL_H
#define KEYBOARD_CONTROL_H

#include <atomic>
#include <thread>
#include <string>
#include <functional>
#include <termios.h>

#include "control/cmd_translator.h"
#include "device/device_driver.h"
#include "network/tcp_server.h"
#include "network/tcp_control_client.h"
#include "util/log.h"

/*
 * 键盘控制器（双设备版）
 *
 * 架构角色:
 * - 本地终端: raw 模式读取键盘 → InputAdapter(设备感知) → CmdTranslator → DeviceDriver
 * - 网络服务端: TcpServer 接收远程指令 → InputAdapter → CmdTranslator → DeviceDriver
 * - 网络客户端: TcpControlClient 接收外部 Server 指令 → InputAdapter → CmdTranslator → DeviceDriver
 * - joy_control 后台线程: 5Hz 自动发送当前速度 (仅云迹)
 * - HUD 显示线程: 双设备状态实时刷新
 *
 * Tab 键切换激活设备，本地键盘和网络指令走同一条翻译链路。
 */
class KeyboardController {
public:
    KeyboardController(CmdTranslator* translator, TcpServer* server, TcpControlClient* client = nullptr);
    ~KeyboardController();

    /* 启动控制主循环（阻塞） */
    void run();

    /* 停止 */
    void stop();

    /* 处理远程网络指令（由 TcpServer 回调调用） */
    void handleRemoteCommand(const std::string& cmd, int client_fd);
    /* 指定网络来源，用于 MQTT 等非 TCP 通道的日志与统一解析。 */
    void handleRemoteCommandFrom(const std::string& cmd, const std::string& source);

private:
    /* 终端 raw 模式 */
    void enableRawMode();
    void disableRawMode();

    /* joy_control 后台发送线程 (仅云迹) */
    void joySendThread();

    /* 状态刷新线程 */
    void statusDisplayThread();

    /* 处理本地按键 */
    void handleLocalKey(char c);

    /* 刷新 HUD 显示 */
    void refreshDisplay();

    /* 显示当前设备的帮助 */
    void printHelp();

    CmdTranslator*       translator_;
    TcpServer*           server_;
    TcpControlClient*    client_;

    std::atomic<bool>    running_{false};

    /* 后台线程 */
    std::thread          joy_thread_;
    std::thread          display_thread_;

    /* 终端原始设置 */
    bool                 raw_mode_ = false;
    struct termios       orig_termios_;
};

#endif /* KEYBOARD_CONTROL_H */
