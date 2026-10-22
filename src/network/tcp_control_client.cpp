#include "tcp_control_client.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

TcpControlClient::TcpControlClient() {}

TcpControlClient::~TcpControlClient()
{
    stop();
}

void TcpControlClient::setRegistration(const std::string& edge_id,
                                       const std::string& robot_id,
                                       const std::string& device)
{
    std::lock_guard<std::mutex> lock(registration_mutex_);
    edge_id_ = edge_id;
    robot_id_ = robot_id;
    device_ = device;
}

bool TcpControlClient::start(const std::string& server_ip, int server_port,
                             int reconnect_interval_ms)
{
    if (running_.load()) return true;

    server_ip_ = server_ip;
    server_port_ = server_port;
    reconnect_interval_ms_ = reconnect_interval_ms;
    if (reconnect_interval_ms_ < 100) reconnect_interval_ms_ = 100;

    recv_buf_.clear();
    running_ = true;
    worker_thread_ = std::thread(&TcpControlClient::workerThreadFn, this);

    LOG_INFO("TCP Client 已启动，目标 Server: %s:%d，重连间隔: %d ms",
             server_ip_.c_str(), server_port_, reconnect_interval_ms_);
    return true;
}

void TcpControlClient::stop()
{
    if (!running_.exchange(false)) return;

    closeSocket();
    if (worker_thread_.joinable()) worker_thread_.join();

    if (connected_.exchange(false)) notifyStatus(false);
}

bool TcpControlClient::connectOnce()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_WARN("TCP Client socket 创建失败: %s", strerror(errno));
        return false;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &reuse, sizeof(reuse));
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port_);
    if (inet_pton(AF_INET, server_ip_.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERR("TCP Client 无效 Server IP: %s", server_ip_.c_str());
        close(fd);
        return false;
    }

    LOG_INFO("TCP Client 正在连接外部 Server %s:%d ...",
             server_ip_.c_str(), server_port_);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_WARN("TCP Client 连接失败: %s", strerror(errno));
        close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        fd_ = fd;
    }

    connected_ = true;

    /* TCP 建链成功后立即注册；断线重连成功时也会再次注册。 */
    if (!sendRegistration()) {
        LOG_WARN("TCP Client 注册报文发送失败，将关闭连接并自动重连");
        closeSocket();
        connected_ = false;
        return false;
    }

    notifyStatus(true);
    LOG_INFO("TCP Client 已连接外部 Server %s:%d",
             server_ip_.c_str(), server_port_);
    return true;
}

void TcpControlClient::closeSocket()
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        fd = fd_;
        fd_ = -1;
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

void TcpControlClient::workerThreadFn()
{
    while (running_) {
        if (!connected_) {
            if (!connectOnce()) {
                for (int waited = 0; running_ && waited < reconnect_interval_ms_; waited += 100) {
                    usleep(100 * 1000);
                }
                continue;
            }
            recv_buf_.clear();
        }

        int fd;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            fd = fd_;
        }
        if (fd < 0) {
            connected_ = false;
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("TCP Client poll 失败: %s", strerror(errno));
            connected_ = false;
            closeSocket();
            notifyStatus(false);
        } else if (rc > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG_WARN("外部 Server 连接断开 (revents=0x%x)", pfd.revents);
                connected_ = false;
                closeSocket();
                notifyStatus(false);
            } else if (pfd.revents & POLLIN) {
                char buf[2048];
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    recv_buf_.append(buf, static_cast<size_t>(n));

                    size_t pos = 0;
                    while (true) {
                        size_t nl = recv_buf_.find('\n', pos);
                        if (nl == std::string::npos) break;

                        std::string line = recv_buf_.substr(pos, nl - pos);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (!line.empty() && cmd_cb_) cmd_cb_(line);
                        pos = nl + 1;
                    }
                    if (pos > 0) recv_buf_.erase(0, pos);
                } else if (n == 0) {
                    LOG_WARN("外部 Server 主动关闭连接");
                    connected_ = false;
                    closeSocket();
                    notifyStatus(false);
                } else if (errno != EINTR) {
                    LOG_WARN("TCP Client recv 失败: %s", strerror(errno));
                    connected_ = false;
                    closeSocket();
                    notifyStatus(false);
                }
            }
        }

        if (running_ && !connected_) {
            continue;
        }
    }

    closeSocket();
}

bool TcpControlClient::send(const std::string& data)
{
    if (!connected_.load()) return false;

    std::string msg = data;
    if (msg.empty() || msg.back() != '\n') msg.push_back('\n');

    std::lock_guard<std::mutex> send_lock(send_mutex_);
    int fd;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        fd = fd_;
    }
    if (fd < 0) return false;

    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = ::send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;

        LOG_WARN("TCP Client 发送失败: %s", strerror(errno));
        connected_ = false;
        closeSocket();
        notifyStatus(false);
        return false;
    }
    return true;
}

bool TcpControlClient::sendRegistration()
{
    std::string edge_id;
    std::string robot_id;
    std::string device;
    {
        std::lock_guard<std::mutex> lock(registration_mutex_);
        edge_id = edge_id_;
        robot_id = robot_id_;
        device = device_;
    }

    if (edge_id.empty() || robot_id.empty() || device.empty()) {
        LOG_WARN("TCP Client 注册信息不完整，未发送注册报文 (edgeId=%s robotId=%s device=%s)",
                 edge_id.c_str(), robot_id.c_str(), device.c_str());
        return false;
    }

    /* 当前 ID 由配置提供，按 JSON 字符串规则做最小转义。 */
    auto jsonEscape = [](const std::string& value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
            }
        }
        return out;
    };

    std::ostringstream oss;
    oss << "{\"cmd\":\"register\",\"edgeId\":\""
        << jsonEscape(edge_id)
        << "\",\"robotId\":\""
        << jsonEscape(robot_id)
        << "\",\"device\":\""
        << jsonEscape(device)
        << "\"}";

    if (!send(oss.str())) {
        return false;
    }

    LOG_INFO("TCP Client 注册成功: edgeId=%s robotId=%s device=%s",
             edge_id.c_str(), robot_id.c_str(), device.c_str());
    return true;
}

void TcpControlClient::notifyStatus(bool connected)
{
    if (status_cb_) status_cb_(connected);
}

std::string TcpControlClient::getServerAddress() const
{
    return server_ip_ + ":" + std::to_string(server_port_);
}
