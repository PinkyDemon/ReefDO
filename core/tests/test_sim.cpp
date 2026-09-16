#include <cmath>

#include "catch_amalgamated.hpp"

#include "reefdo/sim.hpp"
#include "reefdo/solubility.hpp"

using namespace reefdo::sim;
using Catch::Matchers::WithinAbs;
using reefdo::probe::Status;

namespace
{
const std::array<bool, 6> ALL_OFF{};
}

TEST_CASE("Tank: photosynthesis by day, respiration at night, exchange pulls toward 100 %", "[sim]")
{
    TankConfig c;
    c.sat0Pct = 100.0f;
    Tank t(c);
    REQUIRE(t.LightsOn(12 * 60));
    REQUIRE_FALSE(t.LightsOn(23 * 60));
    for(int i = 0; i < 6 * 360; ++i)
        t.Step(10.0f, 12 * 60, ALL_OFF); // six hours of daylight
    REQUIRE(t.SatPct() > 103.0f);
    REQUIRE(t.SatPct() < 106.0f); // equilibrium: 100 + 3/0.6 = 105
    REQUIRE_THAT(t.TempC(), WithinAbs(26.5, 1e-4));
    for(int i = 0; i < 6 * 360; ++i)
        t.Step(10.0f, 23 * 60, ALL_OFF); // six hours of night
    REQUIRE(t.SatPct() < 92.0f);
    REQUIRE(t.SatPct() > 88.0f); // equilibrium: 100 - 6/0.6 = 90
    REQUIRE_THAT(t.TempC(), WithinAbs(25.6, 1e-4));
    REQUIRE_THAT(t.DoMgl(), WithinAbs(reefdo::solubility::MglFromSaturation(t.SatPct(), 25.6f, 35.0f), 1e-4));

    std::array<bool, 6> bubbler{};
    bubbler[0] = true;
    t.SetDeviceK(0, 3.0f);
    for(int i = 0; i < 360; ++i)
        t.Step(10.0f, 23 * 60, bubbler); // an hour of bubbling at night
    REQUIRE(t.SatPct() > 97.0f);         // 100 - 6/3.6 = 98.3
}

TEST_CASE("Tank: a stalled return pump stops the exchange until a power cycle", "[sim]")
{
    TankConfig c;
    c.returnPumpDevice = 4;
    c.sat0Pct = 95.0f;
    Tank t(c);
    std::array<bool, 6> on{};
    on[4] = true;
    t.StallReturnPump(true);
    REQUIRE(t.ReturnPumpStalled());
    for(int i = 0; i < 180; ++i)
        t.Step(10.0f, 23 * 60, on); // 30 min, pump powered but stalled: falls at 6 %/h
    REQUIRE_THAT(t.SatPct(), WithinAbs(92.0, 0.05));
    on[4] = false;
    t.Step(10.0f, 23 * 60, on); // power cut...
    on[4] = true;
    t.Step(10.0f, 23 * 60, on); // ...and back: the stall clears
    REQUIRE_FALSE(t.ReturnPumpStalled());
    const float before = t.SatPct();
    for(int i = 0; i < 180; ++i)
        t.Step(10.0f, 23 * 60, on);
    REQUIRE(t.SatPct() > before - 1.0f); // still sinking toward 90, but far slower than the 3 %/30 min of the stall
    t.SetSat(0.5f);
    for(int i = 0; i < 100; ++i)
        t.Step(10.0f, 23 * 60, ALL_OFF);
    REQUIRE(t.SatPct() == 0.0f); // clamped, never negative
}

TEST_CASE("Probe model: lag, noise, seawater vs freshwater reference, calibration counter", "[sim]")
{
    TankConfig c;
    c.sat0Pct = 100.0f;
    Tank t(c);
    ProbeModel m;
    m.noisePct = 0.0f;
    Probe p(t, m, 3);
    t.SetSat(90.0f);
    const auto r1 = p.Poll();
    REQUIRE(r1.status == Status::Ok);
    REQUIRE(r1.reading->satPct > 95.0f); // lagging behind the step
    REQUIRE(r1.reading->satPct < 100.0f);
    for(int i = 0; i < 20; ++i)
        p.Poll();
    const auto r2 = p.Poll();
    REQUIRE_THAT(r2.reading->satPct, WithinAbs(90.0, 0.1));
    REQUIRE_THAT(r2.reading->doMgl,
                 WithinAbs(reefdo::solubility::MglFromSaturation(90.0f, r2.reading->tempC, 35.0f), 0.01));

    ProbeModel fw = m;
    fw.freshwaterReference = true;
    Probe pf(t, fw, 3);
    for(int i = 0; i < 30; ++i)
        pf.Poll();
    const auto rf = pf.Poll();
    REQUIRE(rf.reading->doMgl > r2.reading->doMgl * 1.15f); // ~1.22× the seawater number

    ProbeModel noisy = m;
    noisy.noisePct = 1.0f;
    Probe pn(t, noisy, 11);
    float lo = 999;
    float hi = -999;
    for(int i = 0; i < 200; ++i)
    {
        const float s = pn.Poll().reading->satPct;
        lo = s < lo ? s : lo;
        hi = s > hi ? s : hi;
    }
    REQUIRE(hi - lo > 1.0f);
    REQUIRE(hi - lo <= 2.0f);
    REQUIRE(p.Calibrations() == 0);
    REQUIRE(p.AirCalibrate() == reefdo::probe::CalResult::Ok);
    REQUIRE(p.Calibrations() == 1);
    Probe zeroSeed(t, m, 0); // seed 0 is replaced, not used as-is
    REQUIRE(zeroSeed.Poll().status == Status::Ok);
}

TEST_CASE("Probe model: every fault knob", "[sim]")
{
    TankConfig c;
    Tank t(c);
    ProbeModel m;
    m.noisePct = 0.0f;
    Probe p(t, m, 5);
    REQUIRE(p.Poll().status == Status::Ok);

    p.Model().dropout = true;
    REQUIRE(p.Poll().status == Status::Timeout);
    REQUIRE_FALSE(p.Poll().reading.has_value());
    p.Model().dropout = false;

    p.Model().crcErrorEvery = 2;
    REQUIRE(p.Poll().status == Status::FrameError); // poll #4
    REQUIRE(p.Poll().status == Status::Ok);
    REQUIRE(p.Poll().status == Status::FrameError);
    p.Model().crcErrorEvery = 0;

    p.Model().implausible = true;
    REQUIRE(p.Poll().status == Status::Implausible);
    p.Model().implausible = false;

    const auto good = p.Poll();
    p.Model().stuck = true;
    const auto s1 = p.Poll();
    const auto s2 = p.Poll();
    REQUIRE(s1.status == Status::Stuck);
    REQUIRE(s1.reading->doMgl == good.reading->doMgl);
    REQUIRE(s2.reading->tempC == good.reading->tempC);
    p.Model().stuck = false;

    p.Model().bubblePct = 8.0f;
    REQUIRE(p.Poll().reading->satPct > t.SatPct() + 7.0f);

    Probe fresh(t, m, 5); // stuck before any reading: nothing to repeat, falls through to a live reading
    fresh.Model().stuck = true;
    REQUIRE(fresh.Poll().status == Status::Ok);
}
