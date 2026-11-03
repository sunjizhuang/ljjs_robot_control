#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_rtm_cmd.py —— RTM 信令联调工具（PC 端, x86_64）

封装声网 SDK 预编译示例 hello_rtm（example/out/x86_64/hello_rtm），
向机器人 RTM uid 发送与 TCP/MQTT 完全同格式的 JSON 控制指令。

用法:
  # 交互模式 (每行一条指令, Ctrl-C 退出)
  python3 test_rtm_cmd.py -i <APP_ID> -p <机器人rtm_uid, 默认 robot-1>

  # 单条指令
  python3 test_rtm_cmd.py -i <APP_ID> -p robot-1 --cmd '{"cmd":"mode","mode":"stand","device":"lite3"}'

  # 批量指令 (文件中每行一条 JSON, 逐行下发)
  python3 test_rtm_cmd.py -i <APP_ID> -p robot-1 --file cmds.txt

常用指令:
  {"cmd":"av_status"}
  {"cmd":"av_join"}
  {"cmd":"av_leave"}
  {"cmd":"mode","mode":"stand","device":"lite3"}
  {"cmd":"axis","axis":"x","value":3000,"device":"lite3"}
  {"cmd":"action","action":"greet","device":"lite3"}
"""
import argparse
import os
import subprocess
import sys
import time

DEMO_DIR_CANDIDATES = [
    os.environ.get("AGORA_RTSA_EXAMPLE", ""),
    "../../shengwang/agora_rtsa_sdk/example",           # E:\01work\灵境具身 布局
    "/opt/agora_rtsa_sdk/example",
]


def find_hello_rtm():
    for d in DEMO_DIR_CANDIDATES:
        if not d:
            continue
        for sub in ("out/x86_64", "out/aarch64"):
            p = os.path.join(d, sub, "hello_rtm")
            if os.path.isfile(p):
                return p
    return None


def main():
    ap = argparse.ArgumentParser(description="RTM 信令联调 (封装 hello_rtm)")
    ap.add_argument("-i", "--app-id", required=True, help="声网 App ID")
    ap.add_argument("-u", "--uid", default="operator-1", help="本端 RTM uid (操作员)")
    ap.add_argument("-p", "--peer", default="robot-1", help="机器人 RTM uid")
    ap.add_argument("-t", "--token", default="", help="RTM Token (Token 鉴权时必填)")
    ap.add_argument("--cmd", dest="cmd", default="", help="单条指令 JSON")
    ap.add_argument("--file", dest="cmd_file", default="", help="批量指令文件 (每行一条 JSON)")
    ap.add_argument("--interval", type=float, default=0.5, help="批量模式指令间隔秒")
    ap.add_argument("--demo-dir", default="", help="hello_rtm 所在目录 (默认自动查找)")
    args = ap.parse_args()

    demo = None
    if args.demo_dir:
        demo = os.path.join(args.demo_dir, "hello_rtm")
    else:
        demo = find_hello_rtm()
    if not demo or not os.path.isfile(demo):
        print("错误: 未找到 hello_rtm 可执行文件。")
        print("  请通过 --demo-dir 或环境变量 AGORA_RTSA_EXAMPLE 指定 example/out/x86_64 目录")
        sys.exit(1)

    base_cmd = [demo, "-i", args.app_id, "-u", args.uid, "-p", args.peer]
    if args.token:
        base_cmd += ["-t", args.token]

    # ── 单条 / 批量: 管道 stdin (hello_rtm 交互模式: 每行一条消息发给 peer) ──
    lines = []
    if args.cmd:
        lines.append(args.cmd.strip())
    if args.cmd_file:
        with open(args.cmd_file, "r", encoding="utf-8") as f:
            for ln in f:
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    lines.append(ln)

    if lines:
        print("== 批量下发 %d 条指令给 %s ==" % (len(lines), args.peer))
        stdin_data = "\n".join(lines) + "\n"
        # hello_rtm 逐行读 stdin 即发; 为控制节奏用 Popen 写入
        p = subprocess.Popen(base_cmd, stdin=subprocess.PIPE)
        try:
            for ln in lines:
                print("TX -> %s" % ln)
                p.stdin.write((ln + "\n").encode())
                p.stdin.flush()
                time.sleep(args.interval)
            time.sleep(2)   # 等 on_rtm_send_data_result
        except KeyboardInterrupt:
            pass
        finally:
            p.terminate()
            p.wait(timeout=5)
        return

    # ── 交互模式 ──
    print("== RTM 交互模式: 本端 uid=%s, 对端=%s ==" % (args.uid, args.peer))
    print("   每行输入一条 JSON 指令并回车; Ctrl-C 退出")
    try:
        rc = subprocess.call(base_cmd)
        sys.exit(rc)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
