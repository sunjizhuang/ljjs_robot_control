#!/bin/bash
# 启动脚本（YAML 配置文件方式）
#
# 用法:
#   ./run.sh                       # 使用默认配置搜索路径
#   ./run.sh /path/to/config.yaml  # 指定配置文件

cd "$(dirname "$0")/.."

CONFIG="${1:-}"

if [ ! -f "./lljs_ctrl" ]; then
    echo "错误: 可执行文件 ./lljs_ctrl 不存在，请先编译:"
    echo "  ./scripts/build.sh  或  make"
    exit 1
fi

# 校验配置
if [ -n "$CONFIG" ]; then
    ./lljs_ctrl --check-config -c "$CONFIG"
    if [ $? -ne 0 ]; then
        echo "配置校验失败，退出"
        exit 1
    fi
    ./lljs_ctrl -c "$CONFIG"
else
    # 默认搜索: ./config.yaml → ./config/config.yaml → /etc/lljs_ctrl/config.yaml
    ./lljs_ctrl
fi
