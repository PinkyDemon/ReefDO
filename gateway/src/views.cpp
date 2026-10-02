#include "reefdo/gateway/views.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>

#include <ArduinoJson.h>

#include "reefdo/slot_json.hpp"
#include "reefdo/solubility.hpp"
#include "reefdo/version.hpp"

namespace reefdo::gateway
{

using ladder::Level;

namespace
{

constexpr const char* OUTCOME_NAMES[] = {"none", "pass", "fail", "inconclusive", "unchecked"};
constexpr const char* PHASE_NAMES[] = {"idle", "deficit", "running", "tail", "settle"};
constexpr const char* PROBE_NAMES[] = {"ok", "stuck", "timeout", "frame_error", "exception", "implausible"};

constexpr const char* LINK_NAMES[] = {"none", "pending", "ok", "lost"};
constexpr const char* TUYA_ERRORS[] = {"none", "connect", "send", "timeout", "frame", "auth", "refused", "crypto"};
constexpr const char* DEMAND_NAMES[] = {"ladder", "test", "boost", "window", "manual"}; // bit 0 up

std::size_t Finish(JsonDocument& pDoc, std::span<char> pOut)
{
    if(measureJson(pDoc) + 1 > pOut.size()) return 0;
    return serializeJson(pDoc, pOut.data(), pOut.size());
}

void PutOptional(JsonObject pO, const char* pKey, std::optional<float> pV)
{
    if(pV.has_value())
    {
        pO[pKey] = *pV;
    }
    else
    {
        pO[pKey] = nullptr;
    }
}

bool Emit(ISink& pSink, std::span<const char> pBuf, std::size_t pN)
{
    return pSink.Write(std::string_view(pBuf.data(), pN));
}

// {"device":1..DEVICES,...} → index; nullopt with the message set when the number is missing or out of range.
std::optional<std::size_t> DeviceIndex(JsonVariantConst pO, Command& pCmd)
{
    const JsonVariantConst dev = pO["device"];
    if(!dev.is<uint32_t>() || dev.as<uint32_t>() < 1 || dev.as<uint32_t>() > DEVICES)
    {
        pCmd.message = "no such device number";
        return std::nullopt;
    }
    return dev.as<uint32_t>() - 1;
}

// {"s":seconds} for suspend and suspend_alerts; nullopt with the message set when the field is unusable.
std::optional<uint32_t> Seconds(JsonVariantConst pO, Command& pCmd)
{
    if(!pO["s"].is<uint32_t>())
    {
        pCmd.message = "s must be a number of seconds (0 ends it)";
        return std::nullopt;
    }
    return pO["s"].as<uint32_t>();
}

// First tier-A index whose record lies at or after `from_ts`, scanning back from the newest (a chart asks for
// the last day, not the first). Records without a clock (ts 0) are skipped over.
std::size_t FirstAtOrAfter(const log::RecordLog& pL, uint32_t pFromTs)
{
    std::size_t i = pL.Count();
    while(i > 0)
    {
        const std::optional<log::Record> r = pL.At(i - 1);
        if(r.has_value() && r->ts != 0 && r->ts < pFromTs) break;
        --i;
    }
    return i;
}

} // namespace

std::size_t StatusJson(const app::App& pApp, const app::Clock& pClock, std::span<char> pOut)
{
    const app::Status& s = pApp.GetStatus();
    const config::Config& c = pApp.GetConfig();
    JsonDocument doc;

    doc["fw"] = VERSION;
    doc["uptime_s"] = s.uptimeS;
    doc["clock_known"] = s.clockKnown;
    if(pClock.unixS.has_value()) doc["unix"] = *pClock.unixS;
    doc["tz"] = pClock.tzOffsetS;
    doc["next_seq"] = s.nextSeq;
    doc["ticks"] = s.ticks;

    JsonObject probe = doc["probe"].to<JsonObject>();
    PutOptional(probe, "do", s.doMgl);
    PutOptional(probe, "sat", s.satPct);
    PutOptional(probe, "temp", s.tempC);
    probe["slope"] = s.slopeMglPer10min;
    probe["slope_2min"] = s.fastSlopeMglPer10min; // sudden-drop detection, mg/L per 10 min
    probe["status"] = PROBE_NAMES[static_cast<uint8_t>(s.probe)];
    probe["failures"] = s.consecutiveFailures;

    JsonObject corr = doc["correction"].to<JsonObject>();
    corr["scale"] = c.correction.scale;
    corr["offset"] = c.correction.offset;
    corr["factory"] = c.correction.IsFactory();
    corr["seawater_scale_hint"] = solubility::SeawaterScale(s.tempC.value_or(26.0f), c.salinityPsu);

    JsonObject lad = doc["ladder"].to<JsonObject>();
    lad["level"] = config::Name(s.level);

    lad["fault"] = s.fault;
    lad["silenced"] = s.silenced;
    lad["suppressed"] = s.suppressed; // day_alarm suppress in effect
    lad["suspect"] = s.suspect;       // a sudden drop: dwells extended by sudden_drop.extend_s
    lad["alerts_suspended"] = s.alertsSuspended;
    lad["alerts_suspend_s"] = s.alertsSuspendLeftS; // until it resumes by itself; 0 when not suspended
    lad["maintenance"] = s.maintenance;
    lad["maintenance_s"] = s.maintenanceLeftS; // until it ends by itself; 0 when off
    // What the buzzer plays now: the configured pattern of the state shown, when the ladder lets it sound.
    lad["buzzer"] = s.sound ? config::Name(c.signals.of(s.level, s.fault).buzzer) : "off";
    JsonObject th = lad["enters_below"].to<JsonObject>();
    th["blue"] = c.ladder.blue.mgl - c.ladder.blue.hysteresis;
    th["yellow"] = c.ladder.yellow.mgl - c.ladder.yellow.hysteresis;
    th["red"] = c.ladder.red.mgl - c.ladder.red.hysteresis;
    JsonObject lv = lad["leaves_above"].to<JsonObject>();
    lv["blue"] = c.ladder.blue.mgl + c.ladder.blue.hysteresis;
    lv["yellow"] = c.ladder.yellow.mgl + c.ladder.yellow.hysteresis;
    lv["red"] = c.ladder.red.mgl + c.ladder.red.hysteresis;

    JsonArray devices = doc["devices"].to<JsonArray>();
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        JsonObject d = devices.add<JsonObject>();
        d["name"] = c.devices[i].name.view();
        d["trigger"] = config::Name(c.ladder.devices[i].trigger);
        config::PutSlot(d["slot"].to<JsonObject>(), c.devices[i].slot, true); // as in the config, the key masked
        d["pulse"] = c.ladder.devices[i].mode == ladder::Mode::PulseOff;
        d["on"] = s.deviceOn[i];
        const slot::Relay* relay = c.devices[i].slot.AsRelay();
        d["energised"] = relay != nullptr && relay->channel - 1u < RELAYS && s.relayEnergised[relay->channel - 1];
        d["failed"] = s.deviceFailed[i];
        d["test_s"] = c.test.devices[i].testS;
        d["suspend_s"] = s.suspendLeftS[i];
        if(s.manual[i].has_value())
        {
            d["manual"] = *s.manual[i];
        }
        else
        {
            d["manual"] = nullptr; // automatic
        }
        if(s.tuyaLink[i] == app::Link::None)
        {
            d["link"] = nullptr; // not a plug
        }
        else
        {
            JsonObject t = d["link"].to<JsonObject>();
            t["state"] = LINK_NAMES[static_cast<uint8_t>(s.tuyaLink[i])];
            t["error"] = TUYA_ERRORS[static_cast<uint8_t>(s.tuyaError[i])];
        }
        JsonArray why = d["why"].to<JsonArray>();
        for(std::size_t b = 0; b < std::size(DEMAND_NAMES); ++b)
        {
            if(s.deviceWhy[i] & (1u << b)) why.add(DEMAND_NAMES[b]);
        }
    }
    JsonArray relays = doc["relays"].to<JsonArray>(); // energised, per channel
    for(const bool e : s.relayEnergised)
        relays.add(e);

    JsonObject svc = doc["test"].to<JsonObject>();
    svc["running"] = s.testRunning;
    svc["phase"] = PHASE_NAMES[static_cast<uint8_t>(s.testPhase)];
    svc["device"] = s.testDevice + 1;
    svc["inconclusive_alert"] = s.inconclusiveAlert;
    svc["exclusive"] = c.test.exclusive;
    char hhmm[6];
    config::FormatHhmm(c.test.windowStartMin, hhmm);
    svc["window_start"] = std::string_view(hhmm);
    config::FormatHhmm(c.test.windowEndMin, hhmm);
    svc["window_end"] = std::string_view(hhmm);

    JsonObject bst = doc["boost"].to<JsonObject>();
    bst["enabled"] = c.boost.enabled;
    bst["running"] = s.boostRunning;
    bst["target_mgl"] = c.boost.targetMgl;
    config::FormatHhmm(c.boost.windowStartMin, hhmm);
    bst["window_start"] = std::string_view(hhmm);
    config::FormatHhmm(c.boost.windowEndMin, hhmm);
    bst["window_end"] = std::string_view(hhmm);

    JsonObject logs = doc["log"].to<JsonObject>();
    logs["a"] = pApp.LogA().Count();
    logs["a_capacity"] = pApp.LogA().Capacity();
    logs["b"] = pApp.LogB().Count();
    logs["e"] = pApp.LogE().Count();
    logs["d"] = pApp.LogD().Count();
    return Finish(doc, pOut);
}

std::size_t TestJson(const app::App& pApp, std::span<char> pOut)
{
    const selftest::State& st = pApp.TestState();
    const config::Config& c = pApp.GetConfig();
    JsonDocument doc;
    doc["running"] = st.phase != selftest::Phase::Idle;
    doc["phase"] = PHASE_NAMES[static_cast<uint8_t>(st.phase)];
    doc["inconclusive_streak"] = st.p.inconclusiveStreak;
    doc["inconclusive_alert"] = st.p.inconclusiveAlert;
    if(st.p.lastRunDay.has_value()) doc["last_run_day"] = *st.p.lastRunDay;
    doc["exclusive"] = c.test.exclusive;
    doc["no_fail_above_mgl"] = c.test.noFailAboveMgl;
    doc["run_duration_s"] = selftest::RunDurationS(c.test);
    JsonArray devices = doc["devices"].to<JsonArray>();
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        JsonObject d = devices.add<JsonObject>();
        d["name"] = c.devices[i].name.view();
        d["test_s"] = c.test.devices[i].testS;
        d["min_response_pct"] = c.test.devices[i].minResponsePct;
        d["last_outcome"] = OUTCOME_NAMES[static_cast<uint8_t>(st.p.lastOutcome[i])];
        d["last_response"] = st.p.lastResponse[i];
        d["fail_active"] = st.p.failActive[i];
    }
    return Finish(doc, pOut);
}

config::LoadResult ApplyConfig(app::App& pApp, std::string_view pJson, const app::Clock& pClock)
{
    config::Config cfg;
    const config::LoadResult r =
        config::Load(pJson, cfg, pApp.GetConfig()); // partial documents keep the current values
    if(r.Ok()) pApp.SetConfig(cfg, pClock);
    return r;
}

std::size_t ResultJson(const config::LoadResult& pR, std::span<char> pOut)
{
    JsonDocument doc;
    doc["ok"] = pR.Ok();
    if(!pR.Ok())
    {
        doc["path"] = pR.path.view();
        doc["message"] = pR.message.view();
    }
    return Finish(doc, pOut);
}

Command ApplyCommand(app::App& pApp, std::string_view pJson, const app::Clock& pClock)
{
    Command cmd;
    JsonDocument doc;
    if(deserializeJson(doc, pJson.data(), pJson.size()))
    {
        cmd.message = "invalid JSON";
        return cmd;
    }
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    if(root.isNull())
    {
        cmd.message = "expected an object";
        return cmd;
    }
    if(root["ack"].is<bool>() && root["ack"].as<bool>())
    {
        pApp.Ack();
        cmd.ok = true;
        cmd.message = "acknowledged";
        return cmd;
    }
    if(root["maintenance"].is<bool>())
    {
        pApp.SetMaintenance(root["maintenance"].as<bool>(), pClock);
        cmd.ok = true;
        cmd.message = "maintenance updated";
        return cmd;
    }
    if(root["test"].is<const char*>() && std::strcmp(root["test"].as<const char*>(), "run") == 0)
    {
        pApp.RunTest();
        cmd.ok = true;
        cmd.message = "test run requested";
        return cmd;
    }
    if(root["cal"].is<const char*>() && std::strcmp(root["cal"].as<const char*>(), "air") == 0)
    {
        const probe::CalResult r = pApp.AirCalibrate(pClock);
        cmd.ok = r == probe::CalResult::Ok;
        cmd.message = r == probe::CalResult::Ok        ? "air calibration sent"
                      : r == probe::CalResult::Refused ? "maintenance mode required"
                                                       : "the probe did not acknowledge";
        return cmd;
    }

    const JsonVariantConst clear = root["clear"];
    if(clear.is<JsonObjectConst>())
    {
        const std::optional<std::size_t> dev = DeviceIndex(clear, cmd);
        if(!dev.has_value()) return cmd;
        cmd.ok = pApp.ClearFailure(*dev, pClock);
        cmd.message = "test failure cleared";
        return cmd;
    }
    const JsonVariantConst suspend = root["suspend"];
    if(suspend.is<JsonObjectConst>())
    {
        const std::optional<std::size_t> dev = DeviceIndex(suspend, cmd);
        if(!dev.has_value()) return cmd;
        const std::optional<uint32_t> secs = Seconds(suspend, cmd);
        if(!secs.has_value()) return cmd;
        cmd.ok = pApp.SuspendDevice(*dev, *secs, pClock);
        cmd.message = !cmd.ok ? "at most 7200 s" : *secs > 0 ? "device suspended" : "device resumed";
        return cmd;
    }
    const JsonVariantConst alerts = root["suspend_alerts"];
    if(alerts.is<JsonObjectConst>())
    {
        const std::optional<uint32_t> secs = Seconds(alerts, cmd);
        if(!secs.has_value()) return cmd;
        // "level": what the asker saw; a deeper level now refuses (a phone may show an older one).
        std::optional<Level> seen;
        const JsonVariantConst level = alerts["level"];
        for(uint8_t l = 0; l <= 3 && !level.isNull(); ++l)
            if(level.is<const char*>() &&
               std::strcmp(level.as<const char*>(), config::Name(static_cast<Level>(l))) == 0)
                seen = static_cast<Level>(l);
        if(!level.isNull() && !seen.has_value())
        {
            cmd.message = "level must be normal, blue, yellow or red";
            return cmd;
        }
        cmd.ok = pApp.SuspendAlerts(*secs, seen);
        cmd.message = cmd.ok      ? (*secs > 0 ? "alerts suspended" : "alerts resumed")
                      : *secs > 0 ? "no alert to suspend, a FAULT, over 3600 s, or deeper than shown"
                                  : "no alert is suspended";
        return cmd;
    }
    const JsonVariantConst manual = root["manual"];
    if(manual.is<JsonObjectConst>())
    {
        const std::optional<std::size_t> dev = DeviceIndex(manual, cmd);
        if(!dev.has_value()) return cmd;
        const JsonVariantConst on = manual["on"];
        if(!on.is<bool>() && !on.isNull())
        {
            cmd.message = "on must be true, false or null (automatic)";
            return cmd;
        }
        const std::optional<bool> state = on.isNull() ? std::nullopt : std::optional<bool>(on.as<bool>());
        cmd.ok = pApp.SetManual(*dev, state, pClock);
        cmd.message = !cmd.ok              ? "an exclusive test is running"
                      : !state.has_value() ? "automatic"
                      : *state             ? "manual on"
                                           : "manual off";
        return cmd;
    }
    const JsonVariantConst time = root["time"];
    if(time.is<JsonObjectConst>())
    {
        if(!time["unix"].is<uint32_t>())
        {
            cmd.message = "time.unix must be a unix timestamp";
            return cmd;
        }
        cmd.setUnix = time["unix"].as<uint32_t>();
        if(time["tz"].is<int32_t>()) cmd.setTz = time["tz"].as<int32_t>();
        cmd.ok = true;
        cmd.message = "time accepted";
        return cmd;
    }
    cmd.message = "unknown command";
    return cmd;
}

std::size_t ExportCsv(const app::App& pApp, uint32_t pSinceSeq, ISink& pSink)
{
    char buf[160];
    if(!Emit(pSink, buf, log::CsvHeader(buf))) return 0;
    const log::RecordLog& l = pApp.LogA();
    std::size_t n = 0;
    for(std::size_t i = l.LowerBound(pSinceSeq); i < l.Count(); ++i)
    {
        const std::optional<log::Record> r = l.At(i);
        if(!r.has_value()) continue;
        if(!Emit(pSink, buf, log::CsvLine(*r, buf))) break;
        ++n;
    }
    return n;
}

std::size_t EventsCsv(const app::App& pApp, uint32_t pSinceSeq, ISink& pSink)
{
    char buf[160];
    if(!Emit(pSink, buf, log::CsvHeader(buf))) return 0;
    const log::RecordLog& l = pApp.LogE();
    std::size_t n = 0;
    for(std::size_t i = l.LowerBound(pSinceSeq); i < l.Count(); ++i)
    {
        const std::optional<log::Record> r = l.At(i);
        if(!r.has_value()) continue;
        if(!Emit(pSink, buf, log::CsvLine(*r, buf))) break;
        ++n;
    }
    return n;
}

std::size_t SeriesCsv(const app::App& pApp, char pTier, uint32_t pFromTs, uint32_t pToTs, uint32_t pEvery, ISink& pSink)
{
    char buf[160];
    if(pEvery == 0) pEvery = 1;
    std::size_t n = 0;
    if(pTier == 'B')
    {
        static const char HEAD[] = "ts,do_min,do_avg,do_max,sat_avg,temp_min,temp_max,level_max\n";
        if(!pSink.Write(HEAD)) return 0;
        const log::AggregateLog& l = pApp.LogB();
        std::size_t kept = 0;
        for(std::size_t i = 0; i < l.Count(); ++i)
        {
            const std::optional<log::Aggregate> a = l.At(i);
            if(!a.has_value() || a->ts < pFromTs || a->ts > pToTs) continue;
            if(kept++ % pEvery != 0) continue;
            const int len = std::snprintf(buf, sizeof buf, "%lu,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%u\n",
                                          static_cast<unsigned long>(a->ts), a->doMin / 1000.0, a->doAvg / 1000.0,
                                          a->doMax / 1000.0, a->satAvg / 100.0, a->tempMin / 100.0, a->tempMax / 100.0,
                                          a->levelMax);
            if(!Emit(pSink, buf, static_cast<std::size_t>(len))) break;
            ++n;
        }
        return n;
    }
    static const char HEAD[] = "ts,do,sat,temp,level\n";
    if(!pSink.Write(HEAD)) return 0;
    const log::RecordLog& l = pApp.LogA();
    std::size_t kept = 0;
    for(std::size_t i = FirstAtOrAfter(l, pFromTs); i < l.Count(); ++i)
    {
        const std::optional<log::Record> r = l.At(i);
        if(!r.has_value() || r->type != log::Type::Measurement || r->ts < pFromTs) continue;
        if(r->ts > pToTs) break;
        if(kept++ % pEvery != 0) continue;
        const int len =
            std::snprintf(buf, sizeof buf, "%lu,%.3f,%.2f,%.2f,%u\n", static_cast<unsigned long>(r->ts),
                          static_cast<double>(r->f0), static_cast<double>(r->f1), static_cast<double>(r->f2), r->level);
        if(!Emit(pSink, buf, static_cast<std::size_t>(len))) break;
        ++n;
    }
    return n;
}

} // namespace reefdo::gateway
