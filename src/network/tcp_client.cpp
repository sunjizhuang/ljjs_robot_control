#include "tcp_client.h"
#include "util/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

TcpClient::TcpClient() {}

TcpClient::~TcpClient()
{
    disconnect();
}

bool TcpClient::connect(const std::string& ip, int port)
{
    ip_ = ip;
    port_ = port;
    running_ = true;

    /* 创建 socket */
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERR("socket 创建失败: %s", strerror(errno));
        return false;
    }

    /* 设置发送/接收超时 */
    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec  = 3;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* TCP_NODELAY */
    int flag = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERR("无效 IP 地址: %s", ip.c_str());
        close(fd_);
        fd_ = -1;
        return false;
    }

    LOG_INFO("正在连接 %s:%d ...", ip.c_str(), port);

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERR("连接失败: %s", strerror(errno));
        close(fd_);
        fd_ = -1;
        notifyStatus(false);
        return false;
    }

    connected_ = true;
    LOG_INFO("已连接到机器人 %s:%d", ip.c_str(), port);
    notifyStatus(true);

    /* 启动接收线程 */
    recv_thread_ = std::thread(&TcpClient::recvThreadFn, this);

    return true;
}

void TcpClient::disconnect()
{
    running_ = false;
    connected_ = false;

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    if (recv_thread_.joinable())
        recv_thread_.join();

    notifyStatus(false);
}

bool TcpClient::sendCommand(const std::string& cmd)
{
    if (!connected_ || fd_ < 0) return false;

    std::string data = cmd + "\n";

    std::lock_guard<std::mutex> lock(send_mutex_);
    ssize_t n = send(fd_, data.c_str(), data.size(), 0);
    if (n < 0) {
        LOG_ERR("发送失败: %s", strerror(errno));
        connected_ = false;
        notifyStatus(false);
        return false;
    }
    return true;
}

void TcpClient::recvThreadFn()
{
    char buf[4096];

    while (running_ && connected_) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 1000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("poll 失败: %s", strerror(errno));
            break;
        }
        if (rc == 0) continue; /* 超时 */

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_WARN("连接断开 (poll revents=0x%x)", pfd.revents);
            break;
        }

        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_WARN("机器人关闭了连接");
            } else {
                LOG_WARN("recv 失败: %s", strerror(errno));
            }
            break;
        }

        /* 将新数据追加到接收缓冲 */
        recv_buf_.append(buf, n);

        /* 提取完整的 JSON 对象 */
        size_t pos = 0;
        while (pos < recv_buf_.size()) {
            /* 跳过空白字符和换行 */
            while (pos < recv_buf_.size() && 
                   (recv_buf_[pos] == '\n' || recv_buf_[pos] == '\r' || 
                    recv_buf_[pos] == ' '  || recv_buf_[pos] == '\t')) {
                pos++;
            }
            if (pos >= recv_buf_.size()) break;

            if (recv_buf_[pos] != '{') {
                /* 非 JSON 数据（可能是命令回显），跳过 */
                pos++;
                continue;
            }

            size_t end = json_find_end(recv_buf_, pos);
            if (end == std::string::npos) {
                /* JSON 不完整，等待更多数据 */
                break;
            }

            /* 解析并处理完整 JSON */
            std::string json_str = recv_buf_.substr(pos, end - pos);
            JsonValue val;
            if (json_parse(json_str, val)) {
                if (msg_cb_) msg_cb_(val);
            } else {
                LOG_WARN("JSON 解析失败: %s", json_str.substr(0, 100).c_str());
            }

            pos = end;
        }

        /* 移除已处理的数据 */
        if (pos > 0) {
            recv_buf_.erase(0, pos);
        }
    }

    connected_ = false;
    notifyStatus(false);
    LOG_WARN("接收线程退出");
}

void TcpClient::notifyStatus(bool connected)
{
    if (status_cb_) status_cb_(connected);
}
