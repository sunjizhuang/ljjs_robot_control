#!/usr/bin/env python3
"""Lite3 V1.0.7 TCP 测试客户端 + 实时仪表盘。"""
import argparse,json,socket,sys,threading,time
from datetime import datetime

CODES=("0x0901","0x0902","0x0903")

def f(v,n=3):
    if not isinstance(v,list): return "[]"
    return "["+", ".join(f"{float(x):+.{n}f}" for x in v)+"]"
def f12(v): return f(v[:12] if isinstance(v,list) else [],3)

def safe(v, default=0):
    try:return float(v)
    except:return default

class TestClient:
    def __init__(self,host,port):
        self.host,self.port=host,port; self.sock=None; self.running=False; self.recv_thread=None
        self.last={}; self.count={c:0 for c in CODES}; self.first_count={c:0 for c in CODES}; self.last_count_time=time.monotonic(); self.rates={c:0.0 for c in CODES}; self.lock=threading.Lock()
    @staticmethod
    def now(): return datetime.now().strftime("%H:%M:%S")
    def connect(self):
        self.sock=socket.create_connection((self.host,self.port),timeout=5); self.sock.settimeout(1); self.running=True
        self.recv_thread=threading.Thread(target=self._recv_loop,daemon=True); self.recv_thread.start(); print(f"[{self.now()}] TCP 已连接 {self.host}:{self.port}")
    def close(self):
        self.running=False
        if self.sock:
            try:self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            try:self.sock.close()
            except OSError:pass
        self.sock=None
    def send_line(self,line):
        if not self.sock:return False
        try:self.sock.sendall((line.rstrip("\n")+"\n").encode()); print(f"[{self.now()}] TX {line}"); return True
        except OSError as e: print(f"[{self.now()}] 发送失败: {e}"); return False
    def send_json(self,obj): return self.send_line(json.dumps(obj,ensure_ascii=False,separators=(",",":")))
    def _recv_loop(self):
        buf=""
        while self.running:
            try:data=self.sock.recv(8192)
            except socket.timeout:continue
            except OSError:break
            if not data: print(f"\n[{self.now()}] TCP Server 已断开"); break
            buf+=data.decode("utf-8",errors="replace")
            while "\n" in buf:
                line,buf=buf.split("\n",1); line=line.strip()
                if not line:continue
                try:msg=json.loads(line)
                except json.JSONDecodeError: print(f"\n[{self.now()}] RX {line}"); continue
                if isinstance(msg,dict) and msg.get("cmd")=="telemetry": self._telemetry(msg)
                else: print(f"\n[{self.now()}] RX {json.dumps(msg,ensure_ascii=False)}")
        self.running=False
    def _telemetry(self,msg):
        code=msg.get("code","?")
        with self.lock:
            self.last[code]=msg; self.count[code]=self.count.get(code,0)+1
            now=time.monotonic(); dt=now-self.last_count_time
            if dt>=1:
                for c in CODES:self.rates[c]=(self.count.get(c,0)-self.first_count.get(c,0))/dt
                self.first_count=dict(self.count); self.last_count_time=now
    def snapshot(self):
        with self.lock:return dict(self.last),dict(self.count),dict(self.rates)

    def dashboard(self,interval=0.2):
        """ANSI 实时仪表盘；按 q/ESC 退出。输入线程不影响 TCP 接收。"""
        stop=threading.Event()
        def key_loop():
            try:
                import termios,tty,select
                fd=sys.stdin.fileno(); old=termios.tcgetattr(fd); tty.setcbreak(fd)
                try:
                    while self.running and not stop.is_set():
                        r,_,_=select.select([sys.stdin],[],[],0.2)
                        if r and sys.stdin.read(1).lower() in ('q','\\x1b'):stop.set();break
                finally:termios.tcsetattr(fd,termios.TCSADRAIN,old)
            except (ImportError,OSError):
                while self.running and not stop.is_set():
                    try:
                        if input("仪表盘输入 q 退出> ").strip().lower()=='q':stop.set()
                    except (EOFError,KeyboardInterrupt):stop.set()
        threading.Thread(target=key_loop,daemon=True).start()
        print("\\x1b[2J\\x1b[H\\x1b[?25l",end="")
        try:
            while self.running and not stop.is_set():
                last,count,rates=self.snapshot(); s=last.get("0x0901",{}); a=last.get("0x0902",{}); v=last.get("0x0903",{})
                lines=[]
                lines += ["="*92," Lite3 V1.0.7 REAL-TIME DASHBOARD",f" TCP: {self.host}:{self.port}   {self.now()}   [Q/ESC] 退出", "-"*92]
                lines += [f"  TELEMETRY  0901 {rates['0x0901']:6.1f} Hz ({count['0x0901']:6d})   0902 {rates['0x0902']:6.1f} Hz ({count['0x0902']:6d})   0903 {rates['0x0903']:6.1f} Hz ({count['0x0903']:6d})"]
                lines += ["", "[0901 ROBOT STATE]"]
                lines += [f"  basic={s.get('robot_basic_state','-')}  gait={s.get('robot_gait_state','-')}  motion={s.get('robot_motion_state','-')}  need_move={s.get('is_robot_need_move','-')}  zero={s.get('zero_position_flag','-')}"]
                lines += [f"  battery={safe(s.get('battery_level'))*100:.1f}%  RPY={f(s.get('rpy'))}  RPY_vel={f(s.get('rpy_vel'))}"]
                lines += [f"  acc={f(s.get('xyz_acc'))}  pos_world={f(s.get('pos_world'))}  vel_world={f(s.get('vel_world'))}"]
                lines += [f"  vel_body={f(s.get('vel_body'))}  ultrasound={s.get('ultrasound','-')}","", "[0902 JOINT ANGLE]"]
                av=a.get('joint_angle',[]); vv=v.get('joint_vel',[])
                lines += ["  " + "  ".join(f"J{i+1:02d}:{safe(av[i]) if i<len(av) else 0:+.3f}" for i in range(12))]
                lines += ["", "[0903 JOINT VELOCITY]"]
                lines += ["  " + "  ".join(f"J{i+1:02d}:{safe(vv[i]) if i<len(vv) else 0:+.3f}" for i in range(12)), "", "="*92]
                sys.stdout.write("\\x1b[H"+"\\n".join(lines)+"\\x1b[J"); sys.stdout.flush(); time.sleep(interval)
        finally: print("\\x1b[?25h\\n",end="")


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

def ask_float(p,d):
    x=input(f"{p} [{d}]: ").strip(); return float(x) if x else float(d)
def ask_int(p,d):
    x=input(f"{p} [{d}]: ").strip(); return int(x) if x else int(d)
def confirm(t):return input(f"{t} 输入 YES 确认: ").strip().upper()=='YES'
def test_axis(c):
    axis=input("axis [x]: ").strip() or 'x'; value=ask_int('value',3000); duration=ask_float('持续秒数',2)
    if confirm(f"将发送轴 {axis}={value}，机器人可能移动。"):
        c.send_json({'cmd':'axis','axis':axis,'value':value,'device':'lite3'}); time.sleep(max(.1,duration)); c.send_json({'cmd':'axis','axis':axis,'value':0,'device':'lite3'})
def test_gait(c): c.send_json({'cmd':'gait','gait':input('gait [medium]: ').strip() or 'medium','device':'lite3'})
def test_save(c):
    if confirm('保存数据会清零力矩并关闭运动程序，确定？'):c.send_json({'cmd':'save_data','device':'lite3'})
def test_continuous(c):
    x=input('on/off [on]: ').strip().lower() or 'on'; c.send_json({'cmd':'continuous_motion','enabled':x in ('on','1','true','yes'),'device':'lite3'})
def test_velocity(c):
    vx,vy,yaw=ask_float('vx',.2),ask_float('vy',0),ask_float('yaw',.1); d=ask_float('持续秒数',2)
    if confirm(f'将以 vx={vx}, vy={vy}, yaw={yaw} 运动。'):
        c.send_json({'cmd':'mode','mode':'auto','device':'lite3'});time.sleep(.3);c.send_json({'cmd':'velocity','vx':vx,'vy':vy,'yaw':yaw,'device':'lite3'});time.sleep(max(.1,d));c.send_json({'cmd':'velocity','vx':0,'vy':0,'yaw':0,'device':'lite3'})
def test_telemetry(c): c.dashboard()
def run_suite(c):
    test_gait(c); test_continuous(c); test_telemetry(c); test_axis(c); test_velocity(c); test_save(c)

def menu(c):
    print('''\n========== Lite3 V1.0.7 测试客户端 ==========\n1 轴  2 步态  3 保存  4 持续运动  5 速度\n6 接收信息/实时仪表盘  7 全部测试  8 仪表盘  9 键盘实时控制  0 退出\n==============================================''')
    while c.running:
        try:x=input('选择> ').strip().lower()
        except (EOFError,KeyboardInterrupt):break
        try:
            if x=='1':test_axis(c)
            elif x=='2':test_gait(c)
            elif x=='3':test_save(c)
            elif x=='4':test_continuous(c)
            elif x=='5':test_velocity(c)
            elif x=='6' or x=='8':test_telemetry(c)
            elif x=='7':
                test_gait(c);test_continuous(c);test_telemetry(c);test_axis(c);test_velocity(c);test_save(c)
            elif x=='9': keyboard_control(c)
            elif x in ('0','q','quit','exit'):break
        except ValueError as e:print(f'参数错误: {e}')
def main():
    p=argparse.ArgumentParser();p.add_argument('host',nargs='?',default='127.0.0.1');p.add_argument('port',nargs='?',type=int,default=9000);p.add_argument('--suite',action='store_true');p.add_argument('--dashboard',action='store_true');p.add_argument('--keyboard',action='store_true');a=p.parse_args();c=TestClient(a.host,a.port)
    try:c.connect(); keyboard_control(c) if a.keyboard else (c.dashboard() if a.dashboard else (run_suite(c) if a.suite else menu(c)))
    finally:c.close()
if __name__=='__main__':main()
