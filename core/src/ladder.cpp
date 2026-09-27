#include "reefdo/ladder.hpp"

namespace reefdo::ladder
{

namespace
{

constexpr uint64_t Ms(uint32_t pSeconds)
{
    return static_cast<uint64_t>(pSeconds) * 1000u;
}

Level Shallower(Level pL)
{
    return static_cast<Level>(static_cast<uint8_t>(pL) - 1);
} // never called at Normal

// Level a device's trigger corresponds to; nullopt for None and Heat (not level-driven).
std::optional<Level> TriggerLevel(Trigger pT)
{
    switch(pT)
    {
        case Trigger::Blue: return Level::Blue;
        case Trigger::Yellow: return Level::Yellow;
        case Trigger::Red: return Level::Red;
        default: return std::nullopt;
    }
}

bool EntryCondition(Level pL, float pD, float pSlope, const Config& pCfg)
{
    const LevelConfig& lc = pCfg.GetLevel(pL);
    if(pD < lc.mgl - lc.hysteresis) return true;
    return pL == Level::Blue && pCfg.blueSlopeMglPer10min > 0.0f && pSlope <= -pCfg.blueSlopeMglPer10min;
}

void UpdateFault(State& pS, const Input& pIn, const Config& pCfg, Output& pOut)
{
    if(pIn.doMgl.has_value())
    {
        pS.consecutiveFailures = 0;
        if(pS.fault)
        {
            pS.fault = false;
            pOut.events.push_back({EventType::FaultClear});
        }
        return;
    }
    pS.consecutiveFailures += static_cast<uint32_t>(pS.consecutiveFailures != UINT32_MAX); // saturating, branch-free
    if(pCfg.faultAlert && !pS.fault && pS.consecutiveFailures >= pCfg.faultConsecutiveFailures)
    {
        pS.fault = true;
        pS.ackUntilMs.reset(); // a new alarm: an earlier ack does not cover it
        pOut.events.push_back({EventType::FaultEnter});
    }
}

void UpdateHeat(State& pS, const Input& pIn, const Config& pCfg, Output& pOut)
{
    if(!pIn.tempC.has_value()) return; // probe down: keep the last decision
    const float t = *pIn.tempC;
    if(!pS.heat && t >= pCfg.heatOnC)
    {
        pS.heat = true;
        pOut.events.push_back({EventType::HeatOn});
    }
    else if(pS.heat && t <= pCfg.heatOffC)
    {
        pS.heat = false;
        pOut.events.push_back({EventType::HeatOff});
    }
}

// The level state machine proper. Only runs with a valid reading; a failed probe freezes the level.
void UpdateLevel(State& pS, const Input& pIn, const Config& pCfg, Output& pOut)
{
    if(!pIn.doMgl.has_value()) return;
    const float d = *pIn.doMgl;
    const Level prev = pS.level;

    // Entry timers, one per level.
    for(uint8_t i = 1; i <= 3; ++i)
    {
        const Level l = static_cast<Level>(i);
        std::optional<uint64_t>& since = pS.belowSinceMs[i - 1];
        if(EntryCondition(l, d, pIn.slopeMglPer10min, pCfg))
        {
            if(!since.has_value()) since = pIn.nowMs;
        }
        else
        {
            since.reset();
        }
    }

    // Deepest level (deeper than the current one) whose dwell has elapsed wins — levels may be skipped downward.
    Level candidate = pS.level;
    for(uint8_t i = 3; i > static_cast<uint8_t>(pS.level); --i)
    {
        const Level l = static_cast<Level>(i);
        const std::optional<uint64_t>& since = pS.belowSinceMs[i - 1];
        if(since.has_value() && pIn.nowMs - *since >= Ms(pCfg.GetLevel(l).dwellS))
        {
            candidate = l;
            break;
        }
    }

    if(candidate != pS.level)
    {
        pS.level = candidate;
        pS.aboveSinceMs.reset();
    }
    else if(pS.level != Level::Normal)
    {
        // Recovery: one level at a time, after recover_sustain_s above the current level's exit threshold.
        const LevelConfig& lc = pCfg.GetLevel(pS.level);
        if(d > lc.mgl + lc.hysteresis)
        {
            if(!pS.aboveSinceMs.has_value()) pS.aboveSinceMs = pIn.nowMs;
            // night_armed is cleared by step() the moment it stops being night, so it implies `night`.
            const bool floorBlue = pCfg.nightLock && pS.nightArmed;
            const bool canStep = !(floorBlue && pS.level == Level::Blue);
            if(canStep && pIn.nowMs - *pS.aboveSinceMs >= Ms(pCfg.recoverSustainS))
            {
                pS.level = Shallower(pS.level);
                pS.aboveSinceMs.reset();
            }
        }
        else
        {
            pS.aboveSinceMs.reset();
        }
    }

    if(pS.level != prev)
    {
        pOut.events.push_back({EventType::LevelChange, prev, pS.level});
        if(pS.level > prev) pS.ackUntilMs.reset(); // deeper re-arms the alarm
    }
}

// The level whose devices run: the readings' level or, during a FAULT, fault.level when that is deeper.
Level DevicesLevel(const State& pS, const Config& pCfg)
{
    if(!pS.fault) return pS.level;
    const Level fl = pCfg.faultLevel < Level::Yellow ? Level::Yellow : pCfg.faultLevel;
    return fl > pS.level ? fl : pS.level;
}

void UpdateDevices(State& pS, const Input& pIn, const Config& pCfg, Level pAt, bool pSilenced, Output& pOut)
{
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const DeviceConfig& dc = pCfg.devices[i];
        bool active = false;
        if(dc.trigger == Trigger::Heat)
        {
            active = pS.heat;
        }
        else
        {
            const std::optional<Level> tl = TriggerLevel(dc.trigger);
            active = tl.has_value() && *tl <= pAt;
        }

        if(dc.mode == Mode::PulseOff)
        {
            PulseState& p = pS.pulse[i];
            const bool intervalOk =
                !p.lastStartMs.has_value() || pIn.nowMs - *p.lastStartMs >= Ms(pCfg.pulseMinIntervalS);
            // A new pulse needs an activation edge, a quiet controller, and no pulse still running —
            // the last one guarantees the device is powered for at least one tick between pulses.
            if(active && !p.wasActive && !p.active && !pS.fault && !pIn.maintenance && intervalOk)
            {
                p.active = true;
                p.startedMs = pIn.nowMs;
                p.lastStartMs = pIn.nowMs;
                pOut.events.push_back({EventType::Pulse, Level::Normal, Level::Normal, static_cast<uint8_t>(i)});
            }
            if(p.active && pIn.nowMs - p.startedMs >= Ms(pCfg.pulseS)) p.active = false;
            p.wasActive = active;
            pOut.deviceOn[i] = !p.active; // a pulse device is normally powered
        }
        else if(dc.ackSilences)
        {
            // Alarm device: Blue is silent, Ack silences, maintenance silences.
            pOut.deviceOn[i] = active && pAt >= Level::Yellow && !pSilenced && !pIn.maintenance;
        }
        else
        {
            pOut.deviceOn[i] = active;
        }
    }
}

} // namespace

const LevelConfig& Config::GetLevel(Level pL) const
{
    const LevelConfig* const table[3] = {&blue, &yellow, &red};
    return *table[static_cast<uint8_t>(pL) - 1];
}

bool IsNight(std::optional<uint16_t> pMinuteOfDay, const Config& pCfg)
{
    if(!pMinuteOfDay.has_value()) return pCfg.unknownTimeIsNight;
    const uint16_t m = *pMinuteOfDay;
    if(pCfg.nightStartMin <= pCfg.nightEndMin) return m >= pCfg.nightStartMin && m < pCfg.nightEndMin;
    return m >= pCfg.nightStartMin || m < pCfg.nightEndMin; // wraps midnight
}

Output Step(State& pS, const Input& pIn, const Config& pCfg)
{
    Output out;

    UpdateFault(pS, pIn, pCfg, out);
    UpdateHeat(pS, pIn, pCfg, out);

    const bool night = IsNight(pIn.minuteOfDay, pCfg);
    if(!night) pS.nightArmed = false;

    UpdateLevel(pS, pIn, pCfg, out);
    if(pCfg.nightLock && night && pS.level != Level::Normal) pS.nightArmed = true;

    if(pIn.ackPressed)
    {
        pS.ackUntilMs = pIn.nowMs + Ms(pCfg.ackSilenceS);
        out.events.push_back({EventType::Ack});
    }
    const bool silenced = pS.ackUntilMs.has_value() && pIn.nowMs < *pS.ackUntilMs;

    UpdateDevices(pS, pIn, pCfg, DevicesLevel(pS, pCfg), silenced, out);

    out.level = pS.level;
    out.fault = pS.fault;
    out.silenced = silenced;
    out.heat = pS.heat;
    out.sound = (pS.level != Level::Normal || pS.fault) && !silenced && !pIn.maintenance;
    return out;
}

} // namespace reefdo::ladder
