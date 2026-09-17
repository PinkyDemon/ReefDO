#include "reefdo/config.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

namespace reefdo::config
{

using ladder::Level;
using ladder::Mode;
using ladder::Trigger;

namespace
{

// Enum <-> name tables. Reading searches by name; writing indexes by enum value (contiguous from 0).
template <class E>
struct Named
{
    const char* name;
    E value;
};
constexpr Named<Trigger> TRIGGERS[] = {{"none", Trigger::None},
                                       {"blue", Trigger::Blue},
                                       {"yellow", Trigger::Yellow},
                                       {"red", Trigger::Red},
                                       {"heat", Trigger::Heat}};
constexpr Named<Mode> MODES[] = {{"on", Mode::On}, {"pulse_off", Mode::PulseOff}};
constexpr Named<Level> LEVELS[] = {{"normal", Level::Normal},
                                   {"blue", Level::Blue},
                                   {"yellow", Level::Yellow},
                                   {"red", Level::Red}};
constexpr Named<bool> WIRED[] = {{"NO", false}, {"NC", true}};

constexpr const char* TRIGGER_NAMES[] = {"none", "blue", "yellow", "red", "heat"};
constexpr const char* MODE_NAMES[] = {"on", "pulse_off"};
constexpr const char* LEVEL_NAMES[] = {"normal", "blue", "yellow", "red"};
constexpr const char* WIRED_NAMES[] = {"NO", "NC"};
constexpr const char* LEVEL_KEYS[] = {"blue", "yellow", "red"};
constexpr Named<BuzzerPattern> BUZZERS[] = {
    {"off", BuzzerPattern::Off},       {"chirp", BuzzerPattern::Chirp},   {"beep", BuzzerPattern::Beep},
    {"double", BuzzerPattern::Double}, {"triple", BuzzerPattern::Triple}, {"continuous", BuzzerPattern::Continuous}};
constexpr const char* BUZZER_NAMES[] = {"off", "chirp", "beep", "double", "triple", "continuous"};
constexpr const char* SIGNAL_KEYS[] = {"normal", "blue", "yellow", "red", "fault"};

Signal* SignalSlots(SignalsConfig& pS, std::size_t pI)
{
    Signal* slots[] = {&pS.normal, &pS.blue, &pS.yellow, &pS.red, &pS.fault};
    return slots[pI];
}

// ArduinoJson's float parser is off by an ulp (0.15 → one ulp low); snapping to four decimals keeps a
// config that only went through the UI equal to the stored one.
float Quantise(double pV)
{
    return static_cast<float>(std::round(pV * 10000.0) / 10000.0);
}

// Composes "prefix.key" into a scratch buffer; fail() copies it before the next call.
class Path
{
public:
    const char* of(const char* pPrefix, const char* pKey)
    {
        std::snprintf(mBuf, sizeof mBuf, "%s.%s", pPrefix, pKey);
        return mBuf;
    }

private:
    char mBuf[64]{};
};

// Reads keys out of the document. Every read is a no-op when the key is absent; the first problem is
// recorded in the result and later reads keep going (the caller checks once at the end).
class Reader
{
public:
    explicit Reader(LoadResult& pRes)
        : mRes(pRes)
    {
    }

    void Fail(LoadError pE, const char* pPath, const char* pMsg)
    {
        if(mRes.error != LoadError::None) return;
        mRes.error = pE;
        mRes.path.assign(pPath);
        mRes.message.assign(pMsg);
    }

    JsonObjectConst Object(JsonObjectConst pO, const char* pKey, const char* pPath)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return {};
        if(!v.is<JsonObjectConst>())
        {
            Fail(LoadError::WrongType, pPath, "expected an object");
            return {};
        }
        return v.as<JsonObjectConst>();
    }

    void Number(JsonObjectConst pO, const char* pKey, const char* pPath, float& pOut)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<float>())
        {
            Fail(LoadError::WrongType, pPath, "expected a number");
            return;
        }
        pOut = Quantise(v.as<double>());
    }

    void Number(JsonObjectConst pO, const char* pKey, const char* pPath, uint32_t& pOut)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<uint32_t>())
        {
            Fail(LoadError::WrongType, pPath, "expected a non-negative integer");
            return;
        }
        pOut = v.as<uint32_t>();
    }

    void Flag(JsonObjectConst pO, const char* pKey, const char* pPath, bool& pOut)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<bool>())
        {
            Fail(LoadError::WrongType, pPath, "expected true or false");
            return;
        }
        pOut = v.as<bool>();
    }

    template <std::size_t N>
    void Text(JsonObjectConst pO, const char* pKey, const char* pPath, FixedString<N>& pOut)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<const char*>())
        {
            Fail(LoadError::WrongType, pPath, "expected a string");
            return;
        }
        if(!pOut.assign(v.as<const char*>())) Fail(LoadError::BadValue, pPath, "string too long");
    }

    void Hhmm(JsonVariantConst pV, const char* pPath, uint16_t& pOut)
    {
        if(pV.isNull()) return;
        if(!pV.is<const char*>())
        {
            Fail(LoadError::WrongType, pPath, "expected \"HH:MM\"");
            return;
        }
        const std::optional<uint16_t> m = ParseHhmm(pV.as<const char*>());
        if(!m.has_value())
        {
            Fail(LoadError::BadValue, pPath, "expected \"HH:MM\"");
            return;
        }
        pOut = *m;
    }

    template <class E, std::size_t N>
    void Enumeration(JsonObjectConst pO, const char* pKey, const char* pPath, E& pOut, const Named<E> (&pTable)[N])
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<const char*>())
        {
            Fail(LoadError::WrongType, pPath, "expected a name");
            return;
        }
        const char* s = v.as<const char*>();
        for(const Named<E>& n : pTable)
        {
            if(std::strcmp(n.name, s) == 0)
            {
                pOut = n.value;
                return;
            }
        }
        Fail(LoadError::BadValue, pPath, "unknown name");
    }

private:
    LoadResult& mRes;
};

void ReadLevel(Reader& pR, Path& p_, JsonObjectConst pO, const char* pPrefix, ladder::LevelConfig& pLc)
{
    pR.Number(pO, "mgl", p_.of(pPrefix, "mgl"), pLc.mgl);
    pR.Number(pO, "hysteresis", p_.of(pPrefix, "hysteresis"), pLc.hysteresis);
    pR.Number(pO, "dwell_s", p_.of(pPrefix, "dwell_s"), pLc.dwellS);
}

LoadResult Invalid(const char* pPath, const char* pMsg)
{
    LoadResult r;
    r.error = LoadError::Invalid;
    r.path.assign(pPath);
    r.message.assign(pMsg);
    return r;
}

void WriteLevel(JsonObject pO, const ladder::LevelConfig& pLc)
{
    pO["mgl"] = pLc.mgl;
    pO["hysteresis"] = pLc.hysteresis;
    pO["dwell_s"] = pLc.dwellS;
}

} // namespace

const Signal& SignalsConfig::of(ladder::Level pEffective, bool pIsFault) const
{
    if(pIsFault) return fault;
    if(pEffective == ladder::Level::Blue) return blue;
    if(pEffective == ladder::Level::Yellow) return yellow;
    if(pEffective == ladder::Level::Red) return red;
    return normal;
}

Config Defaults()
{
    Config c;
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof name, "device %u", static_cast<unsigned>(i + 1));
        c.devices[i].name.assign(name);
    }
    return c;
}

std::optional<uint16_t> ParseHhmm(std::string_view pS)
{
    if(pS.size() != 5 || pS[2] != ':') return std::nullopt;
    for(const std::size_t i : {0u, 1u, 3u, 4u})
    {
        if(pS[i] < '0' || pS[i] > '9') return std::nullopt;
    }
    const unsigned h = static_cast<unsigned>((pS[0] - '0') * 10 + (pS[1] - '0'));
    const unsigned m = static_cast<unsigned>((pS[3] - '0') * 10 + (pS[4] - '0'));
    if(h > 23 || m > 59) return std::nullopt;
    return static_cast<uint16_t>(h * 60 + m);
}

void FormatHhmm(uint16_t pMinutes, std::span<char, 6> pOut)
{
    std::snprintf(pOut.data(), pOut.size(), "%02u:%02u", static_cast<unsigned>(pMinutes / 60) % 24u,
                  static_cast<unsigned>(pMinutes % 60));
}

LoadResult Load(std::string_view pJson, Config& pOut)
{
    return Load(pJson, pOut, Defaults());
}

LoadResult Load(std::string_view pJson, Config& pOut, const Config& pBase)
{
    LoadResult res;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, pJson.data(), pJson.size());
    if(err)
    {
        res.error = LoadError::InvalidJson;
        res.message.assign(err.c_str());
        return res;
    }
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    if(root.isNull())
    {
        res.error = LoadError::NotAnObject;
        res.message.assign("the document must be a JSON object");
        return res;
    }

    Config cfg = pBase;
    Reader r(res);
    Path p;

    r.Number(root, "schema", "schema", cfg.schema);
    r.Number(root, "sample_period_s", "sample_period_s", cfg.samplePeriodS);
    r.Number(root, "recover_sustain_s", "recover_sustain_s", cfg.ladder.recoverSustainS);
    r.Number(root, "ack_silence_s", "ack_silence_s", cfg.ladder.ackSilenceS);
    r.Number(root, "salinity_psu", "salinity_psu", cfg.salinityPsu);
    r.Flag(root, "allow_zero_cal", "allow_zero_cal", cfg.allowZeroCal);
    r.Flag(root, "escalate_if_failed", "escalate_if_failed", cfg.ladder.escalateIfFailed);

    const JsonObjectConst levels = r.Object(root, "levels", "levels");
    const JsonObjectConst blue = r.Object(levels, "blue", "levels.blue");
    ReadLevel(r, p, blue, "levels.blue", cfg.ladder.blue);
    r.Number(blue, "slope_mgl_per_10min", "levels.blue.slope_mgl_per_10min", cfg.ladder.blueSlopeMglPer10min);
    ReadLevel(r, p, r.Object(levels, "yellow", "levels.yellow"), "levels.yellow", cfg.ladder.yellow);
    ReadLevel(r, p, r.Object(levels, "red", "levels.red"), "levels.red", cfg.ladder.red);

    const JsonObjectConst night = r.Object(root, "night", "night");
    r.Hhmm(night["start"], "night.start", cfg.ladder.nightStartMin);
    r.Hhmm(night["end"], "night.end", cfg.ladder.nightEndMin);
    r.Flag(night, "lock", "night.lock", cfg.ladder.nightLock);
    r.Flag(night, "unknown_time_is_night", "night.unknown_time_is_night", cfg.ladder.unknownTimeIsNight);

    const JsonObjectConst pulse = r.Object(root, "pulse", "pulse");
    r.Number(pulse, "s", "pulse.s", cfg.ladder.pulseS);
    r.Number(pulse, "min_interval_s", "pulse.min_interval_s", cfg.ladder.pulseMinIntervalS);

    const JsonObjectConst heat = r.Object(root, "heat", "heat");
    r.Number(heat, "on_c", "heat.on_c", cfg.ladder.heatOnC);
    r.Number(heat, "off_c", "heat.off_c", cfg.ladder.heatOffC);

    const JsonObjectConst fault = r.Object(root, "fault", "fault");
    r.Number(fault, "consecutive_failures", "fault.consecutive_failures", cfg.ladder.faultConsecutiveFailures);
    r.Number(fault, "stuck_minutes", "fault.stuck_minutes", cfg.stuckMinutes);
    r.Enumeration(fault, "level", "fault.level", cfg.ladder.faultLevel, LEVELS);
    r.Flag(fault, "alert", "fault.alert", cfg.ladder.faultAlert);

    const JsonObjectConst correction = r.Object(root, "correction", "correction");
    r.Number(correction, "scale", "correction.scale", cfg.correction.scale);
    r.Number(correction, "offset", "correction.offset", cfg.correction.offset);

    const JsonObjectConst devices = r.Object(root, "devices", "devices");
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const char key[2] = {static_cast<char>('1' + i), '\0'};
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "devices.%s", key);
        const JsonObjectConst d = r.Object(devices, key, prefix);
        r.Text(d, "name", p.of(prefix, "name"), cfg.devices[i].name);
        r.Enumeration(d, "wired", p.of(prefix, "wired"), cfg.devices[i].wiredNc, WIRED);
        r.Enumeration(d, "trigger", p.of(prefix, "trigger"), cfg.ladder.devices[i].trigger, TRIGGERS);
        r.Enumeration(d, "mode", p.of(prefix, "mode"), cfg.ladder.devices[i].mode, MODES);
        r.Flag(d, "ack_silences", p.of(prefix, "ack_silences"), cfg.ladder.devices[i].ackSilences);
        r.Number(d, "service_s", p.of(prefix, "service_s"), cfg.devices[i].serviceS);
        r.Number(d, "min_response_pct", p.of(prefix, "min_response_pct"), cfg.devices[i].minResponsePct);
        r.Flag(d, "boost", p.of(prefix, "boost"), cfg.boost.devices[i]);
    }

    const JsonObjectConst service = r.Object(root, "service", "service");
    const JsonVariantConst window = service["window"];
    if(!window.isNull())
    {
        if(!window.is<JsonArrayConst>() || window.as<JsonArrayConst>().size() != 2)
        {
            r.Fail(LoadError::WrongType, "service.window", "expected [\"HH:MM\", \"HH:MM\"]");
        }
        else
        {
            r.Hhmm(window[0], "service.window[0]", cfg.service.windowStartMin);
            r.Hhmm(window[1], "service.window[1]", cfg.service.windowEndMin);
        }
    }
    r.Number(service, "settle_s", "service.settle_s", cfg.service.settleS);
    r.Number(service, "tail_s", "service.tail_s", cfg.service.tailS);
    r.Number(service, "min_headroom_pct", "service.min_headroom_pct", cfg.service.minHeadroomPct);
    r.Number(service, "inconclusive_days", "service.inconclusive_days", cfg.service.inconclusiveDays);
    r.Flag(service, "chirp", "service.chirp", cfg.service.chirp);
    r.Number(service, "induce_deficit_s", "service.induce_deficit_s", cfg.service.induceDeficitS);
    r.Number(service, "induce_deficit_device", "service.induce_deficit_device", cfg.service.induceDeficitDevice);

    const JsonObjectConst boost = r.Object(root, "boost", "boost");
    r.Flag(boost, "enabled", "boost.enabled", cfg.boost.enabled);
    const JsonVariantConst bwindow = boost["window"];
    if(!bwindow.isNull())
    {
        if(!bwindow.is<JsonArrayConst>() || bwindow.as<JsonArrayConst>().size() != 2)
        {
            r.Fail(LoadError::WrongType, "boost.window", "expected [\"HH:MM\", \"HH:MM\"]");
        }
        else
        {
            r.Hhmm(bwindow[0], "boost.window[0]", cfg.boost.windowStartMin);
            r.Hhmm(bwindow[1], "boost.window[1]", cfg.boost.windowEndMin);
        }
    }
    r.Number(boost, "target_mgl", "boost.target_mgl", cfg.boost.targetMgl);

    const JsonObjectConst signals = r.Object(root, "signals", "signals");
    r.Number(signals, "buzzer_hz", "signals.buzzer_hz", cfg.signals.buzzerHz);
    r.Number(signals, "led_brightness", "signals.led_brightness", cfg.signals.ledBrightness);
    for(std::size_t i = 0; i < 5; ++i)
    {
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "signals.%s", SIGNAL_KEYS[i]);
        const JsonObjectConst o = r.Object(signals, SIGNAL_KEYS[i], prefix);
        Signal& sg = *SignalSlots(cfg.signals, i);
        r.Enumeration(o, "buzzer", p.of(prefix, "buzzer"), sg.buzzer, BUZZERS);
        r.Number(o, "volume", p.of(prefix, "volume"), sg.volume);
    }

    const JsonObjectConst ntfy = r.Object(root, "ntfy", "ntfy");
    r.Flag(ntfy, "enabled", "ntfy.enabled", cfg.ntfy.enabled);
    r.Text(ntfy, "topic", "ntfy.topic", cfg.ntfy.topic);
    r.Enumeration(ntfy, "min_level", "ntfy.min_level", cfg.ntfy.minLevel, LEVELS);

    if(!res.Ok()) return res;
    const LoadResult v = Validate(cfg);
    if(!v.Ok()) return v;
    pOut = cfg;
    return res;
}

LoadResult Validate(const Config& pCfg)
{
    const ladder::Config& l = pCfg.ladder;
    Path p;

    if(pCfg.samplePeriodS < 5 || pCfg.samplePeriodS > 60) return Invalid("sample_period_s", "must be 5..60");

    const ladder::LevelConfig* const levels[3] = {&l.blue, &l.yellow, &l.red};
    for(std::size_t i = 0; i < 3; ++i)
    {
        if(levels[i]->hysteresis < 0.0f) return Invalid(p.of("levels", LEVEL_KEYS[i]), "hysteresis must be >= 0");
        if(levels[i]->mgl <= 0.0f) return Invalid(p.of("levels", LEVEL_KEYS[i]), "mgl must be > 0");
    }
    if(l.blue.mgl - l.blue.hysteresis <= l.yellow.mgl + l.yellow.hysteresis)
        return Invalid("levels", "the blue and yellow bands overlap");
    if(l.yellow.mgl - l.yellow.hysteresis <= l.red.mgl + l.red.hysteresis)
        return Invalid("levels", "the yellow and red bands overlap");
    if(l.blueSlopeMglPer10min < 0.0f) return Invalid("levels.blue.slope_mgl_per_10min", "must be >= 0");
    if(l.recoverSustainS < 10) return Invalid("recover_sustain_s", "must be >= 10");
    if(l.heatOnC <= l.heatOffC) return Invalid("heat", "on_c must be above off_c");
    if(l.nightStartMin == l.nightEndMin) return Invalid("night", "start and end must differ");
    if(l.faultConsecutiveFailures < 1) return Invalid("fault.consecutive_failures", "must be >= 1");
    if(l.faultLevel < Level::Yellow) return Invalid("fault.level", "FAULT must act as yellow or red");
    if(l.pulseS < 1) return Invalid("pulse.s", "must be >= 1");
    if(pCfg.correction.scale < 0.5f || pCfg.correction.scale > 1.5f)
        return Invalid("correction.scale", "must be 0.5..1.5");
    if(pCfg.correction.offset < -2.0f || pCfg.correction.offset > 2.0f)
        return Invalid("correction.offset", "must be -2..2");
    if(pCfg.salinityPsu < 0.0f || pCfg.salinityPsu > 50.0f) return Invalid("salinity_psu", "must be 0..50");

    uint32_t serviced = 0;
    uint32_t serviceTotalS = 0;
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "devices.%u", static_cast<unsigned>(i + 1));
        if(l.devices[i].mode == Mode::PulseOff && !pCfg.devices[i].wiredNc)
            return Invalid(p.of(prefix, "wired"),
                           "pulse_off requires NC wiring, or a dead controller cuts this device");
        if(pCfg.devices[i].minResponsePct < 0.0f || pCfg.devices[i].minResponsePct > 50.0f)
            return Invalid(p.of(prefix, "min_response_pct"), "must be 0..50");
        if(pCfg.devices[i].serviceS > 3600) return Invalid(p.of(prefix, "service_s"), "must be <= 3600");
        serviced += static_cast<uint32_t>(pCfg.devices[i].serviceS > 0);
        serviceTotalS += pCfg.devices[i].serviceS;
    }

    const ServiceConfig& s = pCfg.service;
    if(s.windowStartMin >= s.windowEndMin) return Invalid("service.window", "start must be before end");
    // The window must end before the night lock starts; a night that does not wrap midnight must not overlap it at all.
    const bool wraps = l.nightStartMin > l.nightEndMin;
    const bool beforeNight = s.windowEndMin <= l.nightStartMin;
    const bool afterNight = s.windowStartMin >= l.nightEndMin;
    if(wraps ? !beforeNight : !(beforeNight || afterNight))
        return Invalid("service.window", "must end before the night lock starts");
    if(s.minHeadroomPct < 0.0f || s.minHeadroomPct > 20.0f) return Invalid("service.min_headroom_pct", "must be 0..20");
    if(s.inconclusiveDays < 1) return Invalid("service.inconclusive_days", "must be >= 1");
    if(s.induceDeficitS > 600) return Invalid("service.induce_deficit_s", "must be <= 600");
    if(s.induceDeficitS > 0 && (s.induceDeficitDevice < 1 || s.induceDeficitDevice > DEVICES))
        return Invalid("service.induce_deficit_device", "must be 1..6 when induce_deficit_s is set");
    const uint32_t gaps = serviced - static_cast<uint32_t>(serviced > 0);
    const uint32_t needed = serviceTotalS + gaps * s.settleS + serviced * s.tailS + s.induceDeficitS;
    if(needed > static_cast<uint32_t>(s.windowEndMin - s.windowStartMin) * 60u)
        return Invalid("service", "the devices' service runs do not fit in the window");

    const SignalsConfig& sg = pCfg.signals;
    if(pCfg.boost.windowStartMin >= pCfg.boost.windowEndMin) return Invalid("boost.window", "start must be before end");
    if(pCfg.boost.targetMgl < 1.0f || pCfg.boost.targetMgl > 15.0f) return Invalid("boost.target_mgl", "must be 1..15");

    if(sg.buzzerHz < 200 || sg.buzzerHz > 8000) return Invalid("signals.buzzer_hz", "must be 200..8000");
    if(sg.ledBrightness > 100) return Invalid("signals.led_brightness", "must be 0..100");
    if(sg.normal.buzzer != BuzzerPattern::Off) return Invalid("signals.normal.buzzer", "Normal is silent");
    const Signal* all[] = {&sg.normal, &sg.blue, &sg.yellow, &sg.red, &sg.fault};
    for(std::size_t i = 0; i < 5; ++i)
    {
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "signals.%s", SIGNAL_KEYS[i]);
        if(all[i]->volume > 3) return Invalid(p.of(prefix, "volume"), "must be 0..3");
    }

    if(pCfg.ntfy.enabled && pCfg.ntfy.topic.empty()) return Invalid("ntfy.topic", "required when ntfy is enabled");

    return {};
}

std::size_t Write(const Config& pCfg, std::span<char> pOut)
{
    const ladder::Config& l = pCfg.ladder;
    JsonDocument doc;
    char hhmm[6];

    doc["schema"] = pCfg.schema;
    doc["sample_period_s"] = pCfg.samplePeriodS;

    JsonObject levels = doc["levels"].to<JsonObject>();
    WriteLevel(levels["blue"].to<JsonObject>(), l.blue);
    levels["blue"]["slope_mgl_per_10min"] = l.blueSlopeMglPer10min;
    WriteLevel(levels["yellow"].to<JsonObject>(), l.yellow);
    WriteLevel(levels["red"].to<JsonObject>(), l.red);
    doc["recover_sustain_s"] = l.recoverSustainS;

    JsonObject night = doc["night"].to<JsonObject>();
    FormatHhmm(l.nightStartMin, hhmm);
    night["start"] = std::string_view(hhmm);
    FormatHhmm(l.nightEndMin, hhmm);
    night["end"] = std::string_view(hhmm);
    night["lock"] = l.nightLock;
    night["unknown_time_is_night"] = l.unknownTimeIsNight;

    JsonObject pulse = doc["pulse"].to<JsonObject>();
    pulse["s"] = l.pulseS;
    pulse["min_interval_s"] = l.pulseMinIntervalS;

    JsonObject heat = doc["heat"].to<JsonObject>();
    heat["on_c"] = l.heatOnC;
    heat["off_c"] = l.heatOffC;

    JsonObject fault = doc["fault"].to<JsonObject>();
    fault["consecutive_failures"] = l.faultConsecutiveFailures;
    fault["stuck_minutes"] = pCfg.stuckMinutes;
    fault["level"] = LEVEL_NAMES[static_cast<uint8_t>(l.faultLevel)];
    fault["alert"] = l.faultAlert;

    doc["ack_silence_s"] = l.ackSilenceS;
    doc["escalate_if_failed"] = l.escalateIfFailed;

    JsonObject devices = doc["devices"].to<JsonObject>();
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const char key[2] = {static_cast<char>('1' + i), '\0'};
        JsonObject d = devices[std::string_view(key, 1)].to<JsonObject>();
        d["name"] = pCfg.devices[i].name.view();
        d["wired"] = WIRED_NAMES[static_cast<std::size_t>(pCfg.devices[i].wiredNc)];
        d["trigger"] = TRIGGER_NAMES[static_cast<uint8_t>(l.devices[i].trigger)];
        d["mode"] = MODE_NAMES[static_cast<uint8_t>(l.devices[i].mode)];
        d["ack_silences"] = l.devices[i].ackSilences;
        d["service_s"] = pCfg.devices[i].serviceS;
        d["min_response_pct"] = pCfg.devices[i].minResponsePct;
        d["boost"] = pCfg.boost.devices[i];
    }

    JsonObject service = doc["service"].to<JsonObject>();
    JsonArray window = service["window"].to<JsonArray>();
    FormatHhmm(pCfg.service.windowStartMin, hhmm);
    window.add(std::string_view(hhmm));
    FormatHhmm(pCfg.service.windowEndMin, hhmm);
    window.add(std::string_view(hhmm));
    service["settle_s"] = pCfg.service.settleS;
    service["tail_s"] = pCfg.service.tailS;
    service["min_headroom_pct"] = pCfg.service.minHeadroomPct;
    service["inconclusive_days"] = pCfg.service.inconclusiveDays;
    service["chirp"] = pCfg.service.chirp;
    service["induce_deficit_s"] = pCfg.service.induceDeficitS;
    service["induce_deficit_device"] = pCfg.service.induceDeficitDevice;

    JsonObject correction = doc["correction"].to<JsonObject>();
    correction["scale"] = pCfg.correction.scale;
    correction["offset"] = pCfg.correction.offset;
    doc["salinity_psu"] = pCfg.salinityPsu;
    doc["allow_zero_cal"] = pCfg.allowZeroCal;

    JsonObject boost = doc["boost"].to<JsonObject>();
    boost["enabled"] = pCfg.boost.enabled;
    JsonArray bwindow = boost["window"].to<JsonArray>();
    FormatHhmm(pCfg.boost.windowStartMin, hhmm);
    bwindow.add(std::string_view(hhmm));
    FormatHhmm(pCfg.boost.windowEndMin, hhmm);
    bwindow.add(std::string_view(hhmm));
    boost["target_mgl"] = pCfg.boost.targetMgl;

    JsonObject signals = doc["signals"].to<JsonObject>();
    signals["buzzer_hz"] = pCfg.signals.buzzerHz;
    signals["led_brightness"] = pCfg.signals.ledBrightness;
    const Signal* all[] = {&pCfg.signals.normal, &pCfg.signals.blue, &pCfg.signals.yellow, &pCfg.signals.red,
                           &pCfg.signals.fault};
    for(std::size_t i = 0; i < 5; ++i)
    {
        JsonObject o = signals[SIGNAL_KEYS[i]].to<JsonObject>();
        o["buzzer"] = BUZZER_NAMES[static_cast<uint8_t>(all[i]->buzzer)];
        o["volume"] = all[i]->volume;
    }

    JsonObject ntfy = doc["ntfy"].to<JsonObject>();
    ntfy["enabled"] = pCfg.ntfy.enabled;
    ntfy["topic"] = pCfg.ntfy.topic.view();
    ntfy["min_level"] = LEVEL_NAMES[static_cast<uint8_t>(pCfg.ntfy.minLevel)];

    if(measureJson(doc) + 1 > pOut.size()) return 0;
    return serializeJson(doc, pOut.data(), pOut.size());
}

} // namespace reefdo::config
