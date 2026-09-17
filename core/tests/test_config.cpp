#include <functional>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/config.hpp"

using namespace reefdo::config;
using Catch::Matchers::WithinAbs;
using reefdo::FixedString;
using reefdo::ladder::Level;
using reefdo::ladder::Mode;
using reefdo::ladder::Trigger;

namespace
{

// The reference configuration document.
const char* sExample = R"({
  "schema": 1,
  "sample_period_s": 10,
  "levels": {
    "blue":   { "mgl": 5.8, "hysteresis": 0.15, "dwell_s": 120, "slope_mgl_per_10min": 0 },
    "yellow": { "mgl": 5.2, "hysteresis": 0.15, "dwell_s": 60 },
    "red":    { "mgl": 4.5, "hysteresis": 0.20, "dwell_s": 0 }
  },
  "recover_sustain_s": 600,
  "night": { "start": "21:00", "end": "08:00", "lock": false, "unknown_time_is_night": true },
  "pulse": { "s": 10, "min_interval_s": 1800 },
  "heat": { "on_c": 28.0, "off_c": 27.5 },
  "fault": { "consecutive_failures": 5, "stuck_minutes": 30, "level": "yellow", "alert": true },
  "ack_silence_s": 1800,
  "devices": {
    "1": { "name": "small bubbler",    "wired": "NC", "trigger": "blue",   "mode": "on",        "service_s": 300, "min_response_pct": 1.0, "boost": true },
    "2": { "name": "extra powerhead",  "wired": "NC", "trigger": "blue",   "mode": "on",        "service_s": 300, "min_response_pct": 0 },
    "3": { "name": "strong air pump",  "wired": "NC", "trigger": "yellow", "mode": "on",        "service_s": 300, "min_response_pct": 1.0 },
    "4": { "name": "siren",            "wired": "NO", "trigger": "yellow", "mode": "on",        "ack_silences": true },
    "5": { "name": "return pump",      "wired": "NC", "trigger": "blue",   "mode": "pulse_off" },
    "6": { "name": "unused",           "wired": "NO", "trigger": "none" }
  },
  "service": { "window": ["19:30", "21:00"], "settle_s": 120, "tail_s": 60, "min_headroom_pct": 3.0,
               "inconclusive_days": 5, "escalate_if_failed": true, "chirp": true,
               "induce_deficit_s": 0, "induce_deficit_device": 5 },
  "boost": { "enabled": true, "window": ["19:30", "20:00"], "target_mgl": 6.5 },
  "correction": { "scale": 1.0, "offset": 0.0 },
  "salinity_psu": 35.0,
  "allow_zero_cal": false,
  "ntfy": { "enabled": false, "topic": "", "min_level": "blue" },
  "signals": { "buzzer_hz": 2400, "led_brightness": 40,
               "normal": { "buzzer": "off", "volume": 0 },
               "blue":   { "buzzer": "off", "volume": 0 },
               "yellow": { "buzzer": "beep", "volume": 2 },
               "red":    { "buzzer": "continuous", "volume": 3 },
               "fault":  { "buzzer": "triple", "volume": 2 } }
})";

Config Example()
{
    Config c;
    REQUIRE(Load(sExample, c).Ok());
    return c;
}

std::string Dump(const Config& pC)
{
    std::vector<char> buf(4096);
    const std::size_t n = Write(pC, buf);
    REQUIRE(n > 0);
    return std::string(buf.data(), n);
}

} // namespace

TEST_CASE("FixedString copies, truncates, terminates", "[config][fixed_string]")
{
    FixedString<4> s;
    REQUIRE(s.empty());
    REQUIRE(s.assign("abc"));
    REQUIRE(s.view() == "abc");
    REQUIRE(s.c_str()[3] == '\0');
    REQUIRE_FALSE(s.assign("abcdef")); // truncated to capacity
    REQUIRE(s.view() == "abcd");
    REQUIRE(s.size() == 4);
    REQUIRE(FixedString<4>::Capacity() == 4);
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(FixedString<4>("ab") == FixedString<4>("ab"));
    REQUIRE_FALSE(FixedString<4>("ab") == FixedString<4>("ac"));
}

TEST_CASE("HH:MM parsing and formatting", "[config]")
{
    REQUIRE(ParseHhmm("00:00") == uint16_t{0});
    REQUIRE(ParseHhmm("19:30") == uint16_t{19 * 60 + 30});
    REQUIRE(ParseHhmm("23:59") == uint16_t{23 * 60 + 59});
    for(const char* bad :
        {"", "9:00", "19:3", "24:00", "12:60", "12-00", "ab:cd", "1a:00", "12:0x", "19:300", " 9:00", "1-:00"})
    {
        INFO(bad);
        REQUIRE_FALSE(ParseHhmm(bad).has_value());
    }
    char buf[6];
    FormatHhmm(0, buf);
    REQUIRE(std::string(buf) == "00:00");
    FormatHhmm(19 * 60 + 5, buf);
    REQUIRE(std::string(buf) == "19:05");
    FormatHhmm(23 * 60 + 59, buf);
    REQUIRE(std::string(buf) == "23:59");
}

TEST_CASE("Defaults are valid, switch nothing, and survive a write/load round trip", "[config]")
{
    const Config d = Defaults();
    REQUIRE(Validate(d).Ok());
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        REQUIRE(d.ladder.devices[i].trigger == Trigger::None);
        REQUIRE_FALSE(d.devices[i].wiredNc);
    }
    REQUIRE(d.devices[0].name.view() == "device 1");
    REQUIRE(d.devices[5].name.view() == "device 6");
    REQUIRE(d.correction.IsFactory());

    Config back;
    const LoadResult r = Load(Dump(d), back);
    INFO(r.path.view() << ": " << r.message.view());
    REQUIRE(r.Ok());
    REQUIRE(back == d);
}

TEST_CASE("The example document loads into the expected fields", "[config]")
{
    const Config c = Example();
    REQUIRE(c.samplePeriodS == 10);
    REQUIRE_THAT(c.ladder.blue.mgl, WithinAbs(5.8, 1e-6));
    REQUIRE_THAT(c.ladder.blue.hysteresis, WithinAbs(0.15, 1e-6));
    REQUIRE(c.ladder.blue.dwellS == 120);
    REQUIRE(c.ladder.yellow.dwellS == 60);
    REQUIRE(c.ladder.red.dwellS == 0);
    REQUIRE(c.ladder.nightStartMin == 21 * 60);
    REQUIRE(c.ladder.nightEndMin == 8 * 60);
    REQUIRE_FALSE(c.ladder.nightLock);
    REQUIRE(c.ladder.faultLevel == Level::Yellow);
    REQUIRE(c.stuckMinutes == 30);
    REQUIRE(c.devices[0].name.view() == "small bubbler");
    REQUIRE(c.devices[0].wiredNc);
    REQUIRE(c.ladder.devices[0].trigger == Trigger::Blue);
    REQUIRE(c.devices[0].serviceS == 300);
    REQUIRE_THAT(c.devices[0].minResponsePct, WithinAbs(1.0, 1e-6));
    REQUIRE(c.ladder.devices[3].ackSilences);
    REQUIRE_FALSE(c.devices[3].wiredNc);
    REQUIRE(c.ladder.devices[4].mode == Mode::PulseOff);
    REQUIRE(c.devices[4].wiredNc);
    REQUIRE(c.ladder.devices[5].trigger == Trigger::None);
    REQUIRE(c.service.windowStartMin == 19 * 60 + 30);
    REQUIRE(c.service.windowEndMin == 21 * 60);
    REQUIRE(c.service.induceDeficitDevice == 5);
    REQUIRE(c.boost.enabled);
    REQUIRE(c.boost.windowStartMin == 19 * 60 + 30);
    REQUIRE(c.boost.windowEndMin == 20 * 60);
    REQUIRE(c.boost.devices[0]);
    REQUIRE_FALSE(c.boost.devices[1]);
    REQUIRE(c.ladder.escalateIfFailed);
    REQUIRE(c.correction.IsFactory());
    REQUIRE(c.ntfy.minLevel == Level::Blue);
    REQUIRE_FALSE(c.ntfy.enabled);
    REQUIRE(c.signals == SignalsConfig{}); // the example spells out the defaults
    REQUIRE(c.signals.of(Level::Normal, false) == c.signals.normal);
    REQUIRE(c.signals.of(Level::Blue, false) == c.signals.blue);
    REQUIRE(c.signals.of(Level::Yellow, false) == c.signals.yellow);
    REQUIRE(c.signals.of(Level::Red, false) == c.signals.red);
    REQUIRE(c.signals.of(Level::Blue, true) == c.signals.fault);

    Config back;
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);
}

TEST_CASE("A partial document changes only what it mentions; unknown keys are ignored", "[config]")
{
    Config c;
    REQUIRE(Load(R"({"levels":{"blue":{"mgl":6.0}},"whatever":{"x":1},"devices":{"2":{"name":"pump"}}})", c).Ok());
    REQUIRE_THAT(c.ladder.blue.mgl, WithinAbs(6.0, 1e-6));
    REQUIRE_THAT(c.ladder.blue.hysteresis, WithinAbs(0.15, 1e-6)); // untouched default
    REQUIRE(c.devices[1].name.view() == "pump");
    REQUIRE(c.devices[0].name.view() == "device 1");
    REQUIRE(Load("{}", c).Ok());
    REQUIRE(c == Defaults());
}

TEST_CASE("load is all-or-nothing", "[config]")
{
    Config c = Example();
    const Config before = c;
    REQUIRE_FALSE(Load(R"({"levels":{"blue":{"mgl":"six"}}})", c).Ok());
    REQUIRE(c == before);
    REQUIRE_FALSE(Load(R"({"sample_period_s": 1})", c).Ok()); // fails validation, not parsing
    REQUIRE(c == before);
}

TEST_CASE("Document-level errors", "[config]")
{
    Config c;
    LoadResult r = Load("{not json", c);
    REQUIRE(r.error == LoadError::InvalidJson);
    REQUIRE_FALSE(r.message.empty());
    REQUIRE(Load("[1, 2]", c).error == LoadError::NotAnObject);
    REQUIRE(Load("5", c).error == LoadError::NotAnObject);
}

TEST_CASE("Every wrong-type and bad-value path reports its dotted key", "[config]")
{
    struct Case
    {
        const char* json;
        LoadError error;
        const char* path;
    };
    const Case cases[] = {
        {R"({"levels": 5})", LoadError::WrongType, "levels"},
        {R"({"levels":{"blue":{"mgl":"x"}}})", LoadError::WrongType, "levels.blue.mgl"},
        {R"({"levels":{"blue":{"mgl":true}}})", LoadError::WrongType, "levels.blue.mgl"},
        {R"({"sample_period_s": 5.5})", LoadError::WrongType, "sample_period_s"},
        {R"({"sample_period_s": -3})", LoadError::WrongType, "sample_period_s"},
        {R"({"sample_period_s": "10"})", LoadError::WrongType, "sample_period_s"},
        {R"({"service":{"chirp": 1}})", LoadError::WrongType, "service.chirp"},
        {R"({"devices":{"1":{"name": 5}}})", LoadError::WrongType, "devices.1.name"},
        {R"({"devices":{"1":{"name": "a name that is far longer than twenty-four"}}})", LoadError::BadValue,
         "devices.1.name"},
        {R"({"ntfy":{"topic": 7}})", LoadError::WrongType, "ntfy.topic"},
        {R"({"ntfy":{"topic": "0123456789012345678901234567890123456789012345678"}})", LoadError::BadValue,
         "ntfy.topic"},
        {R"({"night":{"start": 2100}})", LoadError::WrongType, "night.start"},
        {R"({"night":{"start": "25:00"}})", LoadError::BadValue, "night.start"},
        {R"({"devices":{"3":{"trigger": "purple"}}})", LoadError::BadValue, "devices.3.trigger"},
        {R"({"signals":{"red":{"buzzer": "siren"}}})", LoadError::BadValue, "signals.red.buzzer"},
        {R"({"signals":{"yellow":{"volume": "loud"}}})", LoadError::WrongType, "signals.yellow.volume"},
        {R"({"devices":{"3":{"trigger": 1}}})", LoadError::WrongType, "devices.3.trigger"},
        {R"({"devices":{"3":{"mode": "off"}}})", LoadError::BadValue, "devices.3.mode"},
        {R"({"devices":{"3":{"mode": false}}})", LoadError::WrongType, "devices.3.mode"},
        {R"({"devices":{"3":{"wired": "nc"}}})", LoadError::BadValue, "devices.3.wired"},
        {R"({"devices":{"3":{"wired": 1}}})", LoadError::WrongType, "devices.3.wired"},
        {R"({"fault":{"level": "orange"}})", LoadError::BadValue, "fault.level"},
        {R"({"fault":{"level": 2}})", LoadError::WrongType, "fault.level"},
        {R"({"fault":{"alert": "yes"}})", LoadError::WrongType, "fault.alert"},
        {R"({"service":{"window": "19:30-21:00"}})", LoadError::WrongType, "service.window"},
        {R"({"service":{"window": ["19:30"]}})", LoadError::WrongType, "service.window"},
        {R"({"service":{"window": ["19:30", "x"]}})", LoadError::BadValue, "service.window[1]"},
        {R"({"service":{"window": [1930, "21:00"]}})", LoadError::WrongType, "service.window[0]"},
        {R"({"boost":{"window": "19:30-20:00"}})", LoadError::WrongType, "boost.window"},
        {R"({"boost":{"window": ["19:30"]}})", LoadError::WrongType, "boost.window"},
        {R"({"boost":{"window": ["19:30", "x"]}})", LoadError::BadValue, "boost.window[1]"},
        {R"({"boost":{"window": [1930, "20:00"]}})", LoadError::WrongType, "boost.window[0]"},
        {R"({"boost":{"enabled": "yes"}})", LoadError::WrongType, "boost.enabled"},
        {R"({"boost":{"target_mgl": "high"}})", LoadError::WrongType, "boost.target_mgl"},
        {R"({"devices":{"1":{"boost": 1}}})", LoadError::WrongType, "devices.1.boost"},
    };
    for(const Case& k : cases)
    {
        INFO(k.json);
        Config c;
        const LoadResult r = Load(k.json, c);
        REQUIRE(r.error == k.error);
        REQUIRE(r.path.view() == k.path);
        REQUIRE_FALSE(r.message.empty());
    }
}

TEST_CASE("Only the first problem is reported", "[config]")
{
    Config c;
    const LoadResult r = Load(R"({"sample_period_s":"x","levels":{"blue":{"mgl":"y"}}})", c);
    REQUIRE(r.error == LoadError::WrongType);
    REQUIRE(r.path.view() == "sample_period_s");
}

TEST_CASE("Every validation rule fires on its own key", "[config]")
{
    struct Rule
    {
        const char* path;
        std::function<void(Config&)> mutate;
    };
    const Rule rules[] = {
        {"sample_period_s", [](Config& pC) { pC.samplePeriodS = 4; }},
        {"sample_period_s", [](Config& pC) { pC.samplePeriodS = 61; }},
        {"levels.yellow", [](Config& pC) { pC.ladder.yellow.hysteresis = -0.1f; }},
        {"levels.red", [](Config& pC) { pC.ladder.red.mgl = 0.0f; }},
        {"levels", [](Config& pC) { pC.ladder.blue.mgl = 5.4f; }}, // blue band touches yellow's
        {"levels", [](Config& pC) { pC.ladder.red.mgl = 5.0f; }},  // red band touches yellow's
        {"levels.blue.slope_mgl_per_10min", [](Config& pC) { pC.ladder.blueSlopeMglPer10min = -1.0f; }},
        {"recover_sustain_s", [](Config& pC) { pC.ladder.recoverSustainS = 5; }},
        {"heat", [](Config& pC) { pC.ladder.heatOffC = 28.0f; }},
        {"night", [](Config& pC) { pC.ladder.nightEndMin = pC.ladder.nightStartMin; }},
        {"fault.consecutive_failures", [](Config& pC) { pC.ladder.faultConsecutiveFailures = 0; }},
        {"fault.level", [](Config& pC) { pC.ladder.faultLevel = Level::Blue; }},
        {"pulse.s", [](Config& pC) { pC.ladder.pulseS = 0; }},
        {"correction.scale", [](Config& pC) { pC.correction.scale = 0.4f; }},
        {"correction.scale", [](Config& pC) { pC.correction.scale = 1.6f; }},
        {"correction.offset", [](Config& pC) { pC.correction.offset = -2.5f; }},
        {"correction.offset", [](Config& pC) { pC.correction.offset = 2.5f; }},
        {"salinity_psu", [](Config& pC) { pC.salinityPsu = -1.0f; }},
        {"salinity_psu", [](Config& pC) { pC.salinityPsu = 51.0f; }},
        {"devices.5.wired", [](Config& pC) { pC.devices[4].wiredNc = false; }}, // pulse_off on NO
        {"devices.2.min_response_pct", [](Config& pC) { pC.devices[1].minResponsePct = -1.0f; }},
        {"devices.2.min_response_pct", [](Config& pC) { pC.devices[1].minResponsePct = 51.0f; }},
        {"devices.1.service_s", [](Config& pC) { pC.devices[0].serviceS = 3601; }},
        {"service.window", [](Config& pC) { pC.service.windowStartMin = pC.service.windowEndMin; }},
        {"service.window", [](Config& pC) { pC.service.windowEndMin = 21 * 60 + 30; }}, // into the night lock
        {"service.min_headroom_pct", [](Config& pC) { pC.service.minHeadroomPct = -1.0f; }},
        {"service.min_headroom_pct", [](Config& pC) { pC.service.minHeadroomPct = 21.0f; }},
        {"service.inconclusive_days", [](Config& pC) { pC.service.inconclusiveDays = 0; }},
        {"service.induce_deficit_s", [](Config& pC) { pC.service.induceDeficitS = 601; }},
        {"boost.window", [](Config& pC) { pC.boost.windowStartMin = pC.boost.windowEndMin; }},
        {"boost.target_mgl", [](Config& pC) { pC.boost.targetMgl = 0.5f; }},
        {"boost.target_mgl", [](Config& pC) { pC.boost.targetMgl = 16.0f; }},
        {"service.induce_deficit_device",
         [](Config& pC)
         {
             pC.service.induceDeficitS = 120;
             pC.service.induceDeficitDevice = 0;
         }},
        {"service.induce_deficit_device",
         [](Config& pC)
         {
             pC.service.induceDeficitS = 120;
             pC.service.induceDeficitDevice = 7;
         }},
        {"service",
         [](Config& pC)
         {
             pC.devices[0].serviceS = 3600;
             pC.devices[1].serviceS = 3600;
         }},                                                        // 7500 s + gaps > 90 min
        {"ntfy.topic", [](Config& pC) { pC.ntfy.enabled = true; }}, // topic empty
        {"signals.buzzer_hz", [](Config& pC) { pC.signals.buzzerHz = 100; }},
        {"signals.buzzer_hz", [](Config& pC) { pC.signals.buzzerHz = 9000; }},
        {"signals.led_brightness", [](Config& pC) { pC.signals.ledBrightness = 101; }},
        {"signals.normal.buzzer", [](Config& pC) { pC.signals.normal.buzzer = BuzzerPattern::Chirp; }},
        {"signals.red.volume", [](Config& pC) { pC.signals.red.volume = 4; }},
    };
    for(const Rule& rule : rules)
    {
        INFO(rule.path);
        Config c = Example();
        rule.mutate(c);
        const LoadResult r = Validate(c);
        REQUIRE(r.error == LoadError::Invalid);
        REQUIRE(r.path.view() == rule.path);
        REQUIRE_FALSE(r.message.empty());
    }
}

TEST_CASE("Service window against a night that does not wrap midnight", "[config]")
{
    Config c = Example();
    c.ladder.nightStartMin = 1 * 60; // 01:00 → 06:00
    c.ladder.nightEndMin = 6 * 60;
    REQUIRE(Validate(c).Ok());    // 19:30–21:00 is after the night
    c.service.windowStartMin = 0; // 00:00–00:30 is before it
    c.service.windowEndMin = 30;
    REQUIRE(Validate(c).Ok());
    c.service.windowStartMin = 30; // 00:30–02:00 overlaps it
    c.service.windowEndMin = 2 * 60;
    REQUIRE(Validate(c).path.view() == "service.window");
}

TEST_CASE("Service window with the deficit run and a single serviced device still has to fit", "[config]")
{
    Config c = Example();
    c.service.induceDeficitS = 120;
    c.service.induceDeficitDevice = 5;
    REQUIRE(Validate(c).Ok());
    c.devices[1].serviceS = 0;
    c.devices[2].serviceS = 0;
    c.service.windowStartMin = 20 * 60; // 60 min window
    c.devices[0].serviceS = 3600;       // one device, no gaps: 3600 + 60 + 120 > 60 min
    REQUIRE(Validate(c).path.view() == "service");
    c.devices[0].serviceS = 0; // nothing serviced at all is fine
    REQUIRE(Validate(c).Ok());
}

TEST_CASE("Enabled ntfy with a topic passes; every enum name round-trips", "[config]")
{
    Config c = Example();
    c.ntfy.enabled = true;
    REQUIRE(c.ntfy.topic.assign("reef-secret-topic"));
    c.ntfy.minLevel = Level::Red;
    c.ladder.faultLevel = Level::Red;
    c.ladder.devices[5].trigger = Trigger::Heat;
    c.ladder.devices[5].trigger = Trigger::Red;
    c.ladder.devices[1].trigger = Trigger::Heat;
    c.ladder.faultAlert = false;
    Config back;
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);
    REQUIRE(Dump(c).find("\"min_level\":\"red\"") != std::string::npos);
    REQUIRE(Dump(c).find("\"trigger\":\"heat\"") != std::string::npos);

    // Every signal name survives a round trip.
    const BuzzerPattern buzzers[] = {BuzzerPattern::Off,    BuzzerPattern::Chirp,  BuzzerPattern::Beep,
                                     BuzzerPattern::Double, BuzzerPattern::Triple, BuzzerPattern::Continuous};
    for(std::size_t i = 0; i < 6; ++i)
    {
        Config s = Example();
        s.signals.yellow.buzzer = buzzers[i];
        s.signals.fault.volume = static_cast<uint32_t>(i % 4);
        Config again;
        REQUIRE(Load(Dump(s), again).Ok());
        REQUIRE(again == s);
    }
}

TEST_CASE("write reports 0 when the buffer is too small", "[config]")
{
    char small[64];
    REQUIRE(Write(Defaults(), small) == 0);
    char big[4096];
    const std::size_t n = Write(Defaults(), big);
    REQUIRE(n > 500);
    REQUIRE(big[0] == '{');
    REQUIRE(big[n - 1] == '}');
}

TEST_CASE("load with a base keeps the base's values for keys the document omits", "[config]")
{
    Config base = Example();
    Config out;
    REQUIRE(Load(R"({"sample_period_s": 20})", out, base).Ok());
    REQUIRE(out.samplePeriodS == 20);
    REQUIRE(out.devices[0].name.view() == "small bubbler");
    REQUIRE(out.ladder.devices[4].mode == Mode::PulseOff);
}
