#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════
# Agora 集成环境准备脚本（在 Linux 交叉编译机上执行，一次性）
#
# 作用:
#   1. 补齐 lib/aarch64 缺失的不带版本号软链接（mosquitto/ssl/crypto）
#   2. 从 RK3588 目标板拷贝 ALSA / Rockchip MPP 的 aarch64 头文件与库
#      到工程 third_party/ 目录（与 mosquitto 同款"工程内自带库"模式，
#      不需要 sysroot，不做源码编译）
#
# 用法:
#   ./scripts/prepare_agora_env.sh <板端IP> [板端SSH用户]
#   例: ./scripts/prepare_agora_env.sh 192.168.1.100 root
#
# 前置条件: 板端已安装 libasound2-dev 与 librockchip-mpp-dev
#   (LPA3588 出厂镜像已确认两者齐全)
# ══════════════════════════════════════════════════════════════
set -e

BOARD_IP=${1:?用法: $0 <板端IP> [用户名，默认 root]}
BOARD_USER=${2:-root}
BOARD="${BOARD_USER}@${BOARD_IP}"

PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="${PROJ_ROOT}/third_party"

echo "══ 工程根目录: ${PROJ_ROOT}"
echo "══ 目标板:     ${BOARD}"

# ─────────────────────────────────────────────
# 1. lib/aarch64 软链接（交叉链接必需）
# ─────────────────────────────────────────────
echo "── [1/3] 检查 lib/aarch64 软链接 ..."
cd "${PROJ_ROOT}/lib/aarch64"
for link in libmosquitto.so:libmosquitto.so.1.6.9 \
            libssl.so:libssl.so.1.1 \
            libcrypto.so:libcrypto.so.1.1; do
    name="${link%%:*}"; target="${link##*:}"
    if [ ! -e "${name}" ]; then
        if [ ! -e "${target}" ]; then
            echo "   ✗ 缺少 ${target}，请先从目标板同步 mosquitto 库"
            exit 1
        fi
        ln -s "${target}" "${name}"
        echo "   + 创建 ${name} -> ${target}"
    else
        echo "   ✓ ${name} 已存在"
    fi
done

# ─────────────────────────────────────────────
# 2. 从板端拷贝 ALSA / MPP 头文件与库
# ─────────────────────────────────────────────
echo "── [2/3] 从板端拷贝 ALSA / MPP ..."

mkdir -p "${TP}/alsa/include" "${TP}/alsa/lib/aarch64"
mkdir -p "${TP}/rockchip_mpp/include" "${TP}/rockchip_mpp/lib/aarch64"

scp -r "${BOARD}:/usr/include/alsa" "${TP}/alsa/include/"
scp    "${BOARD}:/usr/lib/aarch64-linux-gnu/libasound.so*" "${TP}/alsa/lib/aarch64/"

scp -r "${BOARD}:/usr/include/rockchip" "${TP}/rockchip_mpp/include/"
scp    "${BOARD}:/usr/lib/aarch64-linux-gnu/librockchip_mpp.so*" "${TP}/rockchip_mpp/lib/aarch64/"

# ─────────────────────────────────────────────
# 3. 补齐拷入库的不带版本号软链接（链接器需要）
# ─────────────────────────────────────────────
echo "── [3/3] 检查 third_party 软链接 ..."
cd "${TP}/alsa/lib/aarch64"
[ -e libasound.so ] || ln -s libasound.so.2 libasound.so
cd "${TP}/rockchip_mpp/lib/aarch64"
if [ ! -e librockchip_mpp.so ]; then
    ver_target=$(ls librockchip_mpp.so.* 2>/dev/null | sort -V | tail -1)
    [ -n "${ver_target}" ] && ln -s "${ver_target}" librockchip_mpp.so
fi

echo ""
echo "══ 完成。校验目录:"
ls -la "${TP}/alsa/lib/aarch64" "${TP}/rockchip_mpp/lib/aarch64"
echo ""
echo "后续验证编译:"
echo "  cd ${PROJ_ROOT}"
echo "  cmake -S . -B build-arm -DBUILD_PLATFORM=arm -DLLJS_WITH_AGORA=ON"
echo "  cmake --build build-arm -j\$(nproc)"
