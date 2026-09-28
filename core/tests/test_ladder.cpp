#include <functional>

#include "catch_amalgamated.hpp"

#include "reefdo/ladder.hpp"

using namespace reefdo::ladder;

namespace
{

constexpr uint32_t PERIOD_S = 10;

// The example device table.
Config ExampleConfig()
{
    Config c;
    c.devices[0] = {Trigger::Blue, Mode::On, false};       // small bubbler
    c.devices[1] = {Trigger::Blue, Mode::On, false};       // extra powerhead
    c.devices[2] = {Trigger::Yellow, Mode::On, false};     // strong air pump
    c.devices[3] = {Trigger::Yellow, Mode::On, true};      // siren
    c.devices[4] = {Trigger::Blue, Mode::PulseOff, false}; // return pump
    c.devices[5] = {Trigger::Heat, Mode::On, false};       // fan
    return c;
}

struct Bench
{
    Config cfg = ExampleConfig();
    State st;
    uint64_t nowMs = 1'000'000; // never start at 0: timers must not depend on it
    std::optional<uint16_t> minute = uint16_t{12 * 60};
    float temp = 26.0f;

    Input MakeInput(std::optional<float> pD) const
    {
        Input in;
        in.doMgl = pD;
        in.tempC = pD.has_value() ? std::optional<float>(temp) : std::nullopt;
        in.nowMs = nowMs;
        in.minuteOfDay = minute;
        return in;
    }
    // One tick with optional tweaks to the input.
    Output Tick(std::optional<float> pD, const std::function<void(Input&)>& pMod = {})
    {
        Input in = MakeInput(pD);
        if(pMod) pMod(in);
        Output o = Step(st, in, cfg);
        nowMs += PERIOD_S * 1000;
        return o;
    }
    // Hold a reading so that at the last tick it has been held for `seconds` (ticks at 0, 10, ..., seconds).
    Output Hold(std::optional<float> pD, uint32_t pSeconds, const std::function<void(Input&)>& pMod = {})
    {
        Output o;
        for(uint32_t t = 0; t <= pSeconds; t += PERIOD_S)
            o = Tick(pD, pMod);
        return o;
    }
};

bool HasEvent(const Output& pO, EventType pT)
{
    for(const Event& e : pO.events)
        if(e.type == pT) return true;
    return false;
}

} // namespace

TEST_CASE("Normal: nothing on, silent; pulse devices are powered", "[ladder]")
{
    Bench b;
    const Output o = b.Hold(6.5f, 3600);
    REQUIRE(o.level == Level::Normal);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.fault);
    REQUIRE(o.deviceOn == std::array<bool, reefdo::DEVICES>{false, false, false, false, true, false});
    REQUIRE(o.events.empty());
}

TEST_CASE("Blue enters below mgl - h only after dwell; devices and pulse follow", "[ladder]")
{
    Bench b;
    Output o = b.Hold(5.60f, 110);
    REQUIRE(o.level == Level::Normal);
    o = b.Tick(5.60f); // 120 s
    REQUIRE(o.level == Level::Blue);
    REQUIRE(o.sound); // may sound; the configured pattern decides
    REQUIRE(HasEvent(o, EventType::LevelChange));
    REQUIRE_FALSE(HasEvent(o, EventType::EffectiveChange)); // retired: the level is all there is
    REQUIRE(HasEvent(o, EventType::Pulse));
    REQUIRE(o.deviceOn[0]);
    REQUIRE(o.deviceOn[1]);
    REQUIRE_FALSE(o.deviceOn[2]);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE_FALSE(o.deviceOn[4]); // pulse in progress: return pump cut
    o = b.Tick(5.60f);            // 10 s later the pulse is over
    REQUIRE(o.deviceOn[4]);
    REQUIRE(o.events.empty());
}

TEST_CASE("Entry just above the band does not count", "[ladder]")
{
    Bench b;
    REQUIRE(b.Hold(5.66f, 3600).level == Level::Normal);
}

TEST_CASE("A break in the entry condition restarts the dwell", "[ladder]")
{
    Bench b;
    b.Hold(5.60f, 110);
    b.Tick(5.70f);
    REQUIRE(b.Hold(5.60f, 110).level == Level::Normal);
    REQUIRE(b.Tick(5.60f).level == Level::Blue);
}

TEST_CASE("Hovering inside the hysteresis band toggles nothing", "[ladder]")
{
    Bench b;
    b.Hold(5.60f, 120);
    REQUIRE(b.st.level == Level::Blue);
    for(int i = 0; i < 720; ++i)
    { // two hours wandering between 5.66 and 5.94
        const float d = 5.66f + 0.28f * static_cast<float>(i % 7) / 6.0f;
        const Output o = b.Tick(d);
        REQUIRE(o.level == Level::Blue);
        REQUIRE(o.events.empty());
    }
}

TEST_CASE("Recovery needs recover_sustain_s above mgl + h, and a dip resets it", "[ladder]")
{
    Bench b;
    b.Hold(5.60f, 120);
    Output o = b.Hold(5.96f, 590);
    REQUIRE(o.level == Level::Blue);
    o = b.Tick(5.96f);
    REQUIRE(o.level == Level::Normal);
    REQUIRE(HasEvent(o, EventType::LevelChange));
    REQUIRE(o.deviceOn[0] == false);

    b.Hold(5.60f, 120);
    b.Hold(5.96f, 590);
    b.Tick(5.90f); // dip below the exit threshold: sustain restarts
    REQUIRE(b.Hold(5.96f, 590).level == Level::Blue);
    REQUIRE(b.Tick(5.96f).level == Level::Normal);
}

TEST_CASE("Red is immediate and may be reached from Normal directly", "[ladder]")
{
    Bench b;
    const Output o = b.Tick(4.20f);
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn == std::array<bool, reefdo::DEVICES>{true, true, true, true, false, false}); // pump pulsing
}

TEST_CASE("Yellow: audible, siren on, Blue devices stay on", "[ladder]")
{
    Bench b;
    b.Hold(5.60f, 120);
    Output o = b.Hold(5.00f, 50);
    REQUIRE(o.level == Level::Blue);
    o = b.Tick(5.00f); // 60 s dwell
    REQUIRE(o.level == Level::Yellow);
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn[0]);
    REQUIRE(o.deviceOn[2]);
    REQUIRE(o.deviceOn[3]);
}

TEST_CASE("Recovery from Red steps one level at a time", "[ladder]")
{
    Bench b;
    b.Tick(4.0f);
    REQUIRE(b.st.level == Level::Red);
    REQUIRE(b.Hold(7.0f, 590).level == Level::Red);
    REQUIRE(b.Tick(7.0f).level == Level::Yellow);
    REQUIRE(b.Hold(7.0f, 590).level == Level::Yellow);
    REQUIRE(b.Tick(7.0f).level == Level::Blue);
    REQUIRE(b.Hold(7.0f, 590).level == Level::Blue);
    const Output o = b.Tick(7.0f);
    REQUIRE(o.level == Level::Normal);
}

TEST_CASE("FAULT after five failed polls: loud, acts as Yellow, clears on the next reading", "[ladder]")
{
    Bench b;
    b.Hold(6.5f, 60);
    Output o;
    for(int i = 0; i < 4; ++i)
    {
        o = b.Tick(std::nullopt);
        REQUIRE_FALSE(o.fault);
    }
    o = b.Tick(std::nullopt);
    REQUIRE(o.fault);
    REQUIRE(HasEvent(o, EventType::FaultEnter));
    REQUIRE(o.level == Level::Normal); // the level stays what the readings said; FAULT shows as FAULT
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn[0]);
    REQUIRE(o.deviceOn[2]);
    REQUIRE(o.deviceOn[3]);
    REQUIRE(o.deviceOn[4]); // no pulse in FAULT
    REQUIRE_FALSE(HasEvent(o, EventType::Pulse));

    o = b.Tick(std::nullopt); // stays in fault, no duplicate event
    REQUIRE(o.fault);
    REQUIRE_FALSE(HasEvent(o, EventType::FaultEnter));

    o = b.Tick(6.5f);
    REQUIRE_FALSE(o.fault);
    REQUIRE(HasEvent(o, EventType::FaultClear));
    REQUIRE(o.level == Level::Normal);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.deviceOn[2]); // fault_level's devices stop with the FAULT
}

TEST_CASE("Failures must be consecutive to count", "[ladder]")
{
    Bench b;
    for(int round = 0; round < 3; ++round)
    {
        for(int i = 0; i < 4; ++i)
            REQUIRE_FALSE(b.Tick(std::nullopt).fault);
        REQUIRE_FALSE(b.Tick(6.5f).fault);
    }
}

TEST_CASE("FAULT freezes the level machine and never lowers an existing level", "[ladder]")
{
    Bench b;
    b.Tick(4.0f);
    const Output o = b.Hold(std::nullopt, 3600);
    REQUIRE(o.fault);
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.sound);
}

TEST_CASE("FAULT runs fault_level's devices (at least Yellow's), never fewer than the level's own", "[ladder]")
{
    Bench b;
    b.cfg.faultLevel = Level::Blue; // clamped to Yellow
    Output o = b.Hold(std::nullopt, 50);
    REQUIRE(o.level == Level::Normal);
    REQUIRE(o.deviceOn[2]); // a Yellow device

    Bench r;
    r.cfg.faultLevel = Level::Red;
    r.cfg.devices[5] = {Trigger::Red, Mode::On, false};
    o = r.Hold(std::nullopt, 50);
    REQUIRE(o.deviceOn[5]); // a Red device

    Bench y; // already at Red when the probe fails: FAULT as Yellow keeps Red's devices
    y.cfg.faultLevel = Level::Yellow;
    y.cfg.devices[5] = {Trigger::Red, Mode::On, false};
    y.Tick(4.0f);
    o = y.Hold(std::nullopt, 50);
    REQUIRE(o.fault);
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.deviceOn[5]);
}

TEST_CASE("Ack silences buzzer and siren, not the pumps; expires; deeper re-arms", "[ladder]")
{
    Bench b;
    b.Hold(5.00f, 60);
    REQUIRE(b.st.level == Level::Yellow);
    Output o = b.Tick(5.00f, [](Input& pIn) { pIn.ackPressed = true; });
    REQUIRE(HasEvent(o, EventType::Ack));
    REQUIRE(o.silenced);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE(o.deviceOn[2]);
    REQUIRE(o.level == Level::Yellow);

    o = b.Hold(5.00f, 1780);
    REQUIRE(o.silenced);
    o = b.Tick(5.00f); // 1800 s: silence expires
    REQUIRE_FALSE(o.silenced);
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn[3]);

    b.Tick(5.00f, [](Input& pIn) { pIn.ackPressed = true; });
    o = b.Tick(4.0f); // Red re-arms immediately
    REQUIRE(o.level == Level::Red);
    REQUIRE_FALSE(o.silenced);
    REQUIRE(o.sound);
}

TEST_CASE("A FAULT re-arms an acknowledged alarm", "[ladder]")
{
    Bench b;
    b.Hold(5.00f, 60);
    b.Tick(5.00f, [](Input& pIn) { pIn.ackPressed = true; });
    REQUIRE(b.st.ackUntilMs.has_value());
    const Output o = b.Hold(std::nullopt, 50); // the probe stops answering
    REQUIRE(o.fault);
    REQUIRE_FALSE(o.silenced);
    REQUIRE(o.sound);
}

TEST_CASE("Blue keeps a siren assigned to it off; the buzzer may sound if configured", "[ladder]")
{
    Bench b;
    b.cfg.devices[3] = {Trigger::Blue, Mode::On, true};
    Output o = b.Hold(5.60f, 120);
    REQUIRE(o.level == Level::Blue);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE(o.sound);
    o = b.Hold(5.00f, 60);
    REQUIRE(o.level == Level::Yellow);
    REQUIRE(o.deviceOn[3]);
}

TEST_CASE("Pulse: once per activation, rate-limited, never in FAULT or maintenance", "[ladder]")
{
    Bench b;
    Output o = b.Hold(5.60f, 120);
    REQUIRE(HasEvent(o, EventType::Pulse));
    o = b.Hold(5.96f, 600); // recover
    REQUIRE(o.level == Level::Normal);
    o = b.Hold(5.60f, 120); // second activation 12 min later: inside the 30 min interval
    REQUIRE(o.level == Level::Blue);
    REQUIRE_FALSE(HasEvent(o, EventType::Pulse));
    REQUIRE(o.deviceOn[4]);

    b.Hold(5.96f, 600);
    b.Hold(6.5f, 1800); // wait out the interval
    o = b.Hold(5.60f, 120);
    REQUIRE(HasEvent(o, EventType::Pulse));

    SECTION("not in maintenance")
    {
        Bench m;
        o = m.Hold(5.60f, 120, [](Input& pIn) { pIn.maintenance = true; });
        REQUIRE(o.level == Level::Blue);
        REQUIRE_FALSE(HasEvent(o, EventType::Pulse));
        REQUIRE(o.deviceOn[4]);
    }
}

TEST_CASE("Pulse length is exactly pulse_s", "[ladder]")
{
    Bench b;
    b.cfg.pulseS = 30;
    Output o = b.Hold(5.60f, 120);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(5.60f);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(5.60f);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(5.60f); // 30 s elapsed
    REQUIRE(o.deviceOn[4]);
}

TEST_CASE("Pulses never run back to back: a re-activation during a pulse does not restart it", "[ladder]")
{
    Bench b;
    b.cfg.pulseS = 30;
    b.cfg.pulseMinIntervalS = 0;
    b.cfg.blue.dwellS = 0;
    b.cfg.recoverSustainS = 10;
    Output o = b.Tick(5.60f); // Blue at once, pulse starts (T)
    REQUIRE(o.level == Level::Blue);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(6.50f); // T+10: above exit, sustain starts
    REQUIRE(o.level == Level::Blue);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(6.50f); // T+20: Normal; device level inactive
    REQUIRE(o.level == Level::Normal);
    REQUIRE_FALSE(o.deviceOn[4]);
    o = b.Tick(5.60f); // T+30: Blue again while the pulse is ending — no new pulse
    REQUIRE(o.level == Level::Blue);
    REQUIRE_FALSE(HasEvent(o, EventType::Pulse));
    REQUIRE(o.deviceOn[4]); // restored exactly at pulse_s
    o = b.Tick(5.60f);
    REQUIRE(o.deviceOn[4]);
}

TEST_CASE("Heat device follows temperature with hysteresis, independent of level", "[ladder]")
{
    Bench b;
    b.temp = 27.9f;
    Output o = b.Tick(6.5f);
    REQUIRE_FALSE(o.heat);
    b.temp = 28.0f;
    o = b.Tick(6.5f);
    REQUIRE(o.heat);
    REQUIRE(HasEvent(o, EventType::HeatOn));
    REQUIRE(o.deviceOn[5]);
    REQUIRE(o.level == Level::Normal);
    b.temp = 27.6f;
    o = b.Tick(6.5f);
    REQUIRE(o.heat);          // between the thresholds: unchanged
    o = b.Tick(std::nullopt); // probe down: keep the decision
    REQUIRE(o.heat);
    b.temp = 27.5f;
    o = b.Tick(6.5f);
    REQUIRE_FALSE(o.heat);
    REQUIRE(HasEvent(o, EventType::HeatOff));
    REQUIRE_FALSE(o.deviceOn[5]);
}

TEST_CASE("Night lock holds Blue until morning once triggered at night", "[ladder]")
{
    Bench b;
    b.cfg.nightLock = true;
    b.minute = uint16_t{23 * 60};
    b.Hold(5.60f, 120);
    REQUIRE(b.st.level == Level::Blue);
    Output o = b.Hold(7.0f, 3600);
    REQUIRE(o.level == Level::Blue); // would have recovered in 600 s without the lock
    b.minute = uint16_t{9 * 60};
    o = b.Tick(7.0f); // sustain was already satisfied: steps down at once
    REQUIRE(o.level == Level::Normal);
}

TEST_CASE("Night lock still lets Yellow step down to Blue, and is off by default", "[ladder]")
{
    Bench b;
    b.cfg.nightLock = true;
    b.minute = uint16_t{2 * 60};
    b.Hold(5.00f, 60);
    REQUIRE(b.st.level == Level::Yellow);
    REQUIRE(b.Hold(7.0f, 600).level == Level::Blue);
    REQUIRE(b.Hold(7.0f, 3600).level == Level::Blue);

    Bench d; // default: no lock
    d.minute = uint16_t{2 * 60};
    d.Hold(5.60f, 120);
    REQUIRE(d.Hold(7.0f, 600).level == Level::Normal);
}

TEST_CASE("Night lock: unknown clock counts as night (conservative), unless configured otherwise", "[ladder]")
{
    Bench b;
    b.cfg.nightLock = true;
    b.minute = std::nullopt;
    b.Hold(5.60f, 120);
    REQUIRE(b.Hold(7.0f, 3600).level == Level::Blue);

    Bench c;
    c.cfg.nightLock = true;
    c.cfg.unknownTimeIsNight = false;
    c.minute = std::nullopt;
    c.Hold(5.60f, 120);
    REQUIRE(c.Hold(7.0f, 600).level == Level::Normal);
}

TEST_CASE("Night lock armed at night is not carried into a trigger that happens by day", "[ladder]")
{
    Bench b;
    b.cfg.nightLock = true;
    b.minute = uint16_t{14 * 60};
    b.Hold(5.60f, 120);
    REQUIRE(b.Hold(7.0f, 600).level == Level::Normal); // day: no floor
}

TEST_CASE("is_night handles a window that wraps midnight and one that does not", "[ladder]")
{
    Config c; // 21:00 → 08:00
    REQUIRE(IsNight(uint16_t{22 * 60}, c));
    REQUIRE(IsNight(uint16_t{3 * 60}, c));
    REQUIRE(IsNight(uint16_t{21 * 60}, c));
    REQUIRE_FALSE(IsNight(uint16_t{8 * 60}, c));
    REQUIRE_FALSE(IsNight(uint16_t{12 * 60}, c));
    REQUIRE(IsNight(std::nullopt, c));
    c.unknownTimeIsNight = false;
    REQUIRE_FALSE(IsNight(std::nullopt, c));

    c.nightStartMin = 1 * 60; // 01:00 → 06:00, no wrap
    c.nightEndMin = 6 * 60;
    REQUIRE(IsNight(uint16_t{3 * 60}, c));
    REQUIRE_FALSE(IsNight(uint16_t{0}, c));
    REQUIRE_FALSE(IsNight(uint16_t{6 * 60}, c));
}

TEST_CASE("Maintenance mutes buzzer and siren, and keeps level-driven devices on", "[ladder]")
{
    Bench b;
    b.Hold(5.00f, 60);
    const Output o = b.Tick(5.00f, [](Input& pIn) { pIn.maintenance = true; });
    REQUIRE(o.level == Level::Yellow);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE(o.deviceOn[2]);
    REQUIRE(o.deviceOn[0]);
}

TEST_CASE("Slope trigger enters Blue on a fast fall even above the threshold", "[ladder]")
{
    Bench b;
    b.cfg.blueSlopeMglPer10min = 0.5f;
    Output o = b.Hold(6.5f, 120, [](Input& pIn) { pIn.slopeMglPer10min = -0.4f; });
    REQUIRE(o.level == Level::Normal);
    o = b.Hold(6.5f, 120, [](Input& pIn) { pIn.slopeMglPer10min = -0.5f; });
    REQUIRE(o.level == Level::Blue);
}

TEST_CASE("A device with no trigger is never on", "[ladder]")
{
    Bench b;
    b.cfg.devices[5] = {Trigger::None, Mode::On, false};
    const Output o = b.Tick(4.0f);
    REQUIRE(o.level == Level::Red);
    REQUIRE_FALSE(o.deviceOn[5]);
}

TEST_CASE("Event sequence of a full night: Blue, Yellow, recovery", "[ladder]")
{
    Bench b;
    Output o = b.Hold(5.60f, 120);
    REQUIRE(o.events.size() == 2); // LevelChange, Pulse
    REQUIRE(o.events[0].type == EventType::LevelChange);
    REQUIRE(o.events[0].from == Level::Normal);
    REQUIRE(o.events[0].to == Level::Blue);
    REQUIRE(o.events[1].type == EventType::Pulse);
    REQUIRE(o.events[1].device == 4);

    o = b.Hold(5.00f, 60);
    REQUIRE(o.events.size() == 1);
    REQUIRE(o.events[0].to == Level::Yellow);

    o = b.Hold(7.0f, 600);
    REQUIRE(o.events.size() == 1);
    REQUIRE(o.events[0].from == Level::Yellow);
    REQUIRE(o.events[0].to == Level::Blue);
}

TEST_CASE("fault.alert off: failed polls are counted but FAULT never fires", "[ladder]")
{
    Bench b;
    b.cfg.faultAlert = false;
    Output o = b.Hold(6.5f, 60);
    o = b.Hold(std::nullopt, 600);
    REQUIRE_FALSE(o.fault);
    REQUIRE(o.level == Level::Normal);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(HasEvent(o, EventType::FaultEnter));
    o = b.Tick(6.5f);
    REQUIRE_FALSE(HasEvent(o, EventType::FaultClear));
}

TEST_CASE("day_alarm auto_ack: an alarm is heard for DAY_ACK_AFTER_S, then acknowledges itself", "[ladder]")
{
    Bench b; // 12:00: day
    b.cfg.dayAlarm = DayAlarm::AutoAck;
    Output o = b.Hold(5.00f, 60);
    REQUIRE(o.level == Level::Yellow);
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn[3]); // the siren
    b.nowMs -= 5'000;       // a 5 s sample period: heard for 5 s, not yet long enough
    o = b.Tick(5.00f);
    REQUIRE(o.sound);
    REQUIRE(o.events.empty());
    o = b.Tick(5.00f); // heard for DAY_ACK_AFTER_S
    REQUIRE(HasEvent(o, EventType::AutoAck));
    REQUIRE_FALSE(HasEvent(o, EventType::Ack));
    REQUIRE(o.silenced);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE(o.deviceOn[2]); // the pumps run on

    // Deeper re-arms, and is announced again.
    o = b.Tick(4.00f);
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.sound);
    REQUIRE(HasEvent(b.Tick(4.00f), EventType::AutoAck));

    // The ack lasts ack_silence_s like a pressed one; an alarm still standing then is announced again.
    o = b.Hold(4.00f, 1780);
    REQUIRE(o.silenced);
    o = b.Tick(4.00f);
    REQUIRE(o.sound);
    REQUIRE(o.events.empty());
    REQUIRE(HasEvent(b.Tick(4.00f), EventType::AutoAck));

    // The night is untouched.
    Bench n;
    n.cfg.dayAlarm = DayAlarm::AutoAck;
    n.minute = uint16_t{23 * 60};
    o = n.Hold(4.00f, 600);
    REQUIRE(o.sound);
    REQUIRE_FALSE(o.silenced);
}

TEST_CASE("day_alarm suppress: silent by day, the siren too; the level and the pumps run; the night sounds", "[ladder]")
{
    Bench b;
    b.cfg.dayAlarm = DayAlarm::Suppress;
    Output o = b.Hold(4.00f, 60);
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.suppressed);
    REQUIRE_FALSE(o.silenced);
    REQUIRE_FALSE(o.sound);
    REQUIRE_FALSE(o.deviceOn[3]);
    REQUIRE(o.deviceOn[2]);
    REQUIRE(o.deviceOn[0]);

    b.minute = uint16_t{21 * 60};
    o = b.Tick(4.00f);
    REQUIRE_FALSE(o.suppressed);
    REQUIRE(o.sound);
    REQUIRE(o.deviceOn[3]);
    b.minute = std::nullopt; // an unknown clock counts as night
    REQUIRE(b.Tick(4.00f).sound);
}

TEST_CASE("A sudden drop is suspect: every dwell is extend_s longer until the reading is back", "[ladder]")
{
    Bench b; // defaults: 3 mg/L per 10 min over 2 min, 600 s
    const auto fall = [](Input& pIn) { pIn.fastSlopeMglPer10min = -8.0f; };
    Output o = b.Tick(6.5f, fall); // suspect before any threshold is crossed
    REQUIRE(HasEvent(o, EventType::SuddenDrop));
    REQUIRE(o.suspect);
    o = b.Hold(4.00f, 590, fall); // Red's dwell: 0 + 600 s
    REQUIRE(o.level == Level::Normal);
    REQUIRE(o.events.empty()); // logged once
    o = b.Tick(4.00f);         // no longer falling, but not back either
    REQUIRE(o.level == Level::Red);
    REQUIRE(o.suspect);
    o = b.Tick(7.0f);
    REQUIRE(HasEvent(o, EventType::SuddenClear));
    REQUIRE_FALSE(o.suspect);
    REQUIRE(o.level == Level::Red); // recovery as usual
}

TEST_CASE("A sudden dip back within extend_s changes no level; the threshold counts; 0 turns it off", "[ladder]")
{
    Bench b;
    Output o = b.Hold(4.00f, 300, [](Input& pIn) { pIn.fastSlopeMglPer10min = -3.0f; });
    REQUIRE(o.suspect);
    REQUIRE(o.level == Level::Normal);
    o = b.Tick(6.5f, [](Input& pIn) { pIn.fastSlopeMglPer10min = 9.0f; });
    REQUIRE(HasEvent(o, EventType::SuddenClear));
    REQUIRE(o.level == Level::Normal);

    Bench slow;
    o = slow.Tick(4.00f, [](Input& pIn) { pIn.fastSlopeMglPer10min = -2.9f; });
    REQUIRE_FALSE(o.suspect);
    REQUIRE(o.level == Level::Red);

    Bench off;
    off.cfg.suddenSlopeMglPer10min = 0.0f;
    o = off.Tick(4.00f, [](Input& pIn) { pIn.fastSlopeMglPer10min = -50.0f; });
    REQUIRE_FALSE(o.suspect);
    REQUIRE(o.level == Level::Red);
}
