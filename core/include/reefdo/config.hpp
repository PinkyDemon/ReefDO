#pragma once
// The device configuration: one struct, one JSON document, one Validate().
// Load() is all-or-nothing; unknown keys are ignored, missing keys keep their defaults.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/boost.hpp"
#include "reefdo/correction.hpp"
#include "reefdo/devices.hpp"
#include "reefdo/fixed_string.hpp"
#include "reefdo/fixed_vector.hpp"
#include "reefdo/ladder.hpp"
#include "reefdo/slot.hpp"

namespace reefdo::config
{

constexpr std::size_t WINDOWS = 4;                    // always-on windows per device
constexpr std::size_t DOC_MAX = 1024 + 512 * DEVICES; // bytes: the largest document Write() makes (tested)

// A daily span of local time; an end before the start wraps midnight. Start == end is rejected by Validate().
struct TimeWindow
{
    uint16_t startMin = 0;
    uint16_t endMin = 0;
    bool Contains(uint16_t pMinute) const;
    bool operator==(const TimeWindow&) const = default;
};

// A device's own settings; what the ladder, the self test and the boost need of it lives in their configs
// (ladder.devices[i], test.devices[i], boost.devices[i]).
struct DeviceSettings
{
    FixedString<24> name;
    slot::AnySlot slot;                       // what it switches: a relay, a plug or nothing; no two devices share one
    FixedVector<TimeWindow, WINDOWS> windows; // always on inside these; one more demand, like the ladder's
    bool operator==(const DeviceSettings&) const = default;
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
    selftest::Config test;
    NtfyConfig ntfy;
    SignalsConfig signals;
    boost::Config boost;
    bool operator==(const Config&) const = default;
};

// The names the JSON documents use for these enums.
const char* Name(ladder::Level pL);
const char* Name(ladder::Trigger pT);
const char* Name(BuzzerPattern pP);

// Factory defaults: device N on relay N (as far as there are relays), NO, trigger none — a fresh board switches
// nothing.
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
// pRedact: secret slot parameters (a plug's local key) come out as KEY_REDACTED (for anything a browser sees);
// Load keeps the stored value when it reads the placeholder back.
// when it reads that placeholder back.
constexpr const char* KEY_REDACTED = "********";
std::size_t Write(const Config& pCfg, std::span<char> pOut, bool pRedact = false);
// Every slot type's parameters as JSON Schema, {"none": {...}, "relay": {...}, "tuya": {...}}: titles, help,
// limits, defaults. The page builds a device's output editor from it. 0 if it does not fit.
std::size_t SlotSchema(std::span<char> pOut);

// "HH:MM" ↔ minutes after midnight.
std::optional<uint16_t> ParseHhmm(std::string_view pS);
void FormatHhmm(uint16_t pMinutes, std::span<char, 6> pOut); // writes "HH:MM" + NUL

} // namespace reefdo::config
