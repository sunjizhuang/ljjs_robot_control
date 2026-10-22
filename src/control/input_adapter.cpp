#include "input_adapter.h"
#include "util/json.h"
#include "util/log.h"

#include <cctype>

InputCommand InputAdapter::parse(const std::string& text,
                                 const std::string& source,
                                 const std::string& device_type)
{
    /* 去除首尾空白 */
    std::string trimmed;
    size_t start = 0, end = text.size();
    while (start < end && std::isspace((unsigned char)text[start])) start++;
    while (end > start && std::isspace((unsigned char)text[end - 1])) end--;
    trimmed = text.substr(start, end - start);

    if (trimmed.empty()) {
        InputCommand cmd;
        cmd.source = source;
        return cmd;
    }

    /* Tab 字符 → 切换设备 */
    if (trimmed[0] == '\t') {
        InputCommand cmd;
        cmd.type = InputCommand::KEY_SWITCH_DEVICE;
        cmd.source = source;
        return cmd;
    }

    /* JSON 协议: 以 { 开头 */
    if (trimmed[0] == '{') {
        return parseJson(trimmed, source);
    }

    /* 单字符协议: 取第一个字符，按设备类型映射 */
    return parseSingleChar(trimmed[0], source, device_type);
}

InputCommand InputAdapter::parseSingleChar(char c, const std::string& source,
                                           const std::string& device_type)
{
    InputCommand cmd;
    cmd.source = source;

    /* ── 通用按键 (两个设备一致) ── */
    switch (c) {
    case 'w': case 'W': cmd.type = InputCommand::KEY_W;       return cmd;
    case 's': case 'S': cmd.type = InputCommand::KEY_S;       return cmd;
    case ' ':           cmd.type = InputCommand::KEY_STOP;     return cmd;
    case 'h': case 'H': cmd.type = InputCommand::KEY_HELP;    return cmd;
    case 'q': case 'Q': cmd.type = InputCommand::KEY_QUIT;     return cmd;
    default: break;
    }

    /* ── 设备感知按键 ── */
    if (device_type == "lite3") {
        /* Lite3 按键映射 */
        switch (c) {
        case 'a': case 'A': cmd.type = InputCommand::KEY_A;             break;  /* 左平移 */
        case 'd': case 'D': cmd.type = InputCommand::KEY_D;             break;  /* 右平移 */
        case 'f': case 'F': cmd.type = InputCommand::KEY_STOP;          break;  /* 停止 */
        case 'z': case 'Z': cmd.type = InputCommand::KEY_STAND;         break;  /* 站立 */
        case '2':           cmd.type = InputCommand::KEY_ESTOP;         break;  /* 软急停 */
        case 'y': case 'Y': cmd.type = InputCommand::KEY_MOVE_MODE;     break;  /* 移动模式 */
        case 'u': case 'U': cmd.type = InputCommand::KEY_INPLACE_MODE;  break;  /* 原地模式 */
        case 'x': case 'X': cmd.type = InputCommand::KEY_AUTO_MODE;     break;  /* 自主模式 */
        case 'c': case 'C': cmd.type = InputCommand::KEY_MANUAL_MODE;   break;  /* 手动模式 */
        case 'g': case 'G': cmd.type = InputCommand::KEY_GREET;         break;  /* 打招呼 */
        case 'j': case 'J': cmd.type = InputCommand::KEY_JUMP;          break;  /* 跳 */
        case '1':           cmd.type = InputCommand::KEY_TWIST;         break;  /* 扭身体 */
        case 'n': case 'N': cmd.type = InputCommand::KEY_TURN_LEFT_90;  break;  /* 左转90 */
        case 'e': case 'E': cmd.type = InputCommand::KEY_TURN_RIGHT_90; break;  /* 右转90 */
        case 'r': case 'R': cmd.type = InputCommand::KEY_TURN_180;      break;  /* 后转180 */
        default:            cmd.type = InputCommand::UNKNOWN;           break;
        }
    } else {
        /* 云迹按键映射 (默认) */
        switch (c) {
        case 'a': case 'A': cmd.type = InputCommand::KEY_A;             break;  /* 左转 */
        case 'd': case 'D': cmd.type = InputCommand::KEY_D;             break;  /* 右转 */
        case 'x': case 'X': cmd.type = InputCommand::KEY_ESTOP;         break;  /* 急停 */
        case 'z': case 'Z': cmd.type = InputCommand::KEY_RELEASE_ESTOP; break;  /* 解除急停 */
        case 'c': case 'C': cmd.type = InputCommand::KEY_CANCEL;        break;  /* 取消移动 */
        case 'm': case 'M': cmd.type = InputCommand::KEY_MOVE;          break;  /* 导航 */
        case 'l': case 'L': cmd.type = InputCommand::KEY_MARKERS;       break;  /* 点位列表 */
        case 'r': case 'R': cmd.type = InputCommand::KEY_STATUS;        break;  /* 刷新状态 */
        case 'p': case 'P': cmd.type = InputCommand::KEY_POWER;         break;  /* 电源 */
        case 'v': case 'V': cmd.type = InputCommand::KEY_VERSION;       break;  /* 版本 */
        default:            cmd.type = InputCommand::UNKNOWN;           break;
        }
    }

    return cmd;
}

InputCommand InputAdapter::parseJson(const std::string& json_str, const std::string& source)
{
    InputCommand cmd;
    cmd.source = source;

    JsonValue val;
    if (!json_parse(json_str, val) || !val.isObject()) {
        LOG_WARN("JSON 指令解析失败: %s", json_str.substr(0, 80).c_str());
        return cmd;
    }

    std::string action = val["cmd"].asString();
    if (action.empty()) action = val["action"].asString();

    /* 设备目标 */
    std::string dev = val["device"].asString();
    if (dev == "yunji")       cmd.device_target = InputCommand::DEV_YUNJI;
    else if (dev == "lite3")  cmd.device_target = InputCommand::DEV_LITE3;
    else if (dev == "all")    cmd.device_target = InputCommand::DEV_ALL;
    else                      cmd.device_target = InputCommand::DEV_ACTIVE;

    if (action == "move") {
        cmd.marker = val["marker"].asString();
        if (!cmd.marker.empty()) {
            cmd.type = InputCommand::KEY_MOVE;
        } else if (val.has("x") && val.has("y")) {
            cmd.type = InputCommand::CMD_MOVE_LOCATION;
            cmd.x     = val["x"].asNumber();
            cmd.y     = val["y"].asNumber();
            cmd.theta = val["theta"].asNumber();
        }
    } else if (action == "joy") {
        cmd.type    = InputCommand::CMD_JOY;
        cmd.linear  = val["linear"].asNumber();
        cmd.angular = val["angular"].asNumber();
    } else if (action == "estop") {
        cmd.type = val["flag"].asBool() ? InputCommand::KEY_ESTOP : InputCommand::KEY_RELEASE_ESTOP;
    } else if (action == "cancel") {
        cmd.type = InputCommand::KEY_CANCEL;
    } else if (action == "status") {
        cmd.type = InputCommand::KEY_STATUS;
    } else if (action == "markers") {
        cmd.type = InputCommand::KEY_MARKERS;
    } else if (action == "power") {
        cmd.type = InputCommand::KEY_POWER;
    } else if (action == "version") {
        cmd.type = InputCommand::KEY_VERSION;
    } else if (action == "mode") {
        cmd.type = InputCommand::CMD_SET_MODE;
        cmd.mode = val["mode"].asString();
    } else if (action == "action") {
        cmd.type   = InputCommand::CMD_ACTION;
        cmd.action = val["action"].asString();
        /* 快捷映射: action 名 → 对应按键类型 */
        if (cmd.action == "stand")         cmd.type = InputCommand::KEY_STAND;
        else if (cmd.action == "move_mode")cmd.type = InputCommand::KEY_MOVE_MODE;
        else if (cmd.action == "inplace")  cmd.type = InputCommand::KEY_INPLACE_MODE;
        else if (cmd.action == "auto")     cmd.type = InputCommand::KEY_AUTO_MODE;
        else if (cmd.action == "manual")   cmd.type = InputCommand::KEY_MANUAL_MODE;
        else if (cmd.action == "greet")    cmd.type = InputCommand::KEY_GREET;
        else if (cmd.action == "jump")     cmd.type = InputCommand::KEY_JUMP;
        else if (cmd.action == "twist")    cmd.type = InputCommand::KEY_TWIST;
        else if (cmd.action == "forward")  cmd.type = InputCommand::KEY_W;
        else if (cmd.action == "backward") cmd.type = InputCommand::KEY_S;
        else if (cmd.action == "strafe_left")  cmd.type = InputCommand::KEY_A;
        else if (cmd.action == "strafe_right") cmd.type = InputCommand::KEY_D;
        else if (cmd.action == "turn_left_90") cmd.type = InputCommand::KEY_TURN_LEFT_90;
        else if (cmd.action == "turn_right_90")cmd.type = InputCommand::KEY_TURN_RIGHT_90;
        else if (cmd.action == "turn_180")     cmd.type = InputCommand::KEY_TURN_180;
        else if (cmd.action == "stop")         cmd.type = InputCommand::KEY_STOP;
    } else if (action == "axis") {
        cmd.type = InputCommand::CMD_AXIS;
        cmd.axis = val["axis"].asString();
        cmd.axis_value = val["value"].asInt();
    } else if (action == "gait") {
        cmd.type = InputCommand::CMD_GAIT;
        cmd.gait = val["gait"].asString();
    } else if (action == "save_data" || action == "save") {
        cmd.type = InputCommand::CMD_SAVE_DATA;
    } else if (action == "continuous_motion" || action == "continuous") {
        cmd.type = InputCommand::CMD_CONTINUOUS_MOTION;
        cmd.continuous = val.has("enabled") ? val["enabled"].asBool() : (val["value"].asInt() == 1);
    } else if (action == "velocity" || action == "speed") {
        cmd.type = InputCommand::CMD_VELOCITY;
        cmd.linear = val.has("vx") ? val["vx"].asNumber() : val["linear"].asNumber();
        cmd.y = val.has("vy") ? val["vy"].asNumber() : val["lateral"].asNumber();
        cmd.angular = val.has("yaw") ? val["yaw"].asNumber() : val["angular"].asNumber();
    } else if (action == "raw") {
        cmd.type    = InputCommand::CMD_RAW;
        cmd.raw_cmd = val["data"].asString();
    } else if (action == "help") {
        cmd.type = InputCommand::KEY_HELP;
    } else if (action == "quit" || action == "exit") {
        cmd.type = InputCommand::KEY_QUIT;
    } else if (action == "switch_device") {
        cmd.type = InputCommand::KEY_SWITCH_DEVICE;
    }

    return cmd;
}

std::string InputAdapter::deviceTargetStr(InputCommand::DeviceTarget dt)
{
    switch (dt) {
    case InputCommand::DEV_YUNJI:  return "yunji";
    case InputCommand::DEV_LITE3:  return "lite3";
    case InputCommand::DEV_ALL:    return "all";
    default:                       return "active";
    }
}

std::string InputAdapter::toString(const InputCommand& cmd)
{
    switch (cmd.type) {
    /* 通用移动 */
    case InputCommand::KEY_W:             return "W (前进)";
    case InputCommand::KEY_S:             return "S (后退)";
    case InputCommand::KEY_A:             return "A (左)";
    case InputCommand::KEY_D:             return "D (右)";
    case InputCommand::KEY_STOP:          return "STOP (停止)";

    /* 云迹 */
    case InputCommand::KEY_ESTOP:         return "ESTOP (急停)";
    case InputCommand::KEY_RELEASE_ESTOP: return "RELEASE (解除急停)";
    case InputCommand::KEY_CANCEL:        return "CANCEL (取消移动)";
    case InputCommand::KEY_MOVE:          return "MOVE -> " + cmd.marker;
    case InputCommand::KEY_MARKERS:       return "MARKERS (查询点位)";
    case InputCommand::KEY_STATUS:        return "STATUS (刷新状态)";
    case InputCommand::KEY_POWER:         return "POWER (电源)";
    case InputCommand::KEY_VERSION:       return "VERSION (版本)";

    /* Lite3 */
    case InputCommand::KEY_STAND:         return "STAND (站立模式)";
    case InputCommand::KEY_MOVE_MODE:     return "MOVE_MODE (移动模式)";
    case InputCommand::KEY_INPLACE_MODE:  return "INPLACE (原地模式)";
    case InputCommand::KEY_AUTO_MODE:     return "AUTO (自主模式)";
    case InputCommand::KEY_MANUAL_MODE:   return "MANUAL (手动模式)";
    case InputCommand::KEY_GREET:         return "GREET (打招呼)";
    case InputCommand::KEY_JUMP:          return "JUMP (跳)";
    case InputCommand::KEY_TWIST:         return "TWIST (扭身体)";
    case InputCommand::KEY_TURN_LEFT_90:  return "TURN_L90 (左转90°)";
    case InputCommand::KEY_TURN_RIGHT_90: return "TURN_R90 (右转90°)";
    case InputCommand::KEY_TURN_180:      return "TURN_180 (后转180°)";

    /* 通用 */
    case InputCommand::KEY_HELP:          return "HELP (帮助)";
    case InputCommand::KEY_QUIT:          return "QUIT (退出)";
    case InputCommand::KEY_SWITCH_DEVICE: return "SWITCH_DEVICE (切换设备)";

    /* 带参数 */
    case InputCommand::CMD_JOY: {
        char b[128];
        snprintf(b, sizeof(b), "JOY(linear=%.3f, angular=%.3f)", cmd.linear, cmd.angular);
        return std::string(b);
    }
    case InputCommand::CMD_MOVE_LOCATION: {
        char b[128];
        snprintf(b, sizeof(b), "MOVE_LOC(%.2f, %.2f, %.3f)", cmd.x, cmd.y, cmd.theta);
        return std::string(b);
    }
    case InputCommand::CMD_SET_MODE:      return "SET_MODE(" + cmd.mode + ")";
    case InputCommand::CMD_ACTION:        return "ACTION(" + cmd.action + ")";
    case InputCommand::CMD_AXIS: {
        char b[128]; snprintf(b, sizeof(b), "AXIS(%s=%d)", cmd.axis.c_str(), cmd.axis_value); return std::string(b);
    }
    case InputCommand::CMD_GAIT:          return "GAIT(" + cmd.gait + ")";
    case InputCommand::CMD_SAVE_DATA:     return "SAVE_DATA";
    case InputCommand::CMD_CONTINUOUS_MOTION: return cmd.continuous ? "CONTINUOUS(ON)" : "CONTINUOUS(OFF)";
    case InputCommand::CMD_VELOCITY: {
        char b[160]; snprintf(b, sizeof(b), "VELOCITY(vx=%.3f,vy=%.3f,yaw=%.3f)", cmd.linear, cmd.y, cmd.angular); return std::string(b);
    }
    case InputCommand::CMD_RAW:           return "RAW: " + cmd.raw_cmd;
    default:                              return "UNKNOWN";
    }
}
