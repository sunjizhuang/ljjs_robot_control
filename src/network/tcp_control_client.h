#ifndef TCP_CONTROL_CLIENT_H
#define TCP_CONTROL_CLIENT_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/*
 * TCP 控制客户端 —— SC668S/开发主机作为客户端连接外部控制端 Server。
 *
 * 协议与 TcpServer 保持一致：外部 Server 按“每行一条指令”发送，
 * 本类负责连接、断线重连、拆包/粘包处理，并通过回调把完整指令交给上层。
 */
class TcpControlClient {
public:
    typedef std::function<void(const std::string& cmd)> CommandCallback;
    typedef std::function<void(bool connected)> StatusCallback;

    TcpControlClient();
    ~TcpControlClient();

    /* 启动客户端；首次连接失败也会返回 true，并在后台自动重连。 */
    bool start(const std::string& server_ip, int server_port, int reconnect_interval_ms);

    /* 配置连接成功后的注册报文；每次重连成功都会重新上报。 */
    void setRegistration(const std::string& edge_id,
                         const std::string& robot_id,
                         const std::string& device);
    void stop();

    bool isRunning() const { return running_.load(); }
    bool isConnected() const { return connected_.load(); }

    /* 向外部 Server 发送一条控制/状态消息。自动补换行。 */
    bool send(const std::string& data);

    void setCommandCallback(CommandCallback cb) { cmd_cb_ = cb; }
    void setStatusCallback(StatusCallback cb) { status_cb_ = cb; }

    std::string getServerAddress() const;

private:
    void workerThreadFn();
    bool connectOnce();
    void closeSocket();
    void notifyStatus(bool connected);
    bool sendRegistration();

    int fd_ = -1;
    std::string server_ip_;
    int server_port_ = 0;
    int reconnect_interval_ms_ = 5000;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread worker_thread_;
    std::mutex socket_mutex_;
    std::mutex send_mutex_;

    CommandCallback cmd_cb_;
    StatusCallback status_cb_;
    std::string recv_buf_;

    std::mutex registration_mutex_;
    std::string edge_id_;
    std::string robot_id_;
    std::string device_;
};

#endif /* TCP_CONTROL_CLIENT_H */
