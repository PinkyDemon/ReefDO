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
    pOut.events.push_back({pWhy, pDo});
}

} // namespace

Output Step(State& pS, const Input& pIn, const Config& pCfg)
{
    Output out;
    const float mgl = pIn.doMgl.value_or(0.0f);
    const bool usable = pCfg.enabled && AnyDevice(pCfg) && pIn.local.has_value() && pIn.doMgl.has_value();
    const bool open = usable && InWindow(pIn.local->minuteOfDay, pCfg);

    if(pS.running)
    {
        if(!open)
        {
            // Window closed (or the clock / probe went away): the day's boost is over either way.
            if(pIn.local.has_value()) pS.doneDay = pIn.local->dayIndex;
            Stop(pS, out, EventType::WindowEnd, mgl);
            return out;
        }
        if(pIn.serviceRunning || pIn.maintenance)
        {
            Stop(pS, out, EventType::Paused, mgl); // doneDay untouched: resumes if the window is still open
            return out;
        }
        if(mgl >= pCfg.targetMgl)
        {
            pS.doneDay = pIn.local->dayIndex;
            Stop(pS, out, EventType::Reached, mgl);
            return out;
        }
    }
    else if(open && !pIn.serviceRunning && !pIn.maintenance && mgl < pCfg.targetMgl &&
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
