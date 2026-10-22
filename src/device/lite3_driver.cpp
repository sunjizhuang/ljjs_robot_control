#include "lite3_driver.h"
#include "config/yaml_config.h"
#include "util/log.h"
#include "config/config.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

namespace {
static const uint32_t CODE_0901 = 0x0901;
static const uint32_t CODE_0902 = 0x0902;
static const uint32_t CODE_0903 = 0x0903;

static bool finiteDouble(double v) { return std::isfinite(v); }

static int32_t clampAxis(const std::string& axis, int value)
{
    int limit = 0;
    if (axis == "x" || axis == "pitch") limit = 6553;
    else if (axis == "y" || axis == "roll") limit = 12553;
    else if (axis == "yaw") limit = 9553;
    else if (axis == "height") limit = 20000;
    else return value;

	std::max(-limit, std::min(limit, value));

    return value;
/*
    return std::max(-limit, std::min(limit, value));
*/
}
}

Lite3Driver::Lite3Driver()
{
    memset(&server_addr_, 0, sizeof(server_addr_));
}

Lite3Driver::~Lite3Driver() { disconnect(); }

bool Lite3Driver::connect(const std::string& ip, int port)
{
    server_ip_ = ip;
    server_port_ = port;
    receive_port_ = g_config.lite3_receive_port;

    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        LOG_ERR("Lite3: UDP socket 创建失败: %s", strerror(errno));
        return false;
    }

    memset(&server_addr_, 0, sizeof(server_addr_));
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &server_addr_.sin_addr) <= 0) {
        LOG_ERR("Lite3: 无效 IP 地址: %s", ip.c_str());
        close(sock_fd_); sock_fd_ = -1;
        return false;
    }

    /* 接收端独立绑定 43897；官方 SDK network.toml 默认 target_port=43897。 */
    recv_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd_ < 0) {
        LOG_ERR("Lite3: 接收 UDP socket 创建失败: %s", strerror(errno));
        close(sock_fd_); sock_fd_ = -1;
        return false;
    }
    int reuse = 1;
    setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(receive_port_);
    if (bind(recv_fd_, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
        LOG_ERR("Lite3: UDP 接收端口 %d 绑定失败: %s", receive_port_, strerror(errno));
        close(recv_fd_); recv_fd_ = -1;
        close(sock_fd_); sock_fd_ = -1;
        return false;
    }

    connected_ = true;
    heartbeat_running_ = true;
    axis_running_ = true;
    receive_running_ = true;
    heartbeat_thread_ = std::thread(&Lite3Driver::heartbeatThread, this);
    axis_thread_ = std::thread(&Lite3Driver::axisThread, this);
    receive_thread_ = std::thread(&Lite3Driver::receiveThread, this);

    LOG_INFO("Lite3: 已连接 (UDP %s:%d), telemetry=%d", ip.c_str(), port, receive_port_);
    sendCommand(LITE3_CMD_STAND);
    onConnChanged(true);
    return true;
}

void Lite3Driver::disconnect()
{
    connected_ = false;
    heartbeat_running_ = false;
    axis_running_ = false;
    receive_running_ = false;

    /* 唤醒 receive_thread */
    if (recv_fd_ >= 0) shutdown(recv_fd_, SHUT_RDWR);

    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (axis_thread_.joinable()) axis_thread_.join();
    if (receive_thread_.joinable()) receive_thread_.join();

    if (sock_fd_ >= 0) { close(sock_fd_); sock_fd_ = -1; }
    if (recv_fd_ >= 0) { close(recv_fd_); recv_fd_ = -1; }

    axis_active_ = false;
    if (last_lite3_state_.valid) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        last_lite3_state_.valid = false;
    }
    if (conn_cb_) conn_cb_(false);
}

bool Lite3Driver::isConnected() const { return connected_; }

bool Lite3Driver::sendCommand(uint32_t code, uint32_t param, uint32_t type)
{
    if (!connected_ || sock_fd_ < 0) return false;
    Lite3CommandHead cmd;
    cmd.code = code;
    cmd.parameters_size = param;
    cmd.type = type;
    std::lock_guard<std::mutex> lock(send_mutex_);
    ssize_t n = sendto(sock_fd_, &cmd, sizeof(cmd), 0,
                       reinterpret_cast<struct sockaddr*>(&server_addr_), sizeof(server_addr_));
    if (n != (ssize_t)sizeof(cmd)) {
        LOG_ERR("Lite3: 发送失败 (0x%08X): %s", code, strerror(errno));
        return false;
    }
    LOG_DBG("Lite3: 发送 0x%08X (param=%u,type=%u)", code, param, type);
    return true;
}

bool Lite3Driver::sendComplexDouble(uint32_t code, double value)
{
    if (!connected_ || sock_fd_ < 0 || !finiteDouble(value)) return false;
    uint8_t packet[sizeof(Lite3CommandHead) + sizeof(double)] = {0};
    Lite3CommandHead head;
    head.code = code;
    head.parameters_size = sizeof(double);
    head.type = 1;
    memcpy(packet, &head, sizeof(head));
    memcpy(packet + sizeof(head), &value, sizeof(value));
    std::lock_guard<std::mutex> lock(send_mutex_);
    ssize_t n = sendto(sock_fd_, packet, sizeof(packet), 0,
                       reinterpret_cast<struct sockaddr*>(&server_addr_), sizeof(server_addr_));
    if (n != (ssize_t)sizeof(packet)) {
        LOG_ERR("Lite3: 复杂指令发送失败 (0x%08X): %s", code, strerror(errno));
        return false;
    }
    LOG_DBG("Lite3: 发送速度 0x%08X = %.4f", code, value);
    return true;
}

bool Lite3Driver::sendVoiceCommand(uint32_t voice_code) { return sendCommand(LITE3_CMD_VOICE_BASE, voice_code, 0); }

void Lite3Driver::heartbeat() { sendCommand(LITE3_HEARTBEAT_CODE); }

void Lite3Driver::heartbeatThread()
{
    while (heartbeat_running_) {
        heartbeat();
        usleep(g_config.lite3_heartbeat_ms * 1000);
    }
}

void Lite3Driver::axisThread()
{
    /* 轴指令持续发送周期 50ms = 20Hz，满足协议要求。 */
    while (axis_running_) {
        if (connected_ && axis_active_) {
            sendCommand(LITE3_AXIS_Y, (uint32_t)axis_y_.load(), 0);
            sendCommand(LITE3_AXIS_X, (uint32_t)axis_x_.load(), 0);
            sendCommand(LITE3_AXIS_YAW, (uint32_t)axis_yaw_.load(), 0);
            if (axis_roll_.load() != 0) sendCommand(LITE3_AXIS_Y, (uint32_t)axis_roll_.load(), 0);
            if (axis_pitch_.load() != 0) sendCommand(LITE3_AXIS_X, (uint32_t)axis_pitch_.load(), 0);
            if (axis_height_.load() != 0) sendCommand(LITE3_AXIS_HEIGHT, (uint32_t)axis_height_.load(), 0);
        }
        usleep(50000);
    }
}

void Lite3Driver::receiveThread()
{
    uint8_t buf[2048];
    while (receive_running_) {
        struct pollfd pfd;
        pfd.fd = recv_fd_; pfd.events = POLLIN; pfd.revents = 0;
        int rc = poll(&pfd, 1, 500);
        if (!receive_running_) break;
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("Lite3: telemetry poll 失败: %s", strerror(errno));
            continue;
        }
        if (rc == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL)) continue;
        if (!(pfd.revents & POLLIN)) continue;
        ssize_t n = recvfrom(recv_fd_, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n > 0) parsePacket(buf, (size_t)n);
    }
}

void Lite3Driver::parsePacket(const uint8_t* data, size_t size)
{
    if (size < sizeof(Lite3CommandHead)) return;
    Lite3CommandHead head;
    memcpy(&head, data, sizeof(head));
    if (head.type != 1) return;
    if ((size_t)head.parameters_size + sizeof(head) != size) {
        LOG_WARN("Lite3: telemetry 长度异常 code=0x%08X size=%zu payload=%u", head.code, size, head.parameters_size);
        return;
    }
    const uint8_t* payload = data + sizeof(head);

    DeviceStatus callback_status;
    bool have_callback = false;
    std::unique_lock<std::mutex> lock(status_mutex_);
    if (head.code == CODE_0901) {
        if (head.parameters_size != sizeof(Lite3RobotStateUpload)) {
            LOG_WARN("Lite3: 0x0901 payload=%u, expected=%zu", head.parameters_size, sizeof(Lite3RobotStateUpload));
            return;
        }
        Lite3RobotStateUpload raw;
        memcpy(&raw, payload, sizeof(raw));
        Lite3State& s = last_lite3_state_;
        s.valid = true;
        s.robot_basic_state = (uint32_t)raw.robot_basic_state;
        s.robot_gait_state = (uint32_t)raw.robot_gait_state;
        memcpy(s.rpy, raw.rpy, sizeof(s.rpy));
        memcpy(s.rpy_vel, raw.rpy_vel, sizeof(s.rpy_vel));
        memcpy(s.xyz_acc, raw.xyz_acc, sizeof(s.xyz_acc));
        memcpy(s.pos_world, raw.pos_world, sizeof(s.pos_world));
        memcpy(s.vel_world, raw.vel_world, sizeof(s.vel_world));
        memcpy(s.vel_body, raw.vel_body, sizeof(s.vel_body));
        s.touch_down_and_stair_trot = raw.touch_down_and_stair_trot;
        s.is_charging = raw.is_charging != 0;
        s.error_state = raw.error_state;
        s.robot_motion_state = raw.robot_motion_state;
        s.battery_level = raw.battery_level;
        s.task_state = raw.task_state;
        s.is_robot_need_move = raw.is_robot_need_move != 0;
        s.zero_position_flag = raw.zero_position_flag != 0;
        memcpy(s.ultrasound, raw.ultrasound, sizeof(s.ultrasound));
        ++s.state_update_count;
        last_status_.valid = true;
        last_status_.power = (int)std::round(std::max(0.0, std::min(100.0, s.battery_level * 100.0)));
        last_status_.mode = (s.robot_basic_state == 6) ? "force" : "unknown";
        if (status_cb_) {
            callback_status = last_status_;
            have_callback = true;
        }
    } else if (head.code == CODE_0902 && head.parameters_size == sizeof(double) * 12) {
        memcpy(last_lite3_state_.joint_angle, payload, sizeof(last_lite3_state_.joint_angle));
        last_lite3_state_.valid = true;
        ++last_lite3_state_.joint_angle_update_count;
    } else if (head.code == CODE_0903 && head.parameters_size == sizeof(double) * 12) {
        memcpy(last_lite3_state_.joint_vel, payload, sizeof(last_lite3_state_.joint_vel));
        last_lite3_state_.valid = true;
        ++last_lite3_state_.joint_vel_update_count;
    }
    if (have_callback && status_cb_) {
        lock.unlock();
        status_cb_(callback_status);
    }
}

bool Lite3Driver::setMode(const std::string& mode)
{
    if (mode == "auto" || mode == "manual" || mode == "move" || mode == "stand") {
        stopAxis();
    }
    uint32_t code = 0;
    if (mode == "stand") code = LITE3_CMD_STAND;
    else if (mode == "move") code = LITE3_CMD_MOVE_MODE;
    else if (mode == "inplace") code = LITE3_CMD_INPLACE;
    else if (mode == "auto") code = LITE3_CMD_AUTO_MODE;
    else if (mode == "manual") code = LITE3_CMD_MANUAL;
    else return false;
    LOG_INFO("Lite3: 模式切换 → %s (0x%08X)", mode.c_str(), code);
    {
        std::lock_guard<std::mutex> lock(status_mutex_); last_status_.mode = mode;
    }
    return sendCommand(code);
}

bool Lite3Driver::performAction(const std::string& action)
{
    uint32_t code = 0;
    if (action == "greet") code = LITE3_CMD_GREET;
    else if (action == "jump") code = LITE3_CMD_JUMP;
    else if (action == "twist") code = LITE3_CMD_TWIST;
    if (code) return sendCommand(code);

    uint32_t voice = 0;
    if (action == "forward") voice = LITE3_VOICE_FORWARD;
    else if (action == "backward") voice = LITE3_VOICE_BACKWARD;
    else if (action == "strafe_left") voice = LITE3_VOICE_LEFT;
    else if (action == "strafe_right") voice = LITE3_VOICE_RIGHT;
    else if (action == "stop") voice = LITE3_VOICE_STOP;
    else if (action == "turn_left_90") voice = LITE3_VOICE_TURN_L90;
    else if (action == "turn_right_90") voice = LITE3_VOICE_TURN_R90;
    else if (action == "turn_180") voice = LITE3_VOICE_TURN_180;
    else return false;
    return sendVoiceCommand(voice);
}

bool Lite3Driver::setAxis(const std::string& axis, int value)
{
    int32_t v = clampAxis(axis, value);
    if (axis == "x") axis_x_ = v;
    else if (axis == "y") axis_y_ = v;
    else if (axis == "yaw") axis_yaw_ = v;
    else if (axis == "roll") axis_roll_ = v;
    else if (axis == "pitch") axis_pitch_ = v;
    else if (axis == "height") axis_height_ = v;
    else return false;
    axis_active_ = true;
    LOG_INFO("Lite3: Axis %s=%d", axis.c_str(), v);
    return true;
}

bool Lite3Driver::setAxisRaw(uint32_t code, int value)
{
    std::string axis;
    if (code == LITE3_AXIS_X) axis = "x";
    else if (code == LITE3_AXIS_Y) axis = "y";
    else if (code == LITE3_AXIS_YAW) axis = "yaw";
    else if (code == LITE3_AXIS_HEIGHT) axis = "height";
    else return false;
    return setAxis(axis, value);
}

void Lite3Driver::stopAxis()
{
    axis_x_ = axis_y_ = axis_yaw_ = axis_roll_ = axis_pitch_ = axis_height_ = 0;
    if (connected_) {
        sendCommand(LITE3_AXIS_Y, 0, 0);
        sendCommand(LITE3_AXIS_X, 0, 0);
        sendCommand(LITE3_AXIS_YAW, 0, 0);
    }
    axis_active_ = false;
}

bool Lite3Driver::setGait(const std::string& gait)
{
    uint32_t code = 0;
    if (gait == "low" || gait == "slow") code = LITE3_GAIT_LOW;
    else if (gait == "medium" || gait == "mid") code = LITE3_GAIT_MEDIUM;
    else if (gait == "high" || gait == "fast") code = LITE3_GAIT_HIGH;
    else if (gait == "crawl" || gait == "normal_crawl") code = LITE3_GAIT_CRAWL;
    else if (gait == "grip") code = LITE3_GAIT_GRIP;
    else if (gait == "general") code = LITE3_GAIT_GENERAL;
    else if (gait == "high_step") code = LITE3_GAIT_HIGH_STEP;
    else return false;
    LOG_INFO("Lite3: 步态 %s (0x%08X)", gait.c_str(), code);
    return sendCommand(code);
}

bool Lite3Driver::saveData()
{
    LOG_WARN("Lite3: 保存数据指令会清零电机力矩并关闭运动程序");
    return sendCommand(LITE3_CMD_SAVE_DATA);
}

bool Lite3Driver::setContinuousMotion(bool enabled)
{
    return sendCommand(LITE3_CMD_CONTINUOUS, enabled ? 1 : 2, 0);
}

bool Lite3Driver::setVelocity(double vx, double vy, double yaw)
{
    stopAxis();
    if (vx < -1.0 || vx > 1.0 || vy < -0.5 || vy > 0.5 || yaw < -1.5 || yaw > 1.5) {
        LOG_WARN("Lite3: 速度越界 vx=%.3f vy=%.3f yaw=%.3f", vx, vy, yaw);
        return false;
    }
    /* 1.2.12：自主模式下分别发送 0x0140/0x0145/0x0141。 */
    bool ok = true;
    ok = sendComplexDouble(0x0140, vx) && ok;
    ok = sendComplexDouble(0x0145, vy) && ok;
    ok = sendComplexDouble(0x0141, yaw) && ok;
    return ok;
}

bool Lite3Driver::moveToMarker(const std::string& marker) { (void)marker; return false; }
bool Lite3Driver::moveToLocation(double x, double y, double theta) { (void)x; (void)y; (void)theta; return false; }
bool Lite3Driver::cancelMove() { stopAxis(); return sendVoiceCommand(LITE3_VOICE_STOP); }

bool Lite3Driver::joyControl(double linear, double angular)
{
    /* 保留旧接口兼容：Lite3 旧键盘离散控制。连续速度请调用 setVelocity。 */
    if (linear > 0.01) return sendVoiceCommand(LITE3_VOICE_FORWARD);
    if (linear < -0.01) return sendVoiceCommand(LITE3_VOICE_BACKWARD);
    if (angular > 0.01) return sendVoiceCommand(LITE3_VOICE_TURN_L90);
    if (angular < -0.01) return sendVoiceCommand(LITE3_VOICE_TURN_R90);
    return sendVoiceCommand(LITE3_VOICE_STOP);
}

bool Lite3Driver::estop(bool flag)
{
    if (flag) { stopAxis(); return sendCommand(LITE3_CMD_SOFT_ESTOP); }
    return sendCommand(LITE3_CMD_STAND);
}

bool Lite3Driver::requestStatus() { LOG_INFO("Lite3: 当前状态来自 UDP 0x0901 实时上报"); return true; }
bool Lite3Driver::requestInfo() { return true; }
bool Lite3Driver::requestPower() { return true; }
bool Lite3Driver::requestVersion() { return true; }
bool Lite3Driver::requestMarkers() { return false; }
bool Lite3Driver::subscribeStatus(double freq_hz) { (void)freq_hz; return true; }
bool Lite3Driver::unsubscribeStatus() { return true; }

DeviceStatus Lite3Driver::getStatusSnapshot()
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    DeviceStatus s = last_status_;
    s.valid = connected_ && last_lite3_state_.valid;
    s.power = (int)std::round(std::max(0.0, std::min(100.0, last_lite3_state_.battery_level * 100.0)));
    s.pos_x = last_lite3_state_.pos_world[0];
    s.pos_y = last_lite3_state_.pos_world[1];
    s.pos_theta = last_lite3_state_.rpy[2];
    return s;
}

Lite3State Lite3Driver::getLite3StateSnapshot() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return last_lite3_state_;
}

void Lite3Driver::onConnChanged(bool connected)
{
    if (conn_cb_) conn_cb_(connected);
}
