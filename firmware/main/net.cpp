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
#include "sdkconfig.h"
#if CONFIG_REEFDO_QEMU
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#endif
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
portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED; // sStatus: written by the event and watch tasks, read by any

// Every change to sStatus goes through here, so a reader on another task never copies half an update.
template <class F>
void Update(F pChange)
{
    taskENTER_CRITICAL(&sLock);
    pChange(sStatus);
    taskEXIT_CRITICAL(&sLock);
}
bool sApStarted = false;
bool sStaConfigured = false;
bool sPaused = false; // the phone app's provisioning owns the station: no reconnects of ours
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
    Update([](Status& pS) { pS.apActive = true; });
    ESP_LOGW(TAG, "setup AP '%s' up: http://192.168.4.1/", AP_SSID);
}

void StopAp()
{
    if(!sApStarted) return;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    sApStarted = false;
    Update([](Status& pS) { pS.apActive = false; });
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
    Update([&ssid](Status& pS) { pS.ssid.assign(ssid); });
    sStaConfigured = true;
    return true;
}

void OnWifi(void*, esp_event_base_t, int32_t pId, void*)
{
    switch(pId)
    {
        case WIFI_EVENT_STA_START:
            if(sStaConfigured && !sPaused) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            Update(
                [](Status& pS)
                {
                    pS.connected = false;
                    pS.ip.clear();
                });
            if(sStaConfigured && !sPaused) esp_wifi_connect();
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
    Update(
        [&ip](Status& pS)
        {
            pS.ip.assign(ip);
            pS.connected = true;
        });
    sLastConnectedMs = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "connected: %s", ip);
    StopAp();
}

void OnTimeSync(timeval*)
{
    Update([](Status& pS) { pS.timeSynced = true; });
    ESP_LOGI(TAG, "clock set by SNTP");
}

// Raises the setup AP when the station has been down long enough; keeps RSSI fresh.
void Watch(void*)
{
    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time() / 1000;
        const bool connected = GetStatus().connected;
        if(!connected && !sPaused && now - sLastConnectedMs > AP_AFTER_MS) StartAp();
        wifi_ap_record_t ap = {};
        if(connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) Update([&ap](Status& pS) { pS.rssi = ap.rssi; });
    }
}

#if CONFIG_REEFDO_QEMU
// QEMU: its emulated Ethernet (open_eth) stands in for the Wi-Fi station; DHCP from QEMU's user-mode network.
void OnEthIp(void*, esp_event_base_t, int32_t, void* pData)
{
    const ip_event_got_ip_t* e = static_cast<const ip_event_got_ip_t*>(pData);
    char ip[16] = {};
    std::snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
    Update(
        [&ip](Status& pS)
        {
            pS.ssid.assign("(QEMU Ethernet)");
            pS.ip.assign(ip);
            pS.connected = true;
        });
    ESP_LOGI(TAG, "QEMU Ethernet up: %s", ip);
}

void StartQemuEthernet()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&cfg);
    esp_netif_set_hostname(netif, HOSTNAME);
    eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
    phyCfg.autonego_timeout_ms = 100;
    esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&macCfg);
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phyCfg);
    esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&ethCfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &OnEthIp, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}
#endif

} // namespace

void Start()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_REEFDO_QEMU
    StartQemuEthernet();
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    esp_sntp_config_t qemuSntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    qemuSntp.sync_cb = &OnTimeSync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&qemuSntp));
    return;
#endif
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
    taskENTER_CRITICAL(&sLock);
    const Status s = sStatus;
    taskEXIT_CRITICAL(&sLock);
    return s;
}

bool SetCredentials(std::string_view pSsid, std::string_view pPassword)
{
    if(pSsid.empty() || pSsid.size() > 32 || pPassword.size() > 64) return false;
    if(!hal::nvs::SetStr(KEY_SSID, pSsid) || !hal::nvs::SetStr(KEY_PASS, pPassword)) return false;
#if CONFIG_REEFDO_QEMU
    return true; // stored for a board; QEMU has no Wi-Fi
#endif
    esp_wifi_disconnect();
    if(ConfigureSta()) esp_wifi_connect();
    sLastConnectedMs = esp_timer_get_time() / 1000; // give the new network its two minutes before the AP
    return true;
}

void Forget()
{
    hal::nvs::EraseKey(KEY_SSID);
    hal::nvs::EraseKey(KEY_PASS);
#if CONFIG_REEFDO_QEMU
    return;
#endif
    sStaConfigured = false;
    Update([](Status& pS) { pS.ssid.clear(); });
    esp_wifi_disconnect();
    StartAp();
}

bool HasCredentials()
{
    return sStaConfigured;
}

} // namespace net

namespace net
{

bool RememberCredentials(std::string_view pSsid, std::string_view pPassword)
{
    if(pSsid.empty() || pSsid.size() > 32 || pPassword.size() > 64) return false;
    if(!hal::nvs::SetStr(KEY_SSID, pSsid) || !hal::nvs::SetStr(KEY_PASS, pPassword)) return false;
    sStaConfigured = true;
    const std::string_view ssid = pSsid;
    Update([&ssid](Status& pS) { pS.ssid.assign(ssid); });
    return true;
}

void PauseStation()
{
    sPaused = true;
}

void ResumeStation(bool pJoined)
{
    // The provisioning manager forced station mode (the setup AP is gone), may have emptied the station
    // config and switched Wi-Fi storage to flash: put ReefDO's own state back.
    sPaused = false;
#if !CONFIG_REEFDO_QEMU
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    sApStarted = false;
    Update([](Status& pS) { pS.apActive = false; });
    sLastConnectedMs = esp_timer_get_time() / 1000; // two more minutes before the setup AP
    if(!pJoined && ConfigureSta()) esp_wifi_connect();
#else
    static_cast<void>(pJoined);
#endif
}

} // namespace net
