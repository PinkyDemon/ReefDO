#pragma once
// Boost: inside a daily window, run the assigned devices until DO reaches a target or the
// window ends. Once per local day; pauses for a service run or maintenance. Pure function of (state, input, config).
#include <array>
#include <cstdint>
#include <optional>

#include "reefdo/fixed_vector.hpp"
#include "reefdo/service.hpp"

namespace reefdo::boost
{

constexpr std::size_t DEVICES = service::DEVICES;

struct Config
{
    bool enabled = false;
    uint16_t windowStartMin = 19 * 60 + 30; // local minutes after midnight
    uint16_t windowEndMin = 20 * 60;
    float targetMgl = 6.5f; // stop early once DO (corrected, mg/L) is here
    std::array<bool, DEVICES> devices{}; // which devices the boost runs
    bool operator==(const Config&) const = default;
};

struct Input
{
    std::optional<service::LocalTime> local; // nullopt = clock unknown: no boost
    std::optional<float> doMgl;              // nullopt = probe failed: no boost
    bool serviceRunning = false;
    bool maintenance = false;
};

enum class EventType : uint8_t
{
    Start,     // f0 = DO at start
    Reached,   // f0 = DO when the target was met
    WindowEnd, // f0 = DO when the window closed
    Paused,    // a service run or maintenance took over; resumes if the window is still open
};

struct Event
{
    EventType type;
    float doMgl;
};

struct Output
{
    std::array<bool, DEVICES> deviceOn{};
    bool running = false;
    FixedVector<Event, 2> events;
};

struct State
{
    bool running = false;
    std::optional<uint32_t> doneDay; // local day index of the last completed boost
};

Output Step(State& pS, const Input& pIn, const Config& pCfg);

} // namespace reefdo::boost
