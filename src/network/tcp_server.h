#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <set>

/*
 * TCP Server —— 网络通信层
 *
 * 功能:
 * 1. 监听指定端口，接受外部控制端连接
 * 2. 支持多客户端并发连接
 * 3. 接收键盘指令文本行，回调通知上层
 * 4. 可向所有客户端广播状态信息
 */

struct ClientSession {
    int         fd;
    std::string addr;       /* "IP:PORT" */
    std::string recv_buf;   /* 接收缓冲（处理半行） */
};

class TcpServer {
public:
    /* 收到完整指令行的回调 (指令内容, 客户端fd) */
    typedef std::function<void(const std::string& cmd, int client_fd)> CommandCallback;
    /* 客户端连接/断开回调 (fd, addr, 是否连接) */
    typedef std::function<void(int fd, const std::string& addr, bool connected)> ClientCallback;

    TcpServer();
    ~TcpServer();

    /* 启动监听（bind_addr 为空或 "0.0.0.0" 时监听所有接口） */
    bool start(int port, const std::string& bind_addr = "0.0.0.0");
    /* 停止 */
    void stop();
    /* 是否在运行 */
    bool isRunning() const { return running_.load(); }

    /* 向指定客户端发送数据 */
    bool sendTo(int client_fd, const std::string& data);
    /* 向所有客户端广播 */
    void broadcast(const std::string& data);

    /* 设置回调 */
    void setCommandCallback(CommandCallback cb)  { cmd_cb_ = cb; }
    void setClientCallback(ClientCallback cb)    { client_cb_ = cb; }

    /* 获取当前连接的客户端列表 */
    std::vector<std::string> getClientList();

private:
    void acceptThreadFn();
    void clientThreadFn(int fd, std::string addr);

    int                      listen_fd_ = -1;
    std::atomic<bool>        running_{false};
    std::thread              accept_thread_;

    /* 客户端管理 */
    std::mutex               clients_mutex_;
    std::vector<ClientSession> clients_;

    /* 回调 */
    CommandCallback          cmd_cb_;
    ClientCallback           client_cb_;
};

#endif /* TCP_SERVER_H */
