// reefdo_sim: ReefDO on the virtual tank. The real App, the real gateway and the real web page, on this
// computer: http://localhost:8080/ (the page) and http://localhost:8080/sim (the tank's controls).
//   reefdo_sim [--port 8080] [--lan] [--speed 1] [--data reefdo-sim-data] [--fresh] [--tz seconds]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "reefdo/app.hpp"
#include "reefdo/config.hpp"
#include "reefdo/gateway/cloud.hpp"
#include "reefdo/gateway/router.hpp"
#include "reefdo/sim.hpp"
#include "reefdo/version.hpp"

#include "controls.hpp"
#include "demo.hpp"
#include "file_store.hpp"
#include "http_server.hpp"

namespace
{

namespace fs = std::filesystem;
namespace gw = reefdo::gateway;

struct Options
{
    uint16_t port = 8080;
    bool lan = false;
    double speed = 1.0;
    fs::path data = "reefdo-sim-data";
    bool fresh = false;
    std::optional<int32_t> tz;
};

std::optional<Options> ParseArgs(int pArgc, char** pArgv)
{
    Options o;
    for(int i = 1; i < pArgc; ++i)
    {
        const std::string a = pArgv[i];
        const bool hasValue = i + 1 < pArgc;
        if(a == "--port" && hasValue)
            o.port = static_cast<uint16_t>(std::atoi(pArgv[++i]));
        else if(a == "--lan")
            o.lan = true;
        else if(a == "--speed" && hasValue)
            o.speed = std::atof(pArgv[++i]);
        else if(a == "--data" && hasValue)
            o.data = pArgv[++i];
        else if(a == "--fresh")
            o.fresh = true;
        else if(a == "--tz" && hasValue)
            o.tz = std::atoi(pArgv[++i]);
        else
            return std::nullopt;
    }
    return o;
}

std::string ReadFile(const fs::path& pPath)
{
    std::ifstream in(pPath, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool WriteFile(const fs::path& pPath, std::string_view pData)
{
    std::ofstream out(pPath, std::ios::binary | std::ios::trunc);
    out.write(pData.data(), static_cast<std::streamsize>(pData.size()));
    return static_cast<bool>(out);
}

int32_t LocalOffsetS()
{
    try
    {
        return static_cast<int32_t>(
            std::chrono::current_zone()->get_info(std::chrono::system_clock::now()).offset.count());
    }
    catch(...)
    {
        return 0;
    }
}

const char* NotifyName(reefdo::app::NotifyKind pK)
{
    static const char* const NAMES[] = {"level",     "fault",     "fault cleared",
                                        "recovered", "test FAIL", "test inconclusive",
                                        "boot",      "plug lost", "plug back"};
    return NAMES[static_cast<int>(pK)];
}

// The tank's controls as a page: buttons for the commands in controls.cpp.
const char* const CONTROL_PAGE = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ReefDO simulator</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1b1f24;--muted:#667;--line:#dde1e6;--accent:#0a6cff}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--card:#1a1f25;--text:#e6e9ee;--muted:#8a94a0;--line:#2a313a;--accent:#4c8dff}}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif;padding:16px}
main{max-width:760px;margin:auto}h1{font-size:18px;margin:0 0 4px}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:12px 0}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 8px}
button{font:inherit;padding:6px 10px;margin:3px 4px 3px 0;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--text);cursor:pointer}
button:hover{border-color:var(--accent)}input{font:inherit;padding:6px 8px;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--text);width:min(420px,100%)}
pre{white-space:pre-wrap;margin:0;font:12.5px/1.45 ui-monospace,Consolas,monospace}a{color:var(--accent)}</style></head><body><main>
<h1>ReefDO simulator</h1><div style="color:var(--muted)">The virtual tank behind <a href="/" target="_blank">the ReefDO page</a>. Every button is a command; the box takes any (try <code>help</code>).</div>
<div class="card"><h2>Now</h2><pre id="st">…</pre></div>
<div class="card"><h2>Time</h2><button data-c="speed 1">real time</button><button data-c="speed 60">1 min / s</button><button data-c="speed 600">10 min / s</button><button data-c="speed 0">pause</button>
<button data-c="skip 10">+10 min</button><button data-c="skip 60">+1 h</button><button data-c="until 21:00">until 21:00</button><button data-c="until 03:00">until 03:00</button><button data-c="until 08:00">until 08:00</button></div>
<div class="card"><h2>False alarms</h2><button data-c="snail 30 15">snail on the probe (−30 %, 15 min)</button><button data-c="snail 45 40">big snail (−45 %, 40 min)</button><button data-c="bubble 10 20">bubble (+10 %, 20 min)</button></div>
<div class="card"><h2>Real trouble</h2><button data-c="drop 72 20">tank falls to 72 % (20 min)</button><button data-c="drop 55 30">tank falls to 55 % (30 min)</button><button data-c="sat 98">tank back to 98 %</button><button data-c="stall on">return pump stalls</button><button data-c="stall off">…runs again</button><button data-c="night 30">heavy night demand</button><button data-c="night 14">normal night demand</button><button data-c="k 3 0">air pump broken</button><button data-c="k 3 6">air pump fixed</button></div>
<div class="card"><h2>Probe and cloud</h2><button data-c="probe dropout">probe silent</button><button data-c="probe stuck">probe frozen</button><button data-c="probe garbage">probe nonsense</button><button data-c="probe ok">probe fine</button><button data-c="cloud down">cloud down</button><button data-c="cloud up">cloud up</button></div>
<div class="card"><h2>The phone app</h2><button data-c="phone ack">acknowledge</button><button data-c="phone suspend 30">suspend alert 30 min</button><button data-c="phone suspend 0">resume</button></div>
<div class="card"><h2>Command</h2><input id="cmd" placeholder="e.g. snail 40 20" autocomplete="off"><pre id="out" style="margin-top:8px"></pre></div>
<script>
const $=id=>document.getElementById(id);
async function run(c){ const r=await fetch('/sim/cmd',{method:'POST',body:c}); $('out').textContent=await r.text(); status(); }
async function status(){ try{ const r=await fetch('/sim/cmd',{method:'POST',body:'status'}); $('st').textContent=await r.text(); }catch(e){ $('st').textContent='simulator not running'; } }
document.querySelectorAll('button[data-c]').forEach(b=>b.onclick=()=>run(b.dataset.c));
$('cmd').onkeydown=e=>{ if(e.key==='Enter'&&e.target.value.trim()){ run(e.target.value); e.target.value=''; } };
status(); setInterval(status,2000);
</script></main></body></html>)HTML";

// The simulator's side of the API: files instead of NVS, no network, no buzzer.
class Platform final : public gw::IPlatform
{
public:
    Platform(reefdo::app::App& pApp, reefdo::app::Clock& pClock, fs::path pData)
        : mApp(pApp)
        , mClock(pClock)
        , mData(std::move(pData))
    {
        mPassword = ReadFile(mData / "password.txt");
        if(mPassword.empty()) mPassword = "reefdo";
    }
    const std::string& Password() const { return mPassword; }
    std::string configState = "defaults";
    std::string cloudState = "online";
    uint32_t cloudPushes = 0;

    gw::SysInfo Sys() override
    {
        gw::SysInfo s;
        s.ssid.assign("(simulator)");
        s.connected = true;
        s.ip.assign("127.0.0.1");
        s.timeSynced = true;
        s.partition.assign("sim");
        s.image.assign("valid");
        s.probe.assign("sim");
        s.muted = mMuted;
        s.config.assign(configState);
        s.cloud.assign(cloudState);
        s.cloudPushes = cloudPushes;
        return s;
    }
    reefdo::config::LoadResult ApplyConfig(std::string_view pJson) override
    {
        const reefdo::config::LoadResult r = gw::ApplyConfig(mApp, pJson, mClock);
        if(r.Ok())
        {
            std::vector<char> buf(reefdo::config::DOC_MAX);
            const std::size_t n = reefdo::config::Write(mApp.GetConfig(), buf);
            WriteFile(mData / "config.json", std::string_view(buf.data(), n));
            configState = "stored";
        }
        return r;
    }
    void SetTime(uint32_t, std::optional<int32_t> pTz) override
    {
        if(pTz.has_value()) mClock.tzOffsetS = *pTz;
        std::printf("[sim] the page sent its clock; the simulator keeps its own time (zone taken)\n");
    }
    void OutputsChanged() override {}
    bool SetWifi(std::string_view pSsid, std::string_view) override
    {
        std::printf("[sim] Wi-Fi '%.*s' accepted (not used)\n", static_cast<int>(pSsid.size()), pSsid.data());
        return !pSsid.empty() && pSsid.size() <= 32;
    }
    bool SetPassword(std::string_view pPassword) override
    {
        mPassword = pPassword.empty() ? "reefdo" : std::string(pPassword);
        return WriteFile(mData / "password.txt", pPassword);
    }
    void SetMuted(bool pMuted) override { mMuted = pMuted; }
    gw::CloudLink StartCloudLink() override
    {
        gw::CloudLink l;
        l.message.assign("the simulator has a fake cloud: use phone ack / phone suspend");
        return l;
    }
    void TestBuzzer(reefdo::config::BuzzerPattern pP, uint32_t pVolume, uint32_t pHz, uint32_t pS) override
    {
        std::printf("[sim] buzzer test: %s, volume %u, %u Hz, %u s\n", reefdo::config::Name(pP), pVolume, pHz, pS);
    }

private:
    reefdo::app::App& mApp;
    reefdo::app::Clock& mClock;
    fs::path mData;
    std::string mPassword;
    bool mMuted = false;
};

class Collect final : public gw::IResponse
{
public:
    sim::HttpReply reply;
    void Begin(gw::Code pCode, gw::ContentType pType, const char* pFilename) override
    {
        reply.status = static_cast<int>(pCode);
        reply.type = pType == gw::ContentType::Json  ? "application/json"
                     : pType == gw::ContentType::Csv ? "text/csv"
                                                     : "text/plain";
        if(pFilename != nullptr) reply.disposition = std::string("attachment; filename=\"") + pFilename + "\"";
    }
    bool Write(std::string_view pChunk) override
    {
        reply.body.append(pChunk);
        return true;
    }
};

// Lines typed into the terminal, handed to the main loop.
class Console
{
public:
    void Start()
    {
        std::thread(
            [this]
            {
                std::string line;
                while(std::getline(std::cin, line))
                {
                    const std::lock_guard<std::mutex> lock(mMutex);
                    mLines.push_back(line);
                }
            })
            .detach();
    }
    std::vector<std::string> Take()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        std::vector<std::string> out;
        out.swap(mLines);
        return out;
    }

private:
    std::mutex mMutex;
    std::vector<std::string> mLines;
};

} // namespace

int main(int pArgc, char** pArgv)
{
    const std::optional<Options> opt = ParseArgs(pArgc, pArgv);
    if(!opt.has_value())
    {
        std::printf("usage: reefdo_sim [--port 8080] [--lan] [--speed 1] [--data dir] [--fresh] [--tz seconds]\n");
        return 2;
    }
    std::error_code ec;
    if(opt->fresh) fs::remove_all(opt->data, ec);
    fs::create_directories(opt->data, ec);

    // The board's log geometry: 64 KB segments for the big rings, 16 KB for the small ones.
    sim::FileStore a((opt->data / "loga.bin").string(), 80, 64 * 1024);
    sim::FileStore b((opt->data / "logb.bin").string(), 64, 64 * 1024);
    sim::FileStore e((opt->data / "loge.bin").string(), 16, 16 * 1024);
    sim::FileStore d((opt->data / "logd.bin").string(), 16, 16 * 1024);
    if(!a.Ok() || !b.Ok() || !e.Ok() || !d.Ok())
    {
        std::printf("cannot write the log files in %s\n", opt->data.string().c_str());
        return 1;
    }

    reefdo::config::Config cfg = sim::DemoConfig();
    std::string configState = "defaults";
    const std::string stored = ReadFile(opt->data / "config.json");
    if(!stored.empty())
    {
        const reefdo::config::LoadResult r = reefdo::config::Load(stored, cfg);
        configState =
            r.Ok() ? "stored" : "rejected at " + std::string(r.path.view()) + ": " + std::string(r.message.view());
        if(!r.Ok()) cfg = sim::DemoConfig();
    }

    const auto wallNow = std::chrono::system_clock::now();
    reefdo::app::Clock clock;
    clock.nowMs = 5'000;
    clock.unixS =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(wallNow.time_since_epoch()).count());
    clock.tzOffsetS = opt->tz.value_or(LocalOffsetS());

    reefdo::sim::TankConfig tankCfg = sim::DemoTank();
    tankCfg.salinityPsu = cfg.salinityPsu;
    reefdo::sim::Tank tank(tankCfg);
    reefdo::sim::ProbeModel pm;
    pm.dtS = static_cast<float>(cfg.samplePeriodS);
    reefdo::sim::Probe probe(tank, pm, static_cast<uint64_t>(wallNow.time_since_epoch().count()) | 1u);

    reefdo::app::App app(cfg, probe, {a, b, e, d});
    const std::string testBlob = ReadFile(opt->data / "test.bin");
    if(testBlob.size() == sizeof(reefdo::selftest::Persistent))
    {
        reefdo::selftest::Persistent p;
        std::memcpy(&p, testBlob.data(), sizeof p);
        app.RestoreTest(p);
    }
    app.Start(clock, 1);

    Platform platform(app, clock, opt->data);
    platform.configState = configState;
    sim::World world{tank, probe, app, clock};
    world.speed = opt->speed;

    // The fake cloud: the gateway's cloud agent, and what a phone app would show from its updates.
    namespace cl = reefdo::gateway::cloud;
    cl::Agent agent;
    std::array<std::string, cl::PARAMS> phone;
    auto Render = [&]
    {
        world.cloudView = "Level " + phone[static_cast<std::size_t>(cl::Param::Level)] + " · DO (5 min) " +
                          phone[static_cast<std::size_t>(cl::Param::Do)] + " · suspend " +
                          phone[static_cast<std::size_t>(cl::Param::Suspend)] + " min · \"" +
                          phone[static_cast<std::size_t>(cl::Param::Status)] + "\" · pushes " +
                          std::to_string(agent.Pushes());
        for(const cl::TrendSpec& t : cl::TREND_SPECS)
        {
            const std::size_t p = static_cast<std::size_t>(t.param);
            world.cloudView += "\n  " + std::string(cl::PARAM_INFO[p].id) + ": " + phone[p];
        }
    };
    auto ShowTrends = [&](const cl::TrendUpdates& pU)
    {
        for(const cl::TrendUpdate& u : pU)
            phone[static_cast<std::size_t>(u.param)] = u.text.c_str();
        if(!pU.empty()) Render();
    };
    auto Show = [&](const cl::Updates& pU)
    {
        for(const cl::Update& u : pU)
        {
            const cl::ParamInfo& info = cl::PARAM_INFO[static_cast<std::size_t>(u.param)];
            char v[128];
            if(info.kind == cl::Kind::Float)
                std::snprintf(v, sizeof v, "%.2f", static_cast<double>(u.f));
            else if(info.kind == cl::Kind::Int)
                std::snprintf(v, sizeof v, "%d", static_cast<int>(u.i));
            else if(info.kind == cl::Kind::Bool)
                std::snprintf(v, sizeof v, "%s", u.b ? "on" : "off");
            else
                std::snprintf(v, sizeof v, "%s", u.text.c_str());
            phone[static_cast<std::size_t>(u.param)] = v;
            if(u.notify) std::printf("[phone] PUSH: %s\n", u.text.c_str());
        }
        Render();
    };

    sim::HttpServer server;
    if(!server.Listen(opt->port, opt->lan))
    {
        std::printf("cannot listen on port %u (in use?) — try --port\n", opt->port);
        return 1;
    }
    std::printf("ReefDO %s simulator\n  page      http://localhost:%u/\n  controls  http://localhost:%u/sim\n"
                "  data      %s (config %s)\n  speed     %gx — type help\n",
                reefdo::VERSION, opt->port, opt->port, fs::absolute(opt->data).string().c_str(), configState.c_str(),
                world.speed);

    const fs::path www = fs::path(REEFDO_SOURCE_DIR) / "firmware" / "main" / "www";
    const fs::path assets = fs::path(REEFDO_SOURCE_DIR) / "assets";
    std::vector<char> scratch(gw::JSON_MAX);
    world.cloudWrite = [&](int pParam, int pValue) -> std::string
    {
        if(!world.cloudUp) return "the cloud is down: the phone cannot reach the board\n";
        const cl::Updates u = agent.Write(app, clock, platform, static_cast<cl::Param>(pParam), pValue, scratch);
        Show(u);
        const bool suspend = static_cast<cl::Param>(pParam) == cl::Param::Suspend;
        return !suspend                         ? "acknowledged from the phone\n"
               : pValue == 0                    ? "resume sent from the phone (if an alert was suspended)\n"
               : !u.empty() && u[0].i == pValue ? "suspend from the phone accepted (applies at the next sample)\n"
                                                : "suspend refused: no alert, a FAULT, or worse than the app shows\n";
    };

    auto handler = [&](const sim::HttpRequest& pReq) -> sim::HttpReply
    {
        if(pReq.method == "OPTIONS") return {204, "text/plain", "", "no-store", ""};
        if(pReq.path == "/") return {200, "text/html; charset=utf-8", "", "no-cache", ReadFile(www / "index.html")};
        if(pReq.path == "/logo.svg")
            return {200, "image/svg+xml", "", "max-age=86400", ReadFile(assets / "logo_ReefDO.svg")};
        if(pReq.path == "/apple-touch-icon.png")
            return {200, "image/png", "", "max-age=86400", ReadFile(www / "apple-touch-icon.png")};
        if(pReq.path == "/sim") return {200, "text/html; charset=utf-8", "", "no-cache", CONTROL_PAGE};
        if(pReq.path == "/sim/cmd" && pReq.method == "POST")
            return {200, "text/plain", "", "no-store", sim::Execute(world, pReq.body)};
        if(pReq.path == "/api/ota") return {400, "text/plain", "", "no-store", "OTA is not simulated"};
        if(pReq.path.rfind("/api/", 0) != 0) return {404, "text/plain", "", "no-store", "not found"};
        gw::Request r;
        r.method = pReq.method == "PUT" ? gw::Method::Put : pReq.method == "POST" ? gw::Method::Post : gw::Method::Get;
        r.path = pReq.path;
        r.query = pReq.query;
        r.body = pReq.body;
        r.who = gw::Authorise(pReq.authorization, platform.Password());
        Collect out;
        gw::Handle(app, clock, platform, r, out, scratch);
        return out.reply;
    };

    auto sample = [&]
    {
        sim::BeforeSample(world);
        const int64_t local = static_cast<int64_t>(*clock.unixS) + clock.tzOffsetS;
        const uint16_t minute = static_cast<uint16_t>(((local % 86400 + 86400) % 86400) / 60);
        const uint32_t period = app.GetConfig().samplePeriodS;
        tank.Step(static_cast<float>(period), minute, app.GetStatus().deviceOn);
        sim::AfterTankStep(world);
        app.Tick(clock);
        const reefdo::app::Notifications notes = app.TakeNotifications();
        for(const reefdo::app::Notification& n : notes)
            std::printf("[event] %s%s: level %d device %u value %.2f\n", NotifyName(n.kind),
                        n.repeat ? " (repeat)" : "", static_cast<int>(n.level), n.device + 1u,
                        static_cast<double>(n.value));
        Show(agent.Step(app, clock, std::span<const reefdo::app::Notification>(notes.begin(), notes.size()),
                        world.cloudUp));
        ShowTrends(agent.Trends(app, clock, world.cloudUp));
        platform.cloudState = world.cloudUp ? "online (fake)" : "offline (fake)";
        platform.cloudPushes = agent.Pushes();
        if(app.TestPersistentChanged())
        {
            const reefdo::selftest::Persistent& p = app.TestPersistent();
            WriteFile(opt->data / "test.bin", std::string_view(reinterpret_cast<const char*>(&p), sizeof p));
        }
        clock.nowMs += static_cast<uint64_t>(period) * 1000u;
        *clock.unixS += period;
    };

    Console console;
    console.Start();
    auto lastReal = std::chrono::steady_clock::now();
    double pendingMs = 0.0;
    while(!world.quit)
    {
        for(const std::string& line : console.Take())
            std::fputs(sim::Execute(world, line).c_str(), stdout);

        const auto nowReal = std::chrono::steady_clock::now();
        pendingMs += std::chrono::duration<double, std::milli>(nowReal - lastReal).count() * world.speed;
        lastReal = nowReal;
        const double periodMs = app.GetConfig().samplePeriodS * 1000.0;
        for(int n = 0; n < 2000; ++n) // bounded, so the page stays answered while it races
        {
            if(world.untilMinute.has_value())
            {
                sample();
                const int64_t local = static_cast<int64_t>(*clock.unixS) + clock.tzOffsetS;
                if(((local % 86400 + 86400) % 86400) / 60 == *world.untilMinute)
                {
                    world.untilMinute.reset();
                    std::fputs(sim::Summary(world).c_str(), stdout);
                }
            }
            else if(world.skipTicks > 0)
            {
                sample();
                if(--world.skipTicks == 0) std::fputs(sim::Summary(world).c_str(), stdout);
            }
            else if(pendingMs >= periodMs)
            {
                sample();
                pendingMs -= periodMs;
            }
            else
            {
                break;
            }
        }
        if(world.speed == 0.0) pendingMs = 0.0;
        const bool racing = world.untilMinute.has_value() || world.skipTicks > 0;
        const double waitMs = world.speed > 0.0 ? (periodMs - pendingMs) / world.speed : 50.0;
        server.Poll(racing ? 0 : static_cast<int>(std::clamp(waitMs, 1.0, 50.0)), handler);
    }
    return 0;
}
