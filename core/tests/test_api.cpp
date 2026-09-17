#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include <ArduinoJson.h>

#include "reefdo/api.hpp"

#include "scenario.hpp"

using namespace scenario;
using namespace reefdo::api;
using reefdo::log::Type;

namespace
{

struct StringSink : ISink
{
    std::string out;
    std::size_t stopAfter = SIZE_MAX; // simulate a client that goes away
    bool Write(std::string_view pChunk) override
    {
        if(out.size() >= stopAfter) return false;
        out.append(pChunk);
        return true;
    }
    std::size_t Lines() const
    {
        std::size_t n = 0;
        for(char c : out)
            n += (c == '\n');
        return n;
    }
};

std::string Status(const Scenario& pS)
{
    std::vector<char> buf(4096);
    const std::size_t n = StatusJson(pS.app, pS.clock, buf);
    REQUIRE(n > 0);
    return std::string(buf.data(), n);
}

JsonDocument Parse(const std::string& pS)
{
    JsonDocument doc;
    REQUIRE(!deserializeJson(doc, pS));
    return doc;
}

} // namespace

TEST_CASE("status_json describes the device, the ladder thresholds, and every device", "[api]")
{
    Scenario s;
    s.Start();
    s.RunS(120);
    const JsonDocument d = Parse(Status(s));
    REQUIRE(d["clock_known"] == true);
    REQUIRE(d["uptime_s"].as<uint32_t>() >= 110);
    REQUIRE(d["probe"]["status"] == "ok");
    REQUIRE(d["probe"]["do"].as<float>() > 6.0f);
    REQUIRE(d["ladder"]["level"] == "normal");
    REQUIRE(d["ladder"]["enters_below"]["blue"].as<float>() == Catch::Approx(5.65f));
    REQUIRE(d["ladder"]["leaves_above"]["red"].as<float>() == Catch::Approx(4.70f));
    REQUIRE(d["correction"]["factory"] == true);
    REQUIRE(d["correction"]["seawater_scale_hint"].as<float>() > 0.8f);
    REQUIRE(d["devices"].size() == 6);
    REQUIRE(d["devices"][0]["name"] == "small bubbler");
    REQUIRE(d["devices"][0]["wired"] == "NC");
    REQUIRE(d["devices"][4]["pulse"] == true);
    REQUIRE(d["devices"][4]["on"] == true);
    REQUIRE(d["devices"][4]["energised"] == false);
    REQUIRE(d["service"]["window_start"] == "19:30");
    REQUIRE(d["boost"]["enabled"] == false);
    REQUIRE(d["boost"]["running"] == false);
    REQUIRE(d["boost"]["window_end"] == "20:00");
    REQUIRE(d["log"]["a"].as<uint32_t>() > 10);

    // probe down: the optionals become null
    s.probe.Model().dropout = true;
    s.RunS(60);
    const JsonDocument f = Parse(Status(s));
    REQUIRE(f["probe"]["do"].isNull());
    REQUIRE(f["probe"]["status"] == "timeout");
    REQUIRE(f["ladder"]["fault"] == true);
    REQUIRE(f["ladder"]["effective"] == "red");

    Scenario nc; // clock unknown: no "unix" key
    nc.clockKnown = false;
    nc.Start();
    nc.Tick();
    std::vector<char> buf(4096);
    Clock c = nc.clock;
    c.unixS.reset();
    REQUIRE(StatusJson(nc.app, c, buf) > 0);
    const JsonDocument u = Parse(std::string(buf.data()));
    REQUIRE(u["unix"].isNull());
    REQUIRE(u["clock_known"] == false);

    char tiny[64];
    REQUIRE(StatusJson(s.app, s.clock, tiny) == 0);
}

TEST_CASE("service_json reports outcomes, responses and the run duration", "[api]")
{
    Scenario s;
    s.Start();
    std::vector<char> buf(2048);
    REQUIRE(ServiceJson(s.app, buf) > 0);
    JsonDocument d = Parse(std::string(buf.data()));
    REQUIRE(d["running"] == false);
    REQUIRE(d["run_duration_s"].as<uint32_t>() == 3 * 300 + 3 * 60 + 2 * 120);
    REQUIRE(d["devices"][0]["last_outcome"] == "none");
    REQUIRE(d["last_run_day"].isNull());
    s.RunUntil(21, 5);
    REQUIRE(ServiceJson(s.app, buf) > 0);
    d = Parse(std::string(buf.data()));
    REQUIRE(d["devices"][0]["last_outcome"] == "pass");
    REQUIRE(d["devices"][1]["last_outcome"] == "unchecked");
    REQUIRE(d["devices"][0]["last_response"].as<float>() > 1.0f);
    REQUIRE(d["last_run_day"].as<uint32_t>() > 20000);
    char tiny[32];
    REQUIRE(ServiceJson(s.app, tiny) == 0);
}

TEST_CASE("apply_config keeps the current values for keys the document omits", "[api]")
{
    Scenario s;
    s.Start();
    reefdo::config::LoadResult r = ApplyConfig(s.app, R"({"levels":{"blue":{"mgl":6.1}}})", s.clock);
    REQUIRE(r.Ok());
    REQUIRE(s.app.GetConfig().ladder.blue.mgl == Catch::Approx(6.1f));
    REQUIRE(s.app.GetConfig().devices[0].name.view() == "small bubbler"); // untouched
    REQUIRE(s.EventsOf(Type::Config) == 2);
    char buf[256];
    REQUIRE(ResultJson(r, buf) > 0);
    REQUIRE(std::string(buf) == "{\"ok\":true}");

    r = ApplyConfig(s.app, R"({"levels":{"blue":{"mgl":"x"}}})", s.clock);
    REQUIRE_FALSE(r.Ok());
    REQUIRE(ResultJson(r, buf) > 0);
    REQUIRE(std::string(buf).find("\"path\":\"levels.blue.mgl\"") != std::string::npos);
    REQUIRE(s.app.GetConfig().ladder.blue.mgl == Catch::Approx(6.1f));
    char tiny[4];
    REQUIRE(ResultJson(r, tiny) == 0);
}

TEST_CASE("apply_command: every command and every refusal", "[api]")
{
    Scenario s;
    s.Start();
    s.RunS(60);
    REQUIRE_FALSE(ApplyCommand(s.app, "not json", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, "[1]", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"dance":true})", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"ack":false})", s.clock).ok);

    REQUIRE(ApplyCommand(s.app, R"({"ack":true})", s.clock).ok);
    REQUIRE(ApplyCommand(s.app, R"({"service":"run"})", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"service":"walk"})", s.clock).ok);
    s.Tick();
    REQUIRE(s.app.GetStatus().serviceRunning);

    Command c = ApplyCommand(s.app, R"({"cal":"air"})", s.clock);
    REQUIRE_FALSE(c.ok);
    REQUIRE(std::string(c.message) == "maintenance mode required");
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"cal":"zero"})", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"relay":{"device":2,"on":true}})", s.clock).ok); // not in maintenance
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"relay":{"device":0,"on":true}})", s.clock).ok);
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"relay":{"device":"two"}})", s.clock).ok);

    REQUIRE(ApplyCommand(s.app, R"({"maintenance":true})", s.clock).ok);
    s.Tick();
    REQUIRE(s.app.GetStatus().maintenance);
    REQUIRE(ApplyCommand(s.app, R"({"cal":"air"})", s.clock).ok);
    REQUIRE(s.probe.Calibrations() == 1);
    REQUIRE(ApplyCommand(s.app, R"({"relay":{"device":2,"on":true}})", s.clock).ok);
    s.Tick();
    REQUIRE(s.app.GetStatus().deviceOn[1]);
    REQUIRE(ApplyCommand(s.app, R"({"relay":{"device":2}})", s.clock).ok); // clears the override
    s.Tick();
    REQUIRE_FALSE(s.app.GetStatus().deviceOn[1]);
    REQUIRE(ApplyCommand(s.app, R"({"maintenance":false})", s.clock).ok);

    c = ApplyCommand(s.app, R"({"time":{"unix":1789401600,"tz":7200}})", s.clock);
    REQUIRE(c.ok);
    REQUIRE(c.setUnix == 1789401600u);
    REQUIRE(c.setTz == 7200);
    c = ApplyCommand(s.app, R"({"time":{"unix":1789401600}})", s.clock);
    REQUIRE(c.ok);
    REQUIRE_FALSE(c.setTz.has_value());
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"time":{"unix":"now"}})", s.clock).ok);
}

TEST_CASE("Calibration failure is reported as the probe's refusal", "[api]")
{
    Scenario s;
    s.Start();
    s.app.SetMaintenance(true, s.clock);
    s.probe.Model().dropout = true;
    // The simulated probe always acknowledges; the message path for "did not acknowledge" needs a failing probe.
    struct FailingProbe : reefdo::probe::IProbe
    {
        reefdo::probe::PollResult Poll() override { return {}; }
        reefdo::probe::CalResult AirCalibrate() override { return reefdo::probe::CalResult::Failed; }
    } failing;
    FakeStore a(2, 4 * reefdo::log::RECORD_SIZE);
    FakeStore b(2, 4 * reefdo::log::AGGREGATE_SIZE);
    FakeStore e(2, 4 * reefdo::log::RECORD_SIZE);
    FakeStore d(2, 4 * reefdo::log::RECORD_SIZE);
    App app(ExampleConfig(), failing, {a, b, e, d});
    app.Start(s.clock, 1);
    app.SetMaintenance(true, s.clock);
    const Command c = ApplyCommand(app, R"({"cal":"air"})", s.clock);
    REQUIRE_FALSE(c.ok);
    REQUIRE(std::string(c.message) == "the probe did not acknowledge");
}

TEST_CASE("export_csv and events_csv stream everything since a seq, with a header", "[api]")
{
    Scenario s;
    s.Start();
    s.RunS(100);
    StringSink all;
    const std::size_t n = ExportCsv(s.app, 0, all);
    REQUIRE(n == s.app.LogA().Count());
    REQUIRE(all.Lines() == n + 1);
    REQUIRE(all.out.rfind("seq,ts,uptime_s,type,level,flags,f0,f1,f2,f3,aux\n", 0) == 0);

    StringSink since;
    const uint32_t mid = s.app.LogA().At(5)->seq;
    REQUIRE(ExportCsv(s.app, mid, since) == n - 5);

    StringSink ev;
    REQUIRE(EventsCsv(s.app, 0, ev) == s.app.LogE().Count());
    REQUIRE(EventsCsv(s.app, 999999, ev) == 0);

    StringSink gone;
    gone.stopAfter = 200;
    REQUIRE(ExportCsv(s.app, 0, gone) < n);
    StringSink dead;
    dead.stopAfter = 0;
    REQUIRE(ExportCsv(s.app, 0, dead) == 0);
    REQUIRE(EventsCsv(s.app, 0, dead) == 0);
    dead.stopAfter = 60;
    REQUIRE(EventsCsv(s.app, 0, dead) < s.app.LogE().Count());
}

TEST_CASE("series_csv: tier A by time window with decimation, tier B aggregates, and clock-less records", "[api]")
{
    Scenario s;
    s.Start();
    s.RunS(2 * 3600);
    const uint32_t now = *s.clock.unixS;
    StringSink a;
    REQUIRE(SeriesCsv(s.app, 'A', now - 600, now, 1, a) == 60);
    REQUIRE(a.out.rfind("ts,do,sat,temp,level\n", 0) == 0);
    StringSink dec;
    REQUIRE(SeriesCsv(s.app, 'A', now - 600, now, 6, dec) == 10);
    StringSink zeroEvery;
    REQUIRE(SeriesCsv(s.app, 'A', now - 600, now, 0, zeroEvery) == 60);
    StringSink window;
    REQUIRE(SeriesCsv(s.app, 'A', now - 1200, now - 600, 1, window) == 61);
    StringSink b;
    REQUIRE(SeriesCsv(s.app, 'B', 0, now, 1, b) >= 23);
    REQUIRE(b.out.rfind("ts,do_min,do_avg,do_max,sat_avg,temp_min,temp_max,level_max\n", 0) == 0);
    StringSink b2;
    REQUIRE(SeriesCsv(s.app, 'B', now - 3600, now, 2, b2) <= 6);
    StringSink dead;
    dead.stopAfter = 0;
    REQUIRE(SeriesCsv(s.app, 'A', 0, now, 1, dead) == 0);
    REQUIRE(SeriesCsv(s.app, 'B', 0, now, 1, dead) == 0);
    dead.stopAfter = 100;
    REQUIRE(SeriesCsv(s.app, 'A', 0, now, 1, dead) < 100);
    StringSink deadb;
    deadb.stopAfter = 100;
    REQUIRE(SeriesCsv(s.app, 'B', 0, now, 1, deadb) < 24);

    Scenario nc; // records written before the clock was known are skipped over by the time window
    nc.clockKnown = false;
    nc.Start();
    nc.RunS(600);
    nc.clockKnown = true;
    nc.RunS(600);
    StringSink w;
    const uint32_t t = *nc.clock.unixS;
    REQUIRE(SeriesCsv(nc.app, 'A', t - 300, t, 1, w) == 30);
    StringSink everything;
    REQUIRE(SeriesCsv(nc.app, 'A', 0, t, 1, everything) >= 60); // ts 0 records are included when from == 0
}

TEST_CASE("Unreadable flash and odd inputs never break the streams", "[api]")
{
    Scenario s;
    s.Start();
    s.RunS(1200);
    const uint32_t now = *s.clock.unixS;
    REQUIRE_FALSE(ApplyCommand(s.app, R"({"relay":{"device":7,"on":true}})", s.clock).ok);

    StringSink past; // aggregates entirely after the window
    REQUIRE(SeriesCsv(s.app, 'B', 0, now - 3000, 1, past) == 0);

    // A reboot without a clock in the middle of the day: those records sit between timestamped ones.
    Scenario gap;
    gap.Start();
    gap.RunS(600);
    const uint32_t from = *gap.clock.unixS;
    gap.RunS(300);
    gap.clockKnown = false;
    gap.RunS(300);
    gap.clockKnown = true;
    gap.RunS(300);
    StringSink w;
    REQUIRE(SeriesCsv(gap.app, 'A', from, *gap.clock.unixS, 1, w) ==
            60); // 30 + 30 timestamped, the 30 blind ones skipped

    s.a.failRead = true;
    s.b.failRead = true;
    s.e.failRead = true;
    StringSink dead_a;
    StringSink dead_b;
    StringSink dead_e;
    StringSink dead_x;
    REQUIRE(SeriesCsv(s.app, 'A', 0, now, 1, dead_a) == 0);
    REQUIRE(SeriesCsv(s.app, 'B', 0, now, 1, dead_b) == 0);
    REQUIRE(EventsCsv(s.app, 0, dead_e) == 0);
    REQUIRE(ExportCsv(s.app, 0, dead_x) == 0);
}
