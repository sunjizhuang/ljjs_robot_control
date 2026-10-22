#ifndef YUNJI_DRIVER_H
#define YUNJI_DRIVER_H

#include "device_driver.h"
#include "network/tcp_client.h"
#include "util/json.h"

#include <mutex>
#include <atomic>

/*
 * 云迹 SC668s 设备驱动 —— 设备适配层实现
 *
 * 实现 IDeviceDriver 接口，通过 TCP 通信控制云迹机器人。
 * 通信协议: URL 字符串指令 + JSON 返回。
 *
 * 后续可新增其他厂家驱动（串口/CAN/USB HID）实现同一接口，
 * 框架无需改动。
 */
class YunjiDriver : public IDeviceDriver {
public:
    YunjiDriver();
    ~YunjiDriver() override;

    /* 连接/断开 */
    bool connect(const std::string& ip, int port) override;
    void disconnect() override;
    bool isConnected() const override;

    /* 移动控制 */
    bool moveToMarker(const std::string& marker) override;
    bool moveToLocation(double x, double y, double theta) override;
    bool cancelMove() override;

    /* 遥控 */
    bool joyControl(double linear, double angular) override;

    /* 急停 */
    bool estop(bool flag) override;

    /* 状态查询 */
    bool requestStatus() override;
    bool requestInfo() override;
    bool requestPower() override;
    bool requestVersion() override;
    bool requestMarkers() override;

    /* 实时数据订阅 */
    bool subscribeStatus(double freq_hz) override;
    bool unsubscribeStatus() override;

    /* 状态获取 */
    DeviceStatus getStatusSnapshot() override;

    /* 回调设置 */
    void setStatusCallback(StatusCallback cb) override   { status_cb_ = cb; }
    void setNotifCallback(NotifCallback cb) override     { notif_cb_ = cb; }
    void setConnCallback(ConnCallback cb) override       { conn_cb_ = cb; }

    /* 设备标识 */
    std::string getDeviceName() const override { return "Yunji SC668s"; }
    std::string getDeviceType() const override { return "yunji"; }

private:
    void onMessage(const JsonValue& msg);
    void onConnChanged(bool connected);
    bool sendApi(const std::string& path);

    /* 从 JSON 解析状态 */
    DeviceStatus parseStatus(const JsonValue& r);

    TcpClient          tcp_;
    StatusCallback     status_cb_;
    NotifCallback      notif_cb_;
    ConnCallback       conn_cb_;

    std::mutex         status_mutex_;
    DeviceStatus       last_status_;
    std::atomic<bool>  connected_{false};
};

#endif /* YUNJI_DRIVER_H */
