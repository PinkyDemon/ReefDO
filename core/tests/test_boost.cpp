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
    c.holdS = 0; // the first reading at the target ends it; the hold has its own test
    return c;    // 19:30–20:00
}

Input At(uint16_t pMinute, float pMgl, uint32_t pDay = 5)
{
    Input in;
    in.nowMs = (static_cast<uint64_t>(pDay) * 1440u + pMinute) * 60'000u;
    in.local = reefdo::selftest::LocalTime{pMinute, pDay};
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

TEST_CASE("Boost pauses for a test run or maintenance and resumes while the window is open", "[boost]")
{
    State s;
    const Config c = On();
    Output o = Step(s, At(19 * 60 + 31, 4.6f), c);
    REQUIRE(o.running);
    Input busy = At(19 * 60 + 32, 4.7f);
    busy.testRunning = true;
    o = Step(s, busy, c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Paused));
    REQUIRE_FALSE(s.doneDay.has_value());
    o = Step(s, busy, c); // still busy: nothing starts
    REQUIRE_FALSE(o.running);
    REQUIRE(o.events.empty());
    o = Step(s, At(19 * 60 + 40, 4.9f), c); // test over: resumes
    REQUIRE(o.running);
    Input maint = At(19 * 60 + 41, 5.0f);
    maint.maintenance = true;
    o = Step(s, maint, c);
    REQUIRE(Has(o, EventType::Paused));
    o = Step(s, maint, c);
    REQUIRE_FALSE(o.running);
}

TEST_CASE("Boost keeps its devices on until DO has held the target for hold_s; a dip starts the hold over", "[boost]")
{
    State s;
    Config c = On();
    c.holdS = 300;
    Output o = Step(s, At(19 * 60 + 30, 4.0f), c);
    REQUIRE(o.running);
    o = Step(s, At(19 * 60 + 40, 6.6f), c); // at the target: the hold starts, the pump stays on
    REQUIRE(o.running);
    REQUIRE(o.events.empty());
    REQUIRE(s.atTargetSinceMs == At(19 * 60 + 40, 0.0f).nowMs);
    o = Step(s, At(19 * 60 + 44, 6.7f), c);
    REQUIRE(o.running);
    o = Step(s, At(19 * 60 + 45, 6.7f), c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Reached));
    REQUIRE(s.doneDay == 5);
    REQUIRE_FALSE(s.atTargetSinceMs.has_value());

    State t; // the reading dips below the target during the hold: it starts over
    REQUIRE(Step(t, At(19 * 60 + 30, 4.0f), c).running);
    REQUIRE(Step(t, At(19 * 60 + 35, 6.6f), c).running);
    REQUIRE(Step(t, At(19 * 60 + 38, 6.4f), c).running);
    REQUIRE_FALSE(t.atTargetSinceMs.has_value());
    REQUIRE(Step(t, At(19 * 60 + 41, 6.6f), c).running); // 6 min after the first touch, 3 after the dip
    o = Step(t, At(19 * 60 + 46, 6.6f), c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Reached));

    State p; // a pause during the hold forgets it: the resumed run holds anew
    REQUIRE(Step(p, At(19 * 60 + 30, 4.0f), c).running);
    REQUIRE(Step(p, At(19 * 60 + 35, 6.6f), c).running);
    Input busy = At(19 * 60 + 36, 6.6f);
    busy.maintenance = true;
    REQUIRE(Has(Step(p, busy, c), EventType::Paused));
    REQUIRE_FALSE(p.atTargetSinceMs.has_value());
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

TEST_CASE("A running boost rides out a missed reading; the day is not over for a CRC error", "[boost]")
{
    State s;
    Config c = On();
    c.holdS = 300;
    REQUIRE(Step(s, At(19 * 60 + 31, 4.6f), c).running);
    Input gap = At(19 * 60 + 32, 0.0f);
    gap.doMgl.reset();
    Output o = Step(s, gap, c);
    REQUIRE(o.running); // the pump stays on
    REQUIRE(o.deviceOn[0]);
    REQUIRE(o.events.empty());
    REQUIRE_FALSE(s.doneDay.has_value());
    REQUIRE(Step(s, At(19 * 60 + 40, 6.6f), c).running); // the hold starts with the next reading
    gap = At(19 * 60 + 43, 0.0f);
    gap.doMgl.reset();
    REQUIRE(Step(s, gap, c).running); // a gap inside the hold neither ends nor restarts it
    REQUIRE(s.atTargetSinceMs == At(19 * 60 + 40, 0.0f).nowMs);
    o = Step(s, At(19 * 60 + 45, 6.6f), c);
    REQUIRE_FALSE(o.running);
    REQUIRE(Has(o, EventType::Reached));
}
