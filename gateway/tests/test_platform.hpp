#pragma once
// A gateway::IPlatform that records what it was asked to do: the tests' stand-in for the board.
#include <optional>
#include <string>
#include <vector>

#include "reefdo/gateway/router.hpp"

#include "scenario.hpp"

namespace testing
{

using namespace reefdo::gateway;
using scenario::App;
using scenario::Clock;

struct Platform : IPlatform
{
    SysInfo sys;
    bool acceptWifi = true;
    bool acceptPassword = true;
    std::string ssid, pass, password;
    std::optional<uint32_t> unix;
    std::optional<int32_t> tz;
    int outputsChanged = 0;
    std::vector<std::string> configs;
    std::optional<reefdo::config::BuzzerPattern> pattern;
    uint32_t volume = 0, hz = 0, seconds = 0;
    App* app = nullptr;
    Clock* clock = nullptr;

    SysInfo Sys() override { return sys; }
    reefdo::config::LoadResult ApplyConfig(std::string_view pJson) override
    {
        configs.emplace_back(pJson);
        return reefdo::gateway::ApplyConfig(*app, pJson, *clock);
    }
    void SetTime(uint32_t pUnixS, std::optional<int32_t> pTz) override
    {
        unix = pUnixS;
        tz = pTz;
    }
    void OutputsChanged() override { ++outputsChanged; }
    bool SetWifi(std::string_view pSsid, std::string_view pPass) override
    {
        ssid = pSsid;
        pass = pPass;
        return acceptWifi;
    }
    bool SetPassword(std::string_view pPw) override
    {
        password = pPw;
        return acceptPassword;
    }
    void SetMuted(bool pMuted) override { sys.muted = pMuted; }
    CloudLink link;
    int links = 0;
    CloudLink StartCloudLink() override
    {
        ++links;
        return link;
    }
    void TestBuzzer(reefdo::config::BuzzerPattern pP, uint32_t pVol, uint32_t pHz, uint32_t pS) override
    {
        pattern = pP;
        volume = pVol;
        hz = pHz;
        seconds = pS;
    }
};

} // namespace testing
