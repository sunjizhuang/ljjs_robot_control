#ifndef LLJS_MQTT_BROKER_H
#define LLJS_MQTT_BROKER_H

#include <string>
#include <sys/types.h>

/*
 * Mosquitto Broker 进程管理器。
 *
 * 注意：libmosquitto 是 MQTT Client API，不提供 Broker API。
 * lljs_ctrl 通过该类启动/停止官方 mosquitto Broker 进程，
 * 同时 MqttClient 使用 libmosquitto 作为 Broker 的本地 Client，
 * 负责接收其它客户端 PUB 的 JSON，并负责向其它客户端 PUB。
 */
class MqttBroker {
public:
    MqttBroker();
    ~MqttBroker();

    bool start(const std::string& executable,
               const std::string& bind_address,
               int port,
               bool allow_anonymous,
               const std::string& config_file);
    void stop();
    bool isRunning();
    std::string getConfigFile() const { return config_file_; }
    pid_t getPid() const { return pid_; }

private:
    bool writeConfig(const std::string& bind_address,
                     int port,
                     bool allow_anonymous,
                     const std::string& config_file);
    static std::string findExecutable(const std::string& requested);

    pid_t pid_ = -1;
    std::string config_file_;
};

#endif
