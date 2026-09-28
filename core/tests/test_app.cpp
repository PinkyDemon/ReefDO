// App scenarios: the whole pipeline on the virtual tank, at 8640 ticks a simulated day.
#include "catch_amalgamated.hpp"

#include "scenario.hpp"

using namespace scenario;
using Catch::Matchers::WithinAbs;
using reefdo::log::Type;
using reefdo::probe::Status;

TEST_CASE("start: boot, correction and config records, seq recovered across a restart", "[app]")
{
    Scenario s;
    s.Start(3);
    REQUIRE(s.EventsOf(Type::Boot) == 1);
    REQUIRE(s.EventsOf(Type::Correction) == 1);
    REQUIRE(s.EventsOf(Type::Config) == 1);
    REQUIRE(s.EventsOf(Type::TimeSync) == 1);
    REQUIRE(s.NotesOf(NotifyKind::Boot) == 1);
    REQUIRE(s.app.LogA().At(0)->aux == 3); // boot reason
    REQUIRE(s.app.LogE().Count() == 4);    // all four are events
    s.RunS(300);
    const uint32_t seqBefore = s.app.GetStatus().nextSeq;
    REQUIRE(seqBefore > 30);

    // "Reboot" on the same flash: a new App over the same stores continues the sequence.
    Scenario again;
    again.a.segs = s.a.segs;
    again.e.segs = s.e.segs;
    again.Start(1);
    REQUIRE(again.app.LogA().At(0)->aux == 3);              // the old boot is still there
    REQUIRE(again.app.LogA().last()->seq == seqBefore + 3); // boot, correction, config
    REQUIRE(again.app.LogA().last()->seq == again.app.LogE().last()->seq);
}

TEST_CASE("night_sag_normal: two days, nothing fires, every sample logged, aggregates and a daily record", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(48 * 3600);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.NotesOf(NotifyKind::Level) == 0);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 0);
    REQUIRE(s.app.GetStatus().probe == Status::Ok);
    REQUIRE(s.app.GetStatus().doMgl.has_value());
    REQUIRE(*s.app.GetStatus().doMgl > 5.9f);
    REQUIRE(s.app.LogA().Count() > 2000); // 17280 samples on a 2560-record ring: it wrapped and kept the newest
    REQUIRE(s.app.LogB().Count() >= 570); // 48 h / 5 min = 576 buckets, minus the partial ones
    REQUIRE(s.app.LogB().Count() <= 577);
    REQUIRE(s.app.LogD().Count() == 2); // two midnights passed
    const auto day = s.app.LogD().At(0);
    REQUIRE(day->type == Type::Daily);
    REQUIRE(day->f0 < day->f1);                 // min < avg
    REQUIRE(day->f1 < day->f2);                 // avg < max
    REQUIRE(day->f0 > 5.8f);                    // never near Blue
    REQUIRE(s.EventsOf(Type::Correction) == 3); // boot + two evenings at 19:29
    REQUIRE(s.app.GetStatus().uptimeS >= 48 * 3600 - 10);
    REQUIRE(s.app.GetStatus().clockKnown);
}

TEST_CASE("night_crash: Blue at night corrects the sag with the Blue devices, then recovers", "[app]")
{
    reefdo::sim::TankConfig t = ExampleTank();
    t.respirationNightPctPerH = 12.0f; // a heavy night: equilibrium 80 % without help
    Scenario s(ExampleConfig(), t);
    s.Start();
    s.RunUntil(21, 0);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.RunUntilLevel(Level::Blue, 6 * 3600));
    REQUIRE(s.MinuteOfDay() > 21 * 60);
    REQUIRE(s.app.GetStatus().deviceOn[0]);
    REQUIRE(s.app.GetStatus().deviceOn[1]);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[3]);
    REQUIRE(s.app.GetStatus().sound);
    // NC devices: on = coil de-energised; the siren (NO) is off = de-energised
    REQUIRE_FALSE(s.app.GetStatus().relayEnergised[0]);
    REQUIRE(s.app.GetStatus().relayEnergised[2]);
    REQUIRE_FALSE(s.app.GetStatus().relayEnergised[3]);
    REQUIRE(s.pulses == 1); // the return pump was cut once, for one tick
    const Notification* n = s.LastNote(NotifyKind::Level);
    REQUIRE(n != nullptr);
    REQUIRE(n->level == Level::Blue);
    REQUIRE_FALSE(n->urgent);
    REQUIRE(s.NotesOf(NotifyKind::Level) == 1);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 0);
    REQUIRE(s.app.GetStatus().doMgl.value() < 5.95f);

    s.RunUntil(12, 0);
    s.RunUntil(13, 0);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.NotesOf(NotifyKind::Recovered) >= 1); // the sag can cycle Blue ↔ Normal more than once
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[0]);
    REQUIRE(s.app.LogE().Count() > 6);
    bool saw_blue = false;
    bool saw_normal = false;
    for(std::size_t i = 0; i < s.app.LogE().Count(); ++i)
    {
        const auto r = s.app.LogE().At(i);
        if(r->type != Type::Event) continue;
        if((r->flags >> 8) == 1) saw_blue = true;
        if((r->flags >> 8) == 0 && (r->flags & 0xFF) == 1) saw_normal = true;
    }
    REQUIRE(saw_blue);
    REQUIRE(saw_normal);
}

TEST_CASE("pump_stall: the Blue pulse restarts a stalled return pump", "[app]")
{
    Scenario s;
    s.Start();
    s.RunUntil(22, 0);
    s.tank.StallReturnPump(true);
    REQUIRE(s.RunUntilLevel(Level::Blue, 6 * 3600));
    s.RunS(60);
    REQUIRE_FALSE(s.tank.ReturnPumpStalled()); // the pulse power-cycled it
    REQUIRE(s.pulses >= 1);
    s.RunUntil(14, 0);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
}

TEST_CASE("probe_disconnect: FAULT, urgent notifications repeating, siren, ack, clear", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    s.probe.Model().dropout = true;
    s.RunS(60);
    REQUIRE(s.app.GetStatus().fault);
    REQUIRE(s.app.GetStatus().deviceOn[2]); // fault_level's (Red) devices run
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.app.GetStatus().deviceOn[0]);
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.app.GetStatus().deviceOn[3]); // siren
    REQUIRE(s.app.GetStatus().relayEnergised[3]);
    REQUIRE(s.app.GetStatus().sound);
    REQUIRE(s.app.GetStatus().consecutiveFailures >= 5);
    REQUIRE_FALSE(s.app.GetStatus().doMgl.has_value());
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 1);
    REQUIRE(s.LastNote(NotifyKind::Fault)->urgent);
    REQUIRE(s.RecordsOf(Type::Measurement) > 0);
    s.RunS(31 * 60); // repeats every 2 min while unacknowledged (FAULT acts as Red)
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 16);
    REQUIRE(s.LastNote(NotifyKind::Fault)->repeat);
    s.app.Ack();
    s.Tick();
    REQUIRE(s.app.GetStatus().silenced);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[3]); // siren silenced
    REQUIRE(s.app.GetStatus().deviceOn[2]);       // the pump is not
    REQUIRE_FALSE(s.app.GetStatus().sound);
    s.RunS(20 * 60);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 16); // no repeats while silenced
    s.probe.Model().dropout = false;
    s.RunS(30);
    REQUIRE_FALSE(s.app.GetStatus().fault);
    REQUIRE(s.NotesOf(NotifyKind::FaultCleared) == 1);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.LastNote(NotifyKind::Fault)->level == Level::Red); // the push says whose devices it ran
}

TEST_CASE("fast_crash_straight_to_red: Red is immediate, urgent, repeats every 2 min", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    s.tank.SetSat(55.0f);
    s.RunS(120); // probe lag + median
    REQUIRE(s.app.GetStatus().level == Level::Red);
    REQUIRE(s.app.GetStatus().sound);
    for(std::size_t i = 0; i < 5; ++i)
        REQUIRE(s.app.GetStatus().deviceOn[i]);
    REQUIRE(s.LastNote(NotifyKind::Level)->level == Level::Red);
    REQUIRE(s.LastNote(NotifyKind::Level)->urgent);
    const std::size_t before = s.NotesOf(NotifyKind::Level);
    s.tank.SetSat(55.0f);
    s.RunS(10 * 60 + 30);
    REQUIRE(s.NotesOf(NotifyKind::Level) == before + 5);
    REQUIRE(s.LastNote(NotifyKind::Level)->repeat);
}

TEST_CASE("sudden_drop: a step fall is suspect, logged with its slope and flagged; Red waits extend_s", "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.ladder.suddenSlopeMglPer10min = 3.0f; // 600 s by default
    Scenario s(c);
    const auto lastMeasurement = [&s]()
    {
        std::optional<reefdo::log::Record> m;
        for(std::size_t i = 0; i < s.app.LogA().Count(); ++i)
        {
            const std::optional<reefdo::log::Record> r = s.app.LogA().At(i);
            if(r.has_value() && r->type == Type::Measurement) m = r;
        }
        return *m;
    };
    s.Start();
    s.RunS(600);
    REQUIRE_FALSE(s.app.GetStatus().suspect);
    s.tank.SetSat(55.0f);
    s.RunS(120);
    REQUIRE(s.app.GetStatus().suspect);
    REQUIRE(s.app.GetStatus().level == Level::Normal); // Red already without the suspicion (fast_crash_straight_to_red)
    REQUIRE((lastMeasurement().flags & reefdo::app::FLAG_SUSPECT) != 0);
    std::optional<reefdo::log::Record> drop;
    for(std::size_t i = 0; i < s.app.LogE().Count(); ++i)
    {
        const std::optional<reefdo::log::Record> r = s.app.LogE().At(i);
        if(r->type == Type::Event && r->aux == static_cast<uint32_t>(reefdo::ladder::EventType::SuddenDrop)) drop = r;
    }
    REQUIRE(drop.has_value());
    REQUIRE(drop->f1 <= -3.0f);
    s.RunS(360);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.RunUntilLevel(Level::Red, 300));

    // Back: the suspicion clears, and recovery is the usual one.
    s.tank.SetSat(104.0f);
    s.RunS(300);
    REQUIRE_FALSE(s.app.GetStatus().suspect);
    REQUIRE((lastMeasurement().flags & reefdo::app::FLAG_SUSPECT) == 0);
    REQUIRE(s.app.GetStatus().level == Level::Red);
}

TEST_CASE(
    "test_run_daily and test_no_response: the check passes, then a dead bubbler fails, shown but escalating nothing",
    "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.test.noFailAboveMgl = 0.0f; // this tank sits above saturation: judge every miss
    Scenario s(c);
    s.Start();
    s.RunUntil(21, 5);
    REQUIRE(s.app.TestState().p.lastOutcome[0] == reefdo::selftest::Outcome::Pass);
    REQUIRE(s.app.TestState().p.lastOutcome[1] == reefdo::selftest::Outcome::Unchecked);
    REQUIRE(s.app.TestState().p.lastOutcome[2] == reefdo::selftest::Outcome::Pass);
    REQUIRE(s.app.TestPersistentChanged());
    REQUIRE_FALSE(s.app.TestPersistentChanged());
    REQUIRE(s.EventsOf(Type::Test) >= 8);
    REQUIRE(s.NotesOf(NotifyKind::TestFail) == 0);
    REQUIRE_FALSE(s.app.GetStatus().testRunning);
    REQUIRE(s.app.GetStatus().testPhase == reefdo::selftest::Phase::Idle);

    // Next evening the airstone is clogged.
    s.tank.SetDeviceK(0, 0.0f);
    s.RunUntil(21, 5);
    REQUIRE(s.app.TestState().p.lastOutcome[0] == reefdo::selftest::Outcome::Fail);
    REQUIRE(s.app.GetStatus().deviceFailed[0]);
    REQUIRE(s.NotesOf(NotifyKind::TestFail) == 1);
    REQUIRE(s.LastNote(NotifyKind::TestFail)->device == 0);
    // That night a Blue stays Blue: the verdict is shown and pushed, but runs and sounds nothing extra.
    s.tank.SetSat(80.0f); // 5.36 mg/L: below Blue's 5.65, above Yellow's 5.05
    s.RunS(300);
    REQUIRE(s.app.GetStatus().level == Level::Blue);
    REQUIRE(s.app.GetStatus().deviceOn[0]);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]); // no Yellow device because of the failed bubbler
    REQUIRE(s.app.GetStatus().deviceFailed[0]);

    // Cleaned: run now, the bubbler works again, the flag clears.
    s.tank.SetSat(104.0f);
    s.RunUntil(13, 0);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    s.tank.SetSat(
        106.0f); // the check needs distance from 100 %: at 13:00 the tank has not yet climbed to its day equilibrium
    s.RunS(600);
    s.tank.SetDeviceK(0, 3.0f);
    s.app.RunTest();
    s.RunS(1500);
    REQUIRE_FALSE(s.app.GetStatus().deviceFailed[0]);
    REQUIRE(s.app.TestPersistentChanged());
}

TEST_CASE("test_inconclusive: a tank pinned at 100 % all evening raises the alert after five days", "[app]")
{
    reefdo::sim::TankConfig t = ExampleTank();
    t.photosynthesisPctPerH = 2.0f; // day equilibrium exactly 100 %
    t.kFlowPerH = 3.0f;             // and firmly held there
    Scenario s(ExampleConfig(), t);
    s.probe.Model().noisePct = 0.05f;
    s.Start();
    for(int d = 0; d < 5; ++d)
        s.RunUntil(21, 30);
    REQUIRE(s.app.GetStatus().inconclusiveAlert);
    REQUIRE(s.NotesOf(NotifyKind::TestInconclusive) == 1);
}

TEST_CASE("maintenance: calibration and the audit records", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    REQUIRE(s.app.AirCalibrate(s.clock) == reefdo::probe::CalResult::Refused);
    s.app.SetMaintenance(true, s.clock);
    s.app.SetMaintenance(true, s.clock); // idempotent: one record
    REQUIRE(s.EventsOf(Type::Command) == 1);
    s.Tick();
    REQUIRE(s.app.GetStatus().maintenance);
    REQUIRE(s.app.AirCalibrate(s.clock) == reefdo::probe::CalResult::Ok);
    REQUIRE(s.probe.Calibrations() == 1);
    REQUIRE(s.EventsOf(Type::Command) == 2);
    s.app.SetMaintenance(false, s.clock);
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().maintenance);
    REQUIRE(s.EventsOf(Type::Command) == 3);
    s.app.SetMaintenance(false, s.clock); // not in maintenance: nothing to end, nothing logged
    REQUIRE(s.EventsOf(Type::Command) == 3);
}

TEST_CASE("The page's device test is the manual switch: the coil follows at once, in either wiring", "[app][manual]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(s.app.SetManual(2, true, s.clock)); // the strong air pump, NC; no maintenance mode needed
    REQUIRE(st.deviceOn[2]);                    // immediately, not at the next sample
    REQUIRE_FALSE(st.relayEnergised[2]);        // NC: on = de-energised
    REQUIRE(s.app.SetManual(2, std::nullopt, s.clock));
    REQUIRE_FALSE(st.deviceOn[2]); // back to the automatic state, which is off
    REQUIRE(st.relayEnergised[2]);
    REQUIRE(s.app.SetManual(3, true, s.clock)); // the siren, NO: on = energised
    REQUIRE(st.relayEnergised[3]);
    REQUIRE(s.app.SetManual(3, std::nullopt, s.clock));
    REQUIRE_FALSE(st.relayEnergised[3]);
}

TEST_CASE("set_config logs a Config record, plus Correction only when it changed", "[app]")
{
    Scenario s;
    s.Start();
    reefdo::config::Config c = s.cfg;
    c.ladder.blue.mgl = 6.0f;
    REQUIRE(s.app.SetConfig(c, s.clock));
    REQUIRE(s.EventsOf(Type::Config) == 2);
    REQUIRE(s.EventsOf(Type::Correction) == 1);
    REQUIRE_THAT(s.app.GetConfig().ladder.blue.mgl, WithinAbs(6.0, 1e-6));
    c.correction.scale = 0.82f;
    REQUIRE(s.app.SetConfig(c, s.clock));
    REQUIRE(s.EventsOf(Type::Correction) == 2);
    REQUIRE_THAT(s.app.LogA().last()->f0, WithinAbs(0.82, 1e-6));
    s.RunS(100);
    REQUIRE(*s.app.GetStatus().doMgl < 5.8f); // the corrected value now runs ~18 % low: the ladder sees it
}

TEST_CASE("clock: unknown at boot, then set; jumps are logged as TimeSync", "[app]")
{
    Scenario s;
    s.clockKnown = false;
    s.Start();
    REQUIRE(s.EventsOf(Type::TimeSync) == 0);
    s.RunS(120);
    REQUIRE_FALSE(s.app.GetStatus().clockKnown);
    REQUIRE(s.app.LogA().last()->ts == 0);
    REQUIRE((s.app.LogA().last()->flags & reefdo::app::FLAG_CLOCK_UNKNOWN) != 0);
    REQUIRE(s.app.LogB().Count() == 0); // no aggregates without a clock
    s.clockKnown = true;
    s.RunS(20);
    REQUIRE(s.app.GetStatus().clockKnown);
    REQUIRE(s.EventsOf(Type::TimeSync) == 1);
    REQUIRE(s.app.LogA().last()->ts != 0);
    *s.clock.unixS += 3600; // the clock jumps an hour
    s.RunS(20);
    REQUIRE(s.EventsOf(Type::TimeSync) == 2);
    s.RunS(600);
    REQUIRE(s.EventsOf(Type::TimeSync) == 2); // steady: no more
}

TEST_CASE("The correction parameters are logged once a day just before the test window", "[app]")
{
    Scenario s;
    s.Start();
    s.RunUntil(19, 40);
    REQUIRE(s.EventsOf(Type::Correction) == 2); // boot + 19:29
    s.RunUntil(19, 40);
    REQUIRE(s.EventsOf(Type::Correction) == 3);
}

TEST_CASE("Stuck probe readings are flagged in the log and count towards FAULT", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    s.probe.Model().stuck = true;
    s.RunS(30);
    REQUIRE(s.app.GetStatus().probe == Status::Stuck);
    REQUIRE(s.app.GetStatus().doMgl.has_value());
    REQUIRE((s.app.LogA().last()->flags & reefdo::app::FLAG_STUCK) != 0);
    REQUIRE_FALSE(s.app.GetStatus().fault); // not yet: five frozen polls
    s.RunS(30);
    REQUIRE(s.app.GetStatus().fault); // a frozen probe is a broken probe
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    s.probe.Model().stuck = false;
    s.RunS(30);
    REQUIRE_FALSE(s.app.GetStatus().fault);
}

TEST_CASE("Recovery from Red steps down through Yellow and Blue with informational notifications", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    s.tank.SetSat(55.0f);
    s.RunS(120);
    REQUIRE(s.app.GetStatus().level == Level::Red);
    s.tank.SetSat(104.0f);
    s.RunS(40 * 60);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    bool sawStepDown = false;
    for(const Notification& n : s.notes)
        if(n.kind == NotifyKind::Level && !n.urgent && !n.repeat && n.level == Level::Yellow) sawStepDown = true;
    REQUIRE(sawStepDown);
    REQUIRE(s.NotesOf(NotifyKind::Recovered) == 1);
}

TEST_CASE("A backward clock jump is a TimeSync too", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    *s.clock.unixS -= 600;
    s.RunS(20);
    REQUIRE(s.EventsOf(Type::TimeSync) == 2);
    REQUIRE(s.app.LogE().last()->f0 < -500.0f);
}

TEST_CASE("A test with an unknown clock and a run absorbed by an alarm are logged with their flags", "[app]")
{
    Scenario s;
    s.Start();
    s.RunUntil(21, 5); // one scheduled run with a known clock
    s.clockKnown = false;
    s.RunS(24 * 3600 + 60);
    bool clockUnknownRun = false;
    for(std::size_t i = 0; i < s.app.LogE().Count(); ++i)
    {
        const auto r = s.app.LogE().At(i);
        if(r->type == Type::Test && (r->aux & 0xFF) == 0 && (r->flags & 2)) clockUnknownRun = true;
    }
    REQUIRE(clockUnknownRun);

    Scenario a;
    a.Start();
    a.RunUntil(19, 31);
    REQUIRE(a.app.GetStatus().testRunning);
    a.tank.SetSat(80.0f); // a crash in the middle of the check
    a.RunS(400);
    REQUIRE_FALSE(a.app.GetStatus().testRunning);
    bool aborted = false;
    for(std::size_t i = 0; i < a.app.LogE().Count(); ++i)
    {
        const auto r = a.app.LogE().At(i);
        if(r->type == Type::Test && (r->aux & 0xFF) == 3 && (r->flags & 4)) aborted = true;
    }
    REQUIRE(aborted);
}

TEST_CASE("Maintenance during a FAULT: no repeated alarms, the demanded devices stay on", "[app]")
{
    Scenario s;
    s.Start();
    s.probe.Model().dropout = true;
    s.RunS(60);
    REQUIRE(s.app.GetStatus().fault);
    s.app.SetMaintenance(true, s.clock);
    s.Tick();
    const std::size_t before = s.NotesOf(NotifyKind::Fault);
    s.RunS(25 * 60);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == before);
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE_FALSE(s.app.GetStatus().sound);
}

TEST_CASE("A day without a single reading writes no daily record", "[app]")
{
    Scenario s;
    s.Start();
    s.RunUntil(23, 50);
    s.probe.Model().dropout = true;
    s.RunUntil(23, 50); // the whole next day blind
    s.RunUntil(0, 10);
    REQUIRE(s.app.LogD().Count() == 1); // only the first (partial) day
}

TEST_CASE("Heat: a warm day sets the heat flag on the samples", "[app]")
{
    reefdo::sim::TankConfig t = ExampleTank();
    t.tempDayC = 28.6f;
    Scenario s(ExampleConfig(), t);
    s.Start();
    s.RunUntil(12, 0);
    REQUIRE(s.app.GetStatus().heat);
    REQUIRE((s.app.LogA().last()->flags & reefdo::app::FLAG_HEAT) != 0);
}

TEST_CASE("Induced deficit cuts the return pump before the check; persistent test state can be restored", "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.test.induceDeficitS = 120;
    c.test.induceDeficitDevice = 5;
    Scenario s(c);
    s.Start();
    s.RunUntil(19, 30);
    s.RunS(30);
    REQUIRE(s.app.GetStatus().testRunning);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[4]); // the return pump is cut
    REQUIRE(s.app.GetStatus().relayEnergised[4]); // NC: cut = energised
    s.RunS(120);
    REQUIRE(s.app.GetStatus().deviceOn[4]);
    REQUIRE(s.app.GetStatus().deviceOn[0]);

    const reefdo::selftest::Persistent p = s.app.TestPersistent();
    Scenario r;
    r.app.RestoreTest(p);
    REQUIRE(r.app.TestPersistent() == p);
}

TEST_CASE("The boost runs its device inside its window, yields to maintenance, and stops at the target", "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.boost.enabled = true;
    c.boost.windowStartMin = 18 * 60 + 10;
    c.boost.windowEndMin = 18 * 60 + 40;
    c.boost.targetMgl = 6.4f;  // ≈ 96 % in the simulated tank
    c.boost.devices[2] = true; // the strong air pump
    Scenario s(c);
    s.Start();
    s.tank.SetSat(90.0f);
    s.RunUntil(18, 10);
    s.RunS(30);
    REQUIRE(s.app.GetStatus().boostRunning);
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.app.GetStatus().level == Level::Normal);

    s.app.SetMaintenance(true, s.clock);
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().boostRunning);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    s.app.SetMaintenance(false, s.clock);
    s.Tick();
    REQUIRE(s.app.GetStatus().boostRunning); // resumed: the window is still open

    s.RunUntil(18, 40);
    REQUIRE_FALSE(s.app.GetStatus().boostRunning);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    std::size_t reached = 0, paused = 0, starts = 0;
    for(std::size_t i = 0; i < s.app.LogE().Count(); ++i)
    {
        const auto r = s.app.LogE().At(i);
        if(r->type != Type::Boost) continue;
        REQUIRE_THAT(r->f1, WithinAbs(6.4, 1e-6));
        starts += r->aux == static_cast<uint32_t>(reefdo::boost::EventType::Start);
        paused += r->aux == static_cast<uint32_t>(reefdo::boost::EventType::Paused);
        reached += r->aux == static_cast<uint32_t>(reefdo::boost::EventType::Reached);
    }
    REQUIRE(starts == 2);
    REQUIRE(paused == 1);
    REQUIRE(reached == 1);
    REQUIRE(s.EventsOf(Type::Boost) == 4);
}

namespace
{

struct CommandRec
{
    uint32_t aux;
    float f0;
    float f1;
};

std::vector<CommandRec> Commands(const Scenario& pS)
{
    std::vector<CommandRec> out;
    for(std::size_t i = 0; i < pS.app.LogE().Count(); ++i)
    {
        const auto r = pS.app.LogE().At(i);
        if(r->type == Type::Command) out.push_back({r->aux, r->f0, r->f1});
    }
    return out;
}

} // namespace

TEST_CASE("A device window keeps it on inside the window only, and never without a clock", "[app][windows]")
{
    reefdo::config::Config c = ExampleConfig();
    c.devices[5].windows.push_back({18 * 60 + 10, 18 * 60 + 20}); // "unused", NO
    c.devices[5].windows.push_back({3 * 60, 4 * 60});
    Scenario s(c);
    s.Start();
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[5]);
    REQUIRE(s.app.GetStatus().deviceWhy[5] == 0);
    s.RunUntil(18, 10);
    s.Tick();
    REQUIRE(s.app.GetStatus().deviceOn[5]);
    REQUIRE(s.app.GetStatus().relayEnergised[5]); // NO: on = energised
    REQUIRE(s.app.GetStatus().deviceWhy[5] == reefdo::app::DEMAND_WINDOW);
    s.clockKnown = false;
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[5]); // no clock, no window
    s.clockKnown = true;
    s.Tick();
    REQUIRE(s.app.GetStatus().deviceOn[5]);
    s.RunUntil(18, 20);
    s.Tick(); // the tick at 18:20 itself
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[5]);
}

TEST_CASE("An exclusive test holds the windows off and ends manual switches; without exclusive windows keep running",
          "[app][windows][exclusive]")
{
    reefdo::config::Config c = ExampleConfig();
    c.devices[5].windows.push_back({19 * 60, 22 * 60});
    Scenario s(c);
    s.Start();
    s.RunUntil(19, 20);
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.deviceOn[5]);
    REQUIRE(s.app.SetManual(1, true, s.clock)); // the extra powerhead, tested second
    REQUIRE(st.deviceOn[1]);
    s.RunUntil(19, 30);
    s.RunS(30);
    REQUIRE(st.testRunning);
    REQUIRE(st.deviceOn[0]); // under test
    REQUIRE(st.deviceWhy[0] == reefdo::app::DEMAND_TEST);
    REQUIRE_FALSE(st.deviceOn[5]);
    REQUIRE_FALSE(st.deviceOn[1]);
    REQUIRE_FALSE(st.manual[1]);                      // the test took over
    REQUIRE_FALSE(s.app.SetManual(1, true, s.clock)); // and has the tank to itself
    REQUIRE_FALSE(s.app.SetManual(1, false, s.clock));
    REQUIRE(s.app.SetManual(1, std::nullopt, s.clock)); // automatic is always accepted
    REQUIRE_FALSE(st.deviceOn[1]);
    s.RunS(1500);
    REQUIRE_FALSE(st.testRunning);
    REQUIRE(st.deviceOn[5]);
    REQUIRE_FALSE(st.deviceOn[1]);
    const std::vector<CommandRec> cmds = Commands(s);
    REQUIRE(cmds.back().aux == (8u | 1u << 8));
    REQUIRE_THAT(cmds.back().f1, WithinAbs(2.0, 1e-6)); // ended by the test

    reefdo::config::Config open = c;
    open.test.exclusive = false;
    REQUIRE(s.app.SetConfig(open, s.clock));
    s.app.RunTest();
    s.RunS(30);
    REQUIRE(st.testRunning);
    REQUIRE(st.deviceOn[5]); // the window keeps running through the run
    REQUIRE(st.deviceWhy[5] == reefdo::app::DEMAND_WINDOW);
    REQUIRE(s.app.SetManual(2, true, s.clock)); // nothing is measured: manual is fine
    REQUIRE(st.deviceOn[2]);
}

TEST_CASE("Manual switch: on or off by hand until the next scheduled or triggered change; auto hands back",
          "[app][manual]")
{
    reefdo::config::Config c = ExampleConfig();
    c.devices[5].windows.push_back({18 * 60 + 10, 18 * 60 + 20});
    Scenario s(c);
    s.Start();
    s.Tick();
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE_FALSE(s.app.SetManual(reefdo::DEVICES, true, s.clock));
    REQUIRE(s.app.SetManual(5, std::nullopt, s.clock)); // already automatic: accepted, not logged
    REQUIRE(Commands(s).empty());
    REQUIRE(s.app.SetManual(5, true, s.clock));
    REQUIRE(s.app.SetManual(5, true, s.clock)); // already on: one record
    REQUIRE(st.deviceOn[5]);
    REQUIRE(st.manual[5] == true);
    REQUIRE(st.deviceWhy[5] == reefdo::app::DEMAND_MANUAL);
    s.RunS(60);
    REQUIRE(st.deviceOn[5]); // no timer
    s.RunUntil(18, 10);
    s.Tick(); // the window opens: the schedule takes over
    REQUIRE(st.deviceOn[5]);
    REQUIRE_FALSE(st.manual[5].has_value());
    REQUIRE(st.deviceWhy[5] == reefdo::app::DEMAND_WINDOW);
    REQUIRE(s.app.SetManual(5, false, s.clock)); // off by hand, although the window wants it
    REQUIRE_FALSE(st.deviceOn[5]);
    REQUIRE(st.manual[5] == false);
    REQUIRE(st.deviceWhy[5] == reefdo::app::DEMAND_WINDOW);
    s.RunS(60);
    REQUIRE_FALSE(st.deviceOn[5]);
    REQUIRE(s.app.SetManual(5, true, s.clock)); // and on again
    REQUIRE(st.deviceOn[5]);
    s.RunUntil(18, 20);
    s.Tick(); // the window closes: automatic again, off
    REQUIRE_FALSE(st.manual[5].has_value());
    REQUIRE_FALSE(st.deviceOn[5]);

    const std::vector<CommandRec> cmds = Commands(s);
    REQUIRE(cmds.size() == 5);
    REQUIRE(cmds[0].aux == (6u | 5u << 8));
    REQUIRE(cmds[1].aux == (8u | 5u << 8));
    REQUIRE_THAT(cmds[1].f1, WithinAbs(1.0, 1e-6)); // a schedule took over
    REQUIRE(cmds[2].aux == (7u | 5u << 8));
    REQUIRE(cmds[3].aux == (6u | 5u << 8));
    REQUIRE(cmds[4].aux == (8u | 5u << 8));
    REQUIRE(s.app.SetManual(5, false, s.clock));
    REQUIRE(s.app.SetManual(5, std::nullopt, s.clock)); // back to automatic by hand
    REQUIRE_THAT(Commands(s).back().f1, WithinAbs(0.0, 1e-6));
}

TEST_CASE("A device switched off by hand during an alert comes back when the level changes", "[app][manual]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    s.tank.SetSat(80.0f);
    REQUIRE(s.RunUntilLevel(Level::Blue, 900));
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.deviceOn[0]);
    REQUIRE(s.app.SetManual(0, false, s.clock)); // the bubbler, a Blue device
    REQUIRE_FALSE(st.deviceOn[0]);
    REQUIRE(st.deviceWhy[0] == reefdo::app::DEMAND_LADDER); // still wanted, shown as such
    s.app.SetMaintenance(true, s.clock);
    s.app.SetMaintenance(false, s.clock);
    REQUIRE_FALSE(st.deviceOn[0]); // ReefDO maintenance does not end it
    s.tank.SetSat(60.0f);
    for(uint32_t t = 0; t < 900 && st.level == Level::Blue; t += TICK_S)
        s.Tick();
    REQUIRE(st.level > Level::Blue); // deeper: the ladder's devices are its own again
    REQUIRE_FALSE(st.manual[0].has_value());
    REQUIRE(st.deviceOn[0]);
}

TEST_CASE("Device maintenance is independent of ReefDO maintenance; out of order means off even at Red",
          "[app][device_maintenance]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    REQUIRE_FALSE(s.app.SuspendDevice(reefdo::DEVICES, 600, s.clock));
    REQUIRE_FALSE(s.app.SuspendDevice(2, reefdo::app::SUSPEND_MAX_S + 1, s.clock));
    REQUIRE(s.app.SuspendDevice(2, 1800, s.clock)); // the strong air pump, NC; no maintenance mode needed
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.suspendLeftS[2] == 1800);
    REQUIRE_FALSE(st.deviceOn[2]);
    REQUIRE(st.relayEnergised[2]); // NC: off = energised

    s.tank.SetSat(55.0f);
    s.RunS(120);
    REQUIRE(st.level == Level::Red);
    REQUIRE(st.deviceWhy[2] & reefdo::app::DEMAND_LADDER); // wanted...
    REQUIRE_FALSE(st.deviceOn[2]);                         // ...but out of order
    REQUIRE(st.deviceOn[0]);
    REQUIRE(st.sound); // the alarm is not touched
    const auto last = s.app.LogA().last();
    REQUIRE(last->type == Type::Measurement);
    REQUIRE(last->flags & reefdo::app::FLAG_SUSPENDED);

    s.app.SetMaintenance(true, s.clock);
    REQUIRE(s.app.SetManual(2, true, s.clock)); // not even by hand (the page's device test)
    REQUIRE_FALSE(st.deviceOn[2]);
    REQUIRE(s.app.SetManual(2, std::nullopt, s.clock));
    s.app.SetMaintenance(false, s.clock);
    REQUIRE(st.suspendLeftS[2] > 0); // leaving ReefDO maintenance does not touch device maintenance
    REQUIRE_FALSE(st.deviceOn[2]);

    REQUIRE(s.app.SuspendDevice(2, 0, s.clock)); // back in order: on at once, the ladder wants it
    REQUIRE(st.deviceOn[2]);
    REQUIRE(st.suspendLeftS[2] == 0);
    REQUIRE(s.app.SuspendDevice(0, 30, s.clock));
    s.RunS(40);
    REQUIRE(st.suspendLeftS[0] == 0);
    REQUIRE(st.deviceOn[0]);
    REQUIRE(s.app.SuspendDevice(3, 0, s.clock)); // not out: accepted, not logged

    std::vector<CommandRec> ends;
    for(const CommandRec& r : Commands(s))
        if((r.aux & 0xff) == 5) ends.push_back(r);
    REQUIRE(ends.size() == 2);
    REQUIRE(ends[0].aux == (5u | 2u << 8));
    REQUIRE_THAT(ends[0].f1, WithinAbs(0.0, 1e-6)); // by command
    REQUIRE(ends[1].aux == (5u | 0u << 8));
    REQUIRE_THAT(ends[1].f1, WithinAbs(1.0, 1e-6)); // time ran out
}

TEST_CASE("A plug device drives no relay; three failures in a row mark the plug lost, one success brings it back",
          "[app][tuya]")
{
    reefdo::config::Config c = ExampleConfig();
    c.devices[5].slot = Plug("bf0123456789abcdefgh"); // relay 6 is free now
    Scenario s(c);
    s.Start();
    s.Tick();
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Pending);
    REQUIRE(st.tuyaLink[0] == reefdo::app::Link::None);
    REQUIRE(s.app.SetManual(5, true, s.clock));
    REQUIRE(st.deviceOn[5]);
    REQUIRE_FALSE(st.relayEnergised[5]); // the plug is on; the free channel rests

    REQUIRE_FALSE(s.app.ReportTuya(0, reefdo::tuya::Error::None, s.clock)); // a relay
    REQUIRE_FALSE(s.app.ReportTuya(reefdo::DEVICES, reefdo::tuya::Error::None, s.clock));
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::None, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Ok);
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::Timeout, s.clock));
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::Timeout, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Ok); // two misses are Wi-Fi weather
    REQUIRE(st.tuyaError[5] == reefdo::tuya::Error::Timeout);
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::Connect, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Lost);
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::Connect, s.clock)); // no second alarm
    for(const Notification& n : s.app.TakeNotifications())
        s.notes.push_back(n);
    REQUIRE(s.NotesOf(NotifyKind::DeviceLost) == 1);
    REQUIRE(s.LastNote(NotifyKind::DeviceLost)->device == 5);
    REQUIRE(s.LastNote(NotifyKind::DeviceLost)->urgent);
    REQUIRE(s.app.ReportTuya(5, reefdo::tuya::Error::None, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Ok);
    for(const Notification& n : s.app.TakeNotifications())
        s.notes.push_back(n);
    REQUIRE(s.NotesOf(NotifyKind::DeviceBack) == 1);
    std::size_t lost = 0, back = 0;
    for(const CommandRec& r : Commands(s))
    {
        lost += r.aux == (9u | 5u << 8);
        back += r.aux == (10u | 5u << 8);
    }
    REQUIRE(lost == 1);
    REQUIRE(back == 1);

    // A new config: an unchanged plug keeps its link, a changed one starts over
    reefdo::config::Config same = c;
    same.devices[0].name.assign("renamed");
    REQUIRE(s.app.SetConfig(same, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Ok);
    same.devices[5].slot.AsTuya()->ip.assign("192.168.1.51");
    REQUIRE(s.app.SetConfig(same, s.clock));
    REQUIRE(st.tuyaLink[5] == reefdo::app::Link::Pending);
}

TEST_CASE("A clogged airstone in supersaturated water: unchecked, logged with its DO and the limit, no alert",
          "[app][selftest]")
{
    Scenario s; // the default limit; the example tank sits around 104 % (~6.9 mg/L)
    s.Start();
    s.RunUntil(21, 5);
    REQUIRE(s.app.TestState().p.lastOutcome[0] == reefdo::selftest::Outcome::Pass); // a pass is still a pass
    s.tank.SetDeviceK(0, 0.0f);
    s.RunUntil(21, 5);
    REQUIRE(s.app.TestState().p.lastOutcome[0] == reefdo::selftest::Outcome::Unchecked);
    REQUIRE_FALSE(s.app.GetStatus().deviceFailed[0]);
    REQUIRE(s.NotesOf(NotifyKind::TestFail) == 0);

    std::optional<reefdo::log::Record> end;
    for(std::size_t i = 0; i < s.app.LogE().Count(); ++i)
    {
        const std::optional<reefdo::log::Record> r = s.app.LogE().At(i);
        const bool deviceEnd = r->type == Type::Test && (r->aux & 0xff) == 2 && ((r->aux >> 8) & 0xff) == 0;
        if(deviceEnd) end = r;
    }
    REQUIRE(end.has_value());
    REQUIRE((end->flags & 8) != 0);
    REQUIRE(((end->aux >> 16) & 0xff) == static_cast<uint32_t>(reefdo::selftest::Outcome::Unchecked));
    REQUIRE(end->f1 > 6.1f);
    REQUIRE_THAT(end->f2, WithinAbs(6.1, 1e-6));
}

TEST_CASE("Relays follow their devices: more devices than relays, a freed channel rests at once, a bad number is "
          "ignored",
          "[app][relays]")
{
    reefdo::config::Config c = ExampleConfig();
    c.devices[5].slot = reefdo::slot::AnySlot(); // device 6 gives relay 6 to device 8, an NC bubbler
    c.devices[7].slot = RelayOn(6, true);
    Scenario s(c);
    s.Start();
    s.Tick();
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE_FALSE(st.deviceOn[7]);
    REQUIRE(st.relayEnergised[5]); // NC and off: the coil holds it off
    REQUIRE(s.app.SetManual(7, true, s.clock));
    REQUIRE_FALSE(st.relayEnergised[5]);
    REQUIRE(s.app.SetManual(7, false, s.clock));
    REQUIRE(st.relayEnergised[5]);

    reefdo::config::Config moved = c;
    moved.devices[7].slot = reefdo::slot::AnySlot();
    REQUIRE(s.app.SetConfig(moved, s.clock));
    REQUIRE_FALSE(st.relayEnergised[5]); // nobody's channel: de-energised before the next sample

    reefdo::config::Config bad = c; // Validate() never lets this through; the App still stays inside its channels
    bad.devices[7].slot = RelayOn(static_cast<uint32_t>(reefdo::RELAYS + 1), true);
    REQUIRE(s.app.SetConfig(bad, s.clock));
    s.Tick();
    REQUIRE_FALSE(st.relayEnergised[5]);
}

TEST_CASE("Every plug going lost at once is pushed, and one coming back too: none dropped", "[app][tuya]")
{
    reefdo::config::Config c = ExampleConfig();
    for(reefdo::config::DeviceSettings& d : c.devices)
        d.slot = Plug("bf0123456789abcdefgh");
    Scenario s(c);
    s.Start();
    s.Tick();
    (void)s.app.TakeNotifications();
    for(uint32_t k = 0; k < reefdo::app::TUYA_LOST_AFTER; ++k)
        for(std::size_t i = 0; i < reefdo::DEVICES; ++i)
            REQUIRE(s.app.ReportTuya(i, reefdo::tuya::Error::Connect, s.clock));
    REQUIRE(s.app.ReportTuya(0, reefdo::tuya::Error::None, s.clock)); // and one comes straight back
    std::size_t lost = 0;
    std::size_t back = 0;
    for(const Notification& n : s.app.TakeNotifications())
    {
        lost += n.kind == NotifyKind::DeviceLost;
        back += n.kind == NotifyKind::DeviceBack;
    }
    REQUIRE(lost == reefdo::DEVICES);
    REQUIRE(back == 1);
}

TEST_CASE("Maintenance mode ends by itself after 30 min, logged as such; entering it again restarts the countdown",
          "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.maintenanceLeftS == 0);
    s.app.SetMaintenance(true, s.clock);
    REQUIRE(st.maintenanceLeftS == reefdo::app::MAINTENANCE_S);
    s.RunS(600);
    REQUIRE(st.maintenanceLeftS == reefdo::app::MAINTENANCE_S - 590);
    s.app.SetMaintenance(true, s.clock); // again: a fresh 30 min, and no second record
    REQUIRE(st.maintenanceLeftS == reefdo::app::MAINTENANCE_S);
    s.RunS(reefdo::app::MAINTENANCE_S - 10);
    REQUIRE(st.maintenance);
    REQUIRE_FALSE(st.sound); // still hushed
    s.RunS(20);
    REQUIRE_FALSE(st.maintenance);
    REQUIRE(st.maintenanceLeftS == 0);

    const std::vector<CommandRec> cmds = Commands(s);
    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0].aux == 1u);                     // on
    REQUIRE(cmds[1].aux == 2u);                     // off...
    REQUIRE_THAT(cmds[1].f1, WithinAbs(1.0, 1e-6)); // ...because the time ran out
    s.app.SetMaintenance(true, s.clock);
    s.app.SetMaintenance(false, s.clock);
    REQUIRE_THAT(Commands(s).back().f1, WithinAbs(0.0, 1e-6)); // by command
}

TEST_CASE("A failed test verdict can be cleared by hand, logged; it never changed what runs", "[app][selftest]")
{
    Scenario s;
    s.Start();
    reefdo::selftest::Persistent p;
    p.failActive[2] = true; // the strong air pump failed some evening
    s.app.RestoreTest(p);
    s.Tick();
    REQUIRE(s.app.TestPersistentChanged()); // the restored history, saved once (as the firmware does)
    const reefdo::app::Status& st = s.app.GetStatus();
    REQUIRE(st.deviceFailed[2]);
    REQUIRE_FALSE(st.deviceOn[2]); // Normal: a failed Yellow device stays off
    REQUIRE_FALSE(s.app.ClearFailure(reefdo::DEVICES, s.clock));
    REQUIRE(s.app.ClearFailure(0, s.clock)); // nothing to clear: accepted, not logged
    REQUIRE(Commands(s).empty());
    REQUIRE(s.app.ClearFailure(2, s.clock));
    REQUIRE_FALSE(st.deviceFailed[2]);
    REQUIRE_FALSE(s.app.TestPersistent().failActive[2]);
    REQUIRE(s.app.TestPersistentChanged()); // the firmware saves it
    const std::vector<CommandRec> cmds = Commands(s);
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].aux == (11u | 2u << 8));
    s.Tick();
    REQUIRE_FALSE(st.deviceFailed[2]); // and it stays cleared
}
