#include "notify.hpp"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace notify
{

namespace
{

const char* const TAG = "ntfy";

struct Message
{
    char topic[49];
    char title[48];
    char body[120];
    char priority[12];
    char tags[32];
};

QueueHandle_t sQueue = nullptr;
uint32_t sSent = 0;
uint32_t sFailed = 0;

const char* LevelName(reefdo::ladder::Level pL)
{
    static const char* const NAMES[] = {"Normal", "Blue", "Yellow", "Red"};
    return NAMES[static_cast<int>(pL)];
}

bool Format(const reefdo::app::Notification& pN, const reefdo::config::Config& pCfg, Message& pM)
{
    using reefdo::app::NotifyKind;
    std::memset(&pM, 0, sizeof pM);
    std::strncpy(pM.topic, pCfg.ntfy.topic.c_str(), sizeof pM.topic - 1);
    std::strncpy(pM.priority, pN.urgent ? "high" : "default", sizeof pM.priority - 1);
    const char* dev = pN.device >= 1 && pN.device <= 6 ? pCfg.devices[pN.device - 1].name.c_str() : "";
    switch(pN.kind)
    {
        case NotifyKind::Level:
            std::snprintf(pM.title, sizeof pM.title, "ReefDO %s%s", LevelName(pN.level), pN.repeat ? " (still)" : "");
            std::snprintf(pM.body, sizeof pM.body, "DO %.2f mg/L", static_cast<double>(pN.value));
            std::strncpy(pM.tags, pN.level == reefdo::ladder::Level::Red ? "rotating_light" : "warning",
                         sizeof pM.tags - 1);
            break;
        case NotifyKind::Fault:
            std::strncpy(pM.title, "ReefDO FAULT", sizeof pM.title - 1);
            std::snprintf(pM.body, sizeof pM.body, "probe not answering; acting as %s", LevelName(pN.level));
            std::strncpy(pM.tags, "x", sizeof pM.tags - 1);
            break;
        case NotifyKind::FaultCleared:
            std::strncpy(pM.title, "ReefDO probe back", sizeof pM.title - 1);
            std::strncpy(pM.tags, "white_check_mark", sizeof pM.tags - 1);
            break;
        case NotifyKind::Recovered:
            std::strncpy(pM.title, "ReefDO back to Normal", sizeof pM.title - 1);
            std::snprintf(pM.body, sizeof pM.body, "DO %.2f mg/L", static_cast<double>(pN.value));
            std::strncpy(pM.tags, "white_check_mark", sizeof pM.tags - 1);
            break;
        case NotifyKind::ServiceFail:
            std::snprintf(pM.title, sizeof pM.title, "ReefDO service FAIL: %s", dev);
            std::snprintf(pM.body, sizeof pM.body, "device %u responded %.1f %% — check it tonight", pN.device,
                          static_cast<double>(pN.value));
            std::strncpy(pM.tags, "wrench", sizeof pM.tags - 1);
            break;
        case NotifyKind::ServiceInconclusive:
            std::strncpy(pM.title, "ReefDO service inconclusive", sizeof pM.title - 1);
            std::snprintf(pM.body, sizeof pM.body, "%u evenings without a usable check",
                          static_cast<unsigned>(pN.value));
            std::strncpy(pM.tags, "question", sizeof pM.tags - 1);
            break;
        case NotifyKind::Boot:
            std::strncpy(pM.title, "ReefDO booted", sizeof pM.title - 1);
            std::snprintf(pM.body, sizeof pM.body, "reset reason %u", static_cast<unsigned>(pN.value));
            std::strncpy(pM.tags, "electric_plug", sizeof pM.tags - 1);
            break;
    }
    return pM.topic[0] != '\0';
}

bool Post(const Message& pM)
{
    char url[96];
    std::snprintf(url, sizeof url, "https://ntfy.sh/%s", pM.topic);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == nullptr) return false;
    esp_http_client_set_header(client, "Title", pM.title);
    esp_http_client_set_header(client, "Priority", pM.priority);
    esp_http_client_set_header(client, "Tags", pM.tags);
    esp_http_client_set_post_field(client, pM.body, static_cast<int>(std::strlen(pM.body)));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if(err != ESP_OK || status < 200 || status >= 300)
    {
        ESP_LOGW(TAG, "push failed: %s (HTTP %d)", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

void Task(void*)
{
    Message m;
    for(;;)
    {
        if(xQueueReceive(sQueue, &m, portMAX_DELAY) != pdTRUE) continue;
        if(Post(m))
            ++sSent;
        else
            ++sFailed;
    }
}

} // namespace

void Start()
{
    sQueue = xQueueCreate(8, sizeof(Message));
    xTaskCreatePinnedToCore(Task, "ntfy", 6144, nullptr, 2, nullptr, 0);
}

void Push(const reefdo::app::Notification& pN, const reefdo::config::Config& pCfg)
{
    if(!pCfg.ntfy.enabled || sQueue == nullptr) return;
    if(pN.kind == reefdo::app::NotifyKind::Level && pN.level < pCfg.ntfy.minLevel) return;
    Message m;
    if(!Format(pN, pCfg, m)) return;
    if(xQueueSend(sQueue, &m, 0) != pdTRUE) ++sFailed;
}

bool SendTest(const reefdo::config::Config& pCfg)
{
    if(pCfg.ntfy.topic.empty()) return false;
    Message m = {};
    std::strncpy(m.topic, pCfg.ntfy.topic.c_str(), sizeof m.topic - 1);
    std::strncpy(m.title, "ReefDO test", sizeof m.title - 1);
    std::strncpy(m.body, "push notifications work", sizeof m.body - 1);
    std::strncpy(m.priority, "default", sizeof m.priority - 1);
    std::strncpy(m.tags, "white_check_mark", sizeof m.tags - 1);
    return Post(m);
}

uint32_t Sent()
{
    return sSent;
}
uint32_t Failed()
{
    return sFailed;
}

} // namespace notify
