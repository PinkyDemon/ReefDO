#pragma once
// Scenario runner: the example configuration, a virtual tank wired to the app's device outputs,
// fake flash, and a clock that starts on an evening in September 2026 (local = UTC+2). Ten-second ticks.
#include <vector>

#include "reefdo/app.hpp"
#include "reefdo/config.hpp"
#include "reefdo/sim.hpp"

#include "fake_store.hpp"

namespace scenario
{

using reefdo::app::App;
using reefdo::app::Clock;
using reefdo::app::Notification;
using reefdo::app::NotifyKind;
using reefdo::ladder::Level;
using reefdo::ladder::Mode;
using reefdo::ladder::Trigger;

constexpr uint32_t TICK_S = 10;
constexpr int32_t TZ = 2 * 3600;
// 2026-09-14 18:00:00 local (UTC+2) = 16:00 UTC
constexpr uint32_t START_UNIX = 1789401600u;

inline reefdo::config::Config ExampleConfig()
{
    reefdo::config::Config c = reefdo::config::Defaults();
    auto dev =
        [&c](std::size_t pI, const char* pName, bool pNc, Trigger pT, Mode pM, bool pAck, uint32_t pSvc, float pResp)
    {
        c.devices[pI].name.assign(pName);
        c.devices[pI].wiredNc = pNc;
        c.devices[pI].serviceS = pSvc;
        c.devices[pI].minResponsePct = pResp;
        c.ladder.devices[pI] = {pT, pM, pAck};
    };
    dev(0, "small bubbler", true, Trigger::Blue, Mode::On, false, 300, 1.0f);
    dev(1, "extra powerhead", true, Trigger::Blue, Mode::On, false, 300, 0.0f);
    dev(2, "strong air pump", true, Trigger::Yellow, Mode::On, false, 300, 1.0f);
    dev(3, "siren", false, Trigger::Yellow, Mode::On, true, 0, 0.0f);
    dev(4, "return pump", true, Trigger::Blue, Mode::PulseOff, false, 0, 0.0f);
    dev(5, "unused", false, Trigger::None, Mode::On, false, 0, 0.0f);
    return c;
}

inline reefdo::sim::TankConfig ExampleTank()
{
    reefdo::sim::TankConfig t;
    t.kDevicePerH = {3.0f, 0.3f, 6.0f, 0.0f, 0.0f, 0.0f};
    t.returnPumpDevice = 4;
    t.sat0Pct = 104.0f;
    return t;
}

struct Scenario
{
    FakeStore a{40, 64 * reefdo::log::RECORD_SIZE};
    FakeStore b{6, 100 * reefdo::log::AGGREGATE_SIZE};
    FakeStore e{6, 50 * reefdo::log::RECORD_SIZE};
    FakeStore d{2, 10 * reefdo::log::RECORD_SIZE};
    reefdo::config::Config cfg;
    reefdo::sim::Tank tank;
    reefdo::sim::Probe probe;
    App app;
    Clock clock;
    bool clockKnown = true;
    std::vector<Notification> notes;
    std::vector<std::array<bool, 6>> relayHistory; // energised per tick (kept short by callers who care)
    uint32_t pulses = 0;                           // ticks device 4 was unpowered

    explicit Scenario(reefdo::config::Config pC = ExampleConfig(), reefdo::sim::TankConfig pT = ExampleTank(),
                      reefdo::sim::ProbeModel pM = {})
        : cfg(pC)
        , tank(pT)
        , probe(tank, pM, 7)
        , app(cfg, probe, {a, b, e, d})
    {
        clock.nowMs = 5'000;
        clock.unixS = START_UNIX;
        clock.tzOffsetS = TZ;
    }

    void Start(uint32_t pReason = 1)
    {
        Clock c = clock;
        if(!clockKnown) c.unixS.reset();
        app.Start(c, pReason);
        for(const Notification& n : app.TakeNotifications())
            notes.push_back(n);
    }

    uint16_t MinuteOfDay() const
    {
        const uint32_t l = static_cast<uint32_t>(static_cast<int64_t>(clock.unixS.value_or(START_UNIX)) + TZ);
        return static_cast<uint16_t>((l % 86400u) / 60u);
    }

    void Tick()
    {
        Clock c = clock;
        if(!clockKnown) c.unixS.reset();
        app.Tick(c);
        for(const Notification& n : app.TakeNotifications())
            notes.push_back(n);
        const reefdo::app::Status& s = app.GetStatus();
        pulses += static_cast<uint32_t>(!s.deviceOn[4]);
        tank.Step(static_cast<float>(TICK_S), MinuteOfDay(), s.deviceOn);
        clock.nowMs += TICK_S * 1000;
        if(clock.unixS.has_value()) *clock.unixS += TICK_S;
    }
    void RunS(uint32_t pSeconds)
    {
        for(uint32_t t = 0; t < pSeconds; t += TICK_S)
            Tick();
    }
    // Ticks until the local clock next reads hh:mm (always at least one tick, so calling it twice spans a day).
    void RunUntil(uint32_t pHh, uint32_t pMm)
    {
        const uint16_t target = static_cast<uint16_t>(pHh * 60 + pMm);
        while(MinuteOfDay() == target)
            Tick(); // leave the target minute if we are already in it
        while(MinuteOfDay() != target)
            Tick();
    }
    // Ticks until the ladder reaches `level` or `max_s` elapse; true if reached.
    bool RunUntilLevel(Level pLevel, uint32_t pMaxS)
    {
        for(uint32_t t = 0; t < pMaxS; t += TICK_S)
        {
            Tick();
            if(app.GetStatus().level == pLevel) return true;
        }
        return false;
    }
    std::size_t EventsOf(reefdo::log::Type pT) const
    {
        std::size_t n = 0;
        for(std::size_t i = 0; i < app.LogE().Count(); ++i)
        {
            const std::optional<reefdo::log::Record> r = app.LogE().At(i);
            n += (r.has_value() && r->type == pT);
        }
        return n;
    }

    std::size_t NotesOf(NotifyKind pK) const
    {
        std::size_t n = 0;
        for(const Notification& x : notes)
            n += (x.kind == pK);
        return n;
    }
    const Notification* LastNote(NotifyKind pK) const
    {
        for(auto it = notes.rbegin(); it != notes.rend(); ++it)
            if(it->kind == pK) return &*it;
        return nullptr;
    }
    std::size_t RecordsOf(reefdo::log::Type pT) const
    {
        std::size_t n = 0;
        for(std::size_t i = 0; i < app.LogA().Count(); ++i)
        {
            const std::optional<reefdo::log::Record> r = app.LogA().At(i);
            n += (r.has_value() && r->type == pT);
        }
        return n;
    }
};

} // namespace scenario
