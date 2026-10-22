Mosquitto Broker binary
=======================

lljs_ctrl 的 MQTT Broker(Server) 角色使用 Eclipse Mosquitto 官方 mosquitto 可执行文件。
请使用与目标机架构匹配的 Ubuntu 20.04/Focal 版本。

推荐：
  ./scripts/fetch_mosquitto_ubuntu20.sh

该脚本会把官方 mosquitto 放到本目录，并把 libmosquitto.so* 放到 ../lib、mosquitto.h 放到 ../include。
如果系统已安装 /usr/sbin/mosquitto 或 /usr/bin/mosquitto，则无需把二进制复制到本目录。
