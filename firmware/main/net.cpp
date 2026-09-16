#include "net.hpp"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/nvs_store.hpp"
#include "mdns.h"

namespace net
{

namespace
{

const char* const TAG = "net";
const char* const KEY_SSID = "ssid";
const char* const KEY_PASS = "pass";
const char* const AP_SSID = "reefdo-setup";
const char* const HOSTNAME = "reefdo";
constexpr uint32_t AP_AFTER_MS = 120000; // station not up for this long → raise the setup AP

esp_netif_t* sSta = nullptr;
esp_netif_t* sAp = nullptr;
Status sStatus;
bool sApStarted = false;
bool sStaConfigured = false;
int64_t sLastConnectedMs = 0;

void StartAp()
{
    if(sApStarted) return;
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), AP_SSID, sizeof cfg.ap.ssid);
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(AP_SSID));
    cfg.ap.channel = 1;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 2;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    sApStarted = true;
    sStatus.apActive = true;
    ESP_LOGW(TAG, "setup AP '%s' up: http://192.168.4.1/", AP_SSID);
}

void StopAp()
{
    if(!sApStarted) return;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    sApStarted = false;
    sStatus.apActive = false;
    ESP_LOGI(TAG, "setup AP down");
}

// Loads the stored credentials into the station config; false when there are none.
bool ConfigureSta()
{
    char ssid[33] = {};
    char pass[65] = {};
    std::size_t n = 0;
    if(!hal::nvs::GetStr(KEY_SSID, ssid, n) || n == 0) return false;
    hal::nvs::GetStr(KEY_PASS, pass, n);
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof cfg.sta.ssid);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), pass, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    sStatus.ssid.assign(ssid);
    sStaConfigured = true;
    return true;
}

void OnWifi(void*, esp_event_base_t, int32_t pId, void*)
{
    switch(pId)
    {
        case WIFI_EVENT_STA_START:
            if(sStaConfigured) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            sStatus.connected = false;
            sStatus.ip.assign("");
            if(sStaConfigured) esp_wifi_connect();
            break;
        default: break;
    }
}

void OnIp(void*, esp_event_base_t, int32_t pId, void* pData)
{
    if(pId != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t* e = static_cast<const ip_event_got_ip_t*>(pData);
    char ip[16] = {};
    std::snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
    sStatus.ip.assign(ip);
    sStatus.connected = true;
    sLastConnectedMs = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "connected: %s", ip);
    StopAp();
}

void OnTimeSync(timeval*)
{
    sStatus.timeSynced = true;
    ESP_LOGI(TAG, "clock set by SNTP");
}

// Raises the setup AP when the station has been down long enough; keeps RSSI fresh.
void Watch(void*)
{
    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time() / 1000;
        if(!sStatus.connected && now - sLastConnectedMs > AP_AFTER_MS) StartAp();
        if(sStatus.connected)
        {
            wifi_ap_record_t ap = {};
            if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK) sStatus.rssi = ap.rssi;
        }
    }
}

} // namespace

void Start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sSta = esp_netif_create_default_wifi_sta();
    sAp = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sSta, HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnIp, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    const bool haveSta = ConfigureSta();
    ESP_ERROR_CHECK(esp_wifi_start());
    if(!haveSta)
    {
        ESP_LOGW(TAG, "no WiFi credentials stored");
        StartAp();
    }

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("ReefDO");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = &OnTimeSync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    xTaskCreatePinnedToCore(Watch, "net", 4096, nullptr, 2, nullptr, 0);
}

Status GetStatus()
{
    return sStatus;
}

bool SetCredentials(std::string_view pSsid, std::string_view pPassword)
{
    if(pSsid.empty() || pSsid.size() > 32 || pPassword.size() > 64) return false;
    if(!hal::nvs::SetStr(KEY_SSID, pSsid) || !hal::nvs::SetStr(KEY_PASS, pPassword)) return false;
    esp_wifi_disconnect();
    if(ConfigureSta()) esp_wifi_connect();
    sLastConnectedMs = esp_timer_get_time() / 1000; // give the new network its two minutes before the AP
    return true;
}

void Forget()
{
    hal::nvs::EraseKey(KEY_SSID);
    hal::nvs::EraseKey(KEY_PASS);
    sStaConfigured = false;
    sStatus.ssid.assign("");
    esp_wifi_disconnect();
    StartAp();
}

bool HasCredentials()
{
    return sStaConfigured;
}

} // namespace net
