#include <functional>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/service.hpp"

#include "proptest.hpp"

using namespace reefdo::service;
using Catch::Matchers::WithinAbs;
using reefdo::ladder::Level;

namespace
{

constexpr uint32_t TICK = 10;

Config ExampleCfg()
{
    Config c;
    c.devices[0] = {300, 1.0f}; // bubbler, judged
    c.devices[1] = {300, 0.0f}; // powerhead, unchecked
    c.devices[2] = {300, 1.0f}; // strong pump, judged
    c.windowStartMin = 19 * 60 + 30; // the scenarios below are written for 19:30–21:00
    c.windowEndMin = 21 * 60;
    return c;
}

// Runs the service machine on a clock (10 s ticks) with a scripted tank: `tank(out)` adjusts `sat` each tick.
struct Bench
{
    Config cfg = ExampleCfg();
    State st;
    uint64_t nowMs = 100'000'000;
    uint32_t day = 20'000;
    uint32_t secOfDay = 19 * 3600; // 19:00
    bool clockKnown = true;
    float sat = 108.0f;
    Level level = Level::Normal;
    bool fault = false;
    bool maintenance = false;
    bool probeOk = true;
    std::function<void(const Output&)> tank;
    std::vector<Event> events; // everything emitted so far

    Output Tick(bool pRunNow = false)
    {
        Input in;
        in.nowMs = nowMs;
        if(clockKnown) in.local = LocalTime{static_cast<uint16_t>(secOfDay / 60), day};
        if(probeOk) in.satPct = sat;
        in.level = level;
        in.fault = fault;
        in.maintenance = maintenance;
        in.runNow = pRunNow;
        Output o = Step(st, in, cfg);
        for(const Event& e : o.events)
            events.push_back(e);
        nowMs += TICK * 1000;
        secOfDay += TICK;
        if(secOfDay >= 86400)
        {
            secOfDay -= 86400;
            ++day;
        }
        if(tank) tank(o);
        return o;
    }
    Output RunFor(uint32_t pSeconds)
    {
        Output o;
        for(uint32_t t = 0; t < pSeconds; t += TICK)
            o = Tick();
        return o;
    }
    // Ticks up to and including the step at hh:mm:00.
    void GotoTime(uint32_t pHh, uint32_t pMm)
    {
        for(;;)
        {
            const bool at = secOfDay == pHh * 3600 + pMm * 60;
            Tick();
            if(at) break;
        }
    }
    std::size_t Count(EventType pT) const
    {
        std::size_t n = 0;
        for(const Event& e : events)
            n += (e.type == pT);
        return n;
    }
    const Event* last(EventType pT) const
    {
        for(auto it = events.rbegin(); it != events.rend(); ++it)
            if(it->type == pT) return &*it;
        return nullptr;
    }
    // Tank that responds to devices 0 and 2 (falling toward 100 %), not to device 1.
    void ResponsiveTank(float pPerTick0 = -0.05f, float pPerTick2 = -0.05f)
    {
        tank = [this, pPerTick0, pPerTick2](const Output& pO)
        {
            if(pO.deviceOn[0]) sat += pPerTick0;
            if(pO.deviceOn[2]) sat += pPerTick2;
        };
    }
};

} // namespace

TEST_CASE("run_duration_s adds runs, tails, gaps and the deficit", "[service]")
{
    Config c = ExampleCfg();
    REQUIRE(RunDurationS(c) == 3 * 300 + 3 * 60 + 2 * 120);
    c.induceDeficitS = 120;
    c.induceDeficitDevice = 5;
    REQUIRE(RunDurationS(c) == 1320 + 120);
    c.induceDeficitDevice = 0; // not configured → not counted
    REQUIRE(RunDurationS(c) == 1320);
    REQUIRE(RunDurationS(Config{}) == 0);
}

TEST_CASE("A scheduled run starts at the window with headroom and walks the devices in order", "[service]")
{
    Bench b;
    b.ResponsiveTank();
    b.GotoTime(19, 29);
    Output o = b.RunFor(50); // 19:29:10 .. 19:29:50
    REQUIRE_FALSE(o.running);
    o = b.Tick(); // 19:30:00 → start
    REQUIRE(o.running);
    REQUIRE(o.chirp);
    REQUIRE(o.deviceOn == std::array<bool, 6>{true, false, false, false, false, false});
    REQUIRE(b.Count(EventType::RunStart) == 1);
    REQUIRE(b.last(EventType::RunStart)->judged);
    REQUIRE_FALSE(b.last(EventType::RunStart)->clockUnknown);
    REQUIRE(b.Count(EventType::DeviceStart) == 1);

    o = b.RunFor(290); // still running device 0 (300 s)
    REQUIRE(o.deviceOn[0]);
    o = b.Tick(); // 300 s → tail
    REQUIRE_FALSE(o.deviceOn[0]);
    REQUIRE(o.running);
    REQUIRE(b.Count(EventType::DeviceEnd) == 0);
    o = b.RunFor(60); // tail done → device end, settle
    REQUIRE(b.Count(EventType::DeviceEnd) == 1);
    REQUIRE(b.last(EventType::DeviceEnd)->device == 0);
    REQUIRE(b.last(EventType::DeviceEnd)->outcome == Outcome::Pass);
    REQUIRE_THAT(b.last(EventType::DeviceEnd)->response, WithinAbs(1.5, 0.15));
    REQUIRE(o.deviceOn == std::array<bool, 6>{});
    o = b.RunFor(120); // settle → device 1 starts
    REQUIRE(o.deviceOn[1]);
    REQUIRE(b.Count(EventType::DeviceStart) == 2);
    REQUIRE_FALSE(o.chirp);
    b.RunFor(300 + 60 + 120);
    REQUIRE(b.last(EventType::DeviceEnd)->device == 1);
    REQUIRE(b.last(EventType::DeviceEnd)->outcome == Outcome::Unchecked);
    o = b.RunFor(300 + 60);
    REQUIRE(b.Count(EventType::RunEnd) == 1);
    REQUIRE_FALSE(b.last(EventType::RunEnd)->aborted);
    REQUIRE_FALSE(o.running);
    REQUIRE(b.Count(EventType::DeviceEnd) == 3);
    REQUIRE(b.last(EventType::DeviceEnd)->outcome == Outcome::Pass);
    REQUIRE(b.Count(EventType::FailAlert) == 0);
    REQUIRE(o.failed == std::array<bool, 6>{});
    REQUIRE(b.st.p.lastRunDay == b.day);
    REQUIRE(b.st.p.lastOutcome[0] == Outcome::Pass);

    // Not again today; again tomorrow.
    b.GotoTime(20, 30);
    REQUIRE(b.Count(EventType::RunStart) == 1);
    b.GotoTime(19, 30); // next day
    REQUIRE(b.Count(EventType::RunStart) == 2);
}

TEST_CASE("A device with no measurable response fails; a later pass clears the alert", "[service]")
{
    Bench b;
    b.ResponsiveTank(-0.005f, -0.05f); // bubbler: 0.15 % over 300 s — below the 1 % it must show
    b.GotoTime(19, 30);
    b.RunFor(1400);
    REQUIRE(b.Count(EventType::RunEnd) == 1);
    REQUIRE(b.Count(EventType::FailAlert) == 1);
    REQUIRE(b.last(EventType::FailAlert)->device == 0);
    REQUIRE(b.st.p.failActive[0]);
    REQUIRE_FALSE(b.st.p.failActive[2]);
    Output o = b.Tick();
    REQUIRE(o.failed[0]);

    // Airstone cleaned: let the tank settle (the baseline is the last 10 min), then Run now: it responds, the alert
    // clears.
    b.tank = nullptr;
    b.RunFor(700);
    b.ResponsiveTank();
    o = b.Tick(true);
    REQUIRE(o.running);
    b.RunFor(1400);
    REQUIRE(b.Count(EventType::FailCleared) == 1);
    REQUIRE_FALSE(b.st.p.failActive[0]);
    REQUIRE(b.Count(EventType::RunStart) == 2);
    REQUIRE(b.Count(EventType::FailAlert) == 1);
}

TEST_CASE("The pre-run trend is subtracted from the response", "[service]")
{
    Bench b;
    b.tank = [&b](const Output&) { b.sat -= 0.01f; }; // falling 0.6 %/10 min all evening, devices do nothing
    b.GotoTime(19, 30);
    b.RunFor(1400);
    const Event* e = b.last(EventType::DeviceEnd);
    REQUIRE(e->outcome == Outcome::Fail);
    REQUIRE(e->response < 0.2f); // the drift alone would have read ~0.36 % over run + tail
}

TEST_CASE("No headroom: the run waits, then starts at the latest fitting time and comes back inconclusive", "[service]")
{
    Bench b;
    b.sat = 100.5f; // 0.5 % from saturation: nothing to measure
    b.GotoTime(19, 30);
    Output o = b.RunFor(60 * 60); // 20:30
    REQUIRE_FALSE(o.running);
    b.GotoTime(20, 38); // 21:00 - ceil(1320 s / 60) = 22 min: starts without headroom
    REQUIRE(b.Count(EventType::RunStart) == 1);
    REQUIRE_FALSE(b.last(EventType::RunStart)->judged);
    b.RunFor(1400);
    REQUIRE(b.st.p.lastOutcome[0] == Outcome::Inconclusive);
    REQUIRE(b.st.p.lastOutcome[1] == Outcome::Unchecked);
    REQUIRE(b.st.p.lastOutcome[2] == Outcome::Inconclusive);
    REQUIRE(b.Count(EventType::FailAlert) == 0);
    REQUIRE(b.st.p.inconclusiveStreak == 1);

    SECTION("headroom appearing inside the window starts the run at once")
    {
        Bench c;
        c.sat = 100.5f;
        c.GotoTime(19, 50);
        REQUIRE(c.Count(EventType::RunStart) == 0);
        c.sat = 104.0f;
        c.Tick();
        REQUIRE(c.Count(EventType::RunStart) == 1);
        REQUIRE(c.last(EventType::RunStart)->judged);
    }
}

TEST_CASE("Five inconclusive evenings raise the alert once; a judged run resets it", "[service]")
{
    Bench b;
    b.sat = 100.5f;
    for(int d = 0; d < 5; ++d)
    {
        b.GotoTime(20, 38);
        b.RunFor(1400);
    }
    REQUIRE(b.st.p.inconclusiveStreak == 5);
    REQUIRE(b.Count(EventType::InconclusiveAlert) == 1);
    Output o = b.Tick();
    REQUIRE(o.inconclusiveAlert);
    b.GotoTime(20, 38);
    b.RunFor(1400);
    REQUIRE(b.Count(EventType::InconclusiveAlert) == 1); // not repeated while standing
    b.sat = 106.0f;
    b.ResponsiveTank();
    b.GotoTime(19, 30);
    b.RunFor(1400);
    REQUIRE(b.st.p.inconclusiveStreak == 0);
    REQUIRE_FALSE(b.Tick().inconclusiveAlert);
}

TEST_CASE("A window that passes without an attempt is logged as missed, once", "[service]")
{
    Bench b;
    b.GotoTime(21, 5); // jump past the window with nothing having run (clock was 'unknown' until now)
    REQUIRE(b.Count(EventType::RunStart) == 1); // wait — the walk went through 19:30 with headroom
    Bench c;
    c.clockKnown = false;
    c.GotoTime(21, 5);
    REQUIRE(c.Count(EventType::Skipped) == 0);
    c.clockKnown = true;
    c.Tick();
    REQUIRE(c.Count(EventType::Skipped) == 1);
    REQUIRE(c.last(EventType::Skipped)->skip == Skip::Missed);
    c.RunFor(600);
    REQUIRE(c.Count(EventType::Skipped) == 1);
    REQUIRE(c.Count(EventType::RunStart) == 0);
}

TEST_CASE("Skips: not Normal, FAULT, maintenance, nothing to run", "[service]")
{
    SECTION("Blue at the window: skipped, and not retried that day")
    {
        Bench b;
        b.level = Level::Blue;
        b.GotoTime(19, 31);
        REQUIRE(b.Count(EventType::Skipped) == 1);
        REQUIRE(b.last(EventType::Skipped)->skip == Skip::NotNormal);
        b.level = Level::Normal;
        b.GotoTime(20, 30);
        REQUIRE(b.Count(EventType::RunStart) == 0);
    }
    SECTION("FAULT")
    {
        Bench b;
        b.fault = true;
        b.GotoTime(19, 31);
        REQUIRE(b.last(EventType::Skipped)->skip == Skip::Fault);
    }
    SECTION("maintenance")
    {
        Bench b;
        b.maintenance = true;
        b.GotoTime(19, 31);
        REQUIRE(b.last(EventType::Skipped)->skip == Skip::Maintenance);
    }
    SECTION("nothing configured, run now")
    {
        Bench b;
        b.cfg = Config{};
        b.Tick(true);
        REQUIRE(b.last(EventType::Skipped)->skip == Skip::NothingToRun);
        REQUIRE_FALSE(b.st.attemptedDay.has_value());
    }
}

TEST_CASE("Run now starts at any time and does not count as the day's scheduled run", "[service]")
{
    Bench b;
    b.sat = 110.0f;
    b.ResponsiveTank();
    Output o = b.Tick(true); // 19:00, outside the window
    REQUIRE(o.running);
    REQUIRE(o.chirp);
    b.RunFor(1400);
    REQUIRE(b.Count(EventType::RunEnd) == 1);
    REQUIRE_FALSE(b.st.p.lastRunDay.has_value());
    b.GotoTime(19, 30); // the scheduled one still happens
    REQUIRE(b.Count(EventType::RunStart) == 2);
    o = b.Tick(true); // run now while running: ignored
    REQUIRE(b.Count(EventType::RunStart) == 2);
}

TEST_CASE("An alarm absorbs the run: inconclusive device, aborted run, demands cleared", "[service]")
{
    Bench b;
    b.ResponsiveTank();
    b.GotoTime(19, 30);
    b.RunFor(100);
    REQUIRE(b.st.phase == Phase::Running);
    b.level = Level::Blue;
    Output o = b.Tick();
    REQUIRE_FALSE(o.running);
    REQUIRE(o.deviceOn == std::array<bool, 6>{});
    REQUIRE(b.last(EventType::DeviceEnd)->outcome == Outcome::Inconclusive);
    REQUIRE(b.last(EventType::RunEnd)->aborted);
    REQUIRE(b.Count(EventType::DeviceEnd) == 1);
    REQUIRE(b.st.p.inconclusiveStreak == 0); // aborted runs don't count either way

    SECTION("abort during the settle gap ends the run without a device end")
    {
        Bench c;
        c.ResponsiveTank();
        c.GotoTime(19, 30);
        c.RunFor(360 + 30); // device 0 done, in settle
        REQUIRE(c.st.phase == Phase::Settle);
        c.fault = true;
        c.Tick();
        REQUIRE(c.Count(EventType::DeviceEnd) == 1);
        REQUIRE(c.last(EventType::RunEnd)->aborted);
    }
}

TEST_CASE("Maintenance entered mid-run lets the run finish", "[service]")
{
    Bench b;
    b.ResponsiveTank();
    b.GotoTime(19, 30);
    b.RunFor(100);
    b.maintenance = true;
    b.RunFor(1300);
    REQUIRE(b.Count(EventType::RunEnd) == 1);
    REQUIRE_FALSE(b.last(EventType::RunEnd)->aborted);
}

TEST_CASE("Unknown clock: 24 h after the previous run, flagged; never without one", "[service]")
{
    Bench b;
    b.clockKnown = false;
    b.RunFor(2 * 86400);
    REQUIRE(b.Count(EventType::RunStart) == 0);

    Bench c;
    c.ResponsiveTank();
    c.GotoTime(19, 30);
    c.RunFor(1400);
    REQUIRE(c.Count(EventType::RunStart) == 1);
    c.clockKnown = false;
    c.RunFor(86400 - 1400 - 20);
    REQUIRE(c.Count(EventType::RunStart) == 1);
    c.RunFor(60);
    REQUIRE(c.Count(EventType::RunStart) == 2);
    REQUIRE(c.last(EventType::RunStart)->clockUnknown);
}

TEST_CASE("Induced deficit cuts the configured device first; an abort during it ends the run cleanly", "[service]")
{
    Bench b;
    b.cfg.induceDeficitS = 120;
    b.cfg.induceDeficitDevice = 5;
    b.ResponsiveTank();
    b.GotoTime(19, 30);
    Output o = b.Tick();
    REQUIRE(o.running);
    REQUIRE(o.cutDevice == uint8_t{4});
    REQUIRE(o.deviceOn == std::array<bool, 6>{});
    REQUIRE(b.Count(EventType::RunStart) == 0); // the run proper starts after the deficit
    o = b.RunFor(100);
    REQUIRE(o.cutDevice.has_value());
    o = b.Tick(); // 120 s: first device starts
    REQUIRE_FALSE(o.cutDevice.has_value());
    REQUIRE(o.deviceOn[0]);
    REQUIRE(b.Count(EventType::RunStart) == 1);

    Bench c;
    c.cfg.induceDeficitS = 120;
    c.cfg.induceDeficitDevice = 5;
    c.GotoTime(19, 30);
    c.RunFor(50);
    c.level = Level::Yellow;
    c.Tick();
    REQUIRE(c.Count(EventType::DeviceEnd) == 0);
    REQUIRE(c.last(EventType::RunEnd)->aborted);
}

TEST_CASE("A probe dropout at device start makes that device inconclusive", "[service]")
{
    Bench b;
    b.ResponsiveTank();
    b.GotoTime(19, 30);
    b.RunFor(400);     // device 0 done (in settle)
    b.probeOk = false; // device 1 starts without a reading
    b.RunFor(100);
    b.probeOk = true;
    b.RunFor(1000);
    REQUIRE(b.st.p.lastOutcome[0] == Outcome::Pass);
    REQUIRE(b.st.p.lastOutcome[1] == Outcome::Unchecked); // 0 % threshold wins
    Bench c;
    c.cfg.devices[1].minResponsePct = 1.0f;
    c.ResponsiveTank();
    c.GotoTime(19, 30);
    c.RunFor(400);
    c.probeOk = false;
    c.RunFor(100);
    c.probeOk = true;
    c.RunFor(1000);
    REQUIRE(c.st.p.lastOutcome[1] == Outcome::Inconclusive);
    REQUIRE(c.Count(EventType::RunEnd) == 1);
}

TEST_CASE("Persistent state survives a reboot: no second run today, fail flags kept", "[service]")
{
    Bench b;
    b.ResponsiveTank(-0.005f, -0.05f);
    b.GotoTime(19, 30);
    b.RunFor(1400);
    REQUIRE(b.st.p.failActive[0]);
    Bench after;
    after.st.p = b.st.p; // as the app would restore it from NVS
    after.day = b.day;
    after.secOfDay = b.secOfDay;
    Output o = after.Tick();
    REQUIRE(o.failed[0]);
    after.GotoTime(20, 50);
    REQUIRE(after.Count(EventType::RunStart) == 0);
}

TEST_CASE("Service invariants on random evenings", "[service][property]")
{
    proptest::Forall(150,
                     [](proptest::Rng& pRng)
                     {
                         Bench b;
                         for(auto& d : b.cfg.devices)
                         {
                             d.serviceS = pRng.Coin(0.6) ? 60 + pRng.Below(4) * 60 : 0;
                             d.minResponsePct = pRng.Coin() ? 1.0f : 0.0f;
                         }
                         b.cfg.settleS = pRng.Below(3) * 30;
                         b.cfg.tailS = pRng.Below(3) * 30;
                         if(pRng.Coin(0.3))
                         {
                             b.cfg.induceDeficitS = 60;
                             b.cfg.induceDeficitDevice = 1 + pRng.Below(6);
                         }
                         b.sat = pRng.Uniform(95.0f, 110.0f);
                         b.tank = [&b, &pRng](const Output& pO)
                         {
                             for(std::size_t i = 0; i < 6; ++i)
                                 if(pO.deviceOn[i]) b.sat += pRng.Uniform(-0.1f, 0.02f);
                             b.sat += pRng.Uniform(-0.02f, 0.02f);
                         };
                         uint32_t begins = 0;
                         bool wasRunning = false;
                         for(int t = 0; t < 3 * 8640; ++t)
                         { // three days
                             if(pRng.Coin(0.002)) b.level = pRng.Coin() ? Level::Normal : Level::Blue;
                             if(pRng.Coin(0.001)) b.fault = !b.fault;
                             if(pRng.Coin(0.001)) b.maintenance = !b.maintenance;
                             b.probeOk = !pRng.Coin(0.01);
                             const Output o = b.Tick(pRng.Coin(0.0005));
                             begins += static_cast<uint32_t>(o.running && !wasRunning);
                             wasRunning = o.running;

                             int on = 0;
                             for(std::size_t i = 0; i < 6; ++i)
                             {
                                 if(o.deviceOn[i])
                                 {
                                     ++on;
                                     REQUIRE(b.cfg.devices[i].serviceS > 0);
                                 }
                             }
                             REQUIRE(on <= 1);
                             if(b.level != Level::Normal || b.fault) REQUIRE_FALSE(o.running);
                             if(o.cutDevice.has_value())
                             {
                                 REQUIRE(static_cast<uint32_t>(*o.cutDevice) + 1u == b.cfg.induceDeficitDevice);
                                 REQUIRE(on == 0);
                             }
                             REQUIRE(o.events.size() <= o.events.Capacity());
                         }
                         REQUIRE(b.Count(EventType::RunEnd) <= begins);
                         REQUIRE(b.Count(EventType::RunEnd) + 1 >= begins); // at most the last run is still open
                         REQUIRE(b.Count(EventType::RunStart) <= begins);
                     });
}

TEST_CASE("Defensive branches: out-of-range deficit device, a run longer than its window, manual skip with no clock",
          "[service]")
{
    Config c = ExampleCfg();
    c.induceDeficitS = 60;
    c.induceDeficitDevice = 7;
    REQUIRE(RunDurationS(c) == 1320); // 7 is not a device: no deficit phase

    Bench b; // three 3600 s runs cannot fit 19:30–21:00: the latest start clamps to the window start
    b.cfg.devices[0].serviceS = b.cfg.devices[1].serviceS = b.cfg.devices[2].serviceS = 3600;
    b.sat = 100.2f; // no headroom, yet it must start at 19:30
    b.GotoTime(19, 30);
    REQUIRE(b.Count(EventType::RunStart) == 1);

    Bench m;
    m.clockKnown = false;
    m.fault = true;
    m.Tick(true);
    REQUIRE(m.last(EventType::Skipped)->skip == Skip::Fault);
    REQUIRE_FALSE(m.st.attemptedDay.has_value());
}
