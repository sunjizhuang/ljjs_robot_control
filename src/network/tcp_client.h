#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

#include "util/json.h"

/*
 * TCP 客户端 —— 连接云迹机器人，收发数据。
 *
 * 协议: 发送 URL 字符串（以换行结尾），接收 JSON。
 * 收到完整 JSON 后调用回调函数。
 */
class TcpClient {
public:
    /* 收到 JSON 消息的回调 */
    typedef std::function<void(const JsonValue&)> MessageCallback;
    /* 连接状态变化回调 */
    typedef std::function<void(bool connected)> StatusCallback;

    TcpClient();
    ~TcpClient();

    /* 连接到机器人 */
    bool connect(const std::string& ip, int port);
    /* 断开连接 */
    void disconnect();
    /* 是否已连接 */
    bool isConnected() const { return connected_.load(); }

    /* 发送命令（URL 字符串，自动加换行） */
    bool sendCommand(const std::string& cmd);

    /* 设置回调 */
    void setMessageCallback(MessageCallback cb) { msg_cb_ = cb; }
    void setStatusCallback(StatusCallback cb)   { status_cb_ = cb; }

private:
    void recvThreadFn();
    void reconnectThreadFn();
    void notifyStatus(bool connected);

    int                fd_ = -1;
    std::string        ip_;
    int                port_ = 0;
    std::atomic<bool>  connected_{false};
    std::atomic<bool>  running_{false};
    std::thread        recv_thread_;
    std::thread        reconnect_thread_;
    std::mutex         send_mutex_;

    MessageCallback    msg_cb_;
    StatusCallback     status_cb_;
    std::string        recv_buf_;
};

#endif /* TCP_CLIENT_H */
