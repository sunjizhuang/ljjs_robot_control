#ifndef LITE3_DRIVER_H
#define LITE3_DRIVER_H

#include "device_driver.h"

#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>
#include <netinet/in.h>

/* 云深处 Lite3 设备驱动：UDP 控制 + UDP 实时状态接收。 */
#pragma pack(push, 4)
struct Lite3CommandHead {
    uint32_t code;
    uint32_t parameters_size;
    uint32_t type;
};
#pragma pack(pop)

/* 1.3.1 RobotStateUpload 的协议字段。采用 4 字节对齐，与 SDK wire layout 一致。 */
#pragma pack(push, 4)
struct Lite3RobotStateUpload {
    int32_t  robot_basic_state;
    int32_t  robot_gait_state;
    double   rpy[3];
    double   rpy_vel[3];
    double   xyz_acc[3];
    double   pos_world[3];
    double   vel_world[3];
    double   vel_body[3];
    uint32_t touch_down_and_stair_trot;
    uint8_t  is_charging;
    uint32_t error_state;
    int32_t  robot_motion_state;
    double   battery_level;
    int32_t  task_state;
    uint8_t  is_robot_need_move;
    uint8_t  zero_position_flag;
    double   ultrasound[2];
};
#pragma pack(pop)

struct Lite3State {
    bool valid = false;
    uint32_t robot_basic_state = 0;
    uint32_t robot_gait_state = 0;
    double rpy[3] = {0,0,0};
    double rpy_vel[3] = {0,0,0};
    double xyz_acc[3] = {0,0,0};
    double pos_world[3] = {0,0,0};
    double vel_world[3] = {0,0,0};
    double vel_body[3] = {0,0,0};
    uint32_t touch_down_and_stair_trot = 0;
    bool is_charging = false;
    uint32_t error_state = 0;
    int32_t robot_motion_state = 0;
    double battery_level = 0;
    int32_t task_state = 0;
    bool is_robot_need_move = false;
    bool zero_position_flag = false;
    double ultrasound[2] = {0,0};
    double joint_angle[12] = {0};
    double joint_vel[12] = {0};
    uint64_t state_update_count = 0;
    uint64_t joint_angle_update_count = 0;
    uint64_t joint_vel_update_count = 0;
};

class Lite3Driver : public IDeviceDriver {
public:
    Lite3Driver();
    ~Lite3Driver() override;

    bool connect(const std::string& ip, int port) override;
    void disconnect() override;
    bool isConnected() const override;

    bool moveToMarker(const std::string& marker) override;
    bool moveToLocation(double x, double y, double theta) override;
    bool cancelMove() override;
    bool joyControl(double linear, double angular) override;
    bool estop(bool flag) override;

    bool requestStatus() override;
    bool requestInfo() override;
    bool requestPower() override;
    bool requestVersion() override;
    bool requestMarkers() override;
    bool subscribeStatus(double freq_hz) override;
    bool unsubscribeStatus() override;
    DeviceStatus getStatusSnapshot() override;

    void setStatusCallback(StatusCallback cb) override { status_cb_ = cb; }
    void setNotifCallback(NotifCallback cb) override { notif_cb_ = cb; }
    void setConnCallback(ConnCallback cb) override { conn_cb_ = cb; }

    std::string getDeviceName() const override { return "Lite3"; }
    std::string getDeviceType() const override { return "lite3"; }

    bool setMode(const std::string& mode) override;
    bool performAction(const std::string& action) override;

    /* 1.2.3 轴指令：axis=x/y/yaw/roll/pitch/height，值为协议原始 int。 */
    bool setAxis(const std::string& axis, int value) override;
    bool setAxisRaw(uint32_t code, int value);
    void stopAxis();

    /* 1.2.5 步态 */
    bool setGait(const std::string& gait) override;

    /* 1.2.8 / 1.2.9 */
    bool saveData() override;
    bool setContinuousMotion(bool enabled) override;

    /* 1.2.12 自主模式速度 */
    bool setVelocity(double vx, double vy, double yaw) override;

    void heartbeat() override;

    Lite3State getLite3StateSnapshot() const;

private:
    bool sendCommand(uint32_t code, uint32_t param = 0, uint32_t type = 0);
    bool sendComplexDouble(uint32_t code, double value);
    bool sendVoiceCommand(uint32_t voice_code);

    void heartbeatThread();
    void axisThread();
    void receiveThread();
    void parsePacket(const uint8_t* data, size_t size);
    void onConnChanged(bool connected);
    void updateGenericStatus();

    int sock_fd_ = -1;
    int recv_fd_ = -1;
    struct sockaddr_in server_addr_;
    std::string server_ip_;
    int server_port_ = 0;
    int receive_port_ = 43897;

    std::atomic<bool> connected_{false};
    std::atomic<bool> heartbeat_running_{false};
    std::atomic<bool> axis_running_{false};
    std::atomic<bool> receive_running_{false};
    std::thread heartbeat_thread_;
    std::thread axis_thread_;
    std::thread receive_thread_;

    std::atomic<int32_t> axis_y_{0};
    std::atomic<int32_t> axis_x_{0};
    std::atomic<int32_t> axis_yaw_{0};
    std::atomic<int32_t> axis_roll_{0};
    std::atomic<int32_t> axis_pitch_{0};
    std::atomic<int32_t> axis_height_{0};
    std::atomic<bool> axis_active_{false};
    std::mutex send_mutex_;

    StatusCallback status_cb_;
    NotifCallback notif_cb_;
    ConnCallback conn_cb_;

    mutable std::mutex status_mutex_;
    DeviceStatus last_status_;
    Lite3State last_lite3_state_;
};

#endif /* LITE3_DRIVER_H */
