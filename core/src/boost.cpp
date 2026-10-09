#include "reefdo/boost.hpp"

namespace reefdo::boost
{

namespace
{

bool InWindow(uint16_t pMinute, const Config& pCfg)
{
    return pMinute >= pCfg.windowStartMin && pMinute < pCfg.windowEndMin;
}

bool AnyDevice(const Config& pCfg)
{
    for(const bool d : pCfg.devices)
        if(d) return true;
    return false;
}

void Stop(State& pS, Output& pOut, EventType pWhy, float pDo)
{
    pS.running = false;
    pS.atTargetSinceMs.reset();
    pOut.events.push_back({pWhy, pDo});
}

// True once DO has stayed at or above the target for hold_s: the first bubbles lift the reading at the probe long
// before the tank is there, so a dip below the target starts the hold over.
bool Held(State& pS, const Input& pIn, const Config& pCfg, float pMgl)
{
    if(pMgl < pCfg.targetMgl)
    {
        pS.atTargetSinceMs.reset();
        return false;
    }
    if(!pS.atTargetSinceMs.has_value()) pS.atTargetSinceMs = pIn.nowMs;
    return pIn.nowMs - *pS.atTargetSinceMs >= static_cast<uint64_t>(pCfg.holdS) * 1000u;
}

} // namespace

Output Step(State& pS, const Input& pIn, const Config& pCfg)
{
    Output out;
    const float mgl = pIn.doMgl.value_or(0.0f);
    const bool open =
        pCfg.enabled && AnyDevice(pCfg) && pIn.local.has_value() && InWindow(pIn.local->minuteOfDay, pCfg);

    if(pS.running)
    {
        if(!open)
        {
            // Window closed (or the clock went away): the day's boost is over either way.
            if(pIn.local.has_value()) pS.doneDay = pIn.local->dayIndex;
            Stop(pS, out, EventType::WindowEnd, mgl);
            return out;
        }
        if(pIn.testRunning || pIn.maintenance)
        {
            Stop(pS, out, EventType::Paused, mgl); // doneDay untouched: resumes if the window is still open
            return out;
        }
        // A missed reading says nothing about the tank: the pump stays on, the hold waits for the next one.
        if(pIn.doMgl.has_value() && Held(pS, pIn, pCfg, mgl))
        {
            pS.doneDay = pIn.local->dayIndex;
            Stop(pS, out, EventType::Reached, mgl);
            return out;
        }
    }
    else if(open && pIn.doMgl.has_value() && !pIn.testRunning && !pIn.maintenance && mgl < pCfg.targetMgl &&
            pS.doneDay != pIn.local->dayIndex)
    {
        pS.running = true;
        out.events.push_back({EventType::Start, mgl});
    }

    if(pS.running)
    {
        out.running = true;
        out.deviceOn = pCfg.devices;
    }
    return out;
}

} // namespace reefdo::boost
