#!/usr/bin/env python3
"""Lite3 V1.0.7 外部控制端 TCP Server 测试工具。

本程序模拟云端/PC 外部控制端：
  SC668S/开发主机运行 ljjs_ctrl(client 模式) -> 本程序 TCP Server

功能：
  * 显示 register 注册信息
  * 实时显示 0x0901 / 0x0902 / 0x0903 telemetry
  * 菜单逐项测试轴/步态/保存/持续运动/速度
  * 支持 --suite 自动执行测试（运动项仍要求 YES 确认）
"""

import argparse
import json
import socket
import threading
import time
import sys
from datetime import datetime


class ExternalServer:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.server = None
        self.conn = None
        self.addr = None
        self.running = False
        self.lock = threading.Lock()
        self.recv_thread = None
        self.last = {}
        self.count = {"0x0901": 0, "0x0902": 0, "0x0903": 0}
        self.first_count = dict(self.count)
        self.last_count_time = time.monotonic()
        self.rates = dict.fromkeys(self.count, 0.0)

    @staticmethod
    def now():
        return datetime.now().strftime("%H:%M:%S")

    def start(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((self.host, self.port))
        self.server.listen(5)
        print(f"[{self.now()}] 外部控制端 TCP Server: {self.host}:{self.port}")
        print("等待 ljjs_ctrl TCP Client 连接...")
        self.conn, self.addr = self.server.accept()
        self.conn.settimeout(1.0)
        self.running = True
        print(f"[{self.now()}] Client 已连接: {self.addr[0]}:{self.addr[1]}")
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.recv_thread.start()

    def stop(self):
        self.running = False
        if self.conn:
            try: self.conn.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            try: self.conn.close()
            except OSError: pass
        self.conn = None
        if self.server:
            try: self.server.close()
            except OSError: pass
        self.server = None

    def send_json(self, obj):
        data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n"
        try:
            with self.lock:
                self.conn.sendall(data.encode())
            print(f"[{self.now()}] TX {data.strip()}")
            return True
        except OSError as exc:
            print(f"[{self.now()}] 发送失败: {exc}")
            self.running = False
            return False

    def _recv_loop(self):
        buf = ""
        while self.running:
            try:
                data = self.conn.recv(8192)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                print(f"\n[{self.now()}] Client 已断开")
                break
            buf += data.decode("utf-8", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    print(f"\n[{self.now()}] RX 非 JSON: {line}")
                    continue
                if msg.get("cmd") == "register":
                    self._print_register(msg)
                elif msg.get("cmd") == "telemetry":
                    self._print_telemetry(msg)
                else:
                    print(f"\n[{self.now()}] RX {json.dumps(msg, ensure_ascii=False)}")
        self.running = False

    def _print_register(self, msg):
        print("\n" + "=" * 72)
        print(f"[{self.now()}] Lite3 Client 注册")
        print(f"  来源地址 : {self.addr[0]}:{self.addr[1]}")
        print(f"  edgeId   : {msg.get('edgeId', '')}")
        print(f"  robotId  : {msg.get('robotId', '')}")
        print(f"  device   : {msg.get('device', '')}")
        print(f"  原始报文 : {json.dumps(msg, ensure_ascii=False, separators=(',', ':'))}")
        print("=" * 72)

    def _print_telemetry(self, msg):
        code = msg.get("code", "?")
        with self.lock:
            self.last[code] = msg
            self.count[code] = self.count.get(code, 0) + 1
            now = time.monotonic(); dt = now - self.last_count_time
            if dt >= 1.0:
                for c in self.count:
                    self.rates[c] = (self.count.get(c, 0) - self.first_count.get(c, 0)) / dt
                self.first_count = dict(self.count); self.last_count_time = now
        if code == "0x0901":
            print(
                f"\n[{self.now()}] [0901] basic={msg.get('robot_basic_state')} "
                f"gait={msg.get('robot_gait_state')} "
                f"motion={msg.get('robot_motion_state')} "
                f"battery={float(msg.get('battery_level',0))*100:.1f}% "
                f"rpy={fmt3(msg.get('rpy'))} "
                f"vel_body={fmt3(msg.get('vel_body'))} "
                f"pos={fmt3(msg.get('pos_world'))} "
                f"need_move={msg.get('is_robot_need_move')}"
            )
        elif code == "0x0902":
            print(f"\n[{self.now()}] [0902] joint_angle={fmt12(msg.get('joint_angle'))}")
        elif code == "0x0903":
            print(f"\n[{self.now()}] [0903] joint_vel={fmt12(msg.get('joint_vel'))}")

    def snapshot(self):
        with self.lock:
            return dict(self.count), dict(self.last), dict(self.rates)

    def dashboard(self, interval=0.2):
        stop = threading.Event()
        def key_loop():
            try:
                import termios, tty, select
                fd=sys.stdin.fileno(); old=termios.tcgetattr(fd); tty.setcbreak(fd)
                try:
                    while self.running and not stop.is_set():
                        r,_,_=select.select([sys.stdin],[],[],0.2)
                        if r and sys.stdin.read(1).lower() in ('q','\x1b'): stop.set(); break
                finally: termios.tcsetattr(fd,termios.TCSADRAIN,old)
            except (ImportError,OSError):
                while self.running and not stop.is_set():
                    try:
                        if input("仪表盘输入 q 退出> ").strip().lower()=='q': stop.set()
                    except (EOFError,KeyboardInterrupt): stop.set()
        threading.Thread(target=key_loop, daemon=True).start()
        print("\x1b[2J\x1b[H\x1b[?25l", end="")
        try:
            while self.running and not stop.is_set():
                count,last,rates=self.snapshot(); s=last.get("0x0901",{}); a=last.get("0x0902",{}); v=last.get("0x0903",{})
                av=a.get('joint_angle',[]); vv=v.get('joint_vel',[])
                lines=["="*92," Lite3 V1.0.7 EXTERNAL SERVER DASHBOARD",f" LISTEN: {self.host}:{self.port}   {self.now()}   [Q/ESC] 退出","-"*92,
                  f"  TELEMETRY  0901 {rates['0x0901']:6.1f} Hz ({count['0x0901']:6d})   0902 {rates['0x0902']:6.1f} Hz ({count['0x0902']:6d})   0903 {rates['0x0903']:6.1f} Hz ({count['0x0903']:6d})","",
                  "[0901 ROBOT STATE]", f"  basic={s.get('robot_basic_state','-')}  gait={s.get('robot_gait_state','-')}  motion={s.get('robot_motion_state','-')}  need_move={s.get('is_robot_need_move','-')}  zero={s.get('zero_position_flag','-')}",
                  f"  battery={float(s.get('battery_level',0))*100:.1f}%  RPY={fmt3(s.get('rpy'))}  RPY_vel={fmt3(s.get('rpy_vel'))}",
                  f"  acc={fmt3(s.get('xyz_acc'))}  pos_world={fmt3(s.get('pos_world'))}  vel_world={fmt3(s.get('vel_world'))}",
                  f"  vel_body={fmt3(s.get('vel_body'))}  ultrasound={s.get('ultrasound','-')}","","[0902 JOINT ANGLE]",
                  "  "+"  ".join(f"J{i+1:02d}:{float(av[i]) if i<len(av) else 0:+.3f}" for i in range(12)),"","[0903 JOINT VELOCITY]",
                  "  "+"  ".join(f"J{i+1:02d}:{float(vv[i]) if i<len(vv) else 0:+.3f}" for i in range(12)),"","="*92]
                sys.stdout.write("\x1b[H"+"\n".join(lines)+"\x1b[J");sys.stdout.flush();time.sleep(interval)
        finally: print("\x1b[?25h\n",end="")


def fmt3(v):
    return "[" + ", ".join(f"{float(x):+.3f}" for x in (v or [])[:3]) + "]"


def fmt12(v):
    return "[" + ", ".join(f"{float(x):+.3f}" for x in (v or [])[:12]) + "]"



def keyboard_control(s, speed=0.3, strafe=0.2, yaw_speed=0.5, interval=0.05):
    """Lite3 实时键盘控制。

    终端 raw/cbreak 模式只能可靠收到按键事件，不能收到物理 key-up；
    因此采用“按键事件刷新 + 250ms watchdog”的方式：
      W/S -> vx +/-speed, A/D -> vy +/-strafe, J/L -> yaw +/-yaw_speed
      SPACE -> 立即停止, X -> autonomous mode, Z -> stand, Q/ESC -> 退出
    操作系统的按键重复会持续刷新运动命令；停止重复后 250ms 自动发送 0 速度。
    每 50ms 发送一次当前速度，保证控制链路连续。
    """
    import termios, tty, select
    old = None
    fd = sys.stdin.fileno()
    try:
        old = termios.tcgetattr(fd)
        tty.setcbreak(fd)
    except (termios.error, OSError):
        print("当前终端不支持 raw/cbreak 键盘模式。")
        return

    running = True
    last_key = time.monotonic()
    vx = vy = yaw = 0.0
    last_sent = None
    sent_at = 0.0
    watchdog = 0.25
    source_name = "TCP Client" if hasattr(s, "host") else "External Server"

    def send_velocity(a, b, c, force=False):
        nonlocal last_sent, sent_at
        value = (round(a, 4), round(b, 4), round(c, 4))
        now = time.monotonic()
        if force or value != last_sent or now - sent_at >= interval:
            if s.send_json({"cmd":"velocity", "vx":value[0], "vy":value[1], "yaw":value[2], "device":"lite3"}):
                last_sent = value
                sent_at = now

    def stop_robot():
        nonlocal vx, vy, yaw
        vx = vy = yaw = 0.0
        send_velocity(0.0, 0.0, 0.0, True)

    print("\n" + "="*72)
    print(" Lite3 V1.0.7 键盘实时控制模式")
    print("="*72)
    print(" W/S : 前进/后退     A/D : 左/右平移")
    print(" J/L : 左/右旋转     SPACE : 停止")
    print(" X   : 自主模式      Z : 站立")
    print(" Q/ESC : 退出并停止")
    print(f" 默认速度: vx={speed:.2f} m/s, vy={strafe:.2f} m/s, yaw={yaw_speed:.2f} rad/s")
    print(" 注意：终端按键释放没有独立事件，停止键盘重复后约 250ms 自动停车。")
    print("="*72)

    try:
        # Lite3 速度指令要求自主模式；进入模式后再开始发送速度。
        s.send_json({"cmd":"mode","mode":"auto","device":"lite3"})
        while getattr(s, "running", False) and running:
            r, _, _ = select.select([sys.stdin], [], [], interval)
            now = time.monotonic()
            if r:
                ch = sys.stdin.read(1)
                if ch == '\x1b':
                    running = False
                    break
                ch = ch.lower()
                if ch == 'q':
                    running = False
                    break
                elif ch == ' ':
                    last_key = now
                    stop_robot()
                    print("\r[KEY] STOP                                      ", end='', flush=True)
                    continue
                elif ch == 'x':
                    stop_robot()
                    s.send_json({"cmd":"mode","mode":"auto","device":"lite3"})
                    print("\r[KEY] AUTO                                      ", end='', flush=True)
                    last_key = now
                    continue
                elif ch == 'z':
                    stop_robot()
                    s.send_json({"cmd":"action","action":"stand","device":"lite3"})
                    print("\r[KEY] STAND                                     ", end='', flush=True)
                    last_key = now
                    continue
                elif ch == 'w': vx, vy, yaw = speed, 0.0, 0.0
                elif ch == 's': vx, vy, yaw = -speed, 0.0, 0.0
                elif ch == 'a': vx, vy, yaw = 0.0, strafe, 0.0
                elif ch == 'd': vx, vy, yaw = 0.0, -strafe, 0.0
                elif ch == 'j': vx, vy, yaw = 0.0, 0.0, yaw_speed
                elif ch == 'l': vx, vy, yaw = 0.0, 0.0, -yaw_speed
                else:
                    continue
                last_key = now
                send_velocity(vx, vy, yaw, True)
                print(f"\r[KEY] {ch.upper()}  vx={vx:+.2f} vy={vy:+.2f} yaw={yaw:+.2f}       ", end='', flush=True)

            # watchdog: OS key-repeat 停止后，立即回零，避免终端失焦时机器人继续运动。
            if now - last_key > watchdog and (vx != 0.0 or vy != 0.0 or yaw != 0.0):
                stop_robot()
                print("\r[KEY] WATCHDOG STOP                              ", end='', flush=True)
            elif vx != 0.0 or vy != 0.0 or yaw != 0.0:
                send_velocity(vx, vy, yaw)
    finally:
        stop_robot()
        try:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
        except (termios.error, OSError):
            pass
        print("\n键盘实时控制已退出。")

def ask_float(prompt, default):
    text = input(f"{prompt} [{default}]: ").strip()
    return float(text) if text else float(default)


def ask_int(prompt, default):
    text = input(f"{prompt} [{default}]: ").strip()
    return int(text) if text else int(default)


def confirm(text):
    return input(f"{text} 输入 YES 确认: ").strip().upper() == "YES"


def axis_test(s):
    print("\n=== 1.2.3 轴指令 ===")
    axis = input("axis [x]: ").strip() or "x"
    value = ask_int("value", 3000)
    duration = ask_float("持续秒数", 2)
    if not confirm(f"将发送 axis={axis}, value={value}，机器人可能移动。"):
        return
    s.send_json({"cmd":"axis","axis":axis,"value":value,"device":"lite3"})
    time.sleep(max(0.1, duration))
    s.send_json({"cmd":"axis","axis":axis,"value":0,"device":"lite3"})


def gait_test(s):
    print("\n=== 1.2.5 步态切换 ===")
    gait = input("low/medium/high/crawl/grip/general/high_step [medium]: ").strip() or "medium"
    s.send_json({"cmd":"gait","gait":gait,"device":"lite3"})


def save_test(s):
    print("\n=== 1.2.8 保存数据 ===")
    print("会清零电机力矩并关闭运动程序。")
    if confirm("确定发送？"):
        s.send_json({"cmd":"save_data","device":"lite3"})


def continuous_test(s):
    print("\n=== 1.2.9 持续运动 ===")
    enabled = input("on/off [on]: ").strip().lower() or "on"
    s.send_json({"cmd":"continuous_motion","enabled":enabled in ("on","1","true","yes"),"device":"lite3"})


def velocity_test(s):
    print("\n=== 1.2.12 速度指令 ===")
    vx = ask_float("vx [-1,1]", 0.2)
    vy = ask_float("vy [-0.5,0.5]", 0.0)
    yaw = ask_float("yaw [-1.5,1.5]", 0.1)
    duration = ask_float("持续秒数", 2)
    if not confirm(f"将以 vx={vx}, vy={vy}, yaw={yaw} 运动。"):
        return
    s.send_json({"cmd":"mode","mode":"auto","device":"lite3"})
    time.sleep(0.3)
    s.send_json({"cmd":"velocity","vx":vx,"vy":vy,"yaw":yaw,"device":"lite3"})
    time.sleep(max(0.1, duration))
    s.send_json({"cmd":"velocity","vx":0,"vy":0,"yaw":0,"device":"lite3"})


def telemetry_test(s):
    print("\n=== 1.3 接收信息 ===")
    seconds = ask_float("观察秒数", 5)
    before, _ = s.snapshot()
    time.sleep(max(0.1, seconds))
    after, latest = s.snapshot()
    for code in ("0x0901","0x0902","0x0903"):
        print(f"{code}: +{after.get(code,0)-before.get(code,0)} 条")
    if latest.get("0x0901"):
        print("最新 0901:", json.dumps(latest["0x0901"], ensure_ascii=False))


def suite(s):
    gait_test(s)
    continuous_test(s)
    telemetry_test(s)
    axis_test(s)
    velocity_test(s)
    save_test(s)


def menu(s):
    print("""
========== Lite3 V1.0.7 External Server ==========
1  轴指令       1.2.3
2  步态切换     1.2.5
3  保存数据     1.2.8
4  持续运动     1.2.9
5  速度指令     1.2.12
6  接收信息     1.3 / 0901 / 0902 / 0903
7  全部测试
9  键盘实时控制
0  退出
===================================================
""")
    while s.running:
        try:
            choice = input("选择> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        try:
            if choice == "1": axis_test(s)
            elif choice == "2": gait_test(s)
            elif choice == "3": save_test(s)
            elif choice == "4": continuous_test(s)
            elif choice == "5": velocity_test(s)
            elif choice == "6": telemetry_test(s)
            elif choice == "7": suite(s)
            elif choice == "9": keyboard_control(s)
            elif choice in ("0","q","quit","exit"): break
            elif choice: print("无效选择")
        except ValueError as exc:
            print(f"参数错误: {exc}")


def main():
    p = argparse.ArgumentParser(description="Lite3 V1.0.7 External TCP Server")
    p.add_argument("host", nargs="?", default="0.0.0.0")
    p.add_argument("port", nargs="?", type=int, default=9000)
    p.add_argument("--suite", action="store_true")
    p.add_argument("--dashboard", action="store_true")
    p.add_argument("--keyboard", action="store_true")
    args = p.parse_args()
    s = ExternalServer(args.host, args.port)
    try:
        s.start()
        print("后台实时显示 0x0901/0902/0903。")
        if args.keyboard: keyboard_control(s)
        elif args.dashboard: s.dashboard()
        elif args.suite: suite(s)
        else: menu(s)
    finally:
        s.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
