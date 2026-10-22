#ifndef LLJS_MQTT_CLIENT_H
#define LLJS_MQTT_CLIENT_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct mosquitto;

class MqttClient {
public:
    // MQTT 业务 Topic：固定以 device/ 开头。
    // command/# 用于接收不同扩展层级的控制 Topic，例如：
    //   device/command
    //   device/command/lite3
    //   device/command/lite3/axis
    static const char* commandTopicFilter();
    static const char* telemetryTopic();

    typedef std::function<void(const std::string& topic,
                               const std::string& payload)> MessageCallback;
    typedef std::function<void(bool connected)> StatusCallback;

    MqttClient();
    ~MqttClient();

    bool start(const std::string& host, int port,
               const std::string& client_id,
               const std::string& username,
               const std::string& password,
               int reconnect_interval_ms);
    void stop();

    bool isRunning() const { return running_.load(); }
    bool isConnected() const { return connected_.load(); }
    bool publish(const std::string& topic, const std::string& payload);
    bool subscribe(const std::string& topic, int qos = 0);

    void setMessageCallback(MessageCallback cb) { message_cb_ = cb; }
    void setStatusCallback(StatusCallback cb) { status_cb_ = cb; }
    std::string getBrokerAddress() const;

private:
    static void onConnect(struct mosquitto* mosq, void* userdata, int rc);
    static void onDisconnect(struct mosquitto* mosq, void* userdata, int rc);
    static void onMessage(struct mosquitto* mosq, void* userdata,
                          const struct mosquitto_message* message);
    static void onSubscribe(struct mosquitto* mosq, void* userdata,
                            int mid, int qos_count, const int* granted_qos);

    void handleConnect(int rc);
    void handleDisconnect(int rc);
    void handleMessage(const struct mosquitto_message* message);
    void handleSubscribe(int mid, int qos_count, const int* granted_qos);
    void notifyStatus(bool connected);

    struct mosquitto* mosq_ = nullptr;
    std::string host_;
    int port_ = 1883;
    std::string client_id_;
    std::string username_;
    std::string password_;
    std::vector<std::string> subscribe_filters_;
    std::mutex subscription_mutex_;
    int reconnect_interval_ms_ = 5000;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    mutable std::mutex state_mutex_;
    std::mutex publish_mutex_;
    MessageCallback message_cb_;
    StatusCallback status_cb_;
};

#endif
