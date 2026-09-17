#include "catch_amalgamated.hpp"

#include "reefdo/boost.hpp"

using namespace reefdo::boost;

namespace
{

Config On()
{
    Config c;
    c.enabled = true;
    c.devices[0] = true;
    c.devices[2] = true;
    c.targetMgl = 6.5f;
    return c; // 19:30–20:00
}

Input At(uint16_t pMinute, float pMgl, uint32_t pDay = 5)
{
    Input in;
    in.local = reefdo::service::LocalTime{pMinute, pDay};
    in.doMgl = pMgl;
    return in;
}

bool Has(const Output& pO, EventType pT)
{
    for(const Event& e : pO.events)
        if(e.type == pT) return true;
    return false;
}

} // namespace

TEST_CASE("Boost runs its devices inside the window until the target mg/L and then stays off for the day", "[boost]")
{
    State s;
    const Config c = On();
    Output o = Step(s, At(19 * 60 + 29, 4.0f), c);
    REQUIRE_FALSE(o.running); // before the window
    o = Step(s, At(19 * 60 + 30, 4.0f), c);
    REQUIRE(o.running);
    REQUIRE(Has(o, EventType::Start));
    REQUIRE(o.deviceOn[0]);
    REQUIRE_FALSE(o.deviceOn[1]);
    REQUIRE(o.deviceOn[2]);
    o = Step(s, At(19 * 60 + 40, 5.9f), c);
    REQUIRE(o.running);
    REQUIRE(o.events.empty());
    o = Step(s, At(19 * 60 + 45, 6.55f), c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Reached));
    REQUIRE_FALSE(o.deviceOn[0]);
    o = Step(s, At(19 * 60 + 50, 5.2f), c); // fell back below the target: not again today
    REQUIRE_FALSE(o.running);
    o = Step(s, At(19 * 60 + 50, 5.2f, 6), c); // next day
    REQUIRE(o.running);
}

TEST_CASE("Boost stops when the window closes, and never starts above the target or without a reading/clock", "[boost]")
{
    State s;
    const Config c = On();
    Output o = Step(s, At(19 * 60 + 30, 6.5f), c);
    REQUIRE_FALSE(o.running); // already at target
    o = Step(s, At(19 * 60 + 31, 4.6f), c);
    REQUIRE(o.running);
    o = Step(s, At(20 * 60, 5.6f), c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::WindowEnd));
    REQUIRE(s.doneDay == 5);

    State fresh;
    Input noClock = At(19 * 60 + 40, 4.6f);
    noClock.local.reset();
    REQUIRE_FALSE(Step(fresh, noClock, c).running);
    Input noProbe = At(19 * 60 + 40, 4.6f);
    noProbe.doMgl.reset();
    REQUIRE_FALSE(Step(fresh, noProbe, c).running);
    Config off = c;
    off.enabled = false;
    REQUIRE_FALSE(Step(fresh, At(19 * 60 + 40, 4.6f), off).running);
    Config nothing = c;
    nothing.devices.fill(false);
    REQUIRE_FALSE(Step(fresh, At(19 * 60 + 40, 4.6f), nothing).running);
}

TEST_CASE("Boost pauses for a service run or maintenance and resumes while the window is open", "[boost]")
{
    State s;
    const Config c = On();
    Output o = Step(s, At(19 * 60 + 31, 4.6f), c);
    REQUIRE(o.running);
    Input busy = At(19 * 60 + 32, 4.7f);
    busy.serviceRunning = true;
    o = Step(s, busy, c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Paused));
    REQUIRE_FALSE(s.doneDay.has_value());
    o = Step(s, busy, c); // still busy: nothing starts
    REQUIRE_FALSE(o.running);
    REQUIRE(o.events.empty());
    o = Step(s, At(19 * 60 + 40, 4.9f), c); // service over: resumes
    REQUIRE(o.running);
    Input maint = At(19 * 60 + 41, 5.0f);
    maint.maintenance = true;
    o = Step(s, maint, c);
    REQUIRE(Has(o, EventType::Paused));
    o = Step(s, maint, c);
    REQUIRE_FALSE(o.running);
}

TEST_CASE("Losing the clock mid-run ends the boost without marking the day", "[boost]")
{
    State s;
    const Config c = On();
    REQUIRE(Step(s, At(19 * 60 + 31, 4.6f), c).running);
    Input lost = At(19 * 60 + 32, 4.7f);
    lost.local.reset();
    const Output o = Step(s, lost, c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::WindowEnd));
    REQUIRE_FALSE(s.doneDay.has_value());
}
