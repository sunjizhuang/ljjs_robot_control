#include "tcp_server.h"
#include "util/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <algorithm>

TcpServer::TcpServer() {}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::start(int port, const std::string& bind_addr)
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERR("TCP Server socket 创建失败: %s", strerror(errno));
        return false;
    }

    /* 允许地址重用 */
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* 解析绑定地址（空 / 0.0.0.0 表示所有接口） */
    if (bind_addr.empty() || bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERR("TCP Server 无效绑定地址: %s", bind_addr.c_str());
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERR("TCP Server bind 失败 (%s:%d): %s", bind_addr.c_str(), port, strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        LOG_ERR("TCP Server listen 失败: %s", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    LOG_INFO("TCP Server 监听 %s:%d，等待外部控制端连接...", bind_addr.c_str(), port);

    accept_thread_ = std::thread(&TcpServer::acceptThreadFn, this);
    return true;
}

void TcpServer::stop()
{
    if (!running_) return;
    running_ = false;

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    /* 关闭所有客户端连接 */
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& c : clients_) {
            if (c.fd >= 0) close(c.fd);
        }
        clients_.clear();
    }

    if (accept_thread_.joinable())
        accept_thread_.join();
}

void TcpServer::acceptThreadFn()
{
    while (running_) {
        struct pollfd pfd;
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 500);
        if (rc <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("accept 失败: %s", strerror(errno));
            continue;
        }

        /* TCP_NODELAY */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%d",
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port));

        LOG_INFO("外部控制端连接: %s (fd=%d)", addr_str, client_fd);

        /* 添加到客户端列表 */
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            ClientSession s;
            s.fd = client_fd;
            s.addr = addr_str;
            s.recv_buf.clear();
            clients_.push_back(s);
        }

        if (client_cb_) client_cb_(client_fd, addr_str, true);

        /* 为每个客户端启动接收线程 */
        std::thread(&TcpServer::clientThreadFn, this, client_fd, std::string(addr_str)).detach();
    }
}

void TcpServer::clientThreadFn(int fd, std::string addr)
{
    char buf[4096];

    while (running_) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 1000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        /* 将数据追加到该客户端的接收缓冲 */
        std::string* recv_buf = nullptr;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.fd == fd) { recv_buf = &c.recv_buf; break; }
            }
        }
        if (!recv_buf) break;

        recv_buf->append(buf, n);

        /* 按行提取完整指令 */
        size_t pos = 0;
        while (true) {
            size_t nl = recv_buf->find('\n', pos);
            if (nl == std::string::npos) break;

            std::string line = recv_buf->substr(pos, nl - pos);
            /* 去除 \r */
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (!line.empty() && cmd_cb_) {
                cmd_cb_(line, fd);
            }
            pos = nl + 1;
        }

        if (pos > 0) {
            recv_buf->erase(0, pos);
        }
    }

    /* 客户端断开 */
    LOG_INFO("外部控制端断开: %s (fd=%d)", addr.c_str(), fd);
    close(fd);

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [fd](const ClientSession& s) { return s.fd == fd; }),
            clients_.end());
    }

    if (client_cb_) client_cb_(fd, addr, false);
}

bool TcpServer::sendTo(int client_fd, const std::string& data)
{
    std::string msg = data + "\n";
    ssize_t n = send(client_fd, msg.c_str(), msg.size(), 0);
    return n == (ssize_t)msg.size();
}

void TcpServer::broadcast(const std::string& data)
{
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& c : clients_) {
        if (c.fd >= 0) {
            std::string msg = data + "\n";
            send(c.fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
        }
    }
}

std::vector<std::string> TcpServer::getClientList()
{
    std::vector<std::string> list;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& c : clients_) {
        list.push_back(c.addr);
    }
    return list;
}
