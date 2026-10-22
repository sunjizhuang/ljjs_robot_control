#include "network/mqtt_client.h"
#include "mosquitto.h"
#include "util/log.h"

#include <algorithm>

namespace {
std::mutex g_mosquitto_mutex;
int g_mosquitto_users = 0;

bool mosquittoGlobalInit()
{
    std::lock_guard<std::mutex> lock(g_mosquitto_mutex);
    if (g_mosquitto_users == 0) {
        const int rc = mosquitto_lib_init();
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERR("Mosquitto: mosquitto_lib_init 失败 rc=%d (%s)", rc, mosquitto_strerror(rc));
            return false;
        }
    }
    ++g_mosquitto_users;
    return true;
}

void mosquittoGlobalCleanup()
{
    std::lock_guard<std::mutex> lock(g_mosquitto_mutex);
    if (g_mosquitto_users <= 0) return;
    --g_mosquitto_users;
    if (g_mosquitto_users == 0) mosquitto_lib_cleanup();
}
}

MqttClient::MqttClient() = default;

const char* MqttClient::commandTopicFilter()
{
    return "device/command/#";
}

const char* MqttClient::telemetryTopic()
{
    return "device/telemetry";
}
MqttClient::~MqttClient() { stop(); }

bool MqttClient::start(const std::string& host, int port,
                       const std::string& client_id,
                       const std::string& username,
                       const std::string& password,
                       int reconnect_interval_ms)
{
    if (running_.load()) return true;
    if (!mosquittoGlobalInit()) return false;

    host_ = host;
    port_ = port;
    client_id_ = client_id;
    username_ = username;
    password_ = password;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        subscribe_filters_.clear();
        subscribe_filters_.push_back(commandTopicFilter());
    }
    reconnect_interval_ms_ = std::max(100, reconnect_interval_ms);

    mosq_ = mosquitto_new(client_id_.empty() ? nullptr : client_id_.c_str(), true, this);
    if (!mosq_) {
        LOG_ERR("Mosquitto: mosquitto_new 失败");
        mosquittoGlobalCleanup();
        return false;
    }

    mosquitto_connect_callback_set(mosq_, &MqttClient::onConnect);
    mosquitto_disconnect_callback_set(mosq_, &MqttClient::onDisconnect);
    mosquitto_message_callback_set(mosq_, &MqttClient::onMessage);
    mosquitto_subscribe_callback_set(mosq_, &MqttClient::onSubscribe);

    if (!username_.empty()) {
        const char* pw = password_.empty() ? nullptr : password_.c_str();
        const int rc = mosquitto_username_pw_set(mosq_, username_.c_str(), pw);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERR("Mosquitto: 设置用户名密码失败 rc=%d (%s)", rc, mosquitto_strerror(rc));
            mosquitto_destroy(mosq_);
            mosq_ = nullptr;
            mosquittoGlobalCleanup();
            return false;
        }
    }

    const unsigned int delay_s = static_cast<unsigned int>(std::max(1, reconnect_interval_ms_ / 1000));
    const int delay_rc = mosquitto_reconnect_delay_set(mosq_, delay_s, delay_s, false);
    if (delay_rc != MOSQ_ERR_SUCCESS) {
        LOG_WARN("Mosquitto: 设置重连间隔失败 rc=%d (%s)", delay_rc, mosquitto_strerror(delay_rc));
    }

    running_ = true;
    int rc = mosquitto_connect_async(mosq_, host_.c_str(), port_, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR("Mosquitto: connect_async 失败 Broker=%s:%d rc=%d (%s)",
                host_.c_str(), port_, rc, mosquitto_strerror(rc));
        running_ = false;
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        mosquittoGlobalCleanup();
        return false;
    }

    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR("Mosquitto: loop_start 失败 rc=%d (%s)", rc, mosquitto_strerror(rc));
        running_ = false;
        mosquitto_disconnect(mosq_);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        mosquittoGlobalCleanup();
        return false;
    }

    LOG_INFO("MQTT Client 已启动，Broker: %s:%d，订阅过滤器: %s，默认发布: %s，重连间隔: %d ms，使用 libmosquitto",
             host_.c_str(), port_, commandTopicFilter(), telemetryTopic(), reconnect_interval_ms_);
    return true;
}

void MqttClient::stop()
{
    if (!running_.exchange(false)) return;

    struct mosquitto* mosq = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mosq = mosq_;
    }

    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        std::lock_guard<std::mutex> lock(state_mutex_);
        mosq_ = nullptr;
    }

    if (connected_.exchange(false)) notifyStatus(false);
    mosquittoGlobalCleanup();
}

bool MqttClient::publish(const std::string& topic, const std::string& payload)
{
    if (!running_.load() || !connected_.load()) return false;

    std::lock_guard<std::mutex> lock(publish_mutex_);
    struct mosquitto* mosq = nullptr;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        mosq = mosq_;
    }
    if (!mosq) return false;

    int mid = 0;
    const int rc = mosquitto_publish(mosq, &mid, topic.c_str(),
                                     static_cast<int>(payload.size()), payload.data(), 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_WARN("Mosquitto: PUBLISH 失败 topic=%s rc=%d (%s)",
                 topic.c_str(), rc, mosquitto_strerror(rc));
        return false;
    }
    return true;
}

bool MqttClient::subscribe(const std::string& topic, int qos)
{
    if (topic.empty() || qos < 0 || qos > 2) return false;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        if (std::find(subscribe_filters_.begin(), subscribe_filters_.end(), topic) == subscribe_filters_.end()) {
            subscribe_filters_.push_back(topic);
        }
    }

    if (!running_.load() || !connected_.load()) {
        // 允许在未连接时预注册 Topic，连接/重连后自动订阅。
        return true;
    }

    struct mosquitto* mosq = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mosq = mosq_;
    }
    if (!mosq) return false;

    int mid = 0;
    const int rc = mosquitto_subscribe(mosq, &mid, topic.c_str(), qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_WARN("Mosquitto: SUBSCRIBE 失败 topic=%s qos=%d rc=%d (%s)",
                 topic.c_str(), qos, rc, mosquitto_strerror(rc));
        return false;
    }
    LOG_INFO("MQTT: SUBSCRIBE topic=%s QoS=%d", topic.c_str(), qos);
    return true;
}

std::string MqttClient::getBrokerAddress() const
{
    return host_ + ":" + std::to_string(port_);
}

void MqttClient::onConnect(struct mosquitto*, void* userdata, int rc)
{
    static_cast<MqttClient*>(userdata)->handleConnect(rc);
}

void MqttClient::onDisconnect(struct mosquitto*, void* userdata, int rc)
{
    static_cast<MqttClient*>(userdata)->handleDisconnect(rc);
}

void MqttClient::onMessage(struct mosquitto*, void* userdata, const struct mosquitto_message* message)
{
    static_cast<MqttClient*>(userdata)->handleMessage(message);
}

void MqttClient::onSubscribe(struct mosquitto*, void* userdata,
                             int mid, int qos_count, const int* granted_qos)
{
    static_cast<MqttClient*>(userdata)->handleSubscribe(mid, qos_count, granted_qos);
}

void MqttClient::handleConnect(int rc)
{
    if (rc != MOSQ_ERR_SUCCESS) {
        connected_ = false;
        LOG_WARN("MQTT: Broker 连接失败 rc=%d (%s)，libmosquitto 将自动重连",
                 rc, mosquitto_strerror(rc));
        notifyStatus(false);
        return;
    }

    connected_ = true;
    LOG_INFO("MQTT: 已连接 Broker %s", getBrokerAddress().c_str());
    notifyStatus(true);

    std::vector<std::string> filters;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        filters = subscribe_filters_;
    }
    for (const auto& filter : filters) {
        subscribe(filter, 0);
    }
}

void MqttClient::handleDisconnect(int rc)
{
    if (connected_.exchange(false)) {
        LOG_WARN("MQTT: Broker 断开 rc=%d (%s)，libmosquitto 将自动重连",
                 rc, mosquitto_strerror(rc));
        notifyStatus(false);
    }
}

void MqttClient::handleMessage(const struct mosquitto_message* message)
{
    if (!message || !message->topic) return;
    std::string payload;
    if (message->payload && message->payloadlen > 0) {
        payload.assign(static_cast<const char*>(message->payload), static_cast<size_t>(message->payloadlen));
    }
    if (message_cb_) message_cb_(message->topic, payload);
}

void MqttClient::handleSubscribe(int mid, int qos_count, const int* granted_qos)
{
    if (qos_count <= 0 || !granted_qos) {
        LOG_WARN("MQTT: SUBACK mid=%d 无授权 QoS", mid);
        return;
    }
    LOG_INFO("MQTT: SUBACK mid=%d granted_qos=%d", mid, granted_qos[0]);
}

void MqttClient::notifyStatus(bool connected)
{
    if (status_cb_) status_cb_(connected);
}
