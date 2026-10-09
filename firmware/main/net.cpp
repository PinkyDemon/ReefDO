#include "net.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>

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
#include "lwip/sockets.h"
#include "mdns.h"
#include "sampler.hpp"
#include "web.hpp"

namespace net
{

namespace
{

const char* const TAG = "net";
const char* const KEY_SSID = "ssid";
const char* const KEY_PASS = "pass";
const char* const AP_SSID = "reefdo-setup";
const char* const HOSTNAME = "reefdo";
constexpr uint32_t AP_AFTER_MS = 120000;           // station not up for this long → raise the setup AP
constexpr uint32_t WEB_CHECK_EVERY_S = 60;         // the board asks its own web server for /api/sys this often
constexpr uint32_t WEB_CHECK_TIMEOUT_S = 5;        // and gives up on an answer after this long
constexpr uint32_t WEB_DEAD_AFTER = 5;             // failures in a row that count as a dead web server
constexpr uint32_t WEB_RESTART_MIN_UPTIME_S = 600; // never within the first minutes after a boot

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
bool sApStarted = false; // the setup AP is up; raised and lowered by the Watch task only (and Start / Forget)
bool sStaConfigured = false;
bool sPaused = false;                     // the phone app's provisioning owns the station: no reconnects of ours
std::atomic<int64_t> sLastConnectedMs{0}; // written on the event, web and cloud tasks, read by Watch
std::atomic<int64_t> sLinkSinceMs{0};     // when the station last came up or went down, for the log
uint32_t sWebFails = 0;                   // failed self-checks of the web server in a row (Watch only)
TaskHandle_t sWatch = nullptr;

int64_t NowMs()
{
    return esp_timer_get_time() / 1000;
}

float SinceS(int64_t pMs)
{
    return static_cast<float>(NowMs() - pMs) / 1000.0f;
}

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

void OnWifi(void*, esp_event_base_t, int32_t pId, void* pData)
{
    switch(pId)
    {
        case WIFI_EVENT_STA_START:
            if(sStaConfigured && !sPaused) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            // Every failed reconnect attempt comes here too: only the loss of a working link goes into the log.
            if(GetStatus().connected)
            {
                const uint8_t reason = static_cast<const wifi_event_sta_disconnected_t*>(pData)->reason;
                sampler::ReportSystem(reefdo::app::SystemEvent::WifiLost, SinceS(sLinkSinceMs), reason);
                sLinkSinceMs = NowMs();
                ESP_LOGW(TAG, "Wi-Fi lost (reason %u)", reason);
            }
            Update(
                [](Status& pS)
                {
                    pS.connected = false;
                    pS.ip.clear();
                });
            if(sStaConfigured && !sPaused) esp_wifi_connect();
            break;
        }
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
    sLastConnectedMs = NowMs();
    sampler::ReportSystem(reefdo::app::SystemEvent::WifiBack, SinceS(sLinkSinceMs));
    sLinkSinceMs = NowMs();
    ESP_LOGI(TAG, "connected: %s", ip); // the Watch task lowers the setup AP, if it was up
}

// connect() with a deadline of its own: lwIP's blocking one retries the SYN for minutes against a listener whose
// backlog is full, which is just the state a wedged web server is in.
bool ConnectWithin(int pFd, const sockaddr_in& pTo, uint32_t pSeconds)
{
    const int flags = fcntl(pFd, F_GETFL, 0);
    fcntl(pFd, F_SETFL, flags | O_NONBLOCK);
    bool ok = connect(pFd, reinterpret_cast<const sockaddr*>(&pTo), sizeof pTo) == 0;
    if(!ok && errno == EINPROGRESS)
    {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(pFd, &w);
        timeval tv{};
        tv.tv_sec = pSeconds;
        int err = 0;
        socklen_t len = sizeof err;
        ok = select(pFd + 1, nullptr, &w, nullptr, &tv) > 0 && getsockopt(pFd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
             err == 0;
    }
    fcntl(pFd, F_SETFL, flags);
    return ok;
}

// GET /api/sys from the board's own address, as a browser would: true when the web server answers 200 in time.
// The httpd thread is single: a dead one answers nobody, and nothing but a restart revives it.
bool WebAnswers(const Status& pS)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false; // no socket left: as dead for a browser
    timeval tv{};
    tv.tv_sec = WEB_CHECK_TIMEOUT_S;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = inet_addr(pS.ip.c_str());
    static const char REQUEST[] = "GET /api/sys HTTP/1.0\r\nHost: reefdo\r\nConnection: close\r\n\r\n";
    char answer[16] = {};
    const bool ok = ConnectWithin(fd, to, WEB_CHECK_TIMEOUT_S) &&
                    send(fd, REQUEST, sizeof REQUEST - 1, 0) == static_cast<int>(sizeof REQUEST - 1) &&
                    recv(fd, answer, sizeof answer - 1, 0) >= 12 && std::memcmp(answer, "HTTP/1.1 200", 12) == 0;
    close(fd);
    return ok;
}

// A web server that has stopped answering for WEB_DEAD_AFTER checks is logged each time and, once the App has
// nothing a reboot would interrupt, cured the only way there is. One busy with a long request is not dead.
void CheckWeb(const Status& pS)
{
    if(WebAnswers(pS))
    {
        sWebFails = 0;
        return;
    }
    if(web::Busy())
    {
        ESP_LOGW(TAG, "the web server did not answer the check, but it is busy with a request");
        return;
    }
    ++sWebFails;
    ESP_LOGE(TAG, "the web server did not answer (%lu in a row)", static_cast<unsigned long>(sWebFails));
    sampler::ReportSystem(reefdo::app::SystemEvent::WebCheckFailed, static_cast<float>(sWebFails));
    if(sWebFails < WEB_DEAD_AFTER || NowMs() < static_cast<int64_t>(WEB_RESTART_MIN_UPTIME_S) * 1000) return;
    bool allowed = false;
    {
        sampler::Guard guard;
        allowed = sampler::App().RestartAllowed();
    }
    if(allowed) sampler::Restart(reefdo::app::RestartWhy::WebDead);
}

void OnTimeSync(timeval*)
{
    Update([](Status& pS) { pS.timeSynced = true; });
    ESP_LOGI(TAG, "clock set by SNTP");
}

// Raises the setup AP when the station has been down long enough and lowers it once it is up (one task owns the
// mode, so a connection arriving mid-decision cannot leave an open AP behind); keeps RSSI fresh; checks the web
// server once a minute while the station is up.
void Watch(void*)
{
    uint32_t seconds = 0;
    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const Status s = GetStatus();
        if(s.connected)
            StopAp();
        else if(!sPaused && NowMs() - sLastConnectedMs > AP_AFTER_MS)
            StartAp();
        wifi_ap_record_t ap = {};
        if(s.connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) Update([&ap](Status& pS) { pS.rssi = ap.rssi; });
        if(++seconds % WEB_CHECK_EVERY_S == 0 && s.connected && !sPaused) CheckWeb(s);
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

    xTaskCreatePinnedToCore(Watch, "net", 4096 + 1024, nullptr, 2, &sWatch, 0); // + the web check's socket work
}

uint32_t StackLeft()
{
    return sWatch != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(sWatch)) : UINT32_MAX;
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
    if(sPaused) return false; // the phone app's link window drives the station: not now
    if(!hal::nvs::SetStr(KEY_SSID, pSsid) || !hal::nvs::SetStr(KEY_PASS, pPassword)) return false;
#if CONFIG_REEFDO_QEMU
    return true; // stored for a board; QEMU has no Wi-Fi
#endif
    esp_wifi_disconnect();
    if(ConfigureSta()) esp_wifi_connect();
    sLastConnectedMs = NowMs(); // give the new network its two minutes before the AP
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
    sLastConnectedMs = NowMs(); // two more minutes before the setup AP
    if(!pJoined && ConfigureSta()) esp_wifi_connect();
#else
    static_cast<void>(pJoined);
#endif
}

} // namespace net
