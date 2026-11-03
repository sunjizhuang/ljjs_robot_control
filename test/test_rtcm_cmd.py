#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_rtcm_cmd.py —— RTCM 低延迟信令联调工具（PC 端, x86_64）

封装声网 SDK 预编译示例 hello_rtcm（example/out/x86_64/hello_rtcm）。
RTCM 路径特点: 与音视频同一 RTC 通道、延迟极低、单条 ≤1024B，
须在机器人 av_join 入频后使用（未入频请用 test_rtm_cmd.py）。

用法:
  # 发送模式: 加入机器人所在频道, 每 1s 向全频道广播一条指令 (Ctrl-C 停止)
  python3 test_rtcm_cmd.py -i <APP_ID> -c robot-av -u 2001 -p 1001 --cmd '{"cmd":"av_status"}'

  # 接收模式: 监听频道内 RTCM 消息 (观察机器人遥测/回执)
  python3 test_rtcm_cmd.py -i <APP_ID> -c robot-av -u 2001 --recv

uid 约定: 机器人=1001 (rtsa.uid), 操作员=2001 (任意, 不冲突即可)
"""
import argparse
import os
import subprocess
import sys

DEMO_DIR_CANDIDATES = [
    os.environ.get("AGORA_RTSA_EXAMPLE", ""),
    "../../shengwang/agora_rtsa_sdk/example",
    "/opt/agora_rtsa_sdk/example",
]


def find_hello_rtcm():
    for d in DEMO_DIR_CANDIDATES:
        if not d:
            continue
        for sub in ("out/x86_64", "out/aarch64"):
            p = os.path.join(d, sub, "hello_rtcm")
            if os.path.isfile(p):
                return p
    return None


def main():
    ap = argparse.ArgumentParser(description="RTCM 信令联调 (封装 hello_rtcm)")
    ap.add_argument("-i", "--app-id", required=True, help="声网 App ID")
    ap.add_argument("-c", "--channel", default="robot-av", help="机器人音视频频道 (rtsa.channel)")
    ap.add_argument("-u", "--uid", type=int, required=True, help="本端数字 uid")
    ap.add_argument("-p", "--peer", type=int, default=1001, help="机器人 uid (单播目标; 0=仅广播)")
    ap.add_argument("-t", "--token", default="", help="RTSA Token (Token 鉴权时必填)")
    ap.add_argument("--cmd", default="", help="要下发的指令 JSON (发送模式)")
    ap.add_argument("--recv", action="store_true", help="接收模式: 监听频道内 RTCM 消息")
    ap.add_argument("--demo-dir", default="", help="hello_rtcm 所在目录 (默认自动查找)")
    args = ap.parse_args()

    demo = os.path.join(args.demo_dir, "hello_rtcm") if args.demo_dir else find_hello_rtcm()
    if not demo or not os.path.isfile(demo):
        print("错误: 未找到 hello_rtcm 可执行文件。")
        print("  请通过 --demo-dir 或环境变量 AGORA_RTSA_EXAMPLE 指定 example/out/x86_64 目录")
        sys.exit(1)

    cmd = [demo, "-i", args.app_id, "-c", args.channel, "-u", str(args.uid),
           "-p", str(args.peer)]
    if args.token:
        cmd += ["-t", args.token]
    if args.recv:
        cmd += ["-r"]
    elif args.cmd:
        cmd += ["-s", args.cmd]
    else:
        print("错误: 需要指定 --cmd (发送) 或 --recv (接收)")
        sys.exit(1)

    mode = "接收" if args.recv else "发送"
    print("== RTCM %s模式: channel=%s uid=%d ==" % (mode, args.channel, args.uid))
    if args.cmd:
        print("   每 1s 广播+单播: %s" % args.cmd)
    try:
        rc = subprocess.call(cmd)
        sys.exit(rc)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
