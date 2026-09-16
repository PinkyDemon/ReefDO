#include "reefdo/service.hpp"

#include <cmath>

namespace reefdo::service
{

namespace
{

constexpr uint64_t Ms(uint32_t pS)
{
    return static_cast<uint64_t>(pS) * 1000u;
}
constexpr uint64_t DAY_MS = 24ull * 3600u * 1000u;

std::size_t NextServiced(const Config& pCfg, std::size_t pFrom)
{
    for(std::size_t i = pFrom; i < DEVICES; ++i)
    {
        if(pCfg.devices[i].serviceS > 0) return i;
    }
    return DEVICES;
}

uint32_t ServicedCount(const Config& pCfg)
{
    uint32_t n = 0;
    for(const DeviceConfig& d : pCfg.devices)
        n += static_cast<uint32_t>(d.serviceS > 0);
    return n;
}

bool DeficitConfigured(const Config& pCfg)
{
    return pCfg.induceDeficitS > 0 && pCfg.induceDeficitDevice >= 1 && pCfg.induceDeficitDevice <= DEVICES;
}

bool HasHeadroom(std::optional<float> pSat, const Config& pCfg)
{
    return pSat.has_value() && std::fabs(*pSat - 100.0f) >= pCfg.minHeadroomPct;
}

void Emit(Output& pOut, Event pE)
{
    pOut.events.push_back(pE);
}

// Scheduled start decision. May emit Skipped(Missed) once per day.
bool ScheduledDue(State& pS, const Input& pIn, const Config& pCfg, Output& pOut)
{
    if(!pIn.local.has_value())
    {
        // Clock unknown: 24 h (monotonic) after the previous run lands in the same window if that one was scheduled.
        if(pS.lastRunMs.has_value() && pIn.nowMs - *pS.lastRunMs >= DAY_MS)
        {
            pS.clockUnknown = true;
            return true;
        }
        return false;
    }
    const uint32_t day = pIn.local->dayIndex;
    const uint16_t m = pIn.local->minuteOfDay;
    if(pS.attemptedDay == day || pS.p.lastRunDay == day) return false;
    if(m < pCfg.windowStartMin) return false;
    if(m >= pCfg.windowEndMin)
    {
        pS.attemptedDay = day;
        Event e{EventType::Skipped};
        e.skip = Skip::Missed;
        Emit(pOut, e);
        return false;
    }
    // Latest start that still finishes inside the window; before that, only start once there is headroom to measure.
    const uint32_t needMin = (RunDurationS(pCfg) + 59) / 60;
    const uint32_t spanMin = static_cast<uint32_t>(pCfg.windowEndMin - pCfg.windowStartMin);
    const uint32_t latest = pCfg.windowEndMin - (needMin < spanMin ? needMin : spanMin);
    if(m < latest && !HasHeadroom(pIn.satPct, pCfg)) return false;
    pS.clockUnknown = false;
    return true;
}

void BeginDevice(State& pS, const Input& pIn, const Config& pCfg, std::size_t pI, bool pFirst, Output& pOut)
{
    pS.device = pI;
    pS.phase = Phase::Running;
    pS.phaseStartedMs = pIn.nowMs;
    pS.deviceStartedMs = pIn.nowMs;
    pS.sat0 = pIn.satPct.value_or(0.0f);
    pS.slope0 = pS.slope.SlopePer10min();
    pS.peak = 0.0f;
    if(pFirst)
    {
        pS.judged = HasHeadroom(pIn.satPct, pCfg);
        Event e{EventType::RunStart};
        e.judged = pS.judged;
        e.clockUnknown = pS.clockUnknown;
        Emit(pOut, e);
        pOut.chirp = pCfg.chirp;
    }
    pS.deviceJudged = pS.judged && pIn.satPct.has_value();
    Event e{EventType::DeviceStart};
    e.device = static_cast<uint8_t>(pI);
    Emit(pOut, e);
}

void Measure(State& pS, const Input& pIn)
{
    if(!pIn.satPct.has_value()) return;
    const float t10 = static_cast<float>(pIn.nowMs - pS.deviceStartedMs) / 600000.0f;
    const float dev = std::fabs(*pIn.satPct - (pS.sat0 + pS.slope0 * t10));
    if(dev > pS.peak) pS.peak = dev;
}

void EndDevice(State& pS, const Config& pCfg, Output& pOut)
{
    const std::size_t i = pS.device;
    Outcome o = Outcome::Inconclusive;
    if(!pS.aborted)
    {
        if(pCfg.devices[i].minResponsePct <= 0.0f)
        {
            o = Outcome::Unchecked;
        }
        else if(pS.deviceJudged)
        {
            o = pS.peak >= pCfg.devices[i].minResponsePct ? Outcome::Pass : Outcome::Fail;
        }
    }
    pS.p.lastOutcome[i] = o;
    pS.p.lastResponse[i] = pS.peak;
    if(o == Outcome::Pass && pS.p.failActive[i])
    {
        pS.p.failActive[i] = false;
        Event e{EventType::FailCleared};
        e.device = static_cast<uint8_t>(i);
        Emit(pOut, e);
    }
    if(o == Outcome::Fail)
    {
        pS.p.failActive[i] = true;
        Event e{EventType::FailAlert};
        e.device = static_cast<uint8_t>(i);
        e.response = pS.peak;
        Emit(pOut, e);
    }
    Event e{EventType::DeviceEnd};
    e.device = static_cast<uint8_t>(i);
    e.outcome = o;
    e.response = pS.peak;
    Emit(pOut, e);
}

void EndRun(State& pS, const Config& pCfg, Output& pOut)
{
    if(!pS.manual && !pS.aborted)
    {
        if(pS.judged)
        {
            pS.p.inconclusiveStreak = 0;
            pS.p.inconclusiveAlert = false;
        }
        else
        {
            ++pS.p.inconclusiveStreak;
            if(pS.p.inconclusiveStreak >= pCfg.inconclusiveDays && !pS.p.inconclusiveAlert)
            {
                pS.p.inconclusiveAlert = true;
                Emit(pOut, Event{EventType::InconclusiveAlert});
            }
        }
    }
    Event e{EventType::RunEnd};
    e.aborted = pS.aborted;
    Emit(pOut, e);
    pS.phase = Phase::Idle;
}

void StartRun(State& pS, const Input& pIn, const Config& pCfg, bool pManual, Output& pOut)
{
    pS.manual = pManual;
    pS.aborted = false;
    pS.runStartedMs = pIn.nowMs;
    pS.lastRunMs = pIn.nowMs;
    if(pIn.local.has_value() && !pManual)
    {
        pS.attemptedDay = pIn.local->dayIndex;
        pS.p.lastRunDay = pIn.local->dayIndex;
    }
    if(DeficitConfigured(pCfg))
    {
        pS.phase = Phase::Deficit;
        pS.phaseStartedMs = pIn.nowMs;
        return;
    }
    BeginDevice(pS, pIn, pCfg, NextServiced(pCfg, 0), true, pOut);
}

void Advance(State& pS, const Input& pIn, const Config& pCfg, Output& pOut)
{
    if(pIn.fault || pIn.level != ladder::Level::Normal)
    {
        pS.aborted = true;
        if(pS.phase == Phase::Running || pS.phase == Phase::Tail) EndDevice(pS, pCfg, pOut);
        EndRun(pS, pCfg, pOut);
        return;
    }
    const uint64_t elapsed = pIn.nowMs - pS.phaseStartedMs;
    switch(pS.phase)
    {
        case Phase::Deficit:
            if(elapsed >= Ms(pCfg.induceDeficitS))
            {
                BeginDevice(pS, pIn, pCfg, NextServiced(pCfg, 0), true, pOut);
                pOut.deviceOn[pS.device] = true;
            }
            else
            {
                pOut.cutDevice = static_cast<uint8_t>(pCfg.induceDeficitDevice - 1);
            }
            break;
        case Phase::Running:
            Measure(pS, pIn);
            if(elapsed >= Ms(pCfg.devices[pS.device].serviceS))
            {
                pS.phase = Phase::Tail;
                pS.phaseStartedMs = pIn.nowMs;
            }
            else
            {
                pOut.deviceOn[pS.device] = true;
            }
            break;
        case Phase::Tail:
            Measure(pS, pIn);
            if(elapsed >= Ms(pCfg.tailS))
            {
                EndDevice(pS, pCfg, pOut);
                const std::size_t next = NextServiced(pCfg, pS.device + 1);
                if(next == DEVICES)
                {
                    EndRun(pS, pCfg, pOut);
                }
                else
                {
                    pS.device = next;
                    pS.phase = Phase::Settle;
                    pS.phaseStartedMs = pIn.nowMs;
                }
            }
            break;
        default: // Settle
            if(elapsed >= Ms(pCfg.settleS))
            {
                BeginDevice(pS, pIn, pCfg, pS.device, false, pOut);
                pOut.deviceOn[pS.device] = true;
            }
            break;
    }
}

} // namespace

uint32_t RunDurationS(const Config& pCfg)
{
    const uint32_t n = ServicedCount(pCfg);
    uint32_t total = 0;
    for(const DeviceConfig& d : pCfg.devices)
        total += d.serviceS;
    total += n * pCfg.tailS + (n - static_cast<uint32_t>(n > 0)) * pCfg.settleS;
    total += DeficitConfigured(pCfg) ? pCfg.induceDeficitS : 0;
    return total;
}

Output Step(State& pS, const Input& pIn, const Config& pCfg)
{
    Output out;
    if(pIn.satPct.has_value()) pS.slope.Push(pIn.nowMs, *pIn.satPct);

    if(pS.phase == Phase::Idle)
    {
        const bool manual = pIn.runNow;
        if(manual || ScheduledDue(pS, pIn, pCfg, out))
        {
            std::optional<Skip> skip;
            if(ServicedCount(pCfg) == 0)
            {
                skip = Skip::NothingToRun;
            }
            else if(pIn.fault)
            {
                skip = Skip::Fault;
            }
            else if(pIn.level != ladder::Level::Normal)
            {
                skip = Skip::NotNormal;
            }
            else if(pIn.maintenance)
            {
                skip = Skip::Maintenance;
            }
            if(skip.has_value())
            {
                if(pIn.local.has_value() && !manual) pS.attemptedDay = pIn.local->dayIndex;
                Event e{EventType::Skipped};
                e.skip = *skip;
                Emit(out, e);
            }
            else
            {
                StartRun(pS, pIn, pCfg, manual, out);
                if(pS.phase == Phase::Running) out.deviceOn[pS.device] = true;
                if(pS.phase == Phase::Deficit) out.cutDevice = static_cast<uint8_t>(pCfg.induceDeficitDevice - 1);
            }
        }
    }
    else
    {
        Advance(pS, pIn, pCfg, out);
    }

    out.running = pS.phase != Phase::Idle;
    out.failed = pS.p.failActive;
    out.inconclusiveAlert = pS.p.inconclusiveAlert;
    return out;
}

} // namespace reefdo::service
