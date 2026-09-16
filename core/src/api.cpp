#include "reefdo/api.hpp"

#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "reefdo/solubility.hpp"
#include "reefdo/version.hpp"

namespace reefdo::api
{

using ladder::Level;

namespace
{

constexpr const char* LEVEL_NAMES[] = {"normal", "blue", "yellow", "red"};
constexpr const char* TRIGGER_NAMES[] = {"none", "blue", "yellow", "red", "heat"};
constexpr const char* OUTCOME_NAMES[] = {"none", "pass", "fail", "inconclusive", "unchecked"};
constexpr const char* PHASE_NAMES[] = {"idle", "deficit", "running", "tail", "settle"};
constexpr const char* PROBE_NAMES[] = {"ok", "stuck", "timeout", "frame_error", "exception", "implausible"};
constexpr const char* BUZZER_NAMES[] = {"off", "beep", "continuous", "fault"};
constexpr const char* LED_NAMES[] = {"green", "blue", "yellow", "red", "purple", "cyan"};

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
    probe["status"] = PROBE_NAMES[static_cast<uint8_t>(s.probe)];
    probe["failures"] = s.consecutiveFailures;

    JsonObject corr = doc["correction"].to<JsonObject>();
    corr["scale"] = c.correction.scale;
    corr["offset"] = c.correction.offset;
    corr["factory"] = c.correction.IsFactory();
    corr["seawater_scale_hint"] = solubility::SeawaterScale(s.tempC.value_or(26.0f), c.salinityPsu);

    JsonObject lad = doc["ladder"].to<JsonObject>();
    lad["level"] = LEVEL_NAMES[static_cast<uint8_t>(s.level)];
    lad["effective"] = LEVEL_NAMES[static_cast<uint8_t>(s.effective)];
    lad["fault"] = s.fault;
    lad["silenced"] = s.silenced;
    lad["heat"] = s.heat;
    lad["maintenance"] = s.maintenance;
    lad["buzzer"] = BUZZER_NAMES[static_cast<uint8_t>(s.buzzer)];
    lad["led"] = LED_NAMES[static_cast<uint8_t>(s.led)];
    JsonObject th = lad["enters_below"].to<JsonObject>();
    th["blue"] = c.ladder.blue.mgl - c.ladder.blue.hysteresis;
    th["yellow"] = c.ladder.yellow.mgl - c.ladder.yellow.hysteresis;
    th["red"] = c.ladder.red.mgl - c.ladder.red.hysteresis;
    JsonObject lv = lad["leaves_above"].to<JsonObject>();
    lv["blue"] = c.ladder.blue.mgl + c.ladder.blue.hysteresis;
    lv["yellow"] = c.ladder.yellow.mgl + c.ladder.yellow.hysteresis;
    lv["red"] = c.ladder.red.mgl + c.ladder.red.hysteresis;

    JsonArray devices = doc["devices"].to<JsonArray>();
    for(std::size_t i = 0; i < app::DEVICES; ++i)
    {
        JsonObject d = devices.add<JsonObject>();
        d["name"] = c.devices[i].name.view();
        d["trigger"] = TRIGGER_NAMES[static_cast<uint8_t>(c.ladder.devices[i].trigger)];
        d["wired"] = c.devices[i].wiredNc ? "NC" : "NO";
        d["pulse"] = c.ladder.devices[i].mode == ladder::Mode::PulseOff;
        d["on"] = s.deviceOn[i];
        d["energised"] = s.relayEnergised[i];
        d["failed"] = s.deviceFailed[i];
        d["service_s"] = c.devices[i].serviceS;
    }

    JsonObject svc = doc["service"].to<JsonObject>();
    svc["running"] = s.serviceRunning;
    svc["phase"] = PHASE_NAMES[static_cast<uint8_t>(s.servicePhase)];
    svc["device"] = s.serviceDevice + 1;
    svc["inconclusive_alert"] = s.inconclusiveAlert;
    char hhmm[6];
    config::FormatHhmm(c.service.windowStartMin, hhmm);
    svc["window_start"] = std::string_view(hhmm);
    config::FormatHhmm(c.service.windowEndMin, hhmm);
    svc["window_end"] = std::string_view(hhmm);

    JsonObject logs = doc["log"].to<JsonObject>();
    logs["a"] = pApp.LogA().Count();
    logs["a_capacity"] = pApp.LogA().Capacity();
    logs["b"] = pApp.LogB().Count();
    logs["e"] = pApp.LogE().Count();
    logs["d"] = pApp.LogD().Count();
    return Finish(doc, pOut);
}

std::size_t ServiceJson(const app::App& pApp, std::span<char> pOut)
{
    const service::State& st = pApp.ServiceState();
    const config::Config& c = pApp.GetConfig();
    JsonDocument doc;
    doc["running"] = st.phase != service::Phase::Idle;
    doc["phase"] = PHASE_NAMES[static_cast<uint8_t>(st.phase)];
    doc["inconclusive_streak"] = st.p.inconclusiveStreak;
    doc["inconclusive_alert"] = st.p.inconclusiveAlert;
    if(st.p.lastRunDay.has_value()) doc["last_run_day"] = *st.p.lastRunDay;
    service::Config sc;
    for(std::size_t i = 0; i < app::DEVICES; ++i)
    {
        sc.devices[i].serviceS = c.devices[i].serviceS;
        sc.devices[i].minResponsePct = c.devices[i].minResponsePct;
    }
    sc.settleS = c.service.settleS;
    sc.tailS = c.service.tailS;
    sc.induceDeficitS = c.service.induceDeficitS;
    sc.induceDeficitDevice = c.service.induceDeficitDevice;
    doc["run_duration_s"] = service::RunDurationS(sc);
    JsonArray devices = doc["devices"].to<JsonArray>();
    for(std::size_t i = 0; i < app::DEVICES; ++i)
    {
        JsonObject d = devices.add<JsonObject>();
        d["name"] = c.devices[i].name.view();
        d["service_s"] = c.devices[i].serviceS;
        d["min_response_pct"] = c.devices[i].minResponsePct;
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
    if(root["service"].is<const char*>() && std::strcmp(root["service"].as<const char*>(), "run") == 0)
    {
        pApp.RunService();
        cmd.ok = true;
        cmd.message = "service run requested";
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
    const JsonVariantConst relay = root["relay"];
    if(relay.is<JsonObjectConst>())
    {
        const JsonVariantConst dev = relay["device"];
        if(!dev.is<uint32_t>() || dev.as<uint32_t>() < 1 || dev.as<uint32_t>() > app::DEVICES)
        {
            cmd.message = "device must be 1..6";
            return cmd;
        }
        std::optional<bool> on;
        if(relay["on"].is<bool>()) on = relay["on"].as<bool>();
        cmd.ok = pApp.SetDeviceOverride(dev.as<uint32_t>() - 1, on);
        cmd.message = cmd.ok ? "override set" : "maintenance mode required";
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

} // namespace reefdo::api
