#ifndef DEVICE_DRIVER_H
#define DEVICE_DRIVER_H

#include <string>
#include <functional>
#include <mutex>

/*
 * 设备适配层 —— 抽象接口 (多厂家兼容)
 *
 * 支持: 云迹 SC668s (TCP API) / 云深处 Lite3 (UDP 指令码) / 后续串口/CAN/USB HID
 * 新增虚方法均有默认实现，旧驱动无需修改即可兼容。
 */

/* ── 机器人状态 (通用) ── */
struct DeviceStatus {
    bool        valid        = false;
    std::string move_target;
    std::string move_status;      /* idle / running / succeeded / failed / canceled */
    std::string running_status;
    int         move_retry   = 0;
    bool        charging     = false;
    bool        estop        = false;
    bool        soft_estop   = false;
    bool        hard_estop   = false;
    int         power        = 0;
    double      pos_x        = 0;
    double      pos_y        = 0;
    double      pos_theta    = 0;
    int         floor        = 1;
    std::string mode;             /* 当前模式: stand/move/manual/auto/inplace... */
    std::string error_code;
};

/* ── 通知事件 (通用) ── */
struct DeviceNotification {
    std::string code;
    std::string level;
    std::string description;
    std::string data;
};

/* ── 设备驱动抽象接口 ── */
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() {}

    /* 连接/断开 */
    virtual bool connect(const std::string& ip, int port) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    /* ── 移动控制 ── */
    virtual bool moveToMarker(const std::string& marker) = 0;
    virtual bool moveToLocation(double x, double y, double theta) = 0;
    virtual bool cancelMove() = 0;

    /* ── 直接遥控 ── */
    virtual bool joyControl(double linear, double angular) = 0;

    /* ── 急停 ── */
    virtual bool estop(bool flag) = 0;

    /* ── 状态查询 ── */
    virtual bool requestStatus() = 0;
    virtual bool requestInfo() = 0;
    virtual bool requestPower() = 0;
    virtual bool requestVersion() = 0;
    virtual bool requestMarkers() = 0;

    /* ── 实时数据订阅 ── */
    virtual bool subscribeStatus(double freq_hz) = 0;
    virtual bool unsubscribeStatus() = 0;

    /* ── 状态获取 ── */
    virtual DeviceStatus getStatusSnapshot() = 0;

    /* ── 回调设置 ── */
    typedef std::function<void(const DeviceStatus&)>      StatusCallback;
    typedef std::function<void(const DeviceNotification&)> NotifCallback;
    typedef std::function<void(bool)>                      ConnCallback;

    virtual void setStatusCallback(StatusCallback cb) = 0;
    virtual void setNotifCallback(NotifCallback cb) = 0;
    virtual void setConnCallback(ConnCallback cb) = 0;

    /* ════ 新增: 设备标识 (默认实现) ════ */
    virtual std::string getDeviceName() const { return "unknown"; }
    virtual std::string getDeviceType() const { return "unknown"; }

    /* ════ 新增: 模式切换 (Lite3: stand/move/manual/auto/inplace) ════ */
    virtual bool setMode(const std::string& mode) { (void)mode; return false; }

    /* ════ 新增: 执行特殊动作 ════ */
    /* Lite3: greet/jump/twist/forward/backward/strafe_left/strafe_right/
     *        turn_left_90/turn_right_90/turn_180/stop */
    virtual bool performAction(const std::string& action) { (void)action; return false; }

    /* ════ Lite3 扩展控制接口（其它设备保持默认 false） ════ */
    virtual bool setAxis(const std::string& axis, int value) { (void)axis; (void)value; return false; }
    virtual bool setGait(const std::string& gait) { (void)gait; return false; }
    virtual bool saveData() { return false; }
    virtual bool setContinuousMotion(bool enabled) { (void)enabled; return false; }
    virtual bool setVelocity(double vx, double vy, double yaw) {
        (void)vx; (void)vy; (void)yaw; return false;
    }

    /* ════ 新增: 心跳 (Lite3 需要 20ms 周期发送) ════ */
    virtual void heartbeat() {}
};

#endif /* DEVICE_DRIVER_H */
