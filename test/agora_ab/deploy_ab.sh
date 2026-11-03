#!/bin/bash
# ══════════════════════════════════════════════════════════════
# deploy_ab.sh — 部署 A/B 工具到板端 (在 Linux 构建机/容器内执行)
#
# 用法:
#   cd test/agora_ab && ./deploy_ab.sh root@<board-ip> [目标目录]
#   ./deploy_ab.sh root@192.168.199.6            # 默认 /opt/lljs/ab
# ══════════════════════════════════════════════════════════════
set -e

REMOTE=${1:?用法: ./deploy_ab.sh <user>@<board-ip> [目标目录]}
DEST=${2:-/opt/lljs/ab}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AB="$ROOT/test/agora_ab"

[ -x "$AB/build/agora_ab" ] || {
    echo "错误: $AB/build/agora_ab 不存在, 先执行:"
    echo "  cd $AB && mkdir -p build && cd build && cmake .. && make"
    exit 1
}

STAGE="$(mktemp -d)"
cp "$AB/build/agora_ab" "$STAGE/"
cp -L "$ROOT/third_party/agora_sdk/lib/aarch64/libagora-rtc-sdk.so" "$STAGE/"

# 测试素材: 优先工具目录内 testdata/, 其次 SDK 仓库 example/test_data (可用 AB_TEST_DATA 覆盖)
SDK_DATA="${AB_TEST_DATA:-}"
if [ -z "$SDK_DATA" ] && [ -f "$AB/testdata/send_video.h264" ]; then
    SDK_DATA="$AB/testdata"
elif [ -z "$SDK_DATA" ]; then
    SDK_DATA="$ROOT/../../shengwang/agora_rtsa_sdk/example/test_data"
fi
if [ -f "$SDK_DATA/send_video.h264" ]; then
    cp "$SDK_DATA/send_video.h264" "$SDK_DATA/send_audio_16k_1ch.pcm" "$STAGE/"
    echo "==> 已携带测试素材: send_video.h264 + send_audio_16k_1ch.pcm"
else
    echo "⚠ 未找到测试素材目录 $SDK_DATA"
    echo "  音频 A/B 仍可进行; 视频 A/B 请手动上传 .h264 文件"
fi

echo "==> 上传到 $REMOTE:$DEST ..."
tar czf - -C "$STAGE" . | ssh "$REMOTE" "mkdir -p $DEST && tar xzf - -C $DEST"

cat <<DONE

═══════════════════════════════════════════════════
部署完成。板端执行 (先停掉 lljs_ctrl!):

  ssh $REMOTE
  cd $DEST
  LD_LIBRARY_PATH=. ./agora_ab \\
      -i <你的AppID> -c testChannel -u 1002 \\
      -v send_video.h264 -a send_audio_16k_1ch.pcm -f 15

Web 端: test/web_demo_h264.html, Token 留空, 频道 testChannel, 点加入。

判定:
  - Web 收到 uid=1002 的画面/声音 → 集成侧问题 (lljs_ctrl)
  - Web 同样收不到               → SDK/环境/账号侧问题
═══════════════════════════════════════════════════
DONE
