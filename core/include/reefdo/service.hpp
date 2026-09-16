#pragma once
// The daily service run: exercise each device with serviceS > 0 inside the evening window and measure the
// tank's response against the pre-run trend. Pure function of (state, input, config).
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "reefdo/filter.hpp"
#include "reefdo/fixed_vector.hpp"
#include "reefdo/ladder.hpp"

namespace reefdo::service
{

constexpr std::size_t DEVICES = ladder::DEVICES;

enum class Outcome : uint8_t
{
    None,
    Pass,
    Fail,
    Inconclusive,
    Unchecked
};
enum class Skip : uint8_t
{
    NotNormal,
    Fault,
    Maintenance,
    Missed,
    NothingToRun
};
enum class Phase : uint8_t
{
    Idle,
    Deficit,
    Running,
    Tail,
    Settle
};

struct DeviceConfig
{
    uint32_t serviceS = 0;       // 0 = not exercised
    float minResponsePct = 0.0f; // 0 = run but don't judge
};

struct Config
{
    uint16_t windowStartMin = 19 * 60 + 30;
    uint16_t windowEndMin = 21 * 60;
    uint32_t settleS = 120;
    uint32_t tailS = 60;
    float minHeadroomPct = 3.0f;
    uint32_t inconclusiveDays = 5;
    bool chirp = true;
    uint32_t induceDeficitS = 0;
    uint32_t induceDeficitDevice = 0; // 1..6
    std::array<DeviceConfig, DEVICES> devices{};
};

struct LocalTime
{
    uint16_t minuteOfDay; // local minutes after midnight
    uint32_t dayIndex;    // local days since the epoch
};

struct Input
{
    uint64_t nowMs = 0;
    std::optional<LocalTime> local; // nullopt = clock unknown
    std::optional<float> satPct;    // median-filtered saturation; nullopt = probe failed
    ladder::Level level = ladder::Level::Normal;
    bool fault = false;
    bool maintenance = false;
    bool runNow = false; // UI / console "Run now"
};

enum class EventType : uint8_t
{
    RunStart,          // aux: judged (1) / not judged (0), clock_unknown flag in `clock_unknown`
    DeviceStart,       // device
    DeviceEnd,         // device, outcome, response
    RunEnd,            // aborted flag in `aborted`
    Skipped,           // skip reason
    FailAlert,         // device: a check failed (raised once per run that fails)
    FailCleared,       // device: a later run passed
    InconclusiveAlert, // inconclusive_days in a row
};

struct Event
{
    EventType type;
    uint8_t device = 0;
    Outcome outcome = Outcome::None;
    float response = 0.0f;
    Skip skip = Skip::NothingToRun;
    bool clockUnknown = false;
    bool aborted = false;
    bool judged = false;
};

struct Output
{
    std::array<bool, DEVICES> deviceOn{}; // service demands (merged with the ladder's by the app)
    std::optional<uint8_t> cutDevice;     // induced deficit in progress: force this device off
    bool running = false;                 // LED white pulse
    bool chirp = false;                   // one tick, at run start (if configured)
    std::array<bool, DEVICES> failed{};   // → ladder::Input::device_failed
    bool inconclusiveAlert = false;
    FixedVector<Event, 8> events;
};

// What survives a reboot (the app stores it in NVS).
struct Persistent
{
    std::array<Outcome, DEVICES> lastOutcome{};
    std::array<float, DEVICES> lastResponse{};
    std::array<bool, DEVICES> failActive{};
    uint32_t inconclusiveStreak = 0;
    bool inconclusiveAlert = false;
    std::optional<uint32_t> lastRunDay;
    bool operator==(const Persistent&) const = default;
};

struct State
{
    Persistent p;
    Phase phase = Phase::Idle;
    std::size_t device = 0; // index being exercised
    uint64_t phaseStartedMs = 0;
    uint64_t runStartedMs = 0;
    uint64_t deviceStartedMs = 0;
    bool deviceJudged = false; // headroom available and a reading present when this device started
    bool judged = false;       // headroom was available at run start
    bool manual = false;
    bool clockUnknown = false;
    bool aborted = false;
    float sat0 = 0.0f;   // baseline at device start
    float slope0 = 0.0f; // %/10 min at device start
    float peak = 0.0f;   // peak |deviation| during run + tail
    std::optional<uint64_t> lastRunMs;
    filter::SlopeEstimator slope{600'000};
    std::optional<uint32_t> attemptedDay; // the day a window attempt was made (missed or started)
};

// Total seconds a full run needs (all serviced devices, gaps, tail, deficit).
uint32_t RunDurationS(const Config& pCfg);

Output Step(State& pS, const Input& pIn, const Config& pCfg);

} // namespace reefdo::service
