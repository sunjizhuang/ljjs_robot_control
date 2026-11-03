#!/bin/bash
# ══════════════════════════════════════════════════════════════
# deploy.sh — 打包编译产物并部署到 RK3588 板端
#
# 用法 (在 Linux 构建机、工程根目录执行):
#   ./scripts/deploy.sh <板端用户>@<板端IP> [目标目录]
#
# 示例:
#   ./scripts/deploy.sh root@192.168.1.100
#   ./scripts/deploy.sh root@192.168.1.100 /opt/lljs
#
# 产物布局 (板端):
#   <目标目录>/
#   ├── lljs_ctrl            可执行 (aarch64)
#   ├── config/config.yaml   配置 (板端已有则不覆盖)
#   └── lib/                 随包动态库
#       ├── libagora-rtc-sdk.so        (声网 SDK)
#       ├── libmosquitto.so.1.6.9      (+ 软链接)
#       ├── libssl.so.1.1 / libcrypto.so.1.1
#       ├── libasound.so.2             (板上缺失时使用)
#       └── librockchip_mpp.so.1       (板上缺失时使用)
# ══════════════════════════════════════════════════════════════
set -e

REMOTE=${1:?用法: ./scripts/deploy.sh <user>@<board-ip> [目标目录]}
DEST=${2:-/opt/lljs}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$(mktemp -d)/lljs_pkg"

[ -x "$ROOT/build-arm/lljs_ctrl" ] || {
    echo "错误: $ROOT/build-arm/lljs_ctrl 不存在, 先执行:"
    echo "  cmake --build build-arm -j\$(nproc)"
    exit 1
}

echo "==> 打包产物..."
mkdir -p "$STAGE/lib" "$STAGE/config"
cp "$ROOT/build-arm/lljs_ctrl" "$STAGE/"

# 随包动态库 (全部复制实体文件, 板端统一走 LD_LIBRARY_PATH)
for f in \
    third_party/agora_sdk/lib/aarch64/libagora-rtc-sdk.so \
    lib/aarch64/libmosquitto.so.1.6.9 \
    lib/aarch64/libssl.so.1.1 \
    lib/aarch64/libcrypto.so.1.1 \
    third_party/alsa/lib/aarch64/libasound.so.2.0.0 \
    third_party/rockchip_mpp/lib/aarch64/librockchip_mpp.so.1; do
    [ -f "$ROOT/$f" ] && cp -L "$ROOT/$f" "$STAGE/lib/" \
        || echo "  跳过 (不存在): $f"
done
# 板端运行时找的是带版本名: libmosquitto.so.1 / libasound.so.2 / librockchip_mpp.so.1
cd "$STAGE/lib"
[ -f libasound.so.2 ]      || cp libasound.so.2.0.0 libasound.so.2
[ -f librockchip_mpp.so.1 ] || { src=$(ls librockchip_mpp.so.1* 2>/dev/null | head -1); \
                                 [ -n "$src" ] && cp "$src" librockchip_mpp.so.1; }
[ -f libmosquitto.so.1 ]   || cp libmosquitto.so.1.6.9 libmosquitto.so.1
cd "$ROOT"

# 配置文件 (板端已有配置时不覆盖, 统一先传为 config.yaml.new)
cp "$ROOT/config/config.yaml" "$STAGE/config/config.yaml.new"

echo "==> 上传到 $REMOTE:$DEST ..."
# 单连接 tar 管道上传: 只需输一次密码, 且避免老版本 scp 不支持 '目录/.'
tar czf - -C "$STAGE" . | ssh "$REMOTE" "mkdir -p $DEST && tar xzf - -C $DEST"

echo "==> 板端依赖检查..."
ssh "$REMOTE" "bash -s" "$DEST" <<CHECK
    DEST=\$1
    echo "  -- 系统级库 (优先使用板上的) --"
    ls /usr/lib/aarch64-linux-gnu/libasound.so.2* 2>/dev/null \
        || echo "  ⚠ 板上无 libasound, 将使用随包库"
    ls /usr/lib/aarch64-linux-gnu/librockchip_mpp.so* 2>/dev/null \
        || echo "  ⚠ 板上无 librockchip_mpp, 将使用随包库"
    echo "  -- 硬件设备 --"
    ls /dev/video* 2>/dev/null | head -3 || echo "  ⚠ 无 /dev/video* (摄像头未接或驱动未加载)"
    arecord -l 2>/dev/null | grep -i "card" | head -2 || echo "  ⚠ 无录音设备 (麦克风)"
    echo "  -- 随包库完整性 (\$DEST/lib) --"
    ls "\$DEST/lib/" 2>/dev/null | head -10
    echo "  -- 配置键差异 (板端 config.yaml 缺失的键) --"
    CFG="\$DEST/config/config.yaml"
    NEW="\$DEST/config/config.yaml.new"
    if [ -f "\$CFG" ] && [ -f "\$NEW" ]; then
        grep -oE '^[[:space:]]*[A-Za-z_0-9]+:' "\$CFG" | tr -d ' :' | sort -u > /tmp/_cfg_old.txt
        grep -oE '^[[:space:]]*[A-Za-z_0-9]+:' "\$NEW" | tr -d ' :' | sort -u > /tmp/_cfg_new.txt
        MISSING=\$(comm -13 /tmp/_cfg_old.txt /tmp/_cfg_new.txt | tr '\n' ' ')
        if [ -n "\$MISSING" ]; then
            echo "  ⚠ 板端配置缺少这些键 (新代码不会生效):"
            echo "      \$MISSING"
            echo "    修复: mv \$NEW \$CFG   (或手工补键后重启)"
        else
            echo "  键集合一致, 无缺失"
        fi
        rm -f /tmp/_cfg_old.txt /tmp/_cfg_new.txt
    else
        echo "  跳过 (无 \$(basename \$CFG) 或 .new)"
    fi
CHECK

cat <<DONE

═══════════════════════════════════════════════════
部署完成。板端运行:
  ssh $REMOTE
  LD_LIBRARY_PATH=$DEST/lib:\$LD_LIBRARY_PATH \\
      $DEST/lljs_ctrl -c $DEST/config/config.yaml

  注意: config.yaml.new 是新配置, 若为全新设备
  mv $DEST/config/config.yaml.new $DEST/config/config.yaml

验证 (PC 端):
  python3 test/test_rtm_cmd.py --app-id XXX --uid pc-1 \\
      --peer <板端rtsa.uid>  # 发 {"cmd":"av_status"} 看回执
═══════════════════════════════════════════════════
DONE
