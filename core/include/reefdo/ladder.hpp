#pragma once
// The escalation ladder: Blue / Yellow / Red with hysteresis and dwell, six generic devices.
// A pure function of (state, input, config): every relay, buzzer and LED decision comes out of Step().
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "reefdo/fixed_vector.hpp"

namespace reefdo::ladder
{

enum class Level : uint8_t
{
    Normal = 0,
    Blue = 1,
    Yellow = 2,
    Red = 3
};
enum class Trigger : uint8_t
{
    None,
    Blue,
    Yellow,
    Red,
    Heat
};
enum class Mode : uint8_t
{
    On,
    PulseOff
};
enum class Buzzer : uint8_t
{
    Off,
    Beep,
    Continuous,
    FaultTriple
};
enum class Led : uint8_t
{
    Green,
    Blue,
    Yellow,
    Red,
    Purple,
    Cyan
};

constexpr std::size_t DEVICES = 6;

struct DeviceConfig
{
    Trigger trigger = Trigger::None;
    Mode mode = Mode::On;     // PulseOff: cut once for pulse_s when its level activates (un-stall a DC pump)
    bool ackSilences = false; // an alarm device (siren/beacon): silent at Blue, silenced by Ack and in maintenance
    bool operator==(const DeviceConfig&) const = default;
};

struct LevelConfig
{
    float mgl;        // threshold
    float hysteresis; // enter below mgl - h, leave above mgl + h
    uint32_t dwellS;  // the entry condition must hold this long
    bool operator==(const LevelConfig&) const = default;
};

struct Config
{
    LevelConfig blue{5.8f, 0.15f, 120};
    LevelConfig yellow{5.2f, 0.15f, 60};
    LevelConfig red{4.5f, 0.20f, 0};
    float blueSlopeMglPer10min = 0.0f; // > 0 enables: a fall steeper than this also counts as Blue's entry condition
    uint32_t recoverSustainS = 600;    // above the exit threshold this long → one level shallower
    uint32_t ackSilenceS = 1800;
    uint32_t faultConsecutiveFailures = 5;
    bool faultAlert = true;        // off: probe failures are logged and counted, but never raise FAULT
    Level faultLevel = Level::Red; // FAULT acts as this level; never shallower than Yellow (FAULT is loud)
    uint32_t pulseS = 10;
    uint32_t pulseMinIntervalS = 1800;
    float heatOnC = 28.0f;
    float heatOffC = 27.5f;
    bool nightLock = false;           // once Blue+ triggers at night, hold at least Blue until night_end
    uint16_t nightStartMin = 21 * 60; // minutes after local midnight
    uint16_t nightEndMin = 8 * 60;
    bool unknownTimeIsNight = true;
    bool escalateIfFailed = true; // a device that failed its service check makes its level act one deeper
    std::array<DeviceConfig, DEVICES> devices{};

    const LevelConfig& GetLevel(Level pL) const; // Blue, Yellow or Red only
    bool operator==(const Config&) const = default;
};

struct Input
{
    std::optional<float> doMgl; // corrected mg/L (Correction::apply); nullopt: the probe failed this tick
    float slopeMglPer10min = 0.0f;
    std::optional<float> tempC;
    uint64_t nowMs = 0;                  // monotonic
    std::optional<uint16_t> minuteOfDay; // local time; nullopt until the clock is known
    bool ackPressed = false;
    bool maintenance = false;
    std::array<bool, DEVICES> deviceFailed{}; // last service check of that device was `fail`
};

enum class EventType : uint8_t
{
    LevelChange,     // from → to (the state machine's own level)
    EffectiveChange, // what the outputs act as (level + escalation + fault); notifications follow this one
    FaultEnter,
    FaultClear,
    Ack,
    Pulse, // device pulsed off
    HeatOn,
    HeatOff,
};

struct Event
{
    EventType type;
    Level from = Level::Normal;
    Level to = Level::Normal;
    uint8_t device = 0;
};

struct Output
{
    Level level = Level::Normal;     // state-machine level
    Level effective = Level::Normal; // level the outputs act as
    bool fault = false;
    bool silenced = false; // Ack in effect
    bool heat = false;
    std::array<bool, DEVICES> deviceOn{}; // semantic "powered"; NO/NC polarity is applied by the caller
    Buzzer buzzer = Buzzer::Off;
    Led led = Led::Green;
    FixedVector<Event, 12> events;
};

struct PulseState
{
    bool active = false;
    bool wasActive = false; // the device's level was active last tick (edge detection)
    uint64_t startedMs = 0;
    std::optional<uint64_t> lastStartMs;
};

struct State
{
    Level level = Level::Normal;
    Level effective = Level::Normal;
    bool fault = false;
    uint32_t consecutiveFailures = 0;
    std::array<std::optional<uint64_t>, 3> belowSinceMs{}; // [Blue-1, Yellow-1, Red-1]
    std::optional<uint64_t> aboveSinceMs;
    std::optional<uint64_t> ackUntilMs;
    bool heat = false;
    bool nightArmed = false;
    std::array<PulseState, DEVICES> pulse{};
};

// Local-time helper, exposed for tests and the service scheduler.
bool IsNight(std::optional<uint16_t> pMinuteOfDay, const Config& pCfg);

Output Step(State& pS, const Input& pIn, const Config& pCfg);

} // namespace reefdo::ladder
