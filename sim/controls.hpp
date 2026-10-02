#pragma once
// The simulator's controls: one text command in, a text answer out. The terminal and the /sim page share them.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "reefdo/app.hpp"
#include "reefdo/sim.hpp"

namespace sim
{

struct World
{
    reefdo::sim::Tank& tank;
    reefdo::sim::Probe& probe;
    reefdo::app::App& app;
    const reefdo::app::Clock& clock;
    double speed = 1.0;                                // simulated seconds per real second; 0 = paused
    uint64_t skipTicks = 0;                            // run this many samples as fast as possible
    std::optional<uint16_t> untilMinute{};             // ... or until the local clock reads this
    std::optional<uint64_t> offsetUntilMs{};           // a snail or a bubble on the probe ends then
    std::optional<float> dropTo{};                     // the tank is falling to this saturation...
    float dropPerS = 0.0f;                             // ...at this rate (% per second)
    float dropAt = 0.0f;                               // where the fall has got to
    bool cloudUp = true;                               // the fake cloud link
    std::string cloudView{};                           // what the phone app would show now
    std::function<std::string(int, int)> cloudWrite{}; // a write from the phone app: (cloud::Param, value)
    bool quit = false;
};

std::string Execute(World& pW, const std::string& pLine);
std::string Help();
std::string Summary(const World& pW);

// Called before every sample: ends a timed probe offset.
void BeforeSample(World& pW);
// Called after the tank's own step: a falling tank falls regardless of what runs.
void AfterTankStep(World& pW);

} // namespace sim
