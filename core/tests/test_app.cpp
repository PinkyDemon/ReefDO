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
    REQUIRE(s.app.GetStatus().effective == Level::Blue);
    REQUIRE(s.app.GetStatus().deviceOn[0]);
    REQUIRE(s.app.GetStatus().deviceOn[1]);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[3]);
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::Beep);
    REQUIRE(s.app.GetStatus().led == reefdo::ladder::Led::Blue);
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
    REQUIRE(s.app.GetStatus().effective == Level::Red);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    REQUIRE(s.app.GetStatus().deviceOn[0]);
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.app.GetStatus().deviceOn[3]); // siren
    REQUIRE(s.app.GetStatus().relayEnergised[3]);
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::FaultTriple);
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
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::Off);
    s.RunS(20 * 60);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == 16); // no repeats while silenced
    s.probe.Model().dropout = false;
    s.RunS(30);
    REQUIRE_FALSE(s.app.GetStatus().fault);
    REQUIRE(s.NotesOf(NotifyKind::FaultCleared) == 1);
    REQUIRE(s.app.GetStatus().effective == Level::Normal);
}

TEST_CASE("fast_crash_straight_to_red: Red is immediate, urgent, repeats every 2 min", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(600);
    s.tank.SetSat(55.0f);
    s.RunS(120); // probe lag + median
    REQUIRE(s.app.GetStatus().level == Level::Red);
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::Continuous);
    REQUIRE(s.app.GetStatus().led == reefdo::ladder::Led::Red);
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

TEST_CASE("service_run_daily and service_no_response: the check passes, then a dead bubbler fails and escalates",
          "[app]")
{
    Scenario s;
    s.Start();
    s.RunUntil(21, 5);
    REQUIRE(s.app.ServiceState().p.lastOutcome[0] == reefdo::service::Outcome::Pass);
    REQUIRE(s.app.ServiceState().p.lastOutcome[1] == reefdo::service::Outcome::Unchecked);
    REQUIRE(s.app.ServiceState().p.lastOutcome[2] == reefdo::service::Outcome::Pass);
    REQUIRE(s.app.ServicePersistentChanged());
    REQUIRE_FALSE(s.app.ServicePersistentChanged());
    REQUIRE(s.EventsOf(Type::Service) >= 8);
    REQUIRE(s.NotesOf(NotifyKind::ServiceFail) == 0);
    REQUIRE_FALSE(s.app.GetStatus().serviceRunning);
    REQUIRE(s.app.GetStatus().servicePhase == reefdo::service::Phase::Idle);

    // Next evening the airstone is clogged.
    s.tank.SetDeviceK(0, 0.0f);
    s.RunUntil(21, 5);
    REQUIRE(s.app.ServiceState().p.lastOutcome[0] == reefdo::service::Outcome::Fail);
    REQUIRE(s.app.GetStatus().deviceFailed[0]);
    REQUIRE(s.NotesOf(NotifyKind::ServiceFail) == 1);
    REQUIRE(s.LastNote(NotifyKind::ServiceFail)->device == 0);
    // That night a Blue acts as Yellow: audible.
    s.tank.SetSat(80.0f); // 5.36 mg/L: below Blue's 5.65, above Yellow's 5.05
    s.RunS(300);
    REQUIRE(s.app.GetStatus().level == Level::Blue);
    REQUIRE(s.app.GetStatus().effective == Level::Yellow);
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::Beep);

    // Cleaned: run now, the bubbler works again, the flag clears.
    s.tank.SetSat(104.0f);
    s.RunUntil(13, 0);
    REQUIRE(s.app.GetStatus().level == Level::Normal);
    s.tank.SetSat(
        106.0f); // the check needs distance from 100 %: at 13:00 the tank has not yet climbed to its day equilibrium
    s.RunS(600);
    s.tank.SetDeviceK(0, 3.0f);
    s.app.RunService();
    s.RunS(1500);
    REQUIRE_FALSE(s.app.GetStatus().deviceFailed[0]);
    REQUIRE(s.app.ServicePersistentChanged());
}

TEST_CASE("service_inconclusive: a tank pinned at 100 % all evening raises the alert after five days", "[app]")
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
    REQUIRE(s.NotesOf(NotifyKind::ServiceInconclusive) == 1);
}

TEST_CASE("maintenance: overrides, calibration, and their audit records", "[app]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    REQUIRE_FALSE(s.app.SetDeviceOverride(2, true)); // not in maintenance
    REQUIRE(s.app.AirCalibrate(s.clock) == reefdo::probe::CalResult::Refused);
    s.app.SetMaintenance(true, s.clock);
    s.app.SetMaintenance(true, s.clock); // idempotent: one record
    REQUIRE(s.EventsOf(Type::Command) == 1);
    s.Tick();
    REQUIRE(s.app.GetStatus().maintenance);
    REQUIRE(s.app.GetStatus().led == reefdo::ladder::Led::Cyan);
    REQUIRE(s.app.SetDeviceOverride(2, true));
    REQUIRE(s.app.GetStatus().deviceOn[2]); // immediately, not at the next sample
    REQUIRE_FALSE(s.app.GetStatus().relayEnergised[2]);
    REQUIRE_FALSE(s.app.SetDeviceOverride(9, true));
    s.Tick();
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE_FALSE(s.app.GetStatus().relayEnergised[2]); // NC: on = de-energised
    REQUIRE(s.app.SetDeviceOverride(2, false));
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.app.SetDeviceOverride(2, std::nullopt));
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]); // back to the automatic state, which is off
    REQUIRE(s.app.SetDeviceOverride(3, true));    // a NO device: on = energised
    REQUIRE(s.app.GetStatus().relayEnergised[3]);
    REQUIRE(s.app.SetDeviceOverride(3, std::nullopt));
    REQUIRE(s.app.AirCalibrate(s.clock) == reefdo::probe::CalResult::Ok);
    REQUIRE(s.probe.Calibrations() == 1);
    REQUIRE(s.EventsOf(Type::Command) == 2);
    REQUIRE(s.app.SetDeviceOverride(2, true));
    s.app.SetMaintenance(false, s.clock);
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[2]); // overrides cleared with maintenance
    REQUIRE(s.EventsOf(Type::Command) == 3);
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

TEST_CASE("The correction parameters are logged once a day just before the service window", "[app]")
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
    REQUIRE(s.app.GetStatus().effective == Level::Red);
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

TEST_CASE("Service with an unknown clock and a run absorbed by an alarm are logged with their flags", "[app]")
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
        if(r->type == Type::Service && (r->aux & 0xFF) == 0 && (r->flags & 2)) clockUnknownRun = true;
    }
    REQUIRE(clockUnknownRun);

    Scenario a;
    a.Start();
    a.RunUntil(19, 31);
    REQUIRE(a.app.GetStatus().serviceRunning);
    a.tank.SetSat(80.0f); // a crash in the middle of the check
    a.RunS(400);
    REQUIRE_FALSE(a.app.GetStatus().serviceRunning);
    bool aborted = false;
    for(std::size_t i = 0; i < a.app.LogE().Count(); ++i)
    {
        const auto r = a.app.LogE().At(i);
        if(r->type == Type::Service && (r->aux & 0xFF) == 3 && (r->flags & 4)) aborted = true;
    }
    REQUIRE(aborted);
}

TEST_CASE("Maintenance during a FAULT: no repeated alarms, an override cannot switch off a demanded device", "[app]")
{
    Scenario s;
    s.Start();
    s.probe.Model().dropout = true;
    s.RunS(60);
    REQUIRE(s.app.GetStatus().fault);
    s.app.SetMaintenance(true, s.clock);
    s.Tick();
    REQUIRE(s.app.SetDeviceOverride(2, false)); // the ladder wants the strong pump on
    const std::size_t before = s.NotesOf(NotifyKind::Fault);
    s.RunS(25 * 60);
    REQUIRE(s.NotesOf(NotifyKind::Fault) == before);
    REQUIRE(s.app.GetStatus().deviceOn[2]);
    REQUIRE(s.app.GetStatus().buzzer == reefdo::ladder::Buzzer::Off);
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

TEST_CASE("Induced deficit cuts the return pump before the check; persistent service state can be restored", "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.service.induceDeficitS = 120;
    c.service.induceDeficitDevice = 5;
    Scenario s(c);
    s.Start();
    s.RunUntil(19, 30);
    s.RunS(30);
    REQUIRE(s.app.GetStatus().serviceRunning);
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[4]); // the return pump is cut
    REQUIRE(s.app.GetStatus().relayEnergised[4]); // NC: cut = energised
    s.RunS(120);
    REQUIRE(s.app.GetStatus().deviceOn[4]);
    REQUIRE(s.app.GetStatus().deviceOn[0]);

    const reefdo::service::Persistent p = s.app.ServicePersistent();
    Scenario r;
    r.app.RestoreService(p);
    REQUIRE(r.app.ServicePersistent() == p);
}

TEST_CASE("The boost runs its device inside its window, yields to maintenance, and stops at the target", "[app]")
{
    reefdo::config::Config c = ExampleConfig();
    c.boost.enabled = true;
    c.boost.windowStartMin = 18 * 60 + 10;
    c.boost.windowEndMin = 18 * 60 + 40;
    c.boost.targetMgl = 6.4f; // ≈ 96 % in the simulated tank
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
