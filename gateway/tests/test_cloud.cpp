#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/gateway/cloud.hpp"

#include "scenario.hpp"
#include "test_platform.hpp"

using namespace scenario;
using namespace reefdo::gateway::cloud;
using reefdo::app::Notification;
using testing::Platform;

namespace
{

const Update* Find(const Updates& pU, Param pP)
{
    for(const Update& u : pU)
        if(u.param == pP) return &u;
    return nullptr;
}

// A scenario with an agent riding along: every sample is also a Step.
struct Bench
{
    Scenario s;
    Agent agent;
    Platform platform;
    std::vector<char> scratch = std::vector<char>(reefdo::gateway::JSON_MAX);
    bool online = true;
    std::vector<Updates> steps;

    explicit Bench(reefdo::config::Config pC = ExampleConfig())
        : s(pC)
    {
        platform.app = &s.app;
        platform.clock = &s.clock;
        s.Start();
    }
    Updates Tick()
    {
        s.app.Tick(s.clock);
        const reefdo::app::Notifications notes = s.app.TakeNotifications();
        const Updates u =
            agent.Step(s.app, s.clock, std::span<const Notification>(notes.begin(), notes.size()), online);
        s.tank.Step(static_cast<float>(TICK_S), s.MinuteOfDay(), s.app.GetStatus().deviceOn);
        s.clock.nowMs += TICK_S * 1000;
        *s.clock.unixS += TICK_S;
        steps.push_back(u);
        return u;
    }
    void RunS(uint32_t pSeconds)
    {
        for(uint32_t t = 0; t < pSeconds; t += TICK_S)
            Tick();
    }
    std::size_t Count(Param pP) const
    {
        std::size_t n = 0;
        for(const Updates& u : steps)
            n += Find(u, pP) != nullptr;
        return n;
    }
    Updates Write(Param pP, int32_t pV) { return agent.Write(s.app, s.clock, platform, pP, pV, scratch); }
};

float Rounded(float pV, float pScale)
{
    return std::round(pV * pScale) / pScale;
}

const TrendUpdate* FindRow(const TrendUpdates& pU, Param pP)
{
    for(const TrendUpdate& u : pU)
        if(u.param == pP) return &u;
    return nullptr;
}

// A trend row's bars as '0'..'7', its gaps as '.'; the numbers after them are left out.
std::string Bars(std::string_view pRow)
{
    static const char* const BARS[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    std::string out;
    std::size_t i = 0;
    while(i < pRow.size())
    {
        const std::string_view rest = pRow.substr(i);
        const auto bar =
            std::find_if(std::begin(BARS), std::end(BARS), [&](const char* pB) { return rest.starts_with(pB); });
        if(bar != std::end(BARS))
        {
            out += static_cast<char>('0' + (bar - std::begin(BARS)));
            i += std::strlen(*bar);
        }
        else if(rest.starts_with("·"))
        {
            out += '.';
            i += std::strlen("·");
        }
        else
        {
            break;
        }
    }
    return out;
}

float Low(std::string_view pRow)
{
    return std::stof(std::string(pRow.substr(pRow.find(" low ") + 5)));
}

} // namespace

TEST_CASE("The parameter table: unique ids, the two writable ones, the three series", "[cloud]")
{
    std::set<std::string> ids;
    for(const ParamInfo& p : PARAM_INFO)
        ids.insert(p.id);
    REQUIRE(ids.size() == PARAMS);
    REQUIRE(PARAM_INFO[static_cast<std::size_t>(Param::Ack)].writable);
    REQUIRE(PARAM_INFO[static_cast<std::size_t>(Param::Suspend)].writable);
    REQUIRE(PARAM_INFO[static_cast<std::size_t>(Param::Suspend)].max == 60);
    std::size_t writable = 0;
    std::size_t series = 0;
    for(const ParamInfo& p : PARAM_INFO)
    {
        writable += p.writable;
        series += p.timeSeries;
    }
    REQUIRE(writable == 2);
    REQUIRE(series == 3);
}

TEST_CASE("Status line: level, DO and every flag that matters to a person", "[cloud]")
{
    reefdo::app::Status s;
    REQUIRE(StatusLine(s).view() == "Normal · no reading");
    s.doMgl = 6.504f;
    REQUIRE(StatusLine(s).view() == "Normal · 6.50 mg/L");
    s.level = Level::Yellow;
    s.suspect = true;
    s.alertsSuspended = true;
    s.alertsSuspendLeftS = 1741;
    s.silenced = true;
    s.maintenance = true;
    REQUIRE(StatusLine(s).view() == "Yellow · 6.50 mg/L · sudden drop · suspended 30 min · silenced · maintenance");
    reefdo::app::Status f;
    f.fault = true;
    f.level = Level::Red;
    REQUIRE(StatusLine(f).view() == "FAULT · no reading · probe not answering");
}

TEST_CASE("Online: everything once, then only what changes; offline: nothing, and everything again after", "[cloud]")
{
    Bench b;
    Updates u = b.Tick();
    REQUIRE(Find(u, Param::Level)->text.view() == "Normal");
    REQUIRE(Find(u, Param::Status)->text.view().starts_with("Normal · "));
    REQUIRE_FALSE(Find(u, Param::Status)->notify);
    REQUIRE(Find(u, Param::Suspend)->i == 0);
    REQUIRE(Find(u, Param::Ack) != nullptr);
    u = b.Tick();
    REQUIRE(Find(u, Param::Level) == nullptr);
    REQUIRE(Find(u, Param::Suspend) == nullptr);
    REQUIRE(Find(u, Param::Ack) == nullptr);

    b.online = false;
    b.RunS(600);
    for(std::size_t i = b.steps.size() - 60; i < b.steps.size(); ++i)
        REQUIRE(b.steps[i].empty());
    b.online = true;
    u = b.Tick();
    REQUIRE(Find(u, Param::Level) != nullptr); // back: the whole picture again
    REQUIRE(Find(u, Param::Ack) != nullptr);
}

TEST_CASE("The status line follows the DO at most once a minute, a state change at once", "[cloud]")
{
    Bench b;
    b.Tick();
    b.steps.clear();
    b.RunS(600);
    const std::size_t statuses = b.Count(Param::Status);
    REQUIRE(statuses >= 1);
    REQUIRE(statuses <= 10); // the DO moves every sample; the line at most once a minute
    b.steps.clear();
    b.s.app.Ack(); // silenced: a state change
    b.Tick();
    b.Tick();
    REQUIRE(b.Count(Param::Status) >= 1);
}

TEST_CASE("Series: one median per 5 minutes and series; nothing without a clock or a reading", "[cloud]")
{
    Bench b;
    b.RunS(3 * SERIES_BUCKET_S);
    const std::size_t points = b.Count(Param::Do);
    REQUIRE(points >= 3); // the first on connecting, then one per bucket
    REQUIRE(points <= 4);
    REQUIRE(b.Count(Param::Saturation) == points);
    REQUIRE(b.Count(Param::Temperature) == points);

    // The median of what the bucket saw.
    Bench m;
    while(*m.s.clock.unixS % SERIES_BUCKET_S != 0)
        m.Tick(); // start at a bucket boundary
    std::vector<float> seen;
    Updates u;
    for(uint32_t t = 0; t <= SERIES_BUCKET_S; t += TICK_S)
    {
        u = m.Tick();
        if(t < SERIES_BUCKET_S) seen.push_back(*m.s.app.GetStatus().doMgl);
    }
    std::sort(seen.begin(), seen.end());
    REQUIRE(Find(u, Param::Do) != nullptr);
    REQUIRE(Find(u, Param::Do)->f == Rounded(seen[seen.size() / 2], 100.0f));
    REQUIRE(Find(u, Param::Saturation)->f > 50.0f);
    REQUIRE(Find(u, Param::Temperature)->f > 20.0f);

    // No clock: no points. No reading for a whole bucket: no point for it.
    Agent a;
    Clock c = m.s.clock;
    c.unixS.reset();
    REQUIRE(Find(a.Step(m.s.app, c, {}, true), Param::Do) == nullptr);
    Bench d;
    d.s.probe.Model().dropout = true;
    d.RunS(2 * SERIES_BUCKET_S);
    REQUIRE(d.Count(Param::Do) == 0);

    // A bucket holds at most BUCKET_MAX samples; more are dropped, the median still comes.
    Agent full;
    Clock same = m.s.clock;
    for(std::size_t i = 0; i < BUCKET_MAX + 10; ++i)
        full.Step(m.s.app, same, {}, true);
    *same.unixS += SERIES_BUCKET_S;
    REQUIRE(Find(full.Step(m.s.app, same, {}, true), Param::Do)->f == Rounded(*m.s.app.GetStatus().doMgl, 100.0f));
}

TEST_CASE("Connecting shows this bucket's median at once, rounded as the app shows it", "[cloud]")
{
    Bench m;
    m.RunS(60); // readings
    while(*m.s.clock.unixS % SERIES_BUCKET_S != 0)
        m.Tick(); // a bucket boundary
    const reefdo::app::Status& s = m.s.app.GetStatus();
    REQUIRE(s.doMgl.has_value());
    Clock c = m.s.clock;
    *c.unixS += 60; // mid-bucket

    // Offline samples, then the link comes up in the same bucket: its median so far, every series.
    Agent a;
    a.Step(m.s.app, c, {}, false);
    *c.unixS += TICK_S;
    const Updates u = a.Step(m.s.app, c, {}, true);
    REQUIRE(Find(u, Param::Do) != nullptr);
    REQUIRE(Find(u, Param::Do)->f == Rounded(*s.doMgl, 100.0f));
    REQUIRE(Find(u, Param::Saturation)->f == Rounded(*s.satPct, 10.0f));
    REQUIRE(Find(u, Param::Temperature)->f == Rounded(*s.tempC, 10.0f));
    *c.unixS += TICK_S;
    REQUIRE(Find(a.Step(m.s.app, c, {}, true), Param::Do) == nullptr); // then only per bucket

    // The link comes up as a bucket closes: that bucket's point only, not a second one for the new bucket.
    Agent e;
    e.Step(m.s.app, c, {}, false);
    *c.unixS += SERIES_BUCKET_S;
    const Updates v = e.Step(m.s.app, c, {}, true);
    REQUIRE(std::count_if(v.begin(), v.end(), [](const Update& pU) { return pU.param == Param::Do; }) == 1);
}

TEST_CASE("Pushes: an alert that needs a person, a FAULT, a failed check, a lost plug — nothing else", "[cloud]")
{
    Bench b;
    b.Tick();
    auto pushes = [&b](Notification pN)
    {
        Agent a;
        a.Step(b.s.app, b.s.clock, {}, true);
        const Updates u = a.Step(b.s.app, b.s.clock, std::span<const Notification>(&pN, 1), true);
        const Update* st = Find(u, Param::Status);
        return st != nullptr && st->notify && a.Pushes() == 1;
    };
    Notification yellow{reefdo::app::NotifyKind::Level};
    yellow.urgent = true;
    REQUIRE(pushes(yellow));
    REQUIRE_FALSE(pushes(Notification{reefdo::app::NotifyKind::Level})); // Blue: silent at the tank, silent here
    REQUIRE(pushes(Notification{reefdo::app::NotifyKind::Fault}));
    REQUIRE(pushes(Notification{reefdo::app::NotifyKind::TestFail}));
    REQUIRE(pushes(Notification{reefdo::app::NotifyKind::DeviceLost}));
    REQUIRE_FALSE(pushes(Notification{reefdo::app::NotifyKind::Recovered}));
    REQUIRE_FALSE(pushes(Notification{reefdo::app::NotifyKind::Boot}));

    // And a real one: the tank crashes to Red.
    b.s.tank.SetSat(55.0f);
    b.RunS(120);
    bool pushed = false;
    for(const Updates& u : b.steps)
        if(const Update* st = Find(u, Param::Status)) pushed = pushed || st->notify;
    REQUIRE(pushed);
    REQUIRE(b.agent.Pushes() >= 1);
    REQUIRE(b.s.app.GetStatus().level == Level::Red);
}

TEST_CASE("Writes: Ack and alert suspend through the router, as the cloud; everything else only reports", "[cloud]")
{
    Bench b;
    b.Tick();
    // Ack: pressed → acknowledged, the button resets; released → only the reset.
    Updates u = b.Write(Param::Ack, 1);
    REQUIRE(Find(u, Param::Ack) != nullptr);
    REQUIRE_FALSE(Find(u, Param::Ack)->b);
    REQUIRE(b.platform.outputsChanged == 1); // it went through /api/cmd
    u = b.Write(Param::Ack, 0);
    REQUIRE(Find(u, Param::Ack) != nullptr);
    REQUIRE(b.platform.outputsChanged == 1);

    // Suspend at Normal: refused, the slider snaps back to 0.
    u = b.Write(Param::Suspend, 30);
    REQUIRE(Find(u, Param::Suspend)->i == 0);
    b.s.tank.SetSat(55.0f);
    b.RunS(120);
    REQUIRE(b.s.app.GetStatus().level == Level::Red);
    u = b.Write(Param::Suspend, 30);
    REQUIRE(Find(u, Param::Suspend)->i == 30);
    b.Tick();
    REQUIRE(b.s.app.GetStatus().alertsSuspended);
    b.steps.clear();
    b.RunS(130);
    REQUIRE(b.Count(Param::Suspend) >= 2); // the minutes count down
    u = b.Write(Param::Suspend, 99);       // out of range: only what holds now
    REQUIRE(Find(u, Param::Suspend)->i == 28);
    u = b.Write(Param::Suspend, -5);
    REQUIRE(Find(u, Param::Suspend)->i == 28);
    u = b.Write(Param::Suspend, 0);
    REQUIRE(Find(u, Param::Suspend)->i == 0);
    b.Tick();
    REQUIRE_FALSE(b.s.app.GetStatus().alertsSuspended);
    REQUIRE(b.Write(Param::Level, 1).empty()); // read-only
}

TEST_CASE("FAULT and maintenance show in the level and the status at once", "[cloud]")
{
    Bench b;
    b.Tick();
    b.steps.clear();
    b.s.app.SetMaintenance(true, b.s.clock);
    const Updates m = b.Tick();
    REQUIRE(Find(m, Param::Status)->text.view().ends_with("maintenance"));
    b.s.app.SetMaintenance(false, b.s.clock);
    b.s.probe.Model().dropout = true;
    b.RunS(120);
    bool faultLevel = false;
    bool pushed = false;
    for(const Updates& u : b.steps)
    {
        if(const Update* l = Find(u, Param::Level)) faultLevel = faultLevel || l->text.view() == "FAULT";
        if(const Update* st = Find(u, Param::Status)) pushed = pushed || st->notify;
    }
    REQUIRE(faultLevel);
    REQUIRE(pushed);
}

TEST_CASE("A sudden drop shows in the status at once", "[cloud]")
{
    reefdo::config::Config c = ExampleConfig();
    c.ladder.suddenSlopeMglPer10min = 3.0f;
    Bench b;
    REQUIRE(b.s.app.SetConfig(c, b.s.clock));
    b.RunS(300);
    b.steps.clear();
    b.s.probe.Model().bubblePct = -40.0f; // a snail on the probe
    b.RunS(120);
    REQUIRE(b.s.app.GetStatus().suspect);
    bool shown = false;
    for(const Updates& u : b.steps)
        if(const Update* st = Find(u, Param::Status))
            shown = shown || st->text.view().find("sudden drop") != std::string::npos;
    REQUIRE(shown);
}

TEST_CASE("Back online in a standing alarm, the phone is told once more; not when silenced", "[cloud]")
{
    auto reconnectPushes = [](bool pSilence, bool pSuspend, float pSat)
    {
        Bench b;
        b.Tick();
        b.online = false;
        b.s.tank.SetSat(pSat);
        b.RunS(120);
        if(pSilence) b.s.app.Ack();
        if(pSuspend) b.s.app.SuspendAlerts(600);
        b.RunS(20);
        b.online = true;
        const Update* st = Find(b.Tick(), Param::Status);
        return st != nullptr && st->notify;
    };
    REQUIRE(reconnectPushes(false, false, 55.0f));       // Red
    REQUIRE_FALSE(reconnectPushes(true, false, 55.0f));  // acknowledged at the tank
    REQUIRE_FALSE(reconnectPushes(false, true, 55.0f));  // suspended
    REQUIRE_FALSE(reconnectPushes(false, false, 98.0f)); // nothing wrong
}

TEST_CASE("A suspend from the app carries the level the app shows, and its minutes are reported again", "[cloud]")
{
    Bench b;
    b.Tick();                   // the app shows Normal
    b.s.tank.SetSat(55.0f);     // meanwhile the tank crashes...
    for(int i = 0; i < 12; ++i) // ...while the link is down: the app still shows Normal
        b.s.app.Tick(b.s.clock), b.s.clock.nowMs += 10'000, *b.s.clock.unixS += 10,
            b.s.tank.Step(10.0f, b.s.MinuteOfDay(), b.s.app.GetStatus().deviceOn);
    REQUIRE(b.s.app.GetStatus().level == Level::Red);
    Updates u = b.Write(Param::Suspend, 30);
    REQUIRE(Find(u, Param::Suspend)->i == 0); // refused: the board is worse off than the app shows
    b.Tick();                                 // the app learns about Red
    u = b.Write(Param::Suspend, 30);
    REQUIRE(Find(u, Param::Suspend)->i == 30);
    u = b.Tick();
    REQUIRE(Find(u, Param::Suspend) != nullptr); // reported again from the real state
    REQUIRE(Find(u, Param::Suspend)->i == 30);

    // Nothing reported yet: a suspend is judged by the board alone.
    Bench fresh;
    fresh.s.tank.SetSat(55.0f);
    fresh.s.RunS(120);
    REQUIRE(fresh.s.app.GetStatus().level == Level::Red);
    REQUIRE(Find(fresh.Write(Param::Suspend, 10), Param::Suspend)->i == 10);
}

TEST_CASE("Trend rows: a bar per block, the window's low with its time and its high; all of them on connecting",
          "[cloud]")
{
    Bench b;
    std::vector<std::pair<uint32_t, float>> seen; // every sample: when, DO
    for(uint32_t t = 0; t < 2 * 3600; t += TICK_S)
    {
        const uint32_t ts = *b.s.clock.unixS;
        b.Tick();
        if(b.s.app.GetStatus().doMgl.has_value()) seen.emplace_back(ts, *b.s.app.GetStatus().doMgl);
    }
    const TrendUpdates u = b.agent.Trends(b.s.app, b.s.clock, true);
    REQUIRE(u.size() == TRENDS);

    // The last hour: 24 bars of 150 s from local time, all seen; the low (its first time) and the high of them.
    const std::string_view hour = FindRow(u, Param::Hour)->text.view();
    REQUIRE(Bars(hour).size() == 24);
    REQUIRE(Bars(hour).find('.') == std::string::npos);
    const int64_t from = ((static_cast<int64_t>(*b.s.clock.unixS) - TICK_S + TZ) / 150 - 23) * 150 - TZ;
    float lo = 1e9f;
    float hi = -1e9f;
    uint32_t loTs = 0;
    for(const auto& [ts, v] : seen)
    {
        if(ts < from) continue;
        if(v < lo)
        {
            lo = v;
            loTs = ts;
        }
        hi = std::max(hi, v);
    }
    const int64_t at = static_cast<int64_t>(loTs) + TZ;
    char want[64];
    std::snprintf(want, sizeof want, " low %.2f at %02d:%02d · high %.2f", static_cast<double>(lo),
                  static_cast<int>(at / 3600 % 24), static_cast<int>(at / 60 % 60), static_cast<double>(hi));
    REQUIRE(std::string(hour).ends_with(want));

    // Longer rows: two hours fill a few of their bars, the rest are gaps; the week names the day.
    const std::string half = Bars(FindRow(u, Param::HalfDay)->text.view());
    REQUIRE(half.size() == 24);
    REQUIRE(std::count(half.begin(), half.end(), '.') >= 19);
    REQUIRE(half.back() != '.');
    REQUIRE(Bars(FindRow(u, Param::Day)->text.view()).size() == 24);
    const std::string_view week = FindRow(u, Param::Week)->text.view();
    REQUIRE(Bars(week).size() == 28);
    const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const float lowest =
        std::min_element(seen.begin(), seen.end(), [](const auto& pA, const auto& pB) { return pA.second < pB.second; })
            ->second;
    std::snprintf(want, sizeof want, " low %.2f ", static_cast<double>(lowest)); // every sample is in its window
    REQUIRE(week.find(want) != std::string_view::npos);
    REQUIRE(std::any_of(std::begin(days), std::end(days), [&](const char* pD)
                        { return week.find(std::string(" ") + pD + " ") != std::string_view::npos; }));
}

TEST_CASE("Trend rows: each at most once a minute, all again when the link comes back, none offline or without a "
          "clock",
          "[cloud]")
{
    Bench b;
    b.RunS(600);
    REQUIRE(b.agent.Trends(b.s.app, b.s.clock, true).size() == TRENDS);
    REQUIRE(b.agent.Trends(b.s.app, b.s.clock, true).empty()); // nothing new yet
    std::vector<uint64_t> hourAt;
    for(uint32_t t = 0; t < 1800; t += TICK_S)
    {
        b.Tick();
        for(const TrendUpdate& u : b.agent.Trends(b.s.app, b.s.clock, true))
            if(u.param == Param::Hour) hourAt.push_back(b.s.clock.nowMs);
    }
    REQUIRE(hourAt.size() >= 2);
    for(std::size_t i = 1; i < hourAt.size(); ++i)
        REQUIRE(hourAt[i] - hourAt[i - 1] >= TREND_MIN_S * 1000u);

    REQUIRE(b.agent.Trends(b.s.app, b.s.clock, false).empty());
    REQUIRE(b.agent.Trends(b.s.app, b.s.clock, true).size() == TRENDS); // back: every row once more
    Clock blind = b.s.clock;
    blind.unixS.reset();
    REQUIRE(b.agent.Trends(b.s.app, blind, true).empty());
}

TEST_CASE("Trend rows after a reboot come back from the 5-minute log; a log too old or unreadable leaves them empty",
          "[cloud]")
{
    Bench b;
    b.RunS(6 * 3600);
    const TrendUpdates live = b.agent.Trends(b.s.app, b.s.clock, true);

    // A reboot: a few samples, then the first call refills what came before them from the log.
    Agent fresh;
    for(int i = 0; i < 3; ++i)
    {
        b.s.app.Tick(b.s.clock);
        fresh.Step(b.s.app, b.s.clock, {}, true);
        b.s.clock.nowMs += TICK_S * 1000;
        *b.s.clock.unixS += TICK_S;
    }
    const TrendUpdates back = fresh.Trends(b.s.app, b.s.clock, true);
    REQUIRE(back.size() == TRENDS);
    for(std::size_t r = 0; r < TRENDS; ++r)
    {
        const std::string was = Bars(live[r].text.view());
        const std::string now = Bars(back[r].text.view());
        REQUIRE(was.size() == now.size());
        const auto gaps = [](const std::string& pB) { return std::count(pB.begin(), pB.end(), '.'); };
        REQUIRE(std::abs(gaps(was) - gaps(now)) <= 2); // the last minutes are not in the log yet
        REQUIRE(std::abs(Low(live[r].text.view()) - Low(back[r].text.view())) <= 0.011f);
    }

    Agent cold; // asked before its first sample: everything from the log
    for(const TrendUpdate& u : cold.Trends(b.s.app, b.s.clock, true))
        REQUIRE(Bars(u.text.view()).find_first_not_of('.') != std::string::npos);

    Agent late; // a week and a day later the log has nothing for any row
    Clock later = b.s.clock;
    *later.unixS += 8 * 86400;
    for(const TrendUpdate& u : late.Trends(b.s.app, later, true))
        REQUIRE(u.text.view() == "no reading");
    b.s.b.failRead = true;
    Agent unread;
    for(const TrendUpdate& u : unread.Trends(b.s.app, b.s.clock, true))
        REQUIRE(u.text.view() == "no reading");
}

TEST_CASE("Trend bars sit on a fixed scale from 0.5 mg/L below Red to 2.5 above Blue", "[cloud]")
{
    reefdo::config::Config under = ExampleConfig(); // every reading far above the scale: full bars
    under.ladder.red.mgl = 0.5f;
    under.ladder.yellow.mgl = 0.8f;
    under.ladder.blue.mgl = 1.0f;
    Bench full(under);
    full.RunS(3600);
    const std::string top = Bars(FindRow(full.agent.Trends(full.s.app, full.s.clock, true), Param::Hour)->text.view());
    REQUIRE(top == std::string(24, '7'));

    reefdo::config::Config over = ExampleConfig(); // every reading below it: the lowest bar
    over.ladder.red.mgl = 14.0f;
    over.ladder.yellow.mgl = 15.0f;
    over.ladder.blue.mgl = 16.0f;
    Bench empty(over);
    empty.RunS(3600);
    const std::string bottom =
        Bars(FindRow(empty.agent.Trends(empty.s.app, empty.s.clock, true), Param::Hour)->text.view());
    REQUIRE(bottom == std::string(24, '0'));
}
