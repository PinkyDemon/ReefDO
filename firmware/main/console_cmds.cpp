#include "console_cmds.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "reefdo/api.hpp"

#include "board.hpp"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/nvs_store.hpp"
#include "indicator.hpp"
#include "net.hpp"
#include "notify.hpp"
#include "sampler.hpp"
#include "web.hpp"

namespace console
{

namespace
{

using reefdo::app::App;

const char* const LEVEL[] = {"normal", "blue", "yellow", "red"};
const char* const PHASE[] = {"idle", "deficit", "running", "tail", "settle"};
const char* const OUTCOME[] = {"-", "pass", "FAIL", "inconclusive", "unchecked"};

bool Is(const char* pA, const char* pB)
{
    return std::strcmp(pA, pB) == 0;
}

// pArgv[pFrom..] joined by spaces; single quotes become double quotes so JSON can be typed without escaping.
std::string_view JoinJson(int pArgc, char** pArgv, int pFrom, char* pBuf, std::size_t pCap)
{
    std::size_t n = 0;
    for(int i = pFrom; i < pArgc; ++i)
    {
        const std::size_t len = std::strlen(pArgv[i]);
        if(n + len + 2 > pCap) return {};
        if(i > pFrom) pBuf[n++] = ' ';
        std::memcpy(pBuf + n, pArgv[i], len);
        n += len;
    }
    pBuf[n] = '\0';
    if(std::memchr(pBuf, '"', n) == nullptr)
    {
        for(std::size_t i = 0; i < n; ++i)
        {
            if(pBuf[i] == '\'') pBuf[i] = '"';
        }
    }
    return {pBuf, n};
}

class StdoutSink : public reefdo::api::ISink
{
public:
    bool Write(std::string_view pChunk) override
    {
        return std::fwrite(pChunk.data(), 1, pChunk.size(), stdout) == pChunk.size();
    }
};

int CmdStatus(int pArgc, char** pArgv)
{
    sampler::Guard guard;
    const App& app = sampler::App();
    const reefdo::app::Clock clock = sampler::ClockNow();
    if(pArgc == 2 && Is(pArgv[1], "json"))
    {
        static char sBuf[2048];
        const std::size_t n = reefdo::api::StatusJson(app, clock, sBuf);
        std::printf("%.*s\n", static_cast<int>(n), sBuf);
        return 0;
    }
    const reefdo::app::Status& s = app.GetStatus();
    std::printf("ReefDO %s  uptime %lu s  heap %u (min %u)  probe %s  clock %s tz %+ld\n",
                esp_app_get_description()->version, static_cast<unsigned long>(s.uptimeS),
                static_cast<unsigned>(esp_get_free_heap_size()),
                static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                sampler::GetProbeSource() == sampler::ProbeSource::Sim ? "SIM" : "rk500",
                clock.unixS ? "set" : "UNKNOWN", static_cast<long>(clock.tzOffsetS));
    if(s.doMgl)
    {
        std::printf("DO %.2f mg/L  sat %.1f %%  temp %.2f C  slope %+.3f mg/L/10min\n", static_cast<double>(*s.doMgl),
                    static_cast<double>(s.satPct.value_or(0.0f)), static_cast<double>(s.tempC.value_or(0.0f)),
                    static_cast<double>(s.slopeMglPer10min));
    }
    else
    {
        std::printf("DO --  probe status %d, %lu consecutive failures\n", static_cast<int>(s.probe),
                    static_cast<unsigned long>(s.consecutiveFailures));
    }
    std::printf("level %s (effective %s)%s%s%s%s\n", LEVEL[static_cast<int>(s.level)],
                LEVEL[static_cast<int>(s.effective)], s.fault ? "  FAULT" : "", s.silenced ? "  silenced" : "",
                s.heat ? "  heat" : "", s.maintenance ? "  MAINTENANCE" : "");
    std::printf("devices:");
    for(std::size_t i = 0; i < reefdo::app::DEVICES; ++i)
    {
        std::printf(" %u=%s%s%s", static_cast<unsigned>(i + 1), s.deviceOn[i] ? "on" : "off",
                    s.relayEnergised[i] ? "(E)" : "", s.deviceFailed[i] ? "!FAILED" : "");
    }
    std::printf("\nservice: %s device %u%s%s\n", PHASE[static_cast<int>(s.servicePhase)], s.serviceDevice,
                s.serviceRunning ? "  RUNNING" : "", s.inconclusiveAlert ? "  inconclusive-streak" : "");
    std::printf("boost: %s\n", s.boostRunning ? "RUNNING" : "idle");
    std::printf("log: A %u  B %u  E %u  D %u  next seq %lu  ticks %lu\n", static_cast<unsigned>(app.LogA().Count()),
                static_cast<unsigned>(app.LogB().Count()), static_cast<unsigned>(app.LogE().Count()),
                static_cast<unsigned>(app.LogD().Count()), static_cast<unsigned long>(s.nextSeq),
                static_cast<unsigned long>(s.ticks));
    if(sampler::Tank() != nullptr)
    {
        std::printf("sim tank: sat %.2f %%  temp %.2f C  DO %.2f mg/L\n",
                    static_cast<double>(sampler::Tank()->SatPct()), static_cast<double>(sampler::Tank()->TempC()),
                    static_cast<double>(sampler::Tank()->DoMgl()));
    }
    return 0;
}

int CmdAck(int, char**)
{
    sampler::Guard guard;
    sampler::App().Ack();
    std::printf("acknowledged\n");
    return 0;
}

int CmdMaint(int pArgc, char** pArgv)
{
    if(pArgc != 2 || (!Is(pArgv[1], "on") && !Is(pArgv[1], "off")))
    {
        std::printf("usage: maint on|off\n");
        return 1;
    }
    sampler::Guard guard;
    sampler::App().SetMaintenance(Is(pArgv[1], "on"), sampler::ClockNow());
    std::printf("maintenance %s\n", pArgv[1]);
    return 0;
}

int CmdService(int pArgc, char** pArgv)
{
    sampler::Guard guard;
    if(pArgc == 2 && Is(pArgv[1], "run"))
    {
        sampler::App().RunService();
        std::printf("service run requested\n");
        return 0;
    }
    if(pArgc == 2 && Is(pArgv[1], "json"))
    {
        static char sBuf[1024];
        const std::size_t n = reefdo::api::ServiceJson(sampler::App(), sBuf);
        std::printf("%.*s\n", static_cast<int>(n), sBuf);
        return 0;
    }
    const reefdo::service::Persistent& p = sampler::App().ServicePersistent();
    const reefdo::config::Config& cfg = sampler::App().GetConfig();
    std::printf("window %02u:%02u-%02u:%02u  last run day %ld  inconclusive streak %lu\n",
                cfg.service.windowStartMin / 60, cfg.service.windowStartMin % 60, cfg.service.windowEndMin / 60,
                cfg.service.windowEndMin % 60, p.lastRunDay ? static_cast<long>(*p.lastRunDay) : -1L,
                static_cast<unsigned long>(p.inconclusiveStreak));
    for(std::size_t i = 0; i < reefdo::app::DEVICES; ++i)
    {
        std::printf("  %u %-24s service_s %lu  last %s  response %.2f %%%s\n", static_cast<unsigned>(i + 1),
                    cfg.devices[i].name.c_str(), static_cast<unsigned long>(cfg.devices[i].serviceS),
                    OUTCOME[static_cast<int>(p.lastOutcome[i])], static_cast<double>(p.lastResponse[i]),
                    p.failActive[i] ? "  FAIL ACTIVE" : "");
    }
    return 0;
}

int CmdCal(int pArgc, char** pArgv)
{
    if(pArgc != 2 || !Is(pArgv[1], "air"))
    {
        std::printf("usage: cal air   (maintenance mode, probe in air)\n");
        return 1;
    }
    sampler::Guard guard;
    const reefdo::probe::CalResult r = sampler::App().AirCalibrate(sampler::ClockNow());
    std::printf("air calibration: %s\n", r == reefdo::probe::CalResult::Ok        ? "ok"
                                         : r == reefdo::probe::CalResult::Refused ? "refused (not in maintenance?)"
                                                                                  : "FAILED");
    return r == reefdo::probe::CalResult::Ok ? 0 : 1;
}

// relay <1..6> on|off|auto — through the App's maintenance override; the ladder's demands still win.
int CmdRelay(int pArgc, char** pArgv)
{
    sampler::Guard guard;
    if(pArgc == 2 && Is(pArgv[1], "auto"))
    {
        for(std::size_t i = 0; i < reefdo::app::DEVICES; ++i)
            sampler::App().SetDeviceOverride(i, std::nullopt);
        std::printf("all overrides cleared\n");
        return 0;
    }
    if(pArgc != 3)
    {
        std::printf("usage: relay <1..6> on|off|auto   |   relay auto\n");
        return 1;
    }
    const int n = std::atoi(pArgv[1]);
    if(n < 1 || n > board::RELAY_COUNT)
    {
        std::printf("device must be 1..%d\n", board::RELAY_COUNT);
        return 1;
    }
    std::optional<bool> on;
    if(Is(pArgv[2], "on"))
        on = true;
    else if(Is(pArgv[2], "off"))
        on = false;
    else if(!Is(pArgv[2], "auto"))
    {
        std::printf("expected on|off|auto\n");
        return 1;
    }
    if(!sampler::App().SetDeviceOverride(static_cast<std::size_t>(n - 1), on))
    {
        std::printf("refused: overrides need maintenance mode (maint on)\n");
        return 1;
    }
    sampler::ApplyRelays();
    std::printf("device %d override: %s\n", n, pArgv[2]);
    return 0;
}

int CmdConfig(int pArgc, char** pArgv)
{
    static char sBuf[4096];
    if(pArgc == 2 && Is(pArgv[1], "get"))
    {
        const std::size_t n = sampler::ConfigJson(sBuf);
        std::printf("%.*s\n", static_cast<int>(n), sBuf);
        return 0;
    }
    if(pArgc == 2 && Is(pArgv[1], "reset"))
    {
        std::printf("%s\n", sampler::ResetConfig() ? "factory defaults restored" : "reset FAILED");
        return 0;
    }
    if(pArgc >= 3 && Is(pArgv[1], "set"))
    {
        const std::string_view json = JoinJson(pArgc, pArgv, 2, sBuf, sizeof sBuf);
        if(json.empty())
        {
            std::printf("document too long\n");
            return 1;
        }
        reefdo::config::LoadResult r;
        if(!sampler::ApplyConfigJson(json, r))
        {
            std::printf("rejected at %s: %s\n", r.path.c_str(), r.message.c_str());
            return 1;
        }
        std::printf("applied and saved\n");
        return 0;
    }
    std::printf("usage: config get | config set <json, partial ok, 'single quotes' allowed> | config reset\n");
    return 1;
}

int CmdTime(int pArgc, char** pArgv)
{
    if(pArgc == 1)
    {
        const reefdo::app::Clock c = sampler::ClockNow();
        if(c.unixS)
        {
            const int64_t l = static_cast<int64_t>(*c.unixS) + c.tzOffsetS;
            std::printf("unix %lu  local %02lld:%02lld  tz %+ld\n", static_cast<unsigned long>(*c.unixS),
                        (l % 86400) / 3600, (l % 3600) / 60, static_cast<long>(c.tzOffsetS));
        }
        else
        {
            std::printf("clock unknown (uptime %llu ms)  tz %+ld\n", static_cast<unsigned long long>(c.nowMs),
                        static_cast<long>(c.tzOffsetS));
        }
        return 0;
    }
    if(pArgc < 2 || pArgc > 3)
    {
        std::printf("usage: time [<unix seconds> [<tz offset seconds>]]\n");
        return 1;
    }
    const long unixS = std::atol(pArgv[1]);
    if(unixS < 1'600'000'000L)
    {
        std::printf("not a plausible unix time\n");
        return 1;
    }
    std::optional<int32_t> tz;
    if(pArgc == 3) tz = static_cast<int32_t>(std::atol(pArgv[2]));
    sampler::SetTime(static_cast<uint32_t>(unixS), tz);
    std::printf("clock set\n");
    return 0;
}

int CmdProbe(int pArgc, char** pArgv)
{
    if(pArgc != 2 || (!Is(pArgv[1], "sim") && !Is(pArgv[1], "rk500")))
    {
        std::printf("usage: probe sim|rk500   (takes effect after reboot)\n");
        return 1;
    }
    sampler::SetProbeSource(Is(pArgv[1], "sim") ? sampler::ProbeSource::Sim : sampler::ProbeSource::Rk500);
    std::printf("probe source %s saved; reboot to apply\n", pArgv[1]);
    return 0;
}

int CmdDebug(int pArgc, char** pArgv)
{
    if(pArgc >= 3 && Is(pArgv[1], "sim"))
    {
        sampler::Guard guard;
        reefdo::sim::Tank* tank = sampler::Tank();
        if(tank == nullptr)
        {
            std::printf("not simulating (probe rk500)\n");
            return 1;
        }
        if(pArgc == 4 && Is(pArgv[2], "sat"))
        {
            tank->SetSat(static_cast<float>(std::atof(pArgv[3])));
            std::printf("tank saturation set to %s %%\n", pArgv[3]);
            return 0;
        }
        if(pArgc == 5 && Is(pArgv[2], "k"))
        {
            const int dev = std::atoi(pArgv[3]);
            if(dev < 1 || dev > board::RELAY_COUNT) return 1;
            tank->SetDeviceK(static_cast<std::size_t>(dev - 1), static_cast<float>(std::atof(pArgv[4])));
            std::printf("device %d exchange rate %s /h\n", dev, pArgv[4]);
            return 0;
        }
        if(pArgc == 4 && Is(pArgv[2], "stall"))
        {
            tank->StallReturnPump(Is(pArgv[3], "on"));
            std::printf("return pump stall %s\n", pArgv[3]);
            return 0;
        }
    }
    if(pArgc == 3 && Is(pArgv[1], "uart"))
    {
        sampler::Uart().SetMute(Is(pArgv[2], "mute"));
        sampler::Uart().SetCorrupt(Is(pArgv[2], "corrupt"));
        std::printf("uart fault injection: %s\n", pArgv[2]);
        return 0;
    }
    if(pArgc == 2 && Is(pArgv[1], "hang"))
    {
        sampler::Hang();
        return 0;
    }
    std::printf("usage: debug sim sat <pct> | debug sim k <dev> <per_h> | debug sim stall on|off\n"
                "       debug uart mute|corrupt|ok | debug hang\n");
    return 1;
}

int CmdExport(int pArgc, char** pArgv)
{
    const uint32_t since = pArgc > 1 ? static_cast<uint32_t>(std::atol(pArgv[1])) : 0;
    StdoutSink sink;
    sampler::Guard guard;
    const std::size_t n = reefdo::api::ExportCsv(sampler::App(), since, sink);
    std::printf("# %u records\n", static_cast<unsigned>(n));
    return 0;
}

int CmdEvents(int pArgc, char** pArgv)
{
    const uint32_t since = pArgc > 1 ? static_cast<uint32_t>(std::atol(pArgv[1])) : 0;
    StdoutSink sink;
    sampler::Guard guard;
    const std::size_t n = reefdo::api::EventsCsv(sampler::App(), since, sink);
    std::printf("# %u events\n", static_cast<unsigned>(n));
    return 0;
}

int CmdBuzzer(int pArgc, char** pArgv)
{
    if(pArgc == 2 && (Is(pArgv[1], "mute") || Is(pArgv[1], "unmute")))
    {
        indicator::SetMuted(Is(pArgv[1], "mute"));
        std::printf("buzzer %sd\n", pArgv[1]);
        return 0;
    }
    if(pArgc >= 4 && Is(pArgv[1], "test"))
    {
        reefdo::config::BuzzerPattern pattern = reefdo::config::BuzzerPattern::Off;
        if(!indicator::ParseBuzzerPattern(pArgv[2], pattern))
        {
            std::printf("pattern: off|chirp|beep|double|triple|continuous\n");
            return 1;
        }
        const uint32_t volume = static_cast<uint32_t>(std::atoi(pArgv[3]));
        const uint32_t hz = pArgc > 4 ? static_cast<uint32_t>(std::atoi(pArgv[4])) : 2400;
        const uint32_t seconds = pArgc > 5 ? static_cast<uint32_t>(std::atoi(pArgv[5])) : 5;
        indicator::TestBuzzer(pattern, volume, hz, seconds);
        std::printf("playing %s at volume %lu, %lu Hz for %lu s\n", pArgv[2], static_cast<unsigned long>(volume),
                    static_cast<unsigned long>(hz), static_cast<unsigned long>(seconds));
        return 0;
    }
    const uint32_t hz = pArgc > 1 ? static_cast<uint32_t>(std::atoi(pArgv[1])) : 2000;
    const uint32_t ms = pArgc > 2 ? static_cast<uint32_t>(std::atoi(pArgv[2])) : 200;
    if(hz < 100 || hz > 10000 || ms > 5000)
    {
        std::printf("usage: buzzer [hz 100..10000] [ms <= 5000]\n");
        return 1;
    }
    board::BuzzerTone(hz, ms);
    return 0;
}

int CmdReboot(int, char**)
{
    std::printf("rebooting\n");
    std::fflush(stdout);
    esp_restart();
    return 0;
}

int CmdFactory(int pArgc, char** pArgv)
{
    if(pArgc != 2 || !Is(pArgv[1], "yes"))
    {
        std::printf("usage: factory yes   (erases config, service history, credentials; logs stay; reboots)\n");
        return 1;
    }
    hal::nvs::EraseAll();
    esp_restart();
    return 0;
}

int CmdWifi(int pArgc, char** pArgv)
{
    if(pArgc == 1 || (pArgc == 2 && Is(pArgv[1], "status")))
    {
        const net::Status s = net::GetStatus();
        std::printf("ssid '%s'  %s  ip %s  rssi %d dBm  setup AP %s  NTP %s  ntfy sent %lu failed %lu\n",
                    s.ssid.c_str(), s.connected ? "connected" : "not connected", s.ip.c_str(), s.rssi,
                    s.apActive ? "up" : "down", s.timeSynced ? "synced" : "not yet",
                    static_cast<unsigned long>(notify::Sent()), static_cast<unsigned long>(notify::Failed()));
        return 0;
    }
    if(pArgc == 2 && Is(pArgv[1], "forget"))
    {
        net::Forget();
        std::printf("credentials erased; setup AP raised\n");
        return 0;
    }
    if(pArgc == 2 || pArgc == 3)
    {
        const bool ok = net::SetCredentials(pArgv[1], pArgc == 3 ? pArgv[2] : "");
        std::printf("%s\n", ok ? "saved; connecting" : "rejected (ssid 1..32, password <= 64)");
        return ok ? 0 : 1;
    }
    std::printf("usage: wifi [status] | wifi <ssid> [password] | wifi forget\n");
    return 1;
}

int CmdPasswd(int pArgc, char** pArgv)
{
    if(pArgc != 2)
    {
        std::printf("usage: passwd <new password> | passwd reset\n");
        return 1;
    }
    const bool ok = web::SetPassword(Is(pArgv[1], "reset") ? "" : pArgv[1]);
    std::printf("%s\n", ok ? "web password updated" : "rejected (max 32)");
    return ok ? 0 : 1;
}

int CmdNtfy(int pArgc, char** pArgv)
{
    if(pArgc != 2 || !Is(pArgv[1], "test"))
    {
        std::printf("usage: ntfy test   (uses ntfy.topic from the config)\n");
        return 1;
    }
    reefdo::config::Config cfg;
    {
        sampler::Guard guard;
        cfg = sampler::App().GetConfig();
    }
    const bool ok = notify::SendTest(cfg);
    std::printf("%s\n", ok ? "sent" : "failed (topic set? network up?)");
    return ok ? 0 : 1;
}

void Add(const char* pCommand, const char* pHelp, esp_console_cmd_func_t pFunc)
{
    esp_console_cmd_t cmd = {};
    cmd.command = pCommand;
    cmd.help = pHelp;
    cmd.func = pFunc;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

} // namespace

void Start()
{
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replConfig.prompt = "reefdo>";
    replConfig.max_cmdline_length = 1024;
    replConfig.task_stack_size = 8192;
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&replConfig, &repl));

    esp_console_register_help_command();
    Add("status", "status [json]", CmdStatus);
    Add("ack", "silence the alarm for ack_silence_s", CmdAck);
    Add("maint", "maint on|off", CmdMaint);
    Add("service", "service [run|json]", CmdService);
    Add("cal", "cal air", CmdCal);
    Add("relay", "relay <1..6> on|off|auto | relay auto  (maintenance)", CmdRelay);
    Add("config", "config get | set <json> | reset", CmdConfig);
    Add("time", "time [unix [tz]]", CmdTime);
    Add("probe", "probe sim|rk500", CmdProbe);
    Add("debug", "debug sim ... | uart ... | hang", CmdDebug);
    Add("export", "export [since_seq]  (tier A as CSV)", CmdExport);
    Add("events", "events [since_seq]  (tier E as CSV)", CmdEvents);
    Add("buzzer", "buzzer test <pattern> <vol 0..3> [hz] [s] | buzzer mute|unmute | buzzer [hz] [ms]", CmdBuzzer);
    Add("reboot", "restart", CmdReboot);
    Add("factory", "factory yes", CmdFactory);
    Add("wifi", "wifi [status] | wifi <ssid> [password] | wifi forget", CmdWifi);
    Add("passwd", "passwd <new> | passwd reset", CmdPasswd);
    Add("ntfy", "ntfy test", CmdNtfy);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace console
