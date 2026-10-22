#ifndef CMD_TRANSLATOR_H
#define CMD_TRANSLATOR_H

#include <string>
#include <atomic>
#include <map>
#include <vector>
#include "input_adapter.h"
#include "device/device_driver.h"
#include "config/config.h"

/*
 * 指令翻译引擎 —— 指令解析与中间层封装 (下)
 *
 * 功能:
 * 1. 多设备管理: 持有多个设备驱动，支持运行时切换激活设备
 * 2. 设备感知执行: 根据激活设备类型翻译同一指令为不同设备操作
 *    - 云迹: WASD → 速度递增 → joy_control
 *    - Lite3: WASD → 离散移动指令 (forward/backward/strafe_left/strafe_right)
 * 3. 速度状态管理: 仅对云迹有效，joy_control 以固定频率发送
 *
 * 设备切换:
 * - Tab 键切换激活设备
 * - JSON {"cmd":"switch_device"} 远程切换
 * - JSON {"device":"lite3"} 指定目标设备
 */
class CmdTranslator {
public:
    CmdTranslator();
    ~CmdTranslator();

    /* ── 多设备管理 ── */
    void addDevice(const std::string& name, IDeviceDriver* device);
    void setActiveDevice(const std::string& name);
    IDeviceDriver* getActiveDevice();
    std::string getActiveDeviceName() const;
    std::string getActiveDeviceType() const;
    std::vector<std::string> getDeviceNames() const;

    /* 切换到下一个设备 */
    std::string switchToNextDevice();

    /* 按名称获取设备 */
    IDeviceDriver* getDevice(const std::string& name);

    /* 执行翻译后的指令 */
    bool execute(const InputCommand& cmd);

    /* ── 速度状态管理 (供 joy_control 线程使用，仅云迹) ── */
    double getLinear()  const { return linear_vel_.load(); }
    double getAngular() const { return angular_vel_.load(); }
    bool   isJoyActive() const { return joy_active_.load(); }
    void   stopJoy()    { joy_active_ = false; }

    /* 重置速度 */
    void resetVelocity();

private:
    /* 根据指令的 device_target 选择设备 */
    IDeviceDriver* selectDevice(const InputCommand& cmd);

    /* 云迹速度递增/递减 */
    void incLinear();
    void decLinear();
    void incAngular();
    void decAngular();

    /* 执行云迹专用指令 */
    bool executeYunji(IDeviceDriver* dev, const InputCommand& cmd);

    /* 执行 Lite3 专用指令 */
    bool executeLite3(IDeviceDriver* dev, const InputCommand& cmd);

    /* 执行通用指令 */
    bool executeCommon(IDeviceDriver* dev, const InputCommand& cmd);

    std::map<std::string, IDeviceDriver*> devices_;
    std::string  active_device_name_;
    IDeviceDriver* active_device_ = nullptr;

    std::atomic<bool>   joy_active_{false};
    std::atomic<double> linear_vel_{0.0};
    std::atomic<double> angular_vel_{0.0};
};

#endif /* CMD_TRANSLATOR_H */
