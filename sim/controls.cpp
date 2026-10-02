#include "controls.hpp"

#include <cstdio>
#include <sstream>
#include <vector>

#include "reefdo/config.hpp"
#include "reefdo/gateway/cloud.hpp"

namespace sim
{

namespace
{

std::vector<std::string> Words(const std::string& pLine)
{
    std::istringstream in(pLine);
    std::vector<std::string> w;
    std::string s;
    while(in >> s)
        w.push_back(s);
    return w;
}

bool Number(const std::string& pS, double& pOut)
{
    char* end = nullptr;
    pOut = std::strtod(pS.c_str(), &end);
    return end != pS.c_str() && *end == '\0';
}

std::string Fmt(const char* pFormat, double pA, double pB = 0.0)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, pFormat, pA, pB);
    return buf;
}

uint16_t LocalMinute(const reefdo::app::Clock& pC)
{
    const int64_t l = static_cast<int64_t>(pC.unixS.value_or(0)) + pC.tzOffsetS;
    return static_cast<uint16_t>(((l % 86400 + 86400) % 86400) / 60);
}

} // namespace

std::string Help()
{
    return "status                     values, level, devices, simulated time\n"
           "speed <x>                  simulated seconds per real second (0 pauses; e.g. 60, 600)\n"
           "skip <minutes>             run that long at once\n"
           "until <HH:MM>              run at once until the local clock reads that\n"
           "sat <pct>                  set the tank's saturation now (a step: the ladder calls that a sudden drop)\n"
           "drop <pct> <minutes>       the tank falls to pct over that time: real trouble, as a tank has it\n"
           "snail <pct> <minutes>      something on the probe: it reads pct lower, then clears\n"
           "bubble <pct> <minutes>     a bubble on the cap: it reads pct higher, then clears\n"
           "probe ok|dropout|stuck|garbage|crc <n>   probe faults (crc: every nth poll)\n"
           "stall on|off               the return pump stops moving water (a power cycle clears it)\n"
           "k <device> <per_h>         how strongly a device moves oxygen\n"
           "night <pct_per_h>          night-time oxygen demand (default 14)\n"
           "cloud up|down              the fake cloud link\n"
           "phone ack                  acknowledge from the phone app\n"
           "phone suspend <min>        suspend the alert from the phone app (0 resumes)\n"
           "help · quit\n";
}

std::string Summary(const World& pW)
{
    const reefdo::app::Status& s = pW.app.GetStatus();
    const char* const levels[] = {"Normal", "Blue", "Yellow", "Red"};
    std::string out;
    char buf[256];
    const uint16_t m = LocalMinute(pW.clock);
    std::snprintf(buf, sizeof buf, "sim time %02u:%02u  speed %gx%s\n", m / 60u, m % 60u, pW.speed,
                  pW.cloudUp ? "" : "  cloud DOWN");
    out += buf;
    std::snprintf(buf, sizeof buf, "tank %.1f %% sat  %.2f mg/L  %.1f C    probe reads %.2f mg/L  %.1f %%\n",
                  static_cast<double>(pW.tank.SatPct()), static_cast<double>(pW.tank.DoMgl()),
                  static_cast<double>(pW.tank.TempC()), static_cast<double>(s.doMgl.value_or(0.0f)),
                  static_cast<double>(s.satPct.value_or(0.0f)));
    out += buf;
    std::snprintf(buf, sizeof buf, "level %s%s%s%s%s  slope %.2f / 2 min %.2f mg/L per 10 min\n",
                  levels[static_cast<int>(s.level)], s.fault ? "  FAULT" : "", s.silenced ? "  silenced" : "",
                  s.suspect ? "  sudden drop" : "", s.alertsSuspended ? "  ALERT SUSPENDED" : "",
                  static_cast<double>(s.slopeMglPer10min), static_cast<double>(s.fastSlopeMglPer10min));
    out += buf;
    out += "devices on:";
    const reefdo::config::Config& cfg = pW.app.GetConfig();
    bool any = false;
    for(std::size_t i = 0; i < reefdo::DEVICES; ++i)
    {
        if(!s.deviceOn[i] || cfg.devices[i].slot.GetKind() == reefdo::slot::Kind::None) continue;
        out += " " + std::to_string(i + 1) + "·" + std::string(cfg.devices[i].name.view());
        any = true;
    }
    out += any ? "\n" : " none\n";
    if(!pW.cloudView.empty()) out += "phone app: " + pW.cloudView + "\n";
    return out;
}

void BeforeSample(World& pW)
{
    if(pW.offsetUntilMs.has_value() && pW.clock.nowMs >= *pW.offsetUntilMs)
    {
        pW.probe.Model().bubblePct = 0.0f;
        pW.offsetUntilMs.reset();
        std::printf("[sim] the probe is clear again\n");
    }
}

void AfterTankStep(World& pW)
{
    if(pW.dropTo.has_value())
    {
        // The fall overrides the tank's own balance (and the devices) while it lasts, one sample at a time.
        const float step = pW.dropPerS * static_cast<float>(pW.app.GetConfig().samplePeriodS);
        const float next = pW.dropAt - step;
        pW.dropAt = next;
        pW.tank.SetSat(next > *pW.dropTo ? next : *pW.dropTo);
        if(next <= *pW.dropTo)
        {
            pW.dropTo.reset();
            std::printf("[sim] the tank has fallen to its target; its own balance takes over again\n");
        }
    }
}

std::string Execute(World& pW, const std::string& pLine)
{
    const std::vector<std::string> w = Words(pLine);
    if(w.empty()) return "";
    const std::string& c = w[0];
    double a = 0.0;
    double b = 0.0;
    reefdo::sim::ProbeModel& pm = pW.probe.Model();

    if(c == "help") return Help();
    if(c == "status") return Summary(pW);
    if(c == "quit" || c == "exit")
    {
        pW.quit = true;
        return "bye\n";
    }
    if(c == "speed" && w.size() == 2 && Number(w[1], a) && a >= 0.0 && a <= 100000.0)
    {
        pW.speed = a;
        return a == 0.0 ? "paused\n" : Fmt("%g simulated seconds per second\n", a);
    }
    if(c == "skip" && w.size() == 2 && Number(w[1], a) && a > 0.0 && a <= 60.0 * 24 * 14)
    {
        const uint32_t period = pW.app.GetConfig().samplePeriodS;
        pW.skipTicks = static_cast<uint64_t>(a * 60.0 / period);
        return Fmt("running %g minutes\n", a);
    }
    if(c == "until" && w.size() == 2)
    {
        const std::optional<uint16_t> m = reefdo::config::ParseHhmm(w[1]);
        if(!m.has_value()) return "until HH:MM\n";
        pW.untilMinute = m;
        return "running until " + w[1] + "\n";
    }
    if(c == "drop" && w.size() == 3 && Number(w[1], a) && Number(w[2], b) && a >= 0.0 && a < pW.tank.SatPct() &&
       b > 0.0 && b <= 600.0)
    {
        pW.dropTo = static_cast<float>(a);
        pW.dropAt = pW.tank.SatPct();
        pW.dropPerS = static_cast<float>((pW.tank.SatPct() - a) / (b * 60.0));
        return Fmt("the tank falls to %.0f %% over %.0f min\n", a, b);
    }
    if(c == "sat" && w.size() == 2 && Number(w[1], a) && a >= 0.0 && a <= 150.0)
    {
        pW.tank.SetSat(static_cast<float>(a));
        return Fmt("tank at %.1f %% saturation\n", a);
    }
    if((c == "snail" || c == "bubble") && w.size() == 3 && Number(w[1], a) && Number(w[2], b) && a > 0.0 &&
       a <= 100.0 && b > 0.0 && b <= 600.0)
    {
        pm.bubblePct = static_cast<float>(c == "snail" ? -a : a);
        pW.offsetUntilMs = pW.clock.nowMs + static_cast<uint64_t>(b * 60000.0);
        return Fmt(c == "snail" ? "the probe reads %.0f %% low for %.0f min\n"
                                : "the probe reads %.0f %% high for %.0f min\n",
                   a, b);
    }
    if(c == "probe" && w.size() >= 2)
    {
        const std::string& m = w[1];
        pm.dropout = m == "dropout";
        pm.stuck = m == "stuck";
        pm.implausible = m == "garbage";
        pm.crcErrorEvery = 0;
        if(m == "crc" && w.size() == 3 && Number(w[2], a) && a >= 1.0) pm.crcErrorEvery = static_cast<uint32_t>(a);
        if(m == "ok" || m == "dropout" || m == "stuck" || m == "garbage" || pm.crcErrorEvery > 0)
            return "probe " + m + "\n";
        return "probe ok|dropout|stuck|garbage|crc <n>\n";
    }
    if(c == "stall" && w.size() == 2 && (w[1] == "on" || w[1] == "off"))
    {
        pW.tank.StallReturnPump(w[1] == "on");
        return w[1] == "on" ? "the return pump is stalled (powered, not moving water)\n" : "the return pump runs\n";
    }
    if(c == "k" && w.size() == 3 && Number(w[1], a) && Number(w[2], b) && a >= 1.0 && a <= reefdo::DEVICES &&
       b >= 0.0 && b <= 100.0)
    {
        pW.tank.SetDeviceK(static_cast<std::size_t>(a) - 1, static_cast<float>(b));
        return Fmt("device %.0f moves oxygen at %.2f per hour\n", a, b);
    }
    if(c == "night" && w.size() == 2 && Number(w[1], a) && a >= 0.0 && a <= 100.0)
    {
        pW.tank.SetNightRespiration(static_cast<float>(a));
        return Fmt("night-time demand %.1f %% per hour\n", a);
    }
    if(c == "phone" && pW.cloudWrite && w.size() >= 2)
    {
        using reefdo::gateway::cloud::Param;
        if(w[1] == "ack" && w.size() == 2) return pW.cloudWrite(static_cast<int>(Param::Ack), 1);
        if(w[1] == "suspend" && w.size() == 3 && Number(w[2], a))
            return pW.cloudWrite(static_cast<int>(Param::Suspend), static_cast<int>(a));
    }
    if(c == "cloud" && w.size() == 2 && (w[1] == "up" || w[1] == "down"))
    {
        pW.cloudUp = w[1] == "up";
        return pW.cloudUp ? "cloud link up\n" : "cloud link down\n";
    }
    return "unknown or incomplete command; try help\n";
}

} // namespace sim
