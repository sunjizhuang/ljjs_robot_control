# Lite3 V1.0.7 功能增量

基于原有 `ljjs_robot_control` 架构，在不改变 Yunji 驱动行为的前提下增加 Lite3 V1.0.7 功能。

## 已实现

1. **1.2.3 轴指令**
   - `0x21010131` Y / 横滚
   - `0x21010130` X / 俯仰
   - `0x21010102` 身体高度
   - `0x21010135` Yaw / 偏航
   - Lite3 轴发送线程按 50 ms（20 Hz）周期发送当前轴值。
2. **1.2.5 步态切换**
   - low / medium / high
   - crawl
   - grip / general / high_step
3. **1.2.8 保存数据**
   - `0x21010C01`
4. **1.2.9 持续运动**
   - `0x21010C06`, 1=开启，2=关闭
5. **1.2.12 速度指令**
   - `0x0140` vx
   - `0x0145` vy
   - `0x0141` yaw
   - 每条为 type=1 + 8 字节 double
6. **1.3 接收信息**
   - UDP 绑定 `43897`
   - `0x0901` RobotStateUpload
   - `0x0902` 12 路关节角度
   - `0x0903` 12 路关节速度
   - 提供 `Lite3Driver::getLite3StateSnapshot()` 获取最新状态

## TCP JSON 示例

```json
{"cmd":"axis","axis":"x","value":3000,"device":"lite3"}
{"cmd":"axis","axis":"yaw","value":2000,"device":"lite3"}
{"cmd":"gait","gait":"medium","device":"lite3"}
{"cmd":"save_data","device":"lite3"}
{"cmd":"continuous_motion","enabled":true,"device":"lite3"}
{"cmd":"velocity","vx":0.3,"vy":0.0,"yaw":0.1,"device":"lite3"}
```

## 网络配置

- Lite3 命令目标端口：43893
- Lite3 telemetry 接收端口：43897
- `config/config.yaml` 增加 `lite3.receive_port`

## 编译验证

已在 Linux x86 环境执行：

```bash
cmake -S . -B build -DBUILD_PLATFORM=x86
cmake --build build -j2
./build/lljs_ctrl --check-config -c config/config.yaml
```

以上编译及配置校验通过。
