#!/bin/bash
# 编译脚本
cd "$(dirname "$0")/.."

echo "=== 编译 lljs_ctrl ==="
make clean && make
if [ $? -ne 0 ]; then
    echo "编译失败"
    exit 1
fi

echo ""
echo "=== 编译成功 ==="
echo "可执行文件: ./lljs_ctrl"
echo ""
echo "快速上手:"
echo "  校验配置:   ./lljs_ctrl --check-config -c config/config.yaml"
echo "  查看配置:   ./lljs_ctrl --dump-config  -c config/config.yaml"
echo "  启动:       ./lljs_ctrl -c config/config.yaml"
echo "  帮助:       ./lljs_ctrl --help"
