#include "cmd_translator.h"
#include "config/yaml_config.h"
#include "util/log.h"

#include <cstdio>

CmdTranslator::CmdTranslator() {}
CmdTranslator::~CmdTranslator() {}

/* ════ 多设备管理 ════ */

void CmdTranslator::addDevice(const std::string& name, IDeviceDriver* device)
{
    if (!device) return;
    devices_[name] = device;
    if (!active_device_) {
        active_device_ = device;
        active_device_name_ = name;
    }
}

void CmdTranslator::setActiveDevice(const std::string& name)
{
    auto it = devices_.find(name);
    if (it != devices_.end()) {
        /* 切换前停止旧设备遥控 */
        if (active_device_ && joy_active_.load()) {
            resetVelocity();
            if (active_device_->isConnected()) {
                active_device_->joyControl(0, 0);
            }
        }
        active_device_ = it->second;
        active_device_name_ = name;
        LOG_INFO("激活设备切换至: %s", name.c_str());
    } else {
        LOG_WARN("设备不存在: %s", name.c_str());
    }
}

IDeviceDriver* CmdTranslator::getActiveDevice()
{
    return active_device_;
}

IDeviceDriver* CmdTranslator::getDevice(const std::string& name)
{
    auto it = devices_.find(name);
    return (it != devices_.end()) ? it->second : nullptr;
}

std::string CmdTranslator::getActiveDeviceName() const
{
    return active_device_name_;
}

std::string CmdTranslator::getActiveDeviceType() const
{
    if (active_device_) return active_device_->getDeviceType();
    return "none";
}

std::vector<std::string> CmdTranslator::getDeviceNames() const
{
    std::vector<std::string> names;
    for (auto& kv : devices_) names.push_back(kv.first);
    return names;
}

std::string CmdTranslator::switchToNextDevice()
{
    if (devices_.size() <= 1) return active_device_name_;

    std::vector<std::string> names;
    for (auto& kv : devices_) names.push_back(kv.first);

    size_t idx = 0;
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == active_device_name_) { idx = i; break; }
    }
    idx = (idx + 1) % names.size();
    setActiveDevice(names[idx]);
    return names[idx];
}

/* ════ 设备选择 ════ */

IDeviceDriver* CmdTranslator::selectDevice(const InputCommand& cmd)
{
    switch (cmd.device_target) {
    case InputCommand::DEV_YUNJI: {
        auto it = devices_.find("yunji");
        return (it != devices_.end()) ? it->second : nullptr;
    }
    case InputCommand::DEV_LITE3: {
        auto it = devices_.find("lite3");
        return (it != devices_.end()) ? it->second : nullptr;
    }
    case InputCommand::DEV_ALL:
        return active_device_;  /* ALL 在 execute() 中特殊处理 */
    default:
        return active_device_;
    }
}

/* ════ 速度管理 (云迹) ════ */

void CmdTranslator::incLinear()
{
    double v = linear_vel_.load() + g_config.linear_step;
    if (v > g_config.max_linear) v = g_config.max_linear;
    linear_vel_.store(v);
    joy_active_ = true;
}

void CmdTranslator::decLinear()
{
    double v = linear_vel_.load() - g_config.linear_step;
    if (v < -g_config.max_linear) v = -g_config.max_linear;
    linear_vel_.store(v);
    joy_active_ = true;
}

void CmdTranslator::incAngular()
{
    double v = angular_vel_.load() + g_config.angular_step;
    if (v > g_config.max_angular) v = g_config.max_angular;
    angular_vel_.store(v);
    joy_active_ = true;
}

void CmdTranslator::decAngular()
{
    double v = angular_vel_.load() - g_config.angular_step;
    if (v < -g_config.max_angular) v = -g_config.max_angular;
    angular_vel_.store(v);
    joy_active_ = true;
}

void CmdTranslator::resetVelocity()
{
    linear_vel_.store(0);
    angular_vel_.store(0);
    joy_active_ = false;
}

/* ════ 执行入口 ════ */

bool CmdTranslator::execute(const InputCommand& cmd)
{
    /* 设备切换 */
    if (cmd.type == InputCommand::KEY_SWITCH_DEVICE) {
        std::string next = switchToNextDevice();
        LOG_INFO("[%s] 切换设备 → %s", cmd.source.c_str(), next.c_str());
        return true;
    }

    /* 帮助 */
    if (cmd.type == InputCommand::KEY_HELP) {
        LOG_INFO("=== 帮助 (当前设备: %s) ===", active_device_name_.c_str());
        if (getActiveDeviceType() == "lite3") {
            LOG_INFO("WASD: 前进/后退/左平移/右平移 (离散动作)");
            LOG_INFO("Space/F: 停止 | 2: 软急停 | Z: 站立 | Y: 移动模式");
            LOG_INFO("U: 原地模式 | X: 自主模式 | C: 手动模式");
            LOG_INFO("G: 打招呼 | J: 跳 | 1: 扭身体");
            LOG_INFO("JSON: {\"cmd\":\"axis\",\"axis\":\"x\",\"value\":3000}");
            LOG_INFO("JSON: {\"cmd\":\"gait\",\"gait\":\"medium\"}");
            LOG_INFO("JSON: {\"cmd\":\"velocity\",\"vx\":0.3,\"vy\":0,\"yaw\":0.1}");
            LOG_INFO("JSON: {\"cmd\":\"continuous_motion\",\"enabled\":true}");
            LOG_INFO("JSON: {\"cmd\":\"save_data\"}");
            LOG_INFO("N: 左转90 | E: 右转90 | R: 后转180");
        } else {
            LOG_INFO("WASD: 遥控移动 (W前 S后 A左 D右, 递增速度)");
            LOG_INFO("Space: 停止 | X: 急停 | Z: 解除急停");
            LOG_INFO("C: 取消移动 | M: 导航 | L: 点位列表");
            LOG_INFO("R: 状态 | P: 电源 | V: 版本");
        }
        LOG_INFO("Tab: 切换设备 | Q: 退出 | H: 帮助");
        LOG_INFO("JSON: {\"cmd\":\"mode\",\"mode\":\"stand\",\"device\":\"lite3\"}");
        LOG_INFO("JSON: {\"cmd\":\"action\",\"action\":\"greet\",\"device\":\"lite3\"}");
        return true;
    }

    /* 退出 */
    if (cmd.type == InputCommand::KEY_QUIT) {
        resetVelocity();
        if (active_device_ && active_device_->isConnected()) {
            active_device_->joyControl(0, 0);
        }
        LOG_INFO("[%s] 请求退出", cmd.source.c_str());
        return true;
    }

    /* ALL 目标: 发送给所有已连接设备 */
    if (cmd.device_target == InputCommand::DEV_ALL) {
        bool ok = false;
        for (auto& kv : devices_) {
            if (kv.second->isConnected()) {
                std::string dtype = kv.second->getDeviceType();
                if (dtype == "yunji")  executeYunji(kv.second, cmd);
                else if (dtype == "lite3") executeLite3(kv.second, cmd);
                else executeCommon(kv.second, cmd);
                ok = true;
            }
        }
        return ok;
    }

    /* 单设备目标 */
    IDeviceDriver* dev = selectDevice(cmd);
    if (!dev) {
        LOG_ERR("目标设备不可用");
        return false;
    }
    if (!dev->isConnected()) {
        LOG_WARN("[%s] 设备 %s 未连接", cmd.source.c_str(), dev->getDeviceName().c_str());
        return false;
    }

    std::string dtype = dev->getDeviceType();
    if (dtype == "yunji")       return executeYunji(dev, cmd);
    else if (dtype == "lite3")  return executeLite3(dev, cmd);
    else                        return executeCommon(dev, cmd);
}

/* ════ 云迹指令执行 ════ */

bool CmdTranslator::executeYunji(IDeviceDriver* dev, const InputCommand& cmd)
{
    switch (cmd.type) {
    /* 遥控速度递增 */
    case InputCommand::KEY_W:    incLinear();  break;
    case InputCommand::KEY_S:    decLinear();  break;
    case InputCommand::KEY_A:    incAngular(); break;
    case InputCommand::KEY_D:    decAngular(); break;

    /* 停止 */
    case InputCommand::KEY_STOP:
        resetVelocity();
        dev->joyControl(0, 0);
        LOG_INFO("[%s] %s: 停止", cmd.source.c_str(), dev->getDeviceName().c_str());
        break;

    /* 急停 */
    case InputCommand::KEY_ESTOP:
        resetVelocity();
        dev->estop(true);
        LOG_INFO("[%s] %s: 急停", cmd.source.c_str(), dev->getDeviceName().c_str());
        break;
    case InputCommand::KEY_RELEASE_ESTOP:
        dev->estop(false);
        LOG_INFO("[%s] %s: 解除急停", cmd.source.c_str(), dev->getDeviceName().c_str());
        break;

    /* 取消移动 */
    case InputCommand::KEY_CANCEL:
        dev->cancelMove();
        LOG_INFO("[%s] %s: 取消移动", cmd.source.c_str(), dev->getDeviceName().c_str());
        break;

    /* 导航 */
    case InputCommand::KEY_MOVE:
        if (!cmd.marker.empty()) {
            dev->moveToMarker(cmd.marker);
            LOG_INFO("[%s] %s: 导航→%s", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.marker.c_str());
        }
        break;
    case InputCommand::CMD_MOVE_LOCATION:
        dev->moveToLocation(cmd.x, cmd.y, cmd.theta);
        LOG_INFO("[%s] %s: 导航→(%.2f,%.2f,%.3f)", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.x, cmd.y, cmd.theta);
        break;

    /* 直接遥控 */
    case InputCommand::CMD_JOY:
        linear_vel_.store(cmd.linear);
        angular_vel_.store(cmd.angular);
        joy_active_ = true;
        LOG_INFO("[%s] %s: 遥控 lin=%.3f ang=%.3f", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.linear, cmd.angular);
        break;

    /* 查询 */
    case InputCommand::KEY_MARKERS: dev->requestMarkers(); break;
    case InputCommand::KEY_STATUS:  dev->requestStatus();  break;
    case InputCommand::KEY_POWER:   dev->requestPower();   break;
    case InputCommand::KEY_VERSION: dev->requestVersion(); break;

    default:
        return executeCommon(dev, cmd);
    }
    return true;
}

/* ════ Lite3 指令执行 ════ */

bool CmdTranslator::executeLite3(IDeviceDriver* dev, const InputCommand& cmd)
{
    switch (cmd.type) {
    /* 移动 (离散动作，非速度递增) */
    case InputCommand::KEY_W:            dev->performAction("forward");      break;
    case InputCommand::KEY_S:            dev->performAction("backward");     break;
    case InputCommand::KEY_A:            dev->performAction("strafe_left");   break;
    case InputCommand::KEY_D:            dev->performAction("strafe_right");  break;
    case InputCommand::KEY_STOP:
        dev->performAction("stop");
        dev->setAxis("x", 0);
        dev->setAxis("y", 0);
        dev->setAxis("yaw", 0);
        break;

    /* 急停 */
    case InputCommand::KEY_ESTOP:
        dev->estop(true);
        LOG_INFO("[%s] %s: 软急停", cmd.source.c_str(), dev->getDeviceName().c_str());
        break;

    /* 模式切换 */
    case InputCommand::KEY_STAND:        dev->setMode("stand");      break;
    case InputCommand::KEY_MOVE_MODE:    dev->setMode("move");       break;
    case InputCommand::KEY_INPLACE_MODE: dev->setMode("inplace");    break;
    case InputCommand::KEY_AUTO_MODE:    dev->setMode("auto");       break;
    case InputCommand::KEY_MANUAL_MODE:  dev->setMode("manual");     break;

    /* 新增 Lite3 协议功能 */
    case InputCommand::CMD_AXIS:
        if (!dev->setAxis(cmd.axis, cmd.axis_value)) return false;
        break;
    case InputCommand::CMD_GAIT:
        if (!dev->setGait(cmd.gait)) return false;
        break;
    case InputCommand::CMD_SAVE_DATA:
        if (!dev->saveData()) return false;
        break;
    case InputCommand::CMD_CONTINUOUS_MOTION:
        if (!dev->setContinuousMotion(cmd.continuous)) return false;
        break;
    case InputCommand::CMD_VELOCITY:
        if (!dev->setVelocity(cmd.linear, cmd.y, cmd.angular)) return false;
        break;

    /* 特殊动作 */
    case InputCommand::KEY_GREET:        dev->performAction("greet");        break;
    case InputCommand::KEY_JUMP:         dev->performAction("jump");         break;
    case InputCommand::KEY_TWIST:        dev->performAction("twist");        break;
    case InputCommand::KEY_TURN_LEFT_90: dev->performAction("turn_left_90"); break;
    case InputCommand::KEY_TURN_RIGHT_90:dev->performAction("turn_right_90");break;
    case InputCommand::KEY_TURN_180:     dev->performAction("turn_180");     break;

    /* 带参数指令 */
    case InputCommand::CMD_SET_MODE:     dev->setMode(cmd.mode);     break;
    case InputCommand::CMD_ACTION:       dev->performAction(cmd.action); break;

    default:
        return executeCommon(dev, cmd);
    }

    LOG_INFO("[%s] %s: %s", cmd.source.c_str(), dev->getDeviceName().c_str(),
             InputAdapter::toString(cmd).c_str());
    return true;
}

/* ════ 通用指令执行 (两个设备都支持的) ════ */

bool CmdTranslator::executeCommon(IDeviceDriver* dev, const InputCommand& cmd)
{
    switch (cmd.type) {
    case InputCommand::CMD_RAW:
        /* 透传指令 — 设备自行解释 */
        LOG_INFO("[%s] %s: RAW: %s", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.raw_cmd.c_str());
        break;

    case InputCommand::CMD_SET_MODE:
        dev->setMode(cmd.mode);
        LOG_INFO("[%s] %s: 设置模式 %s", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.mode.c_str());
        break;

    case InputCommand::CMD_ACTION:
        dev->performAction(cmd.action);
        LOG_INFO("[%s] %s: 执行动作 %s", cmd.source.c_str(), dev->getDeviceName().c_str(), cmd.action.c_str());
        break;

    default:
        LOG_WARN("[%s] %s: 不支持的指令: %s", cmd.source.c_str(), dev->getDeviceName().c_str(),
                 InputAdapter::toString(cmd).c_str());
        return false;
    }
    return true;
}
