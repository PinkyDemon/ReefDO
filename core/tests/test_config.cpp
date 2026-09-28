#include <functional>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/config.hpp"

using namespace reefdo::config;
using Catch::Matchers::WithinAbs;
using reefdo::FixedString;
using reefdo::ladder::DayAlarm;
using reefdo::ladder::Level;
using reefdo::ladder::Mode;
using reefdo::ladder::Trigger;
using reefdo::slot::Kind;
using reefdo::slot::Wiring;

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
    "1": { "name": "small bubbler",   "slot": { "type": "relay", "channel": 1, "wiring": "NC" }, "trigger": "blue",
           "mode": "on", "test_s": 300, "min_response_pct": 1.0, "boost": true },
    "2": { "name": "extra powerhead", "slot": { "type": "relay", "channel": 2, "wiring": "NC" }, "trigger": "blue",
           "mode": "on", "test_s": 300, "min_response_pct": 0 },
    "3": { "name": "strong air pump", "slot": { "type": "relay", "channel": 3, "wiring": "NC" }, "trigger": "yellow",
           "mode": "on", "test_s": 300, "min_response_pct": 1.0 },
    "4": { "name": "siren",           "slot": { "type": "relay", "channel": 4, "wiring": "NO" }, "trigger": "yellow",
           "mode": "on", "ack_silences": true },
    "5": { "name": "return pump",     "slot": { "type": "relay", "channel": 5, "wiring": "NC" }, "trigger": "blue",
           "mode": "pulse_off" },
    "6": { "name": "unused",          "slot": { "type": "relay", "channel": 6, "wiring": "NO" }, "trigger": "none" }
  },
  "test": { "window": ["19:30", "21:00"], "settle_s": 120, "tail_s": 60, "min_headroom_pct": 3.0,
               "inconclusive_days": 5, "chirp": true,
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

// A plug slot with a valid key; the id and address say which plug.
reefdo::slot::Tuya Plug(const char* pId, const char* pIp = "192.168.1.50", uint32_t pDp = 1)
{
    reefdo::slot::Tuya t;
    t.id.assign(pId);
    t.ip.assign(pIp);
    t.key.assign("k3yK3yk3yK3yk3yK");
    t.dp = pDp;
    return t;
}

std::string Dump(const Config& pC)
{
    std::vector<char> buf(DOC_MAX);
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
    for(std::size_t i = 0; i < reefdo::DEVICES; ++i)
    {
        REQUIRE(d.ladder.devices[i].trigger == Trigger::None);
        const reefdo::slot::Relay* r = d.devices[i].slot.AsRelay(); // device N on relay N, as far as there are relays
        if(i < reefdo::RELAYS) REQUIRE(r->channel == i + 1);
        if(i < reefdo::RELAYS) REQUIRE(r->wiring == Wiring::No);
        if(i >= reefdo::RELAYS) REQUIRE(d.devices[i].slot.GetKind() == Kind::None);
    }
    REQUIRE(d.devices[0].name.view() == "device 1");
    REQUIRE(d.devices[5].name.view() == "device 6");
    REQUIRE_THAT(d.test.noFailAboveMgl, WithinAbs(6.1, 1e-6));
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
    REQUIRE(c.devices[0].slot.AsRelay()->wiring == Wiring::Nc);
    REQUIRE(c.ladder.devices[0].trigger == Trigger::Blue);
    REQUIRE(c.test.devices[0].testS == 300);
    REQUIRE_THAT(c.test.devices[0].minResponsePct, WithinAbs(1.0, 1e-6));
    REQUIRE(c.ladder.devices[3].ackSilences);
    REQUIRE(c.devices[3].slot.AsRelay()->wiring == Wiring::No);
    REQUIRE(c.ladder.devices[4].mode == Mode::PulseOff);
    REQUIRE(c.devices[4].slot.AsRelay()->channel == 5);
    REQUIRE(c.ladder.devices[5].trigger == Trigger::None);
    REQUIRE(c.test.windowStartMin == 19 * 60 + 30);
    REQUIRE(c.test.windowEndMin == 21 * 60);
    REQUIRE(c.test.induceDeficitDevice == 5);
    REQUIRE(c.boost.enabled);
    REQUIRE(c.boost.windowStartMin == 19 * 60 + 30);
    REQUIRE(c.boost.windowEndMin == 20 * 60);
    REQUIRE(c.boost.devices[0]);
    REQUIRE_FALSE(c.boost.devices[1]);
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
        {R"({"test":{"chirp": 1}})", LoadError::WrongType, "test.chirp"},
        {R"({"devices":{"1":{"name": 5}}})", LoadError::WrongType, "devices.1.name"},
        {R"({"devices":{"1":{"name": "a name that is far longer than twenty-four"}}})", LoadError::BadValue,
         "devices.1.name"},
        {R"({"ntfy":{"topic": 7}})", LoadError::WrongType, "ntfy.topic"},
        {R"({"ntfy":{"topic": "0123456789012345678901234567890123456789012345678"}})", LoadError::BadValue,
         "ntfy.topic"},
        {R"({"night":{"start": 2100}})", LoadError::WrongType, "night.start"},
        {R"({"night":{"start": "25:00"}})", LoadError::BadValue, "night.start"},
        {R"({"night":{"day_alarm": "mute"}})", LoadError::BadValue, "night.day_alarm"},
        {R"({"sudden_drop": 3})", LoadError::WrongType, "sudden_drop"},
        {R"({"sudden_drop":{"extend_s": "10 min"}})", LoadError::WrongType, "sudden_drop.extend_s"},
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
        {R"({"test":{"window": "19:30-21:00"}})", LoadError::WrongType, "test.window"},
        {R"({"test":{"window": ["19:30"]}})", LoadError::WrongType, "test.window"},
        {R"({"test":{"window": ["19:30", "x"]}})", LoadError::BadValue, "test.window[1]"},
        {R"({"test":{"window": [1930, "21:00"]}})", LoadError::WrongType, "test.window[0]"},
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
        {"sudden_drop.slope_mgl_per_10min", [](Config& pC) { pC.ladder.suddenSlopeMglPer10min = -1.0f; }},
        {"sudden_drop.slope_mgl_per_10min", [](Config& pC) { pC.ladder.blueSlopeMglPer10min = 3.0f; }},
        {"sudden_drop.extend_s", [](Config& pC) { pC.ladder.suddenExtendS = 3601; }},
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
        {"devices.5.mode", [](Config& pC) { pC.devices[4].slot.AsRelay()->wiring = Wiring::No; }}, // pulse_off on NO
        {"devices.5.mode", [](Config& pC) { pC.devices[4].slot.Reset(Kind::None); }},              // or on nothing
        {"devices.2.min_response_pct", [](Config& pC) { pC.test.devices[1].minResponsePct = -1.0f; }},
        {"devices.2.min_response_pct", [](Config& pC) { pC.test.devices[1].minResponsePct = 51.0f; }},
        {"devices.1.test_s", [](Config& pC) { pC.test.devices[0].testS = 3601; }},
        {"test.window", [](Config& pC) { pC.test.windowStartMin = pC.test.windowEndMin; }},
        {"test.window", [](Config& pC) { pC.test.windowEndMin = 21 * 60 + 30; }}, // into the night lock
        {"test.min_headroom_pct", [](Config& pC) { pC.test.minHeadroomPct = -1.0f; }},
        {"test.min_headroom_pct", [](Config& pC) { pC.test.minHeadroomPct = 21.0f; }},
        {"test.inconclusive_days", [](Config& pC) { pC.test.inconclusiveDays = 0; }},
        {"test.induce_deficit_s", [](Config& pC) { pC.test.induceDeficitS = 601; }},
        {"boost.window", [](Config& pC) { pC.boost.windowStartMin = pC.boost.windowEndMin; }},
        {"boost.target_mgl", [](Config& pC) { pC.boost.targetMgl = 0.5f; }},
        {"boost.target_mgl", [](Config& pC) { pC.boost.targetMgl = 16.0f; }},
        {"test.induce_deficit_device",
         [](Config& pC)
         {
             pC.test.induceDeficitS = 120;
             pC.test.induceDeficitDevice = 0;
         }},
        {"test.induce_deficit_device",
         [](Config& pC)
         {
             pC.test.induceDeficitS = 120;
             pC.test.induceDeficitDevice = static_cast<uint32_t>(reefdo::DEVICES + 1);
         }},
        {"test",
         [](Config& pC)
         {
             pC.test.devices[0].testS = 3600;
             pC.test.devices[1].testS = 3600;
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

TEST_CASE("Test window against a night that does not wrap midnight", "[config]")
{
    Config c = Example();
    c.ladder.nightStartMin = 1 * 60; // 01:00 → 06:00
    c.ladder.nightEndMin = 6 * 60;
    REQUIRE(Validate(c).Ok()); // 19:30–21:00 is after the night
    c.test.windowStartMin = 0; // 00:00–00:30 is before it
    c.test.windowEndMin = 30;
    REQUIRE(Validate(c).Ok());
    c.test.windowStartMin = 30; // 00:30–02:00 overlaps it
    c.test.windowEndMin = 2 * 60;
    REQUIRE(Validate(c).path.view() == "test.window");
}

TEST_CASE("Test window with the deficit run and a single tested device still has to fit", "[config]")
{
    Config c = Example();
    c.test.induceDeficitS = 120;
    c.test.induceDeficitDevice = 5;
    REQUIRE(Validate(c).Ok());
    c.test.devices[1].testS = 0;
    c.test.devices[2].testS = 0;
    c.test.windowStartMin = 20 * 60; // 60 min window
    c.test.devices[0].testS = 3600;  // one device, no gaps: 3600 + 60 + 120 > 60 min
    REQUIRE(Validate(c).path.view() == "test");
    c.test.devices[0].testS = 0; // nothing tested at all is fine
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

TEST_CASE("Time windows: inside, outside, the end is exclusive, and a window can wrap midnight", "[config][windows]")
{
    const TimeWindow day{8 * 60, 20 * 60};
    REQUIRE(day.Contains(8 * 60));
    REQUIRE(day.Contains(12 * 60));
    REQUIRE_FALSE(day.Contains(20 * 60));
    REQUIRE_FALSE(day.Contains(7 * 60 + 59));
    const TimeWindow night{22 * 60, 2 * 60};
    REQUIRE(night.Contains(22 * 60));
    REQUIRE(night.Contains(23 * 60 + 59));
    REQUIRE(night.Contains(0));
    REQUIRE(night.Contains(60));
    REQUIRE_FALSE(night.Contains(2 * 60));
    REQUIRE_FALSE(night.Contains(12 * 60));
}

TEST_CASE("Device windows and test.exclusive load, write and round-trip; a document replaces the whole list",
          "[config][windows]")
{
    Config c = Example();
    REQUIRE(c.devices[5].windows.empty());
    REQUIRE(c.test.exclusive); // the default
    REQUIRE(Load(R"({"devices":{"6":{"windows":[["08:00","20:00"],["22:00","02:00"]]}},
                     "test":{"exclusive":false}})",
                 c, c)
                .Ok());
    REQUIRE(c.devices[5].windows.size() == 2);
    REQUIRE(c.devices[5].windows[0] == TimeWindow{8 * 60, 20 * 60});
    REQUIRE(c.devices[5].windows[1] == TimeWindow{22 * 60, 2 * 60});
    REQUIRE_FALSE(c.test.exclusive);

    const std::string json = Dump(c);
    REQUIRE(json.find(R"("windows":[["08:00","20:00"],["22:00","02:00"]])") != std::string::npos);
    REQUIRE(json.find(R"("exclusive":false)") != std::string::npos);
    Config back;
    REQUIRE(Load(json, back).Ok());
    REQUIRE(back == c);

    REQUIRE(Load(R"({"devices":{"6":{"windows":[["10:00","11:00"]]}}})", c, c).Ok());
    REQUIRE(c.devices[5].windows.size() == 1);
    REQUIRE(Load(R"({"devices":{"6":{"windows":[]}}})", c, c).Ok());
    REQUIRE(c.devices[5].windows.empty());
    REQUIRE_FALSE(back == c); // the windows make the difference
}

TEST_CASE("Malformed windows report the device's windows key", "[config][windows]")
{
    struct Case
    {
        const char* json;
        LoadError error;
    };
    const Case cases[] = {
        {R"({"devices":{"6":{"windows":"08:00-20:00"}}})", LoadError::WrongType},
        {R"({"devices":{"6":{"windows":["08:00","20:00"]}}})", LoadError::WrongType},           // not a list of pairs
        {R"({"devices":{"6":{"windows":[["08:00","20:00","21:00"]]}}})", LoadError::WrongType}, // three times
        {R"({"devices":{"6":{"windows":[[800,"20:00"]]}}})", LoadError::WrongType},
        {R"({"devices":{"6":{"windows":[["08:00",2000]]}}})", LoadError::WrongType},
        {R"({"devices":{"6":{"windows":[["8:00","20:00"]]}}})", LoadError::BadValue},
        {R"({"devices":{"6":{"windows":[["01:00","02:00"],["03:00","04:00"],["05:00","06:00"],
                                         ["07:00","08:00"],["09:00","10:00"]]}}})",
         LoadError::BadValue}, // five
    };
    for(const Case& k : cases)
    {
        CAPTURE(k.json);
        Config c = Example();
        const LoadResult r = Load(k.json, c, c);
        REQUIRE(r.error == k.error);
        REQUIRE(r.path.view() == "devices.6.windows");
    }
    Config c = Example();
    REQUIRE(Load(R"({"devices":{"6":{"windows":[["01:00","02:00"],["03:00","04:00"],["05:00","06:00"],
                                                ["07:00","08:00"]]}}})",
                 c, c)
                .Ok()); // four is the limit
}

TEST_CASE("Window validation: start and end differ, and not on pulse or alarm devices", "[config][windows]")
{
    Config c = Example();
    c.devices[5].windows.push_back({9 * 60, 9 * 60});
    LoadResult r = Validate(c);
    REQUIRE(r.error == LoadError::Invalid);
    REQUIRE(r.path.view() == "devices.6.windows");

    c = Example();
    c.devices[4].windows.push_back({9 * 60, 10 * 60}); // the return pump: pulse_off
    REQUIRE(Validate(c).path.view() == "devices.5.windows");
    c = Example();
    c.devices[3].windows.push_back({9 * 60, 10 * 60}); // the siren: ack_silences
    REQUIRE(Validate(c).path.view() == "devices.4.windows");
    c = Example();
    c.devices[1].windows.push_back({9 * 60, 10 * 60}); // an ordinary device
    REQUIRE(Validate(c).Ok());
}

TEST_CASE("Without an exclusive test the deficit is not run, so it does not have to fit the window",
          "[config][windows]")
{
    Config c = Example();
    c.test.induceDeficitS = 600;
    c.test.induceDeficitDevice = 5;
    c.test.windowStartMin = 20 * 60 + 5; // 55 min = 3300 s
    c.test.devices[0].testS = 1800;      // 1800 + 2 × 300 + 3 × 60 + 2 × 120 = 2820 s; with the deficit 3420 s
    REQUIRE(Validate(c).path.view() == "test");
    c.test.exclusive = false;
    REQUIRE(Validate(c).Ok());
}

TEST_CASE("A document from before the rename (service, service_s) still loads into the test settings", "[config]")
{
    Config c;
    REQUIRE(Load(R"({"service":{"window":["19:00","20:00"],"exclusive":false},"devices":{"2":{"service_s":120}}})", c)
                .Ok());
    REQUIRE(c.test.windowStartMin == 19 * 60);
    REQUIRE_FALSE(c.test.exclusive);
    REQUIRE(c.test.devices[1].testS == 120);
    const std::string json = Dump(c);
    REQUIRE(json.find("service") == std::string::npos); // written back under the new names only
    REQUIRE(json.find(R"("test_s":120)") != std::string::npos);
    REQUIRE(Load(R"({"test":{"settle_s":90},"service":{"settle_s":30}})", c).Ok()); // the new name wins
    REQUIRE(c.test.settleS == 90);
}

TEST_CASE("The largest possible configuration still fits DOC_MAX, the firmware's buffers", "[config]")
{
    Config c = Example();
    for(DeviceSettings& d : c.devices)
    {
        d.name.assign("an extraordinarily long");
        for(uint16_t w = 0; w < WINDOWS; ++w)
            d.windows.push_back({static_cast<uint16_t>(w * 60 + 1), static_cast<uint16_t>(w * 60 + 59)});
        reefdo::slot::Tuya t = Plug("0123456789012345678901234567890", "192.168.100.200", 255); // the longest slot
        t.version = reefdo::tuya::Version::V35;
        d.slot = t;
    }
    c.ntfy.topic.assign("012345678901234567890123456789012345678901234567");
    std::vector<char> buf(DOC_MAX);
    REQUIRE(Write(c, buf) > 0);
}

TEST_CASE("Slots load, write and round-trip: a relay, a plug or nothing per device, also past the relay count",
          "[config][slot]")
{
    Config c = Example();
    REQUIRE(Load(R"({"devices":{"6":{"slot":{"type":"none"}},
                     "7":{"name":"skimmer","slot":{"type":"tuya","ip":"192.168.1.50","id":"bf0123456789abcdefgh",
                                                   "key":"k3yK3yk3yK3yk3yK","version":"3.4","dp":2}},
                     "8":{"slot":{"type":"relay","channel":6,"wiring":"NC"}}}})",
                 c, c)
                .Ok());
    REQUIRE(c.devices[5].slot.GetKind() == Kind::None);
    const reefdo::slot::Tuya* t = c.devices[6].slot.AsTuya();
    REQUIRE(t != nullptr);
    REQUIRE(t->ip.view() == "192.168.1.50");
    REQUIRE(t->key.view() == "k3yK3yk3yK3yk3yK");
    REQUIRE(t->version == reefdo::tuya::Version::V34);
    REQUIRE(t->dp == 2);
    REQUIRE(c.devices[7].slot.AsRelay()->channel == 6);
    REQUIRE(c.devices[7].slot.AsRelay()->wiring == Wiring::Nc);
    const std::string json = Dump(c);
    REQUIRE(json.find(R"("slot":{"type":"relay","channel":6,"wiring":"NC"})") != std::string::npos);
    REQUIRE(json.find(R"("slot":{"type":"none"})") != std::string::npos);
    Config back;
    REQUIRE(Load(json, back).Ok());
    REQUIRE(back == c);

    // The same type keeps what the document leaves out; another type starts from that type's defaults.
    REQUIRE(Load(R"({"devices":{"7":{"slot":{"dp":3}}}})", c, c).Ok());
    REQUIRE(c.devices[6].slot.AsTuya()->id.view() == "bf0123456789abcdefgh");
    REQUIRE(c.devices[6].slot.AsTuya()->dp == 3);
    const LoadResult clash = Load(R"({"devices":{"6":{"slot":{"type":"relay"}}}})", c, c); // relay 1 by default
    REQUIRE(clash.path.view() == "devices.6.slot");
    REQUIRE(clash.message.view().find("device 1") != std::string_view::npos);
    REQUIRE(Load(R"({"devices":{"6":{"slot":{"channel":2}}}})", c, c).Ok()); // no type and no slot: nothing to fill
    REQUIRE(c.devices[5].slot.GetKind() == Kind::None);
    REQUIRE(Load(R"({"devices":{"6":{"wired":"NC"}}})", c, c).Ok()); // 1.0 wiring for a device without a relay
    REQUIRE(c.devices[5].slot.GetKind() == Kind::None);
}

TEST_CASE("A plug's local key never reaches a browser; the placeholder keeps the stored key", "[config][slot]")
{
    Config c = Example();
    c.devices[6].slot = Plug("bf0123456789abcdefgh");
    std::vector<char> buf(DOC_MAX);
    const std::string redacted(buf.data(), Write(c, buf, true));
    REQUIRE(redacted.find("k3yK3yk3yK3yk3yK") == std::string::npos);
    REQUIRE(redacted.find(R"("key":"********")") != std::string::npos);
    Config back = c;
    REQUIRE(Load(redacted, back, c).Ok());
    REQUIRE(back == c);
    const std::string full(buf.data(), Write(c, buf));
    REQUIRE(full.find(R"("key":"k3yK3yk3yK3yk3yK")") != std::string::npos);
    Config fromNvs;
    REQUIRE(Load(full, fromNvs).Ok());
    REQUIRE(fromNvs == c);
    REQUIRE(Load(R"({"devices":{"7":{"slot":{"id":"********"}}}})", back, c).Ok()); // not secret: taken as typed
    REQUIRE(back.devices[6].slot.AsTuya()->id.view() == "********");

    Config blank = c; // an empty key stays empty, masked or not
    blank.devices[6].slot.Reset(Kind::Tuya);
    const std::string empty(buf.data(), Write(blank, buf, true));
    REQUIRE(empty.find(R"("key":"")") != std::string::npos);
}

TEST_CASE("Malformed slots report the slot's key", "[config][slot]")
{
    struct Case
    {
        const char* json;
        LoadError error;
        const char* path;
    };
    const Case cases[] = {
        {R"({"devices":{"6":{"slot":5}}})", LoadError::WrongType, "devices.6.slot"},
        {R"({"devices":{"6":{"slot":{"type":"lamp"}}}})", LoadError::BadValue, "devices.6.slot.type"},
        {R"({"devices":{"6":{"slot":{"type":5}}}})", LoadError::BadValue, "devices.6.slot.type"},
        {R"({"devices":{"6":{"slot":{"channel":"two"}}}})", LoadError::WrongType, "devices.6.slot.channel"},
        {R"({"devices":{"6":{"slot":{"wiring":"nc"}}}})", LoadError::BadValue, "devices.6.slot.wiring"},
        {R"({"devices":{"6":{"slot":{"wiring":1}}}})", LoadError::WrongType, "devices.6.slot.wiring"},
        {R"({"devices":{"6":{"slot":{"type":"tuya","key":5}}}})", LoadError::WrongType, "devices.6.slot.key"},
        {R"({"devices":{"6":{"slot":{"type":"tuya","id":"0123456789012345678901234567890123"}}}})", LoadError::BadValue,
         "devices.6.slot.id"},
    };
    for(const Case& k : cases)
    {
        CAPTURE(k.json);
        Config c = Example();
        const LoadResult r = Load(k.json, c, c);
        REQUIRE(r.error == k.error);
        REQUIRE(r.path.view() == k.path);
    }
}

TEST_CASE("Slot parameters are checked against their fields: ranges, lengths, addresses, names", "[config][slot]")
{
    struct Rule
    {
        const char* path;
        const char* message; // a fragment of it
        std::function<void(Config&)> mutate;
    };
    const auto relay = [](Config& pC) -> reefdo::slot::Relay& { return *pC.devices[5].slot.AsRelay(); };
    const auto plug = [](Config& pC) -> reefdo::slot::Tuya&
    {
        pC.devices[6].slot = Plug("bf0123456789abcdefgh");
        return *pC.devices[6].slot.AsTuya();
    };
    const Rule rules[] = {
        {"devices.6.slot.channel", "must be 1..6", [&relay](Config& pC) { relay(pC).channel = 0; }},
        {"devices.6.slot.channel", "must be 1..6", [&relay](Config& pC) { relay(pC).channel = 7; }},
        {"devices.6.slot.wiring", "unknown value", [&relay](Config& pC) { relay(pC).wiring = static_cast<Wiring>(5); }},
        {"devices.7.slot.id", "must be 1..31 characters", [&plug](Config& pC) { plug(pC).id.clear(); }},
        {"devices.7.slot.ip", "must be 7..15 characters", [&plug](Config& pC) { plug(pC).ip.assign("1.2.3"); }},
        {"devices.7.slot.ip", "must be 7..15 characters",
         [&plug](Config& pC) { plug(pC).ip.assign("192.168.100.2000"); }},
        {"devices.7.slot.key", "must be 16 characters",
         [&plug](Config& pC) { plug(pC).key.assign("0123456789abcdef0"); }},
        {"devices.7.slot.ip", "an address like", [&plug](Config& pC) { plug(pC).ip.assign("192.168.1.500"); }},
        {"devices.7.slot.key", "must be 16 characters", [&plug](Config& pC) { plug(pC).key.assign("short"); }},
        {"devices.7.slot.dp", "must be 1..255", [&plug](Config& pC) { plug(pC).dp = 0; }},
        {"devices.7.slot.dp", "must be 1..255", [&plug](Config& pC) { plug(pC).dp = 256; }},
        {"devices.7.slot.version", "unknown value",
         [&plug](Config& pC) { plug(pC).version = static_cast<reefdo::tuya::Version>(9); }},
        {"devices.7.slot.id", "characters", // the first problem is reported
         [&plug](Config& pC)
         {
             plug(pC).id.clear();
             pC.devices[6].slot.AsTuya()->ip.assign("x");
         }},
    };
    for(const Rule& rule : rules)
    {
        CAPTURE(rule.path, rule.message);
        Config c = Example();
        rule.mutate(c);
        const LoadResult r = Validate(c);
        REQUIRE(r.error == LoadError::Invalid);
        REQUIRE(r.path.view() == rule.path);
        REQUIRE(r.message.view().find(rule.message) != std::string_view::npos);
    }

    Config odd = Example(); // what Validate() refuses still writes without reading past the names
    odd.devices[5].slot.AsRelay()->wiring = static_cast<Wiring>(5);
    REQUIRE(Dump(odd).find(R"("wiring":"?")") != std::string::npos);
}

TEST_CASE("Outputs are exclusive (Slot == is IsSame), and a device that is used needs one", "[config][slot]")
{
    struct Rule
    {
        const char* path;
        const char* message; // a fragment of it
        std::function<void(Config&)> mutate;
    };
    const Rule rules[] = {
        {"devices.7.slot", "device 3", [](Config& pC) { pC.devices[6].slot = reefdo::slot::Relay(3, Wiring::No); }},
        {"devices.8.slot", "device 7",
         [](Config& pC)
         {
             pC.devices[6].slot = Plug("plug-a", "192.168.1.50", 1);
             pC.devices[7].slot = Plug("plug-a", "192.168.1.51", 1); // the same plug at a new address
         }},
        {"devices.8.slot", "device 7",
         [](Config& pC)
         {
             pC.devices[6].slot = Plug("plug-a", "192.168.1.50", 2);
             pC.devices[7].slot = Plug("plug-b", "192.168.1.50", 2); // the same address, a new id
         }},
        {"devices.7.slot", "needs a relay", [](Config& pC) { pC.ladder.devices[6].trigger = Trigger::Blue; }},
        {"devices.7.slot", "needs a relay", [](Config& pC) { pC.test.devices[6].testS = 60; }},
        {"devices.7.slot", "needs a relay", [](Config& pC) { pC.boost.devices[6] = true; }},
        {"devices.7.slot", "needs a relay", [](Config& pC) { pC.devices[6].windows.push_back({600, 660}); }},
        {"test.induce_deficit_device", "no relay or plug",
         [](Config& pC)
         {
             pC.test.induceDeficitS = 60;
             pC.test.induceDeficitDevice = 7;
         }},
        {"test.no_fail_above_mgl", "0..15", [](Config& pC) { pC.test.noFailAboveMgl = -0.1f; }},
        {"test.no_fail_above_mgl", "0..15", [](Config& pC) { pC.test.noFailAboveMgl = 15.1f; }},
    };
    for(const Rule& rule : rules)
    {
        CAPTURE(rule.path, rule.message);
        Config c = Example();
        rule.mutate(c);
        const LoadResult r = Validate(c);
        REQUIRE(r.error == LoadError::Invalid);
        REQUIRE(r.path.view() == rule.path);
        REQUIRE(r.message.view().find(rule.message) != std::string_view::npos);
    }

    // Allowed: two outlets of one strip, another plug, a freed relay taken by another device.
    Config ok = Example();
    ok.devices[6].slot = Plug("strip", "192.168.1.60", 1);
    ok.devices[7].slot = Plug("strip", "192.168.1.60", 2);
    REQUIRE(Validate(ok).Ok());
    ok.devices[7].slot = Plug("other", "192.168.1.61", 1);
    REQUIRE(Validate(ok).Ok());
    ok.devices[5].slot.Reset(Kind::None);
    ok.devices[7].slot = reefdo::slot::Relay(6, Wiring::Nc);
    ok.ladder.devices[7].trigger = Trigger::Yellow;
    REQUIRE(Validate(ok).Ok());
}

TEST_CASE("night.day_alarm and sudden_drop load and round-trip; the defaults suspect a sudden drop", "[config]")
{
    Config c;
    REQUIRE(c.ladder.dayAlarm == DayAlarm::Sound);
    REQUIRE(c.ladder.suddenSlopeMglPer10min > 0.0f);
    REQUIRE(
        Load(R"({"night":{"day_alarm":"auto_ack"},"sudden_drop":{"slope_mgl_per_10min":4.5,"extend_s":900}})", c).Ok());
    REQUIRE(c.ladder.dayAlarm == DayAlarm::AutoAck);
    REQUIRE_THAT(c.ladder.suddenSlopeMglPer10min, WithinAbs(4.5, 1e-6));
    REQUIRE(c.ladder.suddenExtendS == 900);
    REQUIRE(Dump(c).find(R"("day_alarm":"auto_ack")") != std::string::npos);
    Config back;
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);

    c.ladder.dayAlarm = DayAlarm::Suppress;
    c.ladder.suddenSlopeMglPer10min = 0.0f; // off: the blue slope has nothing to be steeper than
    c.ladder.blueSlopeMglPer10min = 5.0f;
    REQUIRE(Validate(c).Ok());
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);
    REQUIRE(Dump(c).find(R"("day_alarm":"suppress")") != std::string::npos);
}

TEST_CASE("test.no_fail_above_mgl loads and round-trips", "[config]")
{
    Config c;
    REQUIRE(Load(R"({"test":{"no_fail_above_mgl":6.4}})", c).Ok());
    REQUIRE_THAT(c.test.noFailAboveMgl, WithinAbs(6.4, 1e-6));
    REQUIRE(Dump(c).find(R"("no_fail_above_mgl":6.4)") != std::string::npos);
    Config back;
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);
}

TEST_CASE("The document stored on a 1.0 board loads unchanged: relays 1..6, the old service keys, still valid",
          "[config][registry]")
{
    // GET /api/config of a board running 1.0.0-rc3, verbatim.
    const char* const stored =
        R"({"schema":1,"sample_period_s":10,"levels":{"blue":{"mgl":5.25,"hysteresis":0.25,"dwell_s":300,)"
        R"("slope_mgl_per_10min":1},"yellow":{"mgl":4.75,"hysteresis":0.24,"dwell_s":180},"red":{"mgl":4.25,)"
        R"("hysteresis":0.25,"dwell_s":120}},"recover_sustain_s":300,"night":{"start":"21:00","end":"08:00",)"
        R"("lock":false,"unknown_time_is_night":true},"pulse":{"s":10,"min_interval_s":1800},"heat":{"on_c":28,)"
        R"("off_c":27.5},"fault":{"consecutive_failures":5,"stuck_minutes":30,"level":"red","alert":true},)"
        R"("ack_silence_s":1800,"escalate_if_failed":true,"devices":{"1":{"name":"air pump","wired":"NC",)"
        R"("trigger":"yellow","mode":"on","ack_silences":false,"service_s":300,"min_response_pct":0,)"
        R"("boost":false},"2":{"name":"powerhead","wired":"NO","trigger":"blue","mode":"on","ack_silences":false,)"
        R"("service_s":0,"min_response_pct":0,"boost":false},"3":{"name":"device 3","wired":"NO","trigger":"none",)"
        R"("mode":"on","ack_silences":false,"service_s":0,"min_response_pct":0,"boost":false},"4":{"name":)"
        R"("device 4","wired":"NO","trigger":"none","mode":"on","ack_silences":false,"service_s":0,)"
        R"("min_response_pct":0,"boost":false},"5":{"name":"device 5","wired":"NO","trigger":"none","mode":"on",)"
        R"("ack_silences":false,"service_s":0,"min_response_pct":0,"boost":false},"6":{"name":"device 6",)"
        R"("wired":"NO","trigger":"none","mode":"on","ack_silences":false,"service_s":0,"min_response_pct":0,)"
        R"("boost":false}},"service":{"window":["20:30","21:00"],"settle_s":120,"tail_s":60,"min_headroom_pct":3,)"
        R"("inconclusive_days":5,"chirp":true,"induce_deficit_s":0,"induce_deficit_device":0},"correction":)"
        R"({"scale":0.815,"offset":0},"salinity_psu":35,"allow_zero_cal":false,"boost":{"enabled":false,)"
        R"("window":["19:30","20:00"],"target_mgl":6.5},"signals":{"buzzer_hz":2400,"led_brightness":1,"normal":)"
        R"({"buzzer":"off","volume":0},"blue":{"buzzer":"off","volume":0},"yellow":{"buzzer":"off","volume":0},)"
        R"("red":{"buzzer":"triple","volume":3},"fault":{"buzzer":"triple","volume":3}},"ntfy":{"enabled":false,)"
        R"("topic":"","min_level":"blue"}})";
    Config c;
    const LoadResult r = Load(stored, c);
    INFO(r.path.view() << ": " << r.message.view());
    REQUIRE(r.Ok());
    for(std::size_t i = 0; i < reefdo::RELAYS; ++i)
        REQUIRE(c.devices[i].slot.AsRelay()->channel == i + 1); // every device where it was wired
    for(std::size_t i = reefdo::RELAYS; i < reefdo::DEVICES; ++i)
        REQUIRE(c.devices[i].slot.GetKind() == Kind::None); // the new ones are free
    REQUIRE(c.devices[0].name.view() == "air pump");
    REQUIRE(c.devices[0].slot.AsRelay()->wiring == Wiring::Nc); // its 1.0 "wired"
    REQUIRE(c.devices[1].slot.AsRelay()->wiring == Wiring::No);
    REQUIRE(c.ladder.devices[0].trigger == Trigger::Yellow);
    REQUIRE(c.test.devices[0].testS == 300);
    REQUIRE(c.test.windowStartMin == 20 * 60 + 30);
    REQUIRE(c.test.exclusive); // new keys take their defaults
    REQUIRE_THAT(c.test.noFailAboveMgl, WithinAbs(6.1, 1e-6));
    REQUIRE_THAT(c.correction.scale, WithinAbs(0.815, 1e-6));
    Config back; // and what 1.1 writes back loads to the same thing
    REQUIRE(Load(Dump(c), back).Ok());
    REQUIRE(back == c);
}
