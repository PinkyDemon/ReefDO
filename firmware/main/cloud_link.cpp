#include "cloud_link.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reefdo/gateway/cloud.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_REEFDO_CLOUD
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_rmaker_common_events.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_types.h"
#include "net.hpp"
#include "nvs_flash.h"
#include "platform.hpp"
#include "sampler.hpp"
#if CONFIG_BT_ENABLED
#include "esp_srp.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#endif
#endif

namespace cloud_link
{

namespace
{

enum class Phase : uint8_t
{
    Off,       // not built in, or waiting for the network
    NotLinked, // no device certificate yet
    Linking,   // the Bluetooth window is open
    Offline,   // linked, the cloud not reached (yet)
    Online
};
std::atomic<Phase> sPhase{Phase::Off};
std::atomic<uint32_t> sPushes{0};

#if CONFIG_REEFDO_CLOUD

namespace cl = reefdo::gateway::cloud;
namespace gw = reefdo::gateway;

const char* const TAG = "cloud";
constexpr uint32_t LINK_WINDOW_S = 600;

struct Write
{
    cl::Param param;
    int32_t value;
};

cl::Agent sAgent; // under sampler::Guard only: Step on the sampler task, Write on ours
std::atomic<bool> sRunning{false};
QueueHandle_t sUpdates = nullptr;
QueueHandle_t sWrites = nullptr;
TaskHandle_t sTask = nullptr;
std::array<esp_rmaker_param_t*, cl::PARAMS> sParams{};
esp_rmaker_param_t* sName = nullptr;
char sScratch[gw::JSON_MAX]; // the router's, for the app's writes; our task only

bool HaveFactoryPartition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "fctry") != nullptr;
}

void Apply(const cl::Updates& pU)
{
    for(const cl::Update& u : pU)
    {
        esp_rmaker_param_t* p = sParams[static_cast<std::size_t>(u.param)];
        const cl::Kind kind = cl::PARAM_INFO[static_cast<std::size_t>(u.param)].kind;
        const esp_rmaker_param_val_t v = kind == cl::Kind::Float  ? esp_rmaker_float(u.f)
                                         : kind == cl::Kind::Int  ? esp_rmaker_int(static_cast<int>(u.i))
                                         : kind == cl::Kind::Bool ? esp_rmaker_bool(u.b)
                                                                  : esp_rmaker_str(u.text.c_str()); // copied
        if(u.notify)
        {
            esp_rmaker_param_update_and_notify(p, v);
            ++sPushes;
        }
        else
        {
            esp_rmaker_param_update(p, v);
        }
    }
}

// The app's writes arrive on the SDK's task: queued, answered on ours through the router.
esp_rmaker_error_t OnWrite(const esp_rmaker_device_t*, const esp_rmaker_param_write_req_t pReq[], uint8_t pCount, void*,
                           esp_rmaker_write_ctx_t*)
{
    for(uint8_t i = 0; i < pCount; ++i)
    {
        const esp_rmaker_param_val_t& v = pReq[i].val;
        if(pReq[i].param == sName)
        {
            esp_rmaker_param_update(sName, v); // renamed in the app
            continue;
        }
        for(std::size_t k = 0; k < cl::PARAMS; ++k)
        {
            if(pReq[i].param != sParams[k]) continue;
            const int32_t value = v.type == RMAKER_VAL_TYPE_BOOLEAN   ? (v.val.b ? 1 : 0)
                                  : v.type == RMAKER_VAL_TYPE_INTEGER ? static_cast<int32_t>(v.val.i)
                                                                      : -1;
            const Write w{static_cast<cl::Param>(k), value};
            if(xQueueSend(sWrites, &w, 0) != pdTRUE) ESP_LOGW(TAG, "write from the app dropped: queue full");
        }
    }
    return ESP_RMAKER_OK;
}

void OnEvent(void*, esp_event_base_t, int32_t pId, void*)
{
    if(pId == RMAKER_MQTT_EVENT_CONNECTED)
    {
        sPhase = Phase::Online;
        ESP_LOGI(TAG, "phone app cloud connected");
    }
    else if(pId == RMAKER_MQTT_EVENT_DISCONNECTED)
    {
        sPhase = Phase::Offline;
        ESP_LOGW(TAG, "phone app cloud disconnected");
    }
}

void Declare(esp_rmaker_node_t* pNode);

// Needs the device certificate (fctry): without it the node does not start and the board stays "not linked".
bool TryStart()
{
    if(!HaveFactoryPartition())
    {
        ESP_LOGW(TAG, "no fctry partition (an older partition table): the phone app needs one USB flash");
        return false;
    }
    static esp_rmaker_node_t* sNode = nullptr; // once created, kept: a failed start retries the start only
    if(sNode == nullptr)
    {
        esp_rmaker_config_t cfg = {};
        cfg.enable_time_sync = true; // time-series params need it; the SDK adopts ReefDO's running SNTP
        sNode = esp_rmaker_node_init(&cfg, "ReefDO", "Oxygen failsafe");
        if(sNode == nullptr)
        {
            ESP_LOGI(TAG, "not linked to a phone app");
            return false;
        }
        Declare(sNode);
    }
    sPhase = Phase::Offline; // before the start: its CONNECTED event may come at once
    if(esp_rmaker_start() != ESP_RMAKER_OK)
    {
        ESP_LOGE(TAG, "the phone app agent did not start");
        return false;
    }
    sRunning = true;
    ESP_LOGI(TAG, "linked: reporting to the phone app");
    return true;
}

// The device and its parameters, from the gateway's table; once per boot.
void Declare(esp_rmaker_node_t* pNode)
{
    esp_rmaker_device_t* dev = esp_rmaker_device_create("ReefDO", ESP_RMAKER_DEVICE_OTHER, nullptr);
    esp_rmaker_device_add_bulk_cb(dev, &OnWrite, nullptr);
    sName = esp_rmaker_name_param_create("Name", "ReefDO");
    esp_rmaker_device_add_param(dev, sName);
    for(std::size_t i = 0; i < cl::PARAMS; ++i)
    {
        const cl::ParamInfo& info = cl::PARAM_INFO[i];
        const esp_rmaker_param_val_t v = info.kind == cl::Kind::Float  ? esp_rmaker_float(0.0f)
                                         : info.kind == cl::Kind::Int  ? esp_rmaker_int(0)
                                         : info.kind == cl::Kind::Bool ? esp_rmaker_bool(false)
                                                                       : esp_rmaker_str("");
        const uint8_t props = static_cast<uint8_t>(PROP_FLAG_READ | (info.writable ? PROP_FLAG_WRITE : 0) |
                                                   (info.timeSeries ? PROP_FLAG_TIME_SERIES : 0));
        esp_rmaker_param_t* p = esp_rmaker_param_create(info.id, info.type, v, props);
        esp_rmaker_param_add_ui_type(p, info.ui);
        if(info.kind == cl::Kind::Int && info.max > info.min)
            esp_rmaker_param_add_bounds(p, esp_rmaker_int(info.min), esp_rmaker_int(info.max),
                                        esp_rmaker_int(info.step));
        esp_rmaker_device_add_param(dev, p);
        sParams[i] = p;
    }
    esp_rmaker_device_assign_primary_param(dev, sParams[static_cast<std::size_t>(cl::Param::Do)]);
    esp_rmaker_node_add_device(pNode, dev);
    esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, &OnEvent, nullptr);
}

#if CONFIG_BT_ENABLED
constexpr const char* SRP_USER = "wifiprov";                   // what the RainMaker apps use for security 2
constexpr std::size_t MFG_DATA_LEN = 12;                       // the Neo app's tag, see RunLink
constexpr std::size_t SERVICE_MAX = 31 - 2 - 2 - MFG_DATA_LEN; // 15: the name and the tag share the scan response
char sService[32] = {};                                        // the name the app lists
char sPop[16] = {};                                            // the one-time code
portMUX_TYPE sLinkLock = portMUX_INITIALIZER_UNLOCKED;         // sService, sPop: written by one request, read by others
SemaphoreHandle_t sLinkDone = nullptr;
wifi_sta_config_t sCred = {};
bool sJoined = false; // the app's Wi-Fi credentials worked

void OnProvisioning(void*, esp_event_base_t, int32_t pId, void* pData)
{
    switch(pId)
    {
        case NETWORK_PROV_WIFI_CRED_RECV: std::memcpy(&sCred, pData, sizeof sCred); break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS: // the app set the Wi-Fi too: keep it as ReefDO's own
            sJoined = true;
            net::RememberCredentials(
                std::string_view(reinterpret_cast<const char*>(sCred.ssid),
                                 strnlen(reinterpret_cast<const char*>(sCred.ssid), sizeof sCred.ssid)),
                std::string_view(reinterpret_cast<const char*>(sCred.password),
                                 strnlen(reinterpret_cast<const char*>(sCred.password), sizeof sCred.password)));
            break;
        case NETWORK_PROV_END: xSemaphoreGive(sLinkDone); break;
        default: break;
    }
}

// The Bluetooth window: the app claims the board (its certificate lands in fctry), maps it to the user and may
// send Wi-Fi credentials. Bluetooth runs only for this. The provisioning manager takes the station over while it
// runs (the board is off Wi-Fi meanwhile; the ladder and the relays are not affected); afterwards ReefDO's own
// Wi-Fi, or the one the app set, is back.
void RunLink()
{
    if(sLinkDone == nullptr) sLinkDone = xSemaphoreCreateBinary();
    xSemaphoreTake(sLinkDone, 0); // a late END from an earlier window must not close this one
    sJoined = false;
    ESP_LOGW(TAG, "link window open for %lu min: ESP RainMaker Home → add device → %s, code %s",
             static_cast<unsigned long>(LINK_WINDOW_S / 60), sService, sPop);
    net::PauseStation();
    esp_rmaker_pre_prov_init();
    network_prov_mgr_config_t cfg = {};
    cfg.scheme = network_prov_scheme_ble;
    cfg.scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE;
    if(network_prov_mgr_init(cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "provisioning did not start");
        esp_rmaker_pre_prov_deinit();
        net::ResumeStation(false);
        return;
    }
    // The Neo app lists only boards with this in the scan response (rmaker_app_network's layout): Espressif's
    // company ID, "RMNG", customer 1, device type, subtype, extra (no known kind: the app's default icon).
    std::array<uint8_t, MFG_DATA_LEN> mfg = {0xe5, 0x02, 'R', 'M', 'N', 'G', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    if(network_prov_scheme_ble_set_mfg_data(mfg.data(), mfg.size()) != ESP_OK) // copied (12 B kept per window)
        ESP_LOGW(TAG, "no manufacturer data: the Neo app will not list the board");
    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &OnProvisioning, nullptr);
    // Security 2 (SRP6a): the app's user is "wifiprov", the code shown on the page is the password.
    constexpr int SALT_LEN = 16;
    char* salt = nullptr;
    char* verifier = nullptr;
    int verifierLen = 0;
    const bool srp = esp_srp_gen_salt_verifier(SRP_USER, static_cast<int>(std::strlen(SRP_USER)), sPop,
                                               static_cast<int>(std::strlen(sPop)), &salt, SALT_LEN, &verifier,
                                               &verifierLen) == ESP_OK;
    network_prov_security2_params_t sec = {};
    sec.salt = salt;
    sec.salt_len = SALT_LEN;
    sec.verifier = verifier;
    sec.verifier_len = static_cast<uint16_t>(verifierLen);
    if(srp && network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, &sec, sService, nullptr) == ESP_OK &&
       xSemaphoreTake(sLinkDone, pdMS_TO_TICKS(LINK_WINDOW_S * 1000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "link window closed: nobody linked");
        network_prov_mgr_stop_provisioning();
        xSemaphoreTake(sLinkDone, pdMS_TO_TICKS(5000));
    }
    esp_rmaker_pre_prov_deinit(); // finishes the claim, or gives it up
    esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &OnProvisioning);
    network_prov_mgr_deinit();
    std::free(salt);
    std::free(verifier);
    net::ResumeStation(sJoined);
}
#endif

void WaitForNetwork()
{
    while(!net::GetStatus().connected)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

void Task(void*)
{
    WaitForNetwork();
    while(!TryStart())
    {
        sPhase = Phase::NotLinked;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // a link request (StartLink)
#if CONFIG_BT_ENABLED
        RunLink();
#endif
        sPhase = Phase::NotLinked; // the window is closed; a new request opens another
        WaitForNetwork();
    }
    uint32_t loops = 0;
    TickType_t trendsAt = xTaskGetTickCount();
    for(;;)
    {
        cl::Updates u;
        if(xQueueReceive(sUpdates, &u, pdMS_TO_TICKS(250)) == pdTRUE) Apply(u);
        Write w;
        while(xQueueReceive(sWrites, &w, 0) == pdTRUE)
        {
            const cl::Updates r = [&w]
            {
                sampler::Guard guard;
                return sAgent.Write(sampler::App(), sampler::ClockNow(), platform::Get(), w.param, w.value, sScratch);
            }();
            Apply(r);
        }
        if(xTaskGetTickCount() - trendsAt >= pdMS_TO_TICKS(5000)) // the trend rows: each changes ≤ once a minute
        {
            trendsAt = xTaskGetTickCount();
            const cl::TrendUpdates t = []
            {
                sampler::Guard guard;
                return sAgent.Trends(sampler::App(), sampler::ClockNow(), sPhase == Phase::Online);
            }();
            for(const cl::TrendUpdate& row : t)
                esp_rmaker_param_update(sParams[static_cast<std::size_t>(row.param)],
                                        esp_rmaker_str(row.text.c_str())); // copied
        }
        if(++loops % 2400 == 0 && uxTaskGetStackHighWaterMark(nullptr) < 1024) // every ~10 min
            ESP_LOGW(TAG, "cloud task stack: %u bytes left",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
}

#endif // CONFIG_REEFDO_CLOUD

} // namespace

void Start()
{
#if CONFIG_REEFDO_CLOUD
    sUpdates = xQueueCreate(4, sizeof(cl::Updates));
    sWrites = xQueueCreate(8, sizeof(Write));
    xTaskCreatePinnedToCore(Task, "cloud", 10240, nullptr, 2, &sTask, 0); // TLS publishes run on it
#endif
}

void OnSample(const reefdo::app::App& pApp, const reefdo::app::Clock& pClock,
              std::span<const reefdo::app::Notification> pNotes)
{
#if CONFIG_REEFDO_CLOUD
    if(!sRunning) return;
    const cl::Updates u = sAgent.Step(pApp, pClock, pNotes, sPhase == Phase::Online);
    if(!u.empty() && xQueueSend(sUpdates, &u, 0) != pdTRUE) ESP_LOGW(TAG, "update for the app dropped: queue full");
#else
    static_cast<void>(pApp);
    static_cast<void>(pClock);
    static_cast<void>(pNotes);
#endif
}

reefdo::gateway::CloudLink StartLink()
{
    reefdo::gateway::CloudLink l;
#if !CONFIG_REEFDO_CLOUD
    l.message.assign("this firmware is built without the phone app");
#elif !CONFIG_BT_ENABLED
    l.message.assign("this firmware is built without Bluetooth");
#else
    const Phase now = sPhase.load();
    if(!HaveFactoryPartition())
    {
        l.message.assign("needs the new partition table: flash once over USB");
    }
    else if(now == Phase::Online || now == Phase::Offline)
    {
        l.message.assign("already linked (another account: cloud forget on the console)");
    }
    else if(now == Phase::Linking)
    {
        taskENTER_CRITICAL(&sLinkLock);
        l.service.assign(sService);
        l.pop.assign(sPop);
        taskEXIT_CRITICAL(&sLinkLock);
        l.ok = true;
        l.message.assign("the link window is already open");
    }
    else
    {
        char service[sizeof sService];
        char pop[sizeof sPop];
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        std::snprintf(service, SERVICE_MAX + 1, "PROV_ReefDO%02x%02x", mac[4], mac[5]); // the apps list PROV_*
        std::snprintf(pop, sizeof pop, "%08lx", static_cast<unsigned long>(esp_random()));
        Phase expected = Phase::NotLinked;
        if(!sPhase.compare_exchange_strong(expected, Phase::Linking))
        {
            l.message.assign(expected == Phase::Linking ? "the link window is just opening: try again"
                                                        : "not ready: waiting for the network");
        }
        else
        {
            taskENTER_CRITICAL(&sLinkLock); // published only by the one request that won
            std::memcpy(sService, service, sizeof sService);
            std::memcpy(sPop, pop, sizeof sPop);
            taskEXIT_CRITICAL(&sLinkLock);
            xTaskNotifyGive(sTask);
            l.ok = true;
            l.service.assign(service);
            l.pop.assign(pop);
            l.message.assign("ESP RainMaker Home: add device, this name, this code");
        }
    }
#endif
    return l;
}

bool Forget()
{
#if CONFIG_REEFDO_CLOUD
    return HaveFactoryPartition() && nvs_flash_erase_partition("fctry") == ESP_OK;
#else
    return false;
#endif
}

const char* State()
{
    static const char* const NAMES[] = {"off", "not linked", "linking", "offline", "online"};
    return NAMES[static_cast<int>(sPhase.load())];
}

uint32_t Pushes()
{
    return sPushes;
}

} // namespace cloud_link
