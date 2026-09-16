#include "indicator.hpp"

#include <cstring>

#include "board.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/nvs_store.hpp"
#include "net.hpp"
#include "sampler.hpp"

namespace indicator
{

namespace
{

using reefdo::config::BuzzerPattern;
using reefdo::config::Signal;

const char* const TAG = "indicator";
constexpr uint32_t STEP_MS = 50;
constexpr uint32_t LONG_PRESS_MS = 3000;
const char* const KEY_MUTE = "mute";
bool sMuted = false;

struct Test
{
    BuzzerPattern pattern = BuzzerPattern::Off;
    uint32_t volume = 0;
    uint32_t hz = 2400;
    uint32_t untilMs = 0;
};
Test sTest;
uint32_t sNowMs = 0; // the indicator clock, for scheduling a test

struct Rgb
{
    uint8_t r, g, b;
};

// Triangle wave 0..1.
float Breathe(uint32_t pTMs, uint32_t pPeriodMs)
{
    const float x = static_cast<float>(pTMs % pPeriodMs) / static_cast<float>(pPeriodMs);
    return x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
}

Rgb Scale(Rgb pC, float pK)
{
    return {static_cast<uint8_t>(pC.r * pK), static_cast<uint8_t>(pC.g * pK), static_cast<uint8_t>(pC.b * pK)};
}

// Square wave: on for the first half of each period.
bool Blink(uint32_t pTMs, uint32_t pPeriodMs)
{
    return pTMs % pPeriodMs < pPeriodMs / 2;
}

// Fixed LED codes: green steady = all OK and online, green 0.5 Hz = OK but offline,
// blue 1 Hz / yellow 2 Hz / red 4 Hz = that alert, purple 2 Hz = FAULT, cyan = maintenance,
// white pulse = service run, purple flash over anything = a device failed its check.
Rgb LedFrame(const sampler::Indication& pIn, uint32_t pTMs, bool pPressed, bool pOnline)
{
    const float bright = static_cast<float>(pIn.signals.ledBrightness) / 100.0f;
    if(pPressed) return Scale({255, 255, 255}, bright);
    if(pIn.anyFailed && pTMs % 2000 < 250) return Scale({200, 0, 200}, bright);
    if(pIn.serviceRunning) return Scale({255, 255, 255}, bright * (0.2f + 0.8f * Breathe(pTMs, 1500)));
    if(pIn.maintenance) return Scale({0, 180, 180}, bright);
    if(pIn.fault) return Blink(pTMs, 250) ? Scale({0, 0, 255}, bright) : Scale({255, 0, 0}, bright); // police
    switch(pIn.effective)
    {
        case reefdo::ladder::Level::Blue: return Blink(pTMs, 1000) ? Scale({0, 0, 255}, bright) : Rgb{0, 0, 0};
        case reefdo::ladder::Level::Yellow: return Blink(pTMs, 500) ? Scale({255, 170, 0}, bright) : Rgb{0, 0, 0};
        case reefdo::ladder::Level::Red: return Blink(pTMs, 250) ? Scale({255, 0, 0}, bright) : Rgb{0, 0, 0};
        case reefdo::ladder::Level::Normal: break;
    }
    if(!pOnline && !Blink(pTMs, 2000)) return {0, 0, 0};
    return Scale({0, 255, 0}, bright);
}
// Whether a buzzer pattern is sounding at time t.
bool BuzzerOn(BuzzerPattern mP, uint32_t pTMs)
{
    switch(mP)
    {
        case BuzzerPattern::Off: return false;
        case BuzzerPattern::Chirp: return pTMs % 10000 < 100;
        case BuzzerPattern::Beep: return pTMs % 2000 < 200;
        case BuzzerPattern::Double: return pTMs % 2000 < 300 && (pTMs % 2000) % 150 < 80;
        case BuzzerPattern::Triple: return pTMs % 3000 < 600 && (pTMs % 3000) % 200 < 100;
        case BuzzerPattern::Continuous: return true;
    }
    return false;
}

void Task(void*)
{
    uint32_t tMs = 0;
    uint32_t chirpsSeen = sampler::GetIndication().chirps;
    uint32_t chirpUntil = 0;
    uint32_t pressedMs = 0;
    bool longFired = false;
    bool sounding = false;

    for(;;)
    {
        const sampler::Indication in = sampler::GetIndication();

        // Button: short press = ack, held 3 s = maintenance toggle.
        const bool pressed = board::BootButtonPressed();
        if(pressed)
        {
            pressedMs += STEP_MS;
            if(pressedMs >= LONG_PRESS_MS && !longFired)
            {
                longFired = true;
                sampler::Guard guard;
                const bool on = !sampler::App().GetStatus().maintenance;
                sampler::App().SetMaintenance(on, sampler::ClockNow());
                ESP_LOGI(TAG, "button: maintenance %s", on ? "on" : "off");
                chirpUntil = tMs + 300;
            }
        }
        else if(pressedMs > 0)
        {
            if(!longFired)
            {
                sampler::Guard guard;
                sampler::App().Ack();
                ESP_LOGI(TAG, "button: ack");
                chirpUntil = tMs + 120;
            }
            pressedMs = 0;
            longFired = false;
        }
        if(in.chirps != chirpsSeen)
        {
            chirpsSeen = in.chirps;
            chirpUntil = tMs + 120;
        }

        const Rgb c = LedFrame(in, tMs, pressed, net::GetStatus().connected);
        board::LedRgb(c.r, c.g, c.b);

        // The ladder gates the buzzer (Off when silenced, in maintenance, at Blue); config shapes it.
        const Signal& s = in.signals.of(in.effective, in.fault);
        bool on = in.buzzer != reefdo::ladder::Buzzer::Off && BuzzerOn(s.buzzer, tMs);
        uint32_t hz = in.signals.buzzerHz;
        uint32_t volume = s.volume;
        if(tMs < chirpUntil)
        { // chirps: two rising tones, quiet
            on = true;
            hz = (chirpUntil - tMs) > 60 ? 1800 : 2600;
            volume = 1;
        }
        if(sMuted) on = false;
        if(tMs < sTest.untilMs) // an explicit test plays through the mute
        {
            on = BuzzerOn(sTest.pattern, tMs);
            hz = sTest.hz;
            volume = sTest.volume;
        }
        if(on || sounding) board::BuzzerSet(on, hz, volume);
        sounding = on;

        tMs += STEP_MS;
        sNowMs = tMs;
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

} // namespace

void Start()
{
    int32_t v = 0;
    sMuted = hal::nvs::GetI32(KEY_MUTE, v) && v != 0;
    if(sMuted) ESP_LOGW(TAG, "BUZZER MUTED (buzzer unmute to restore)");
    xTaskCreatePinnedToCore(Task, "indicator", 4096, nullptr, 3, nullptr, 0);
}

void SetMuted(bool pMuted)
{
    sMuted = pMuted;
    hal::nvs::SetI32(KEY_MUTE, pMuted ? 1 : 0);
}

bool Muted()
{
    return sMuted;
}

bool ParseBuzzerPattern(const char* pName, reefdo::config::BuzzerPattern& pOut)
{
    static const char* const NAMES[] = {"off", "chirp", "beep", "double", "triple", "continuous"};
    for(std::size_t i = 0; i < 6; ++i)
    {
        if(std::strcmp(pName, NAMES[i]) == 0)
        {
            pOut = static_cast<reefdo::config::BuzzerPattern>(i);
            return true;
        }
    }
    return false;
}

void TestBuzzer(reefdo::config::BuzzerPattern pPattern, uint32_t pVolume, uint32_t pHz, uint32_t pSeconds)
{
    sTest.pattern = pPattern;
    sTest.volume = pVolume > 3 ? 3 : pVolume;
    sTest.hz = pHz < 200 ? 200 : pHz > 8000 ? 8000 : pHz;
    sTest.untilMs = sNowMs + (pSeconds > 30 ? 30 : pSeconds) * 1000;
}

} // namespace indicator
