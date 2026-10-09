#include "sampler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>

#include <sys/time.h>

#include "reefdo/gateway/views.hpp"

#include "board.hpp"
#include "cloud_link.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/flash_store.hpp"
#include "hal/nvs_store.hpp"
#include "net.hpp"
#include "sdkconfig.h"

namespace sampler
{

namespace
{

const char* const TAG = "sampler";
constexpr uint32_t UNIX_KNOWN_FROM = 1'600'000'000u; // anything earlier is the 1970 default, not a set clock
constexpr uint32_t TEST_BLOB_MAGIC = 0x53564331;     // "SVC1": layout tag for the persisted test outcomes

// NVS keys
const char* const KEY_CONFIG = "cfg";
const char* const KEY_TEST = "svc"; // the old name, so the stored test history survives the rename
const char* const KEY_PROBE = "probe";
const char* const KEY_TZ = "tz";

struct TestBlob
{
    uint32_t magic;
    uint32_t size;
    reefdo::selftest::Persistent p;
};

// selftest::Persistent as it was with six fixed devices (up to 1.1.0-rc1): loaded once, saved in the new layout.
constexpr std::size_t SIX = 6;
struct PersistentSix
{
    std::array<reefdo::selftest::Outcome, SIX> lastOutcome;
    std::array<float, SIX> lastResponse;
    std::array<bool, SIX> failActive;
    uint32_t inconclusiveStreak;
    bool inconclusiveAlert;
    std::optional<uint32_t> lastRunDay;
};
struct TestBlobSix
{
    uint32_t magic;
    uint32_t size;
    PersistentSix p;
};
static_assert(sizeof(TestBlob) >= sizeof(TestBlobSix), "one buffer reads both layouts");

static_assert(static_cast<std::size_t>(board::RELAY_COUNT) == reefdo::RELAYS, "the core's relay count is the board's");

// A health report on its way into the log (ReportSystem → the sampler's tick).
struct SystemReport
{
    reefdo::app::SystemEvent event;
    float f0, f1, f2;
};

struct Host
{
    hal::FlashStore a, b, e, d;
    reefdo::config::Config cfg;
    std::optional<reefdo::sim::Tank> tank;
    std::optional<reefdo::sim::Probe> simProbe;
    hal::Rs485Uart uart;
    std::optional<reefdo::probe::Rk500> rk500;
    std::optional<reefdo::app::App> app;
#if CONFIG_REEFDO_QEMU
    ProbeSource source = ProbeSource::Sim; // QEMU has no probe: the virtual tank
#else
    ProbeSource source = ProbeSource::Rk500; // the real probe unless NVS says otherwise
#endif
    int32_t tz = 0;
    Indication ind;                                      // the indicator's snapshot, under indLock (not the mutex)
    portMUX_TYPE indLock = portMUX_INITIALIZER_UNLOCKED; // the LED task reads it 20 times a second
    bool hung = false;
    bool imageValid = false;
    uint32_t heapHour = UINT32_MAX; // the uptime hour the heap was last logged in
    SemaphoreHandle_t mtx = nullptr;
    QueueHandle_t reports = nullptr;    // SystemReport, from any task
    char json[reefdo::config::DOC_MAX]; // scratch for config documents (under the lock)
    char configState[160] = "defaults"; // "stored", "defaults" or "rejected at <path>: <message>"
};
Host sHost;

bool LoadConfig()
{
    sHost.cfg = reefdo::config::Defaults();
    std::size_t len = 0;
    if(!hal::nvs::GetBlob(KEY_CONFIG, std::span<uint8_t>(reinterpret_cast<uint8_t*>(sHost.json), sizeof sHost.json),
                          len))
    {
        ESP_LOGI(TAG, "no stored config: factory defaults");
        std::strcpy(sHost.configState, "defaults");
        return false;
    }
    const reefdo::config::LoadResult r = reefdo::config::Load(std::string_view(sHost.json, len), sHost.cfg);
    if(!r.Ok())
    {
        ESP_LOGE(TAG, "stored config rejected at %s: %s — factory defaults", r.path.c_str(), r.message.c_str());
        sHost.cfg = reefdo::config::Defaults();
        std::snprintf(sHost.configState, sizeof sHost.configState, "rejected at %s: %s", r.path.c_str(),
                      r.message.c_str());
        return false;
    }
    std::strcpy(sHost.configState, "stored");
    return true;
}

bool SaveConfig()
{
    const std::size_t n = reefdo::config::Write(sHost.cfg, sHost.json);
    return n > 0 &&
           hal::nvs::SetBlob(KEY_CONFIG, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sHost.json), n));
}

void LoadTest()
{
    TestBlob blob{};
    std::size_t len = 0;
    if(!hal::nvs::GetBlob(KEY_TEST, std::span<uint8_t>(reinterpret_cast<uint8_t*>(&blob), sizeof blob), len)) return;
    if(len == sizeof blob && blob.magic == TEST_BLOB_MAGIC && blob.size == sizeof blob.p)
    {
        sHost.app->RestoreTest(blob.p);
        return;
    }
    TestBlobSix six{};
    std::memcpy(&six, &blob, sizeof six);
    if(len != sizeof six || six.magic != TEST_BLOB_MAGIC || six.size != sizeof six.p)
    {
        ESP_LOGW(TAG, "stored test history has another layout: dropped");
        return;
    }
    reefdo::selftest::Persistent p;
    for(std::size_t i = 0; i < SIX && i < reefdo::DEVICES; ++i)
    {
        p.lastOutcome[i] = six.p.lastOutcome[i];
        p.lastResponse[i] = six.p.lastResponse[i];
        p.failActive[i] = six.p.failActive[i];
    }
    p.inconclusiveStreak = six.p.inconclusiveStreak;
    p.inconclusiveAlert = six.p.inconclusiveAlert;
    p.lastRunDay = six.p.lastRunDay;
    sHost.app->RestoreTest(p);
    ESP_LOGI(TAG, "test history carried over from the six-device layout");
}

void SaveTest()
{
    TestBlob blob{TEST_BLOB_MAGIC, sizeof(reefdo::selftest::Persistent), sHost.app->TestPersistent()};
    if(!hal::nvs::SetBlob(KEY_TEST, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&blob), sizeof blob)))
    {
        ESP_LOGE(TAG, "test history not saved");
    }
}

reefdo::probe::Config ProbeConfig()
{
    reefdo::probe::Config p;
    p.stuckSamples = sHost.cfg.stuckMinutes * 60 / (sHost.cfg.samplePeriodS > 0 ? sHost.cfg.samplePeriodS : 10);
    p.allowZeroCal = sHost.cfg.allowZeroCal;
    return p;
}

reefdo::probe::IProbe& BuildProbe()
{
    if(sHost.source == ProbeSource::Rk500)
    {
        sHost.uart.Init(UART_NUM_1, board::RS485_TX_PIN, board::RS485_RX_PIN, 9600);
        sHost.rk500.emplace(sHost.uart, ProbeConfig());
        ESP_LOGI(TAG, "probe: RK500-04 on RS485");
        return *sHost.rk500;
    }
    reefdo::sim::TankConfig t;
    t.kDevicePerH.fill(2.0f); // any assigned device visibly moves the virtual tank
    t.salinityPsu = sHost.cfg.salinityPsu;
    sHost.tank.emplace(t);
    reefdo::sim::ProbeModel pm;
    pm.dtS = static_cast<float>(sHost.cfg.samplePeriodS);
    sHost.simProbe.emplace(*sHost.tank, pm, static_cast<uint64_t>(esp_timer_get_time()) | 1u);
    ESP_LOGW(TAG, "probe: SIMULATED tank (no sensor) — `probe rk500` to switch");
    return *sHost.simProbe;
}

uint16_t LocalMinute(const reefdo::app::Clock& pC)
{
    if(!pC.unixS) return static_cast<uint16_t>((pC.nowMs / 60000) % 1440);
    const int64_t l = static_cast<int64_t>(*pC.unixS) + pC.tzOffsetS;
    return static_cast<uint16_t>(((l % 86400 + 86400) % 86400) / 60);
}

void LogNotification(const reefdo::app::Notification& pN)
{
    static const char* const KIND[] = {
        "level", "fault",       "fault-cleared", "recovered", "test-fail", "test-inconclusive",
        "boot",  "device-lost", "device-back",   "test-alert"};
    static_assert(std::size(KIND) == static_cast<std::size_t>(reefdo::app::NotifyKind::TestAlert) + 1);
    ESP_LOGW(TAG, "notify %s%s: level=%d device=%u value=%.2f%s", KIND[static_cast<int>(pN.kind)],
             pN.repeat ? " (repeat)" : "", static_cast<int>(pN.level), pN.device, static_cast<double>(pN.value),
             pN.urgent ? " URGENT" : "");
}

// A freshly flashed image has until here to prove itself: ten good polls and two minutes, then no rollback.
void MarkImageValid(const reefdo::app::Status& pS)
{
    if(sHost.imageValid) return;
    if(pS.probe != reefdo::probe::Status::Ok || pS.ticks < 10 || pS.uptimeS < 120) return;
    sHost.imageValid = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if(esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image on %s confirmed: %s", running->label, esp_err_to_name(err));
    }
}

// One sample period, under the lock: virtual tank → App → relays → indication → persistence.
void ApplyRelaysLocked()
{
    const reefdo::app::Status& s = sHost.app->GetStatus();
    for(std::size_t i = 0; i < reefdo::RELAYS; ++i)
        board::SetRelay(static_cast<int>(i), s.relayEnergised[i]);
}

// What the other tasks reported since the last sample, and the heap once an hour (the first right after the boot),
// with the tightest stack margin of the tasks that do the network work.
void LogHealthLocked(const reefdo::app::Clock& pClock)
{
    SystemReport r{};
    while(xQueueReceive(sHost.reports, &r, 0) == pdTRUE)
        sHost.app->LogSystem(pClock, r.event, r.f0, r.f1, r.f2);
    const uint32_t hour = sHost.app->GetStatus().uptimeS / 3600u;
    if(hour == sHost.heapHour) return;
    sHost.heapHour = hour;
    const uint32_t stack = std::min(
        {static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)), net::StackLeft(), cloud_link::StackLeft()});
    sHost.app->LogSystem(pClock, reefdo::app::SystemEvent::Heap, static_cast<float>(esp_get_free_heap_size()) / 1024.0f,
                         static_cast<float>(esp_get_minimum_free_heap_size()) / 1024.0f,
                         static_cast<float>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)) / 1024.0f,
                         static_cast<float>(stack));
}

void TickLocked()
{
    const reefdo::app::Clock c = ClockNow();
    if(sHost.tank)
        sHost.tank->Step(static_cast<float>(sHost.cfg.samplePeriodS), LocalMinute(c), sHost.app->GetStatus().deviceOn);
    sHost.app->Tick(c);
    const reefdo::app::Status& s = sHost.app->GetStatus();
    ApplyRelaysLocked();
    LogHealthLocked(c);

    Indication ind = sHost.ind;
    ind.level = s.level;
    ind.sound = s.sound;
    ind.fault = s.fault;
    ind.maintenance = s.maintenance;
    ind.testRunning = s.testRunning;
    ind.anyFailed = false;
    for(const bool f : s.deviceFailed)
        ind.anyFailed |= f;
    ind.chirps += s.chirp ? 1u : 0u;
    ind.signals = sHost.app->GetConfig().signals;
    taskENTER_CRITICAL(&sHost.indLock);
    sHost.ind = ind;
    taskEXIT_CRITICAL(&sHost.indLock);

    const reefdo::app::Notifications notes = sHost.app->TakeNotifications();
    for(const reefdo::app::Notification& n : notes)
        LogNotification(n);
    cloud_link::OnSample(*sHost.app, c, std::span<const reefdo::app::Notification>(notes.begin(), notes.size()));
    if(sHost.app->TestPersistentChanged()) SaveTest();
    MarkImageValid(s);
    // The quiet hour's safety net; never while the phone app's Bluetooth window is open (the App cannot see that).
    if(s.restartDue && !cloud_link::Linking()) Restart(reefdo::app::RestartWhy::Daily);
}

void Task(void*)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    TickType_t last = xTaskGetTickCount();
    for(;;)
    {
        uint32_t periodS = 10;
        {
            Guard guard;
            TickLocked();
            periodS = sHost.cfg.samplePeriodS;
        }
        if(!sHost.hung) esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(periodS * 1000));
    }
}

} // namespace

Guard::Guard()
{
    xSemaphoreTakeRecursive(sHost.mtx, portMAX_DELAY);
}
Guard::~Guard()
{
    xSemaphoreGiveRecursive(sHost.mtx);
}

void Guard::Unlock()
{
    xSemaphoreGiveRecursive(sHost.mtx);
}
void Guard::Lock()
{
    xSemaphoreTakeRecursive(sHost.mtx, portMAX_DELAY);
}

reefdo::app::App& App()
{
    return *sHost.app;
}

reefdo::app::Clock ClockNow()
{
    reefdo::app::Clock c;
    c.nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const time_t now = time(nullptr);
    if(now >= static_cast<time_t>(UNIX_KNOWN_FROM)) c.unixS = static_cast<uint32_t>(now);
    c.tzOffsetS = sHost.tz;
    return c;
}

Indication GetIndication()
{
    taskENTER_CRITICAL(&sHost.indLock);
    const Indication ind = sHost.ind;
    taskEXIT_CRITICAL(&sHost.indLock);
    return ind;
}

ProbeSource GetProbeSource()
{
    return sHost.source;
}

bool SetProbeSource(ProbeSource pSrc)
{
    return hal::nvs::SetI32(KEY_PROBE, static_cast<int32_t>(pSrc));
}

reefdo::sim::Tank* Tank()
{
    return sHost.tank ? &*sHost.tank : nullptr;
}
hal::Rs485Uart& Uart()
{
    return sHost.uart;
}

bool ApplyConfigJson(std::string_view pJson, reefdo::config::LoadResult& pOut)
{
    Guard guard;
    pOut = reefdo::gateway::ApplyConfig(*sHost.app, pJson, ClockNow());
    if(!pOut.Ok()) return false;
    sHost.cfg = sHost.app->GetConfig();
    if(sHost.rk500) sHost.rk500->SetConfig(ProbeConfig()); // stuck_minutes, sample_period_s, allow_zero_cal
    if(!SaveConfig())
    {
        ESP_LOGE(TAG, "config applied but not saved");
        std::strcpy(sHost.configState, "unsaved"); // the page warns: what runs is not what the flash holds
        pOut.error = reefdo::config::LoadError::Invalid;
        pOut.path.assign("nvs");
        pOut.message.assign("applied, but not saved to flash: lost at the next reboot");
        return false;
    }
    std::strcpy(sHost.configState, "stored");
    return true;
}

bool ResetConfig()
{
    Guard guard;
    sHost.cfg = reefdo::config::Defaults();
    sHost.app->SetConfig(sHost.cfg, ClockNow());
    if(sHost.rk500) sHost.rk500->SetConfig(ProbeConfig());
    std::strcpy(sHost.configState, "defaults");
    return hal::nvs::EraseKey(KEY_CONFIG);
}

std::size_t ConfigJson(std::span<char> pOut)
{
    Guard guard;
    return reefdo::config::Write(sHost.app->GetConfig(), pOut, true); // local keys stay on the board
}

void SetTime(uint32_t pUnixS, std::optional<int32_t> pTzOffsetS)
{
    Guard guard;
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(pUnixS);
    settimeofday(&tv, nullptr);
    if(pTzOffsetS)
    {
        sHost.tz = *pTzOffsetS;
        hal::nvs::SetI32(KEY_TZ, sHost.tz);
    }
}

int32_t TzOffsetS()
{
    return sHost.tz;
}

const char* ConfigState()
{
    return sHost.configState;
}

void ApplyRelays()
{
    Guard guard;
    ApplyRelaysLocked();
}

void Hang()
{
    ESP_LOGE(TAG, "sampler will stop feeding the watchdog: expect a panic and a reboot");
    sHost.hung = true;
}

void ReportSystem(reefdo::app::SystemEvent pEvent, float pF0, float pF1, float pF2)
{
    const SystemReport r{pEvent, pF0, pF1, pF2};
    if(sHost.reports == nullptr || xQueueSend(sHost.reports, &r, 0) != pdTRUE)
        ESP_LOGW(TAG, "health report %d dropped", static_cast<int>(pEvent));
}

void Restart(reefdo::app::RestartWhy pWhy)
{
    static const char* const WHY[] = {"command", "daily", "web server dead", "firmware update"};
    static_assert(std::size(WHY) == static_cast<std::size_t>(reefdo::app::RestartWhy::Update) + 1);
    {
        Guard guard;
        sHost.app->LogSystem(ClockNow(), reefdo::app::SystemEvent::Restart, static_cast<float>(pWhy));
    }
    ESP_LOGW(TAG, "restarting: %s", WHY[static_cast<int>(pWhy)]);
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000)); // the record is on flash already; the answer on the wire gets out
    // A dead web server is the one case worth a panic: the core dump on flash keeps every task's stack, the
    // httpd's included, for `idf.py coredump-info`; a clean restart would throw the evidence away.
    if(pWhy == reefdo::app::RestartWhy::WebDead) abort();
    esp_restart();
}

bool Start()
{
    sHost.mtx = xSemaphoreCreateRecursiveMutex();
    sHost.reports = xQueueCreate(16, sizeof(SystemReport));
    Guard guard;

    int32_t v = 0;
    if(hal::nvs::GetI32(KEY_PROBE, v)) sHost.source = v == 0 ? ProbeSource::Sim : ProbeSource::Rk500;
    if(hal::nvs::GetI32(KEY_TZ, v)) sHost.tz = v;
    LoadConfig();

    // Segment = erase block: 64 KB for the two big rings, 16 KB for the small ones so a drop costs little.
    bool storageOk = true;
    storageOk &= sHost.a.Open("loga", 64 * 1024);
    storageOk &= sHost.b.Open("logb", 64 * 1024);
    storageOk &= sHost.e.Open("loge", 16 * 1024);
    storageOk &= sHost.d.Open("logd", 16 * 1024);

    reefdo::probe::IProbe& probe = BuildProbe();
    sHost.app.emplace(sHost.cfg, probe, reefdo::app::Stores{sHost.a, sHost.b, sHost.e, sHost.d});
    LoadTest();
    sHost.app->Start(ClockNow(), static_cast<uint32_t>(esp_reset_reason()));
    TickLocked(); // outputs settle before anything else runs

    xTaskCreatePinnedToCore(Task, "sampler", 8192, nullptr, 5, nullptr, 1);
    ESP_LOGI(TAG, "app_main stack left: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    ESP_LOGI(TAG, "running: %lu s period, %u+%u+%u+%u records logged so far",
             static_cast<unsigned long>(sHost.cfg.samplePeriodS), static_cast<unsigned>(sHost.app->LogA().Count()),
             static_cast<unsigned>(sHost.app->LogB().Count()), static_cast<unsigned>(sHost.app->LogE().Count()),
             static_cast<unsigned>(sHost.app->LogD().Count()));
    return storageOk;
}

} // namespace sampler
