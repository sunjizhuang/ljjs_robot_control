#ifndef CONFIG_H
#define CONFIG_H

/*
 * 编译期固定常量 —— 协议指令码等不随部署变化的值。
 *
 * 运行时可配置项（IP/端口/速度限幅/频率等）全部移至
 * YAML 配置文件，见 yaml_config.h 中的 AppConfig。
 */

/* ===== Lite3 心跳指令码（协议固定） ===== */
#define LITE3_HEARTBEAT_CODE   0x21040001

/* ===== Lite3 模式/动作指令码（协议固定） ===== */
#define LITE3_CMD_STAND        0x21010202  /* 站立模式 */
#define LITE3_CMD_SOFT_ESTOP   0x21020C0E  /* 软急停 */
#define LITE3_CMD_MOVE_MODE    0x21010D06  /* 移动模式 */
#define LITE3_CMD_INPLACE      0x21010D05  /* 原地模式 */
#define LITE3_CMD_AUTO_MODE    0x21010C03  /* 自主模式 */
#define LITE3_CMD_MANUAL       0x21010C02  /* 手动模式 */
#define LITE3_CMD_GREET        0x21010507  /* 打招呼 */
#define LITE3_CMD_JUMP         0x2101050B  /* 跳 */
#define LITE3_CMD_TWIST        0x21010204  /* 扭身体 */

/* 1.2.3 轴指令 */
#define LITE3_AXIS_Y          0x21010131  /* 左右平移 / 横滚 */
#define LITE3_AXIS_X          0x21010130  /* 前后平移 / 俯仰 */
#define LITE3_AXIS_HEIGHT     0x21010102  /* 身体高度 */
#define LITE3_AXIS_YAW        0x21010135  /* 左右转 / 偏航 */

/* 1.2.5 步态切换 */
#define LITE3_GAIT_LOW        0x21010300
#define LITE3_GAIT_MEDIUM     0x21010307
#define LITE3_GAIT_HIGH       0x21010303
#define LITE3_GAIT_CRAWL      0x21010406
#define LITE3_GAIT_GRIP       0x21010402
#define LITE3_GAIT_GENERAL    0x21010401
#define LITE3_GAIT_HIGH_STEP  0x21010407

/* 1.2.8 / 1.2.9 */
#define LITE3_CMD_SAVE_DATA       0x21010C01
#define LITE3_CMD_CONTINUOUS      0x21010C06

#define LITE3_CMD_VOICE_BASE   0x21010C0A  /* 语音/移动指令基码 */
#define LITE3_VOICE_FORWARD    3
#define LITE3_VOICE_BACKWARD   4
#define LITE3_VOICE_LEFT       5
#define LITE3_VOICE_RIGHT      6
#define LITE3_VOICE_STOP       7
#define LITE3_VOICE_TURN_L90   13
#define LITE3_VOICE_TURN_R90   14
#define LITE3_VOICE_TURN_180   15

#endif /* CONFIG_H */
