// Ladder invariants, checked at every tick of random histories.
#include "catch_amalgamated.hpp"

#include "reefdo/ladder.hpp"

#include "proptest.hpp"

using namespace reefdo::ladder;

namespace
{

Config RandomConfig(proptest::Rng& pRng)
{
    Config c;
    // Random but valid device table: any trigger/mode/ack combination.
    for(auto& d : c.devices)
    {
        static const Trigger TRIGGERS[] = {Trigger::None, Trigger::Blue, Trigger::Yellow, Trigger::Red, Trigger::Heat};
        d.trigger = TRIGGERS[pRng.Below(5)];
        d.mode = pRng.Coin(0.25) ? Mode::PulseOff : Mode::On;
        d.ackSilences = d.mode == Mode::On && pRng.Coin(0.3);
    }
    c.blue.dwellS = pRng.Below(4) * 30;
    c.yellow.dwellS = pRng.Below(3) * 30;
    c.red.dwellS = pRng.Below(2) * 10;
    c.recoverSustainS = 60 + pRng.Below(6) * 60;
    c.nightLock = pRng.Coin();
    c.escalateIfFailed = pRng.Coin(0.7);
    c.blueSlopeMglPer10min = pRng.Coin(0.3) ? 0.5f : 0.0f;
    c.pulseS = 10 + pRng.Below(3) * 10;
    c.pulseMinIntervalS = pRng.Below(3) * 300;
    return c;
}

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

} // namespace

TEST_CASE("Ladder invariants hold on random histories", "[ladder][property]")
{
    proptest::Forall(300,
                     [](proptest::Rng& pRng)
                     {
                         Config cfg = RandomConfig(pRng);
                         State st;
                         float d = pRng.Uniform(4.0f, 7.5f);
                         uint64_t now = 500'000;
                         std::array<uint32_t, DEVICES> offRun{}; // consecutive ticks a pulse device has been unpowered
                         Level prevLevel = Level::Normal;
                         bool prevFault = false;

                         for(int t = 0; t < 600; ++t)
                         {
                             // Random walk with occasional jumps, probe dropouts, presses and clock states.
                             d += pRng.Uniform(-0.15f, 0.15f);
                             if(pRng.Coin(0.02)) d = pRng.Uniform(3.5f, 7.5f);
                             if(d < 3.0f) d = 3.0f;
                             if(d > 8.0f) d = 8.0f;

                             Input in;
                             in.nowMs = now;
                             in.doMgl = pRng.Coin(0.08) ? std::nullopt : std::optional<float>(d);
                             in.tempC =
                                 in.doMgl.has_value() ? std::optional<float>(pRng.Uniform(25.0f, 29.0f)) : std::nullopt;
                             in.slopeMglPer10min = pRng.Uniform(-1.0f, 1.0f);
                             in.minuteOfDay = pRng.Coin(0.1)
                                                  ? std::nullopt
                                                  : std::optional<uint16_t>(static_cast<uint16_t>(pRng.Below(1440)));
                             in.ackPressed = pRng.Coin(0.05);
                             in.maintenance = pRng.Coin(0.05);
                             for(auto& f : in.deviceFailed)
                                 f = pRng.Coin(0.1);

                             // Invariant 6 (ack never changes the level): the same tick without the press lands on the
                             // same level.
                             State shadow = st;
                             Input quiet = in;
                             quiet.ackPressed = false;
                             const Output quietOut = Step(shadow, quiet, cfg);

                             const Output o = Step(st, in, cfg);
                             now += 10'000;

                             REQUIRE(o.level == quietOut.level);
                             REQUIRE(o.effective >= o.level);

                             // Invariant 3: deeper is immediate, shallower is exactly one step, and only with a
                             // reading.
                             if(o.level < prevLevel)
                                 REQUIRE(static_cast<int>(prevLevel) - static_cast<int>(o.level) == 1);
                             if(o.level != prevLevel) REQUIRE(in.doMgl.has_value());

                             // Invariant 1: cumulative activation.
                             for(std::size_t i = 0; i < DEVICES; ++i)
                             {
                                 const DeviceConfig& dc = cfg.devices[i];
                                 if(dc.mode == Mode::PulseOff)
                                 {
                                     offRun[i] = o.deviceOn[i] ? 0 : offRun[i] + 1;
                                     REQUIRE(offRun[i] <= cfg.pulseS / 10); // invariant 7: restored after pulse_s
                                     continue;
                                 }
                                 const std::optional<Level> tl = TriggerLevel(dc.trigger);
                                 bool expect = false;
                                 if(dc.trigger == Trigger::Heat)
                                 {
                                     expect = o.heat;
                                 }
                                 else if(tl.has_value())
                                 {
                                     expect = *tl <= o.effective;
                                 }
                                 if(dc.ackSilences)
                                     expect = expect && o.effective >= Level::Yellow && !o.silenced && !in.maintenance;
                                 REQUIRE(o.deviceOn[i] == expect);
                             }

                             // Invariant 4: Blue never gets the loud patterns; Normal is silent.
                             if(o.effective == Level::Blue) REQUIRE(o.buzzer != Buzzer::Continuous);
                             if(o.effective == Level::Normal) REQUIRE(o.buzzer == Buzzer::Off);

                             // Invariant 5: FAULT is loud and acts as at least Yellow.
                             if(o.fault)
                             {
                                 REQUIRE(o.effective >= Level::Yellow);
                                 if(!o.silenced && !in.maintenance) REQUIRE(o.buzzer == Buzzer::FaultTriple);
                                 if(!prevFault) REQUIRE_FALSE(in.doMgl.has_value());
                             }
                             if(!o.fault && prevFault) REQUIRE(in.doMgl.has_value());

                             // Invariant 10: maintenance never makes noise.
                             if(in.maintenance)
                             {
                                 REQUIRE(o.buzzer == Buzzer::Off);
                                 REQUIRE(o.led == Led::Cyan);
                             }
                             if(o.silenced) REQUIRE(o.buzzer == Buzzer::Off);

                             // Events are bounded and the vector never overflowed.
                             REQUIRE(o.events.size() <= o.events.Capacity());

                             prevLevel = o.level;
                             prevFault = o.fault;
                         }
                     });
}

TEST_CASE("Hysteresis: no level change while the reading stays inside every band", "[ladder][property]")
{
    proptest::Forall(100,
                     [](proptest::Rng& pRng)
                     {
                         Config cfg; // defaults: Blue 5.8±0.15
                         State st;
                         uint64_t now = 0;
                         // Get into Blue first.
                         for(int i = 0; i < 20; ++i)
                         {
                             Input in;
                             in.doMgl = 5.5f;
                             in.tempC = 26.0f;
                             in.nowMs = now;
                             in.minuteOfDay = uint16_t{720};
                             Step(st, in, cfg);
                             now += 10'000;
                         }
                         REQUIRE(st.level == Level::Blue);
                         // Then wander strictly inside (5.65, 5.95) for a day.
                         for(int i = 0; i < 8640; ++i)
                         {
                             Input in;
                             in.doMgl = pRng.Uniform(5.66f, 5.94f);
                             in.tempC = 26.0f;
                             in.nowMs = now;
                             in.minuteOfDay = uint16_t{720};
                             const Output o = Step(st, in, cfg);
                             now += 10'000;
                             REQUIRE(o.level == Level::Blue);
                             REQUIRE(o.events.empty());
                         }
                     });
}
