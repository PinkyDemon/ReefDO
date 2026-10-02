#include "platform.hpp"

#include "cloud_link.hpp"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "indicator.hpp"
#include "net.hpp"
#include "sampler.hpp"
#include "web.hpp"

namespace platform
{

namespace
{

namespace gw = reefdo::gateway;

class Board final : public gw::IPlatform
{
public:
    gw::SysInfo Sys() override
    {
        const net::Status n = net::GetStatus();
        gw::SysInfo s;
        s.ssid.assign(n.ssid.view());
        s.connected = n.connected;
        s.ip.assign(n.ip.view());
        s.rssi = n.rssi;
        s.apActive = n.apActive;
        s.timeSynced = n.timeSynced;
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(running, &state);
        s.partition.assign(running->label);
        s.image.assign(state == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
                       : state == ESP_OTA_IMG_VALID        ? "valid"
                                                           : "undefined");
        s.heap = esp_get_free_heap_size();
        s.heapMin = esp_get_minimum_free_heap_size();
        s.probe.assign(sampler::GetProbeSource() == sampler::ProbeSource::Sim ? "sim" : "rk500");
        s.muted = indicator::Muted();
        s.config.assign(sampler::ConfigState());
        s.cloud.assign(cloud_link::State());
        s.cloudPushes = cloud_link::Pushes();
        return s;
    }
    reefdo::config::LoadResult ApplyConfig(std::string_view pJson) override
    {
        reefdo::config::LoadResult r;
        sampler::ApplyConfigJson(pJson, r);
        return r;
    }
    void SetTime(uint32_t pUnixS, std::optional<int32_t> pTzOffsetS) override { sampler::SetTime(pUnixS, pTzOffsetS); }
    void OutputsChanged() override { sampler::ApplyRelays(); }
    bool SetWifi(std::string_view pSsid, std::string_view pPassword) override
    {
        return net::SetCredentials(pSsid, pPassword);
    }
    bool SetPassword(std::string_view pPassword) override { return web::SetPassword(pPassword); }
    void SetMuted(bool pMuted) override { indicator::SetMuted(pMuted); }
    void TestBuzzer(reefdo::config::BuzzerPattern pPattern, uint32_t pVolume, uint32_t pHz, uint32_t pSeconds) override
    {
        indicator::TestBuzzer(pPattern, pVolume, pHz, pSeconds);
    }
    gw::CloudLink StartCloudLink() override { return cloud_link::StartLink(); }
};

Board sBoard;

} // namespace

reefdo::gateway::IPlatform& Get()
{
    return sBoard;
}

} // namespace platform
