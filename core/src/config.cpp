#include "reefdo/config.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "slot_json.hpp"

namespace reefdo::config
{

using ladder::Level;
using ladder::Mode;
using ladder::Trigger;

namespace
{

// Enum names, indexed by the enum's value (contiguous from 0): reading searches them, writing indexes them.
constexpr const char* TRIGGER_NAMES[] = {"none", "blue", "yellow", "red", "heat"};
constexpr const char* MODE_NAMES[] = {"on", "pulse_off"};
constexpr const char* LEVEL_NAMES[] = {"normal", "blue", "yellow", "red"};
constexpr const char* WIRED_NAMES[] = {"NO", "NC"}; // a 1.0 document's device-level wiring
constexpr const char* BUZZER_NAMES[] = {"off", "chirp", "beep", "double", "triple", "continuous"};
constexpr const char* LEVEL_KEYS[] = {"blue", "yellow", "red"};
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

    void Windows(JsonVariantConst pV, const char* pPath, FixedVector<TimeWindow, WINDOWS>& pOut)
    {
        if(pV.isNull()) return;
        if(!pV.is<JsonArrayConst>())
        {
            Fail(LoadError::WrongType, pPath, "expected [[\"HH:MM\", \"HH:MM\"], ...]");
            return;
        }
        FixedVector<TimeWindow, WINDOWS> out;
        for(const JsonVariantConst w : pV.as<JsonArrayConst>())
        {
            if(!w.is<JsonArrayConst>() || w.size() != 2 || !w[0].is<const char*>() || !w[1].is<const char*>())
            {
                Fail(LoadError::WrongType, pPath, "each window is [\"HH:MM\", \"HH:MM\"]");
                return;
            }
            TimeWindow tw;
            Hhmm(w[0], pPath, tw.startMin);
            Hhmm(w[1], pPath, tw.endMin);
            if(!out.push_back(tw))
            {
                Fail(LoadError::BadValue, pPath, "at most 4 windows");
                return;
            }
        }
        pOut = out;
    }

    // One of pNames; the value is its index.
    void Choice(JsonObjectConst pO, const char* pKey, const char* pPath, uint8_t& pOut,
                std::span<const char* const> pNames)
    {
        const JsonVariantConst v = pO[pKey];
        if(v.isNull()) return;
        if(!v.is<const char*>())
        {
            Fail(LoadError::WrongType, pPath, "expected a name");
            return;
        }
        for(std::size_t i = 0; i < pNames.size(); ++i)
        {
            if(std::strcmp(pNames[i], v.as<const char*>()) == 0)
            {
                pOut = static_cast<uint8_t>(i);
                return;
            }
        }
        Fail(LoadError::BadValue, pPath, "unknown name");
    }

    template <class E>
    void Enumeration(JsonObjectConst pO, const char* pKey, const char* pPath, E& pOut,
                     std::span<const char* const> pNames)
    {
        uint8_t v = static_cast<uint8_t>(pOut);
        Choice(pO, pKey, pPath, v, pNames);
        pOut = static_cast<E>(v);
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

// A slot's parameters out of its JSON object, as its Fields describe them.
class SlotReader final : public slot::IFields
{
public:
    SlotReader(Reader& pR, JsonObjectConst pO, const char* pPrefix)
        : mR(pR)
        , mO(pO)
        , mPrefix(pPrefix)
    {
    }
    void Number(const slot::Field& pF, uint32_t& pV) override { mR.Number(mO, pF.key, mPath.of(mPrefix, pF.key), pV); }
    void String(const slot::Field& pF, slot::Text& pV) override
    {
        const JsonVariantConst v = mO[pF.key];
        const bool placeholder =
            pF.secret && v.is<const char*>() && std::strcmp(v.as<const char*>(), KEY_REDACTED) == 0;
        if(!placeholder) mR.Text(mO, pF.key, mPath.of(mPrefix, pF.key), pV); // the placeholder keeps the stored one
    }
    void Choice(const slot::Field& pF, uint8_t& pV) override
    {
        mR.Choice(mO, pF.key, mPath.of(mPrefix, pF.key), pV, pF.choices);
    }

private:
    Reader& mR;
    JsonObjectConst mO;
    const char* mPrefix;
    Path mPath;
};

// devices.N.slot: {"type": ..., parameters}. Another type starts from that type's defaults; the same type keeps the
// stored parameters the document leaves out.
void ReadSlot(Reader& pR, JsonVariantConst pV, const char* pPrefix, slot::AnySlot& pOut)
{
    if(pV.isNull()) return;
    if(!pV.is<JsonObjectConst>())
    {
        pR.Fail(LoadError::WrongType, pPrefix, "expected {\"type\": ..., parameters}");
        return;
    }
    const JsonObjectConst o = pV.as<JsonObjectConst>();
    const JsonVariantConst type = o["type"];
    if(!type.isNull())
    {
        const std::optional<slot::Kind> k =
            type.is<const char*>() ? slot::KindNamed(type.as<const char*>()) : std::nullopt;
        if(!k.has_value())
        {
            Path p;
            pR.Fail(LoadError::BadValue, p.of(pPrefix, "type"), "not a slot type (GET /api/slots lists them)");
            return;
        }
        if(*k != pOut.GetKind()) pOut.Reset(*k);
    }
    slot::Slot* s = pOut.Get();
    if(s == nullptr) return;
    SlotReader reader(pR, o, pPrefix);
    s->Fields(reader);
}

// The first parameter outside its Field's limits, as an Invalid LoadResult.
class SlotChecker final : public slot::IFields
{
public:
    explicit SlotChecker(const char* pPrefix)
        : mPrefix(pPrefix)
    {
    }
    void Number(const slot::Field& pF, uint32_t& pV) override
    {
        if(pV < pF.min || pV > pF.max) Fail(pF, "must be %lu..%lu", pF.min, pF.max);
    }
    void String(const slot::Field& pF, slot::Text& pV) override
    {
        if(pV.size() < pF.min || pV.size() > pF.max)
        {
            Fail(pF, pF.min == pF.max ? "must be %lu characters" : "must be %lu..%lu characters", pF.min, pF.max);
        }
        else if(pF.ipv4 && !tuya::ValidIpv4(pV.view()))
        {
            Fail(pF, "must be an address like 192.168.1.50", 0, 0);
        }
    }
    void Choice(const slot::Field& pF, uint8_t& pV) override
    {
        if(pV >= pF.choices.size()) Fail(pF, "unknown value", 0, 0);
    }
    const LoadResult& Result() const { return mResult; }

private:
    void Fail(const slot::Field& pF, const char* pFormat, uint32_t pA, uint32_t pB)
    {
        if(!mResult.Ok()) return;
        char msg[48];
        std::snprintf(msg, sizeof msg, pFormat, static_cast<unsigned long>(pA), static_cast<unsigned long>(pB));
        mResult = Invalid(mPath.of(mPrefix, pF.key), msg);
    }

    const char* mPrefix;
    Path mPath;
    LoadResult mResult;
};

// A slot's parameters into its JSON object.
class SlotWriter final : public slot::IFields
{
public:
    SlotWriter(JsonObject pO, bool pRedact)
        : mO(pO)
        , mRedact(pRedact)
    {
    }
    void Number(const slot::Field& pF, uint32_t& pV) override { mO[pF.key] = pV; }
    void String(const slot::Field& pF, slot::Text& pV) override
    {
        mO[pF.key] = pF.secret && mRedact && !pV.empty() ? std::string_view(KEY_REDACTED) : pV.view();
    }
    void Choice(const slot::Field& pF, uint8_t& pV) override
    {
        mO[pF.key] = pV < pF.choices.size() ? pF.choices[pV] : "?"; // "?" only for a config Validate() refuses
    }

private:
    JsonObject mO;
    bool mRedact;
};

// A slot type's parameters as JSON Schema properties; the walked slot's values are the defaults.
class SchemaWriter final : public slot::IFields
{
public:
    SchemaWriter(JsonObject pProperties, JsonArray pRequired)
        : mProperties(pProperties)
        , mRequired(pRequired)
    {
    }
    void Number(const slot::Field& pF, uint32_t& pV) override
    {
        JsonObject o = Property(pF, "integer");
        o["minimum"] = pF.min;
        o["maximum"] = pF.max;
        o["default"] = pV;
    }
    void String(const slot::Field& pF, slot::Text& pV) override
    {
        JsonObject o = Property(pF, "string");
        o["minLength"] = pF.min;
        o["maxLength"] = pF.max;
        if(pF.ipv4) o["format"] = "ipv4";
        if(pF.secret) o["writeOnly"] = true;
        o["default"] = pV.view();
    }
    void Choice(const slot::Field& pF, uint8_t& pV) override
    {
        JsonObject o = Property(pF, "string");
        JsonArray names = o["enum"].to<JsonArray>();
        for(const char* const n : pF.choices)
            names.add(n);
        o["default"] = pF.choices[pV];
    }

private:
    JsonObject Property(const slot::Field& pF, const char* pType)
    {
        JsonObject o = mProperties[pF.key].to<JsonObject>();
        o["type"] = pType;
        o["title"] = pF.title;
        o["description"] = pF.help;
        mRequired.add(pF.key);
        return o;
    }

    JsonObject mProperties;
    JsonArray mRequired;
};

void WriteLevel(JsonObject pO, const ladder::LevelConfig& pLc)
{
    pO["mgl"] = pLc.mgl;
    pO["hysteresis"] = pLc.hysteresis;
    pO["dwell_s"] = pLc.dwellS;
}

} // namespace

bool TimeWindow::Contains(uint16_t pMinute) const
{
    if(startMin <= endMin) return pMinute >= startMin && pMinute < endMin;
    return pMinute >= startMin || pMinute < endMin; // wraps midnight
}

void PutSlot(JsonObject pO, const slot::AnySlot& pS, bool pRedact)
{
    pO["type"] = slot::Name(pS.GetKind());
    slot::AnySlot copy = pS; // Fields() walks mutable parameters
    slot::Slot* s = copy.Get();
    if(s == nullptr) return;
    SlotWriter writer(pO, pRedact);
    s->Fields(writer);
}

std::size_t SlotSchema(std::span<char> pOut)
{
    JsonDocument doc;
    for(uint8_t k = 0; k < slot::KINDS; ++k)
    {
        slot::AnySlot defaults;
        defaults.Reset(static_cast<slot::Kind>(k));
        JsonObject t = doc[slot::Name(defaults.GetKind())].to<JsonObject>();
        t["title"] = slot::Title(defaults.GetKind());
        t["type"] = "object";
        SchemaWriter writer(t["properties"].to<JsonObject>(), t["required"].to<JsonArray>());
        slot::Slot* s = defaults.Get();
        if(s != nullptr) s->Fields(writer);
    }
    if(measureJson(doc) + 1 > pOut.size()) return 0;
    return serializeJson(doc, pOut.data(), pOut.size());
}

const char* Name(ladder::Level pL)
{
    return LEVEL_NAMES[static_cast<uint8_t>(pL)];
}

const char* Name(ladder::Trigger pT)
{
    return TRIGGER_NAMES[static_cast<uint8_t>(pT)];
}

const char* Name(BuzzerPattern pP)
{
    return BUZZER_NAMES[static_cast<uint8_t>(pP)];
}

const Signal& SignalsConfig::of(ladder::Level pLevel, bool pIsFault) const
{
    if(pIsFault) return fault;
    if(pLevel == ladder::Level::Blue) return blue;
    if(pLevel == ladder::Level::Yellow) return yellow;
    if(pLevel == ladder::Level::Red) return red;
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
        if(i < RELAYS) c.devices[i].slot = slot::Relay(static_cast<uint32_t>(i + 1), slot::Wiring::No);
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
    r.Enumeration(fault, "level", "fault.level", cfg.ladder.faultLevel, LEVEL_NAMES);
    r.Flag(fault, "alert", "fault.alert", cfg.ladder.faultAlert);

    const JsonObjectConst correction = r.Object(root, "correction", "correction");
    r.Number(correction, "scale", "correction.scale", cfg.correction.scale);
    r.Number(correction, "offset", "correction.offset", cfg.correction.offset);

    const JsonObjectConst devices = r.Object(root, "devices", "devices");
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        char key[12];
        std::snprintf(key, sizeof key, "%u", static_cast<unsigned>(i + 1));
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "devices.%s", key);
        const JsonObjectConst d = r.Object(devices, key, prefix);
        r.Text(d, "name", p.of(prefix, "name"), cfg.devices[i].name);
        char sprefix[32];
        std::snprintf(sprefix, sizeof sprefix, "%s.slot", prefix);
        ReadSlot(r, d["slot"], sprefix, cfg.devices[i].slot);
        // A 1.0 document has no slot but "wired" at the device: that is the wiring of the device's relay.
        bool nc = false;
        r.Enumeration(d, "wired", p.of(prefix, "wired"), nc, WIRED_NAMES);
        slot::Relay* relay = cfg.devices[i].slot.AsRelay();
        if(!d["wired"].isNull() && relay != nullptr) relay->wiring = nc ? slot::Wiring::Nc : slot::Wiring::No;
        r.Enumeration(d, "trigger", p.of(prefix, "trigger"), cfg.ladder.devices[i].trigger, TRIGGER_NAMES);
        r.Enumeration(d, "mode", p.of(prefix, "mode"), cfg.ladder.devices[i].mode, MODE_NAMES);
        r.Flag(d, "ack_silences", p.of(prefix, "ack_silences"), cfg.ladder.devices[i].ackSilences);
        r.Number(d, d["test_s"].isNull() ? "service_s" : "test_s", p.of(prefix, "test_s"), cfg.test.devices[i].testS);
        r.Number(d, "min_response_pct", p.of(prefix, "min_response_pct"), cfg.test.devices[i].minResponsePct);
        r.Flag(d, "boost", p.of(prefix, "boost"), cfg.boost.devices[i]);
        r.Windows(d["windows"], p.of(prefix, "windows"), cfg.devices[i].windows);
    }

    // The self test was called "service" up to 1.0: a stored document of that age still loads.
    const char* const testKey = root["test"].isNull() ? "service" : "test";
    const JsonObjectConst test = r.Object(root, testKey, testKey);
    const JsonVariantConst window = test["window"];
    if(!window.isNull())
    {
        if(!window.is<JsonArrayConst>() || window.as<JsonArrayConst>().size() != 2)
        {
            r.Fail(LoadError::WrongType, "test.window", "expected [\"HH:MM\", \"HH:MM\"]");
        }
        else
        {
            r.Hhmm(window[0], "test.window[0]", cfg.test.windowStartMin);
            r.Hhmm(window[1], "test.window[1]", cfg.test.windowEndMin);
        }
    }
    r.Number(test, "settle_s", "test.settle_s", cfg.test.settleS);
    r.Number(test, "tail_s", "test.tail_s", cfg.test.tailS);
    r.Number(test, "min_headroom_pct", "test.min_headroom_pct", cfg.test.minHeadroomPct);
    r.Number(test, "inconclusive_days", "test.inconclusive_days", cfg.test.inconclusiveDays);
    r.Flag(test, "chirp", "test.chirp", cfg.test.chirp);
    r.Number(test, "induce_deficit_s", "test.induce_deficit_s", cfg.test.induceDeficitS);
    r.Number(test, "induce_deficit_device", "test.induce_deficit_device", cfg.test.induceDeficitDevice);
    r.Flag(test, "exclusive", "test.exclusive", cfg.test.exclusive);
    r.Number(test, "no_fail_above_mgl", "test.no_fail_above_mgl", cfg.test.noFailAboveMgl);

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
        r.Enumeration(o, "buzzer", p.of(prefix, "buzzer"), sg.buzzer, BUZZER_NAMES);
        r.Number(o, "volume", p.of(prefix, "volume"), sg.volume);
    }

    const JsonObjectConst ntfy = r.Object(root, "ntfy", "ntfy");
    r.Flag(ntfy, "enabled", "ntfy.enabled", cfg.ntfy.enabled);
    r.Text(ntfy, "topic", "ntfy.topic", cfg.ntfy.topic);
    r.Enumeration(ntfy, "min_level", "ntfy.min_level", cfg.ntfy.minLevel, LEVEL_NAMES);

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

    uint32_t tested = 0;
    uint32_t testTotalS = 0;
    char msg[48];
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const DeviceSettings& dv = pCfg.devices[i];
        const selftest::DeviceConfig& td = pCfg.test.devices[i];
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "devices.%u", static_cast<unsigned>(i + 1));
        char sprefix[32];
        std::snprintf(sprefix, sizeof sprefix, "%s.slot", prefix);
        slot::AnySlot own = dv.slot; // Fields() walks mutable parameters
        if(slot::Slot* s = own.Get())
        {
            SlotChecker checker(sprefix);
            s->Fields(checker);
            if(!checker.Result().Ok()) return checker.Result();
            for(std::size_t j = 0; j < i; ++j)
            {
                const slot::Slot* other = pCfg.devices[j].slot.Get();
                if(other == nullptr || !(*s == *other)) continue; // Slot == is IsSame: the same output
                std::snprintf(msg, sizeof msg, "already drives device %u", static_cast<unsigned>(j + 1));
                return Invalid(sprefix, msg);
            }
        }
        const slot::Relay* relay = dv.slot.AsRelay();
        if(l.devices[i].mode == Mode::PulseOff && !(relay != nullptr && relay->wiring == slot::Wiring::Nc))
            return Invalid(p.of(prefix, "mode"), "pulse_off needs a relay wired NC, or a dead controller cuts it");
        if(td.minResponsePct < 0.0f || td.minResponsePct > 50.0f)
            return Invalid(p.of(prefix, "min_response_pct"), "must be 0..50");
        if(td.testS > 3600) return Invalid(p.of(prefix, "test_s"), "must be <= 3600");
        for(const TimeWindow& w : dv.windows)
        {
            if(w.startMin == w.endMin) return Invalid(p.of(prefix, "windows"), "a window's start and end must differ");
        }
        if(!dv.windows.empty() && (l.devices[i].mode == Mode::PulseOff || l.devices[i].ackSilences))
            return Invalid(p.of(prefix, "windows"), "not for pulse or alarm devices");
        const bool used =
            l.devices[i].trigger != Trigger::None || td.testS > 0 || pCfg.boost.devices[i] || !dv.windows.empty();
        if(used && dv.slot.GetKind() == slot::Kind::None)
            return Invalid(sprefix, "needs a relay or a plug: it has a trigger, test, boost or window");
        tested += static_cast<uint32_t>(td.testS > 0);
        testTotalS += td.testS;
    }

    const selftest::Config& s = pCfg.test;
    if(s.windowStartMin >= s.windowEndMin) return Invalid("test.window", "start must be before end");
    // The window must end before the night lock starts; a night that does not wrap midnight must not overlap it at all.
    const bool wraps = l.nightStartMin > l.nightEndMin;
    const bool beforeNight = s.windowEndMin <= l.nightStartMin;
    const bool afterNight = s.windowStartMin >= l.nightEndMin;
    if(wraps ? !beforeNight : !(beforeNight || afterNight))
        return Invalid("test.window", "must end before the night lock starts");
    if(s.minHeadroomPct < 0.0f || s.minHeadroomPct > 20.0f) return Invalid("test.min_headroom_pct", "must be 0..20");
    if(s.inconclusiveDays < 1) return Invalid("test.inconclusive_days", "must be >= 1");
    if(s.induceDeficitS > 600) return Invalid("test.induce_deficit_s", "must be <= 600");
    if(s.induceDeficitS > 0 && (s.induceDeficitDevice < 1 || s.induceDeficitDevice > DEVICES))
        return Invalid("test.induce_deficit_device", "must be a device number when induce_deficit_s is set");
    if(s.induceDeficitS > 0 && pCfg.devices[s.induceDeficitDevice - 1].slot.GetKind() == slot::Kind::None)
        return Invalid("test.induce_deficit_device", "that device has no relay or plug to cut");
    if(s.noFailAboveMgl < 0.0f || s.noFailAboveMgl > 15.0f)
        return Invalid("test.no_fail_above_mgl", "must be 0..15 (0 = off)");
    const uint32_t gaps = tested - static_cast<uint32_t>(tested > 0);
    const uint32_t deficitS = s.exclusive ? s.induceDeficitS : 0; // the deficit only exists to be measured
    const uint32_t needed = testTotalS + gaps * s.settleS + tested * s.tailS + deficitS;
    if(needed > static_cast<uint32_t>(s.windowEndMin - s.windowStartMin) * 60u)
        return Invalid("test", "the devices' test runs do not fit in the window");

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

std::size_t Write(const Config& pCfg, std::span<char> pOut, bool pRedact)
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
    fault["level"] = Name(l.faultLevel);
    fault["alert"] = l.faultAlert;

    doc["ack_silence_s"] = l.ackSilenceS;

    JsonObject devices = doc["devices"].to<JsonObject>();
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        char key[12];
        const int keyLen = std::snprintf(key, sizeof key, "%u", static_cast<unsigned>(i + 1));
        JsonObject d = devices[std::string_view(key, static_cast<std::size_t>(keyLen))].to<JsonObject>();
        d["name"] = pCfg.devices[i].name.view();
        PutSlot(d["slot"].to<JsonObject>(), pCfg.devices[i].slot, pRedact);
        d["trigger"] = Name(l.devices[i].trigger);
        d["mode"] = MODE_NAMES[static_cast<uint8_t>(l.devices[i].mode)];
        d["ack_silences"] = l.devices[i].ackSilences;
        d["test_s"] = pCfg.test.devices[i].testS;
        d["min_response_pct"] = pCfg.test.devices[i].minResponsePct;
        d["boost"] = pCfg.boost.devices[i];
        JsonArray windows = d["windows"].to<JsonArray>();
        for(const TimeWindow& w : pCfg.devices[i].windows)
        {
            JsonArray pair = windows.add<JsonArray>();
            FormatHhmm(w.startMin, hhmm);
            pair.add(std::string_view(hhmm));
            FormatHhmm(w.endMin, hhmm);
            pair.add(std::string_view(hhmm));
        }
    }

    JsonObject test = doc["test"].to<JsonObject>();
    JsonArray window = test["window"].to<JsonArray>();
    FormatHhmm(pCfg.test.windowStartMin, hhmm);
    window.add(std::string_view(hhmm));
    FormatHhmm(pCfg.test.windowEndMin, hhmm);
    window.add(std::string_view(hhmm));
    test["settle_s"] = pCfg.test.settleS;
    test["tail_s"] = pCfg.test.tailS;
    test["min_headroom_pct"] = pCfg.test.minHeadroomPct;
    test["inconclusive_days"] = pCfg.test.inconclusiveDays;
    test["chirp"] = pCfg.test.chirp;
    test["induce_deficit_s"] = pCfg.test.induceDeficitS;
    test["induce_deficit_device"] = pCfg.test.induceDeficitDevice;
    test["exclusive"] = pCfg.test.exclusive;
    test["no_fail_above_mgl"] = pCfg.test.noFailAboveMgl;

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
        o["buzzer"] = Name(all[i]->buzzer);
        o["volume"] = all[i]->volume;
    }

    JsonObject ntfy = doc["ntfy"].to<JsonObject>();
    ntfy["enabled"] = pCfg.ntfy.enabled;
    ntfy["topic"] = pCfg.ntfy.topic.view();
    ntfy["min_level"] = Name(pCfg.ntfy.minLevel);

    if(measureJson(doc) + 1 > pOut.size()) return 0;
    return serializeJson(doc, pOut.data(), pOut.size());
}

} // namespace reefdo::config
