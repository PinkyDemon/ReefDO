#pragma once
// The device configuration: one struct, one JSON document, one Validate().
// Load() is all-or-nothing; unknown keys are ignored, missing keys keep their defaults.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/correction.hpp"
#include "reefdo/fixed_string.hpp"
#include "reefdo/ladder.hpp"

namespace reefdo::config
{

constexpr std::size_t DEVICES = ladder::DEVICES;

// Per-device settings the ladder does not need (its own part lives in ladder::Config::devices[i]).
struct DeviceSettings
{
    FixedString<24> name;
    bool wiredNc = false;        // NC: on when the coil is de-energised = on when the controller is dead
    uint32_t serviceS = 0;       // daily service run duration; 0 = not exercised
    float minResponsePct = 0.0f; // saturation-% response the check demands; 0 = run but don't judge
    bool operator==(const DeviceSettings&) const = default;
};

struct ServiceConfig
{
    uint16_t windowStartMin = 19 * 60 + 30; // local minutes after midnight
    uint16_t windowEndMin = 21 * 60;
    uint32_t settleS = 120;
    uint32_t tailS = 60;
    float minHeadroomPct = 3.0f;
    uint32_t inconclusiveDays = 5;
    bool chirp = true;
    uint32_t induceDeficitS = 0;      // 0 = off
    uint32_t induceDeficitDevice = 0; // 1..6; only meaningful when induce_deficit_s > 0
    bool operator==(const ServiceConfig&) const = default;
};

struct NtfyConfig
{
    bool enabled = false;
    FixedString<48> topic;
    ladder::Level minLevel = ladder::Level::Blue;
    bool operator==(const NtfyConfig&) const = default;
};

// How a state sounds. The ladder decides *whether* the buzzer may sound (Normal is silent, ack and maintenance mute);
// these decide *how*. LED codes are fixed, only their brightness is configurable.
enum class BuzzerPattern : uint8_t
{
    Off,
    Chirp,
    Beep,
    Double,
    Triple,
    Continuous
};
struct Signal
{
    BuzzerPattern buzzer = BuzzerPattern::Off;
    uint32_t volume = 0; // 0 silent .. 3 loud
    bool operator==(const Signal&) const = default;
};

struct SignalsConfig
{
    uint32_t buzzerHz = 2400;
    uint32_t ledBrightness = 40; // percent
    Signal normal{BuzzerPattern::Off, 0};
    Signal blue{BuzzerPattern::Off, 0};
    Signal yellow{BuzzerPattern::Beep, 2};
    Signal red{BuzzerPattern::Continuous, 3};
    Signal fault{BuzzerPattern::Triple, 2};
    bool operator==(const SignalsConfig&) const = default;

    // The signal for what the ladder is showing: fault wins, then the effective level.
    const Signal& of(ladder::Level pEffective, bool pFault) const;
};

struct Config
{
    uint32_t schema = 1;
    uint32_t samplePeriodS = 10;
    uint32_t stuckMinutes = 30;
    bool allowZeroCal = false;
    float salinityPsu = 35.0f; // feeds only the UI hint for correction.scale
    Correction correction;
    ladder::Config ladder;
    std::array<DeviceSettings, DEVICES> devices;
    ServiceConfig service;
    NtfyConfig ntfy;
    SignalsConfig signals;
    bool operator==(const Config&) const = default;
};

// Factory defaults: every device unassigned ("device N", NO, trigger none) — a fresh board switches nothing.
Config Defaults();

enum class LoadError : uint8_t
{
    None,
    InvalidJson, // not parseable
    NotAnObject, // parseable, but the root is not an object
    WrongType,   // a key holds the wrong JSON type
    BadValue,    // right type, unusable content (unknown enum name, bad HH:MM, string too long)
    Invalid,     // mapped fine, but validate() rejected it
};

struct LoadResult
{
    LoadError error = LoadError::None;
    FixedString<48> path; // dotted path of the offending key
    FixedString<96> message;
    bool Ok() const { return error == LoadError::None; }
};

LoadResult Load(std::string_view pJson, Config& pOut);                      // defaults + document
LoadResult Load(std::string_view pJson, Config& pOut, const Config& pBase); // base + document (partial updates)
LoadResult Validate(const Config& pCfg);

// Serialise to JSON. Returns bytes written (no terminator), or 0 if it does not fit.
std::size_t Write(const Config& pCfg, std::span<char> pOut);

// "HH:MM" ↔ minutes after midnight.
std::optional<uint16_t> ParseHhmm(std::string_view pS);
void FormatHhmm(uint16_t pMinutes, std::span<char, 6> pOut); // writes "HH:MM" + NUL

} // namespace reefdo::config
