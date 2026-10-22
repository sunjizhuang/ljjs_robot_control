#ifndef INPUT_ADAPTER_H
#define INPUT_ADAPTER_H

#include <string>
#include <functional>

/*
 * 输入适配器模块 —— 指令解析与中间层封装 (上)
 *
 * 功能:
 * 1. WASD 协议解析：解析来自外部控制端的文本指令
 * 2. 扩展按键映射：支持 WASD + 功能键 + JSON 指令
 * 3. 协议版本兼容：同时支持单字符协议和 JSON 协议
 * 4. 设备感知：根据当前激活设备映射冲突按键
 *
 * 单字符协议按激活设备区分:
 *   云迹: w/s/a/d=遥控  x=急停  z=解除  c=取消  m=导航  l=点位  r=状态  p=电源  v=版本
 *   Lite3: w/s/a/d=移动  x=自主  z=站立  c=手动  y=移动模式  u=原地  g=打招呼  j=跳  1=扭  e=右转90  r=后转180  2=软急停
 *   通用: space=停止  h=帮助  q=退出  Tab=切换设备
 */

/* ── 统一指令结构 ── */
struct InputCommand {
    enum Type {
        /* ── 遥控移动 (通用) ── */
        KEY_W, KEY_S, KEY_A, KEY_D,
        KEY_STOP,                       /* 空格停止 */

        /* ── 云迹专用 ── */
        KEY_ESTOP, KEY_RELEASE_ESTOP,  /* 急停/解除 */
        KEY_CANCEL,                     /* 取消移动 */
        KEY_MOVE,                       /* 导航到点位 */
        KEY_MARKERS,                    /* 查询点位 */
        KEY_STATUS,                     /* 刷新状态 */
        KEY_POWER,                      /* 电源 */
        KEY_VERSION,                    /* 版本 */

        /* ── Lite3 专用 ── */
        KEY_STAND,                      /* z - 站立模式 */
        KEY_MOVE_MODE,                  /* y - 移动模式 */
        KEY_INPLACE_MODE,               /* u - 原地模式 */
        KEY_AUTO_MODE,                  /* x - 自主模式 */
        KEY_MANUAL_MODE,                /* c - 手动模式 */
        KEY_GREET,                      /* g - 打招呼 */
        KEY_JUMP,                       /* j - 跳 */
        KEY_TWIST,                      /* 1 - 扭身体 */
        KEY_TURN_LEFT_90,               /* n - 左转90° */
        KEY_TURN_RIGHT_90,              /* e - 右转90° */
        KEY_TURN_180,                   /* r - 后转180° */

        /* ── 通用 ── */
        KEY_HELP,                       /* 帮助 */
        KEY_QUIT,                       /* 退出 */
        KEY_SWITCH_DEVICE,              /* Tab - 切换激活设备 */

        /* ── 带参数指令 (JSON) ── */
        CMD_JOY,                        /* 直接遥控(带参数) */
        CMD_MOVE_LOCATION,              /* 导航到坐标 */
        CMD_SET_MODE,                   /* 设置模式 {"cmd":"mode","mode":"stand"} */
        CMD_ACTION,                     /* 执行动作 {"cmd":"action","action":"greet"} */
        CMD_AXIS,                       /* Lite3 轴指令 */
        CMD_GAIT,                       /* Lite3 步态 */
        CMD_SAVE_DATA,                  /* Lite3 保存数据 */
        CMD_CONTINUOUS_MOTION,          /* Lite3 持续运动 */
        CMD_VELOCITY,                   /* Lite3 自主速度 */
        CMD_RAW,                        /* 透传原始设备指令 */

        UNKNOWN
    };

    /* 设备目标 */
    enum DeviceTarget {
        DEV_ACTIVE,   /* 发往当前激活设备 (默认) */
        DEV_YUNJI,    /* 指定发往云迹 */
        DEV_LITE3,    /* 指定发往 Lite3 */
        DEV_ALL       /* 发往所有已连接设备 */
    };

    Type        type = UNKNOWN;
    DeviceTarget device_target = DEV_ACTIVE;

    std::string marker;         /* KEY_MOVE 目标点位 */
    double      linear  = 0;    /* CMD_JOY 线速度 */
    double      angular = 0;    /* CMD_JOY 角速度 */
    double      x = 0, y = 0, theta = 0;  /* CMD_MOVE_LOCATION */
    std::string mode;           /* CMD_SET_MODE 模式名 */
    std::string action;         /* CMD_ACTION 动作名 */
    std::string axis;           /* CMD_AXIS: x/y/yaw/roll/pitch/height */
    int axis_value = 0;         /* CMD_AXIS 原始 int */
    std::string gait;            /* CMD_GAIT */
    bool continuous = false;     /* CMD_CONTINUOUS_MOTION */
    std::string raw_cmd;        /* CMD_RAW 透传指令 */
    std::string source;         /* 来源标识 ("local" / "IP:PORT") */
};

class InputAdapter {
public:
    /* 解析输入文本为统一指令
     * device_type: "yunji" / "lite3" — 影响单字符按键映射
     */
    static InputCommand parse(const std::string& text,
                              const std::string& source = "remote",
                              const std::string& device_type = "yunji");

    /* 指令转可读字符串（用于日志） */
    static std::string toString(const InputCommand& cmd);

    /* 设备目标转字符串 */
    static std::string deviceTargetStr(InputCommand::DeviceTarget dt);

private:
    /* 单字符协议解析 (设备感知) */
    static InputCommand parseSingleChar(char c, const std::string& source,
                                        const std::string& device_type);

    /* JSON 协议解析 */
    static InputCommand parseJson(const std::string& json_str, const std::string& source);
};

#endif /* INPUT_ADAPTER_H */
