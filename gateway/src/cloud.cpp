#include "reefdo/gateway/cloud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace reefdo::gateway::cloud
{

// The Home app's charts are daily averages, so the trend rows draw the curves; DO and saturation are plain readouts
// (the unit in the name) and still series, for an app that charts them some day.
const std::array<ParamInfo, PARAMS> PARAM_INFO = {{
    {"DO (mg/L)", Kind::Float, false, true, "esp.ui.status", 0, 20, 0},
    {"Saturation (%)", Kind::Float, false, true, "esp.ui.status", 0, 200, 0},
    {"Temperature", Kind::Float, false, true, "esp.ui.text", 0, 50, 0, "esp.param.temperature"},
    {"Level", Kind::Text, false, false, "esp.ui.status"},
    {"Status", Kind::Text, false, false, "esp.ui.status"},
    {"Acknowledge", Kind::Bool, true, false, "esp.ui.trigger"},
    {"Suspend alert (min)", Kind::Int, true, false, "esp.ui.slider", 0, 60, 5},
    {"Last hour", Kind::Text, false, false, "esp.ui.status"},
    {"Last 12 h", Kind::Text, false, false, "esp.ui.status"},
    {"Last 24 h", Kind::Text, false, false, "esp.ui.status"},
    {"Last 7 days", Kind::Text, false, false, "esp.ui.status"},
}};

namespace
{

const char* const LEVELS[] = {"Normal", "Blue", "Yellow", "Red"};
const char* const BARS[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
const char* const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr uint32_t AGGREGATE_S = 300; // a tier-B record: one 5-minute bucket
constexpr std::array<Param, 3> SERIES = {Param::Do, Param::Saturation, Param::Temperature};
constexpr std::array<float, 3> SERIES_SCALE = {100.0f, 10.0f, 10.0f}; // 0.01 mg/L, 0.1 %, 0.1 °C

// The events worth waking someone for: an alert that needs a person, a FAULT, a failed check, a lost plug.
bool Pushed(const app::Notification& pN)
{
    switch(pN.kind)
    {
        case app::NotifyKind::Level: return pN.urgent;
        case app::NotifyKind::Fault:
        case app::NotifyKind::TestFail:
        case app::NotifyKind::DeviceLost: return true;
        default: return false;
    }
}

float Median(const FixedVector<float, BUCKET_MAX>& pV)
{
    std::array<float, BUCKET_MAX> a{};
    std::copy(pV.begin(), pV.end(), a.begin());
    std::sort(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(pV.size()));
    return a[pV.size() / 2];
}

Update Of(Param pP)
{
    Update u;
    u.param = pP;
    return u;
}

// Collects whether the router accepted the command.
class Answer final : public IResponse
{
public:
    bool ok = false;
    void Begin(Code, ContentType, const char*) override {}
    bool Write(std::string_view pChunk) override
    {
        ok = pChunk.starts_with("{\"ok\":true"); // a refusal is text or {"ok":false,...}
        return true;
    }
};

} // namespace

Text StatusLine(const app::Status& pS)
{
    Text t(pS.fault ? "FAULT" : LEVELS[static_cast<int>(pS.level)]);
    char part[32];
    auto add = [&t](std::string_view pPart)
    {
        char buf[Text::Capacity() + 1];
        const int n =
            std::snprintf(buf, sizeof buf, "%s · %.*s", t.c_str(), static_cast<int>(pPart.size()), pPart.data());
        t.assign(std::string_view(buf, std::min(static_cast<std::size_t>(n), Text::Capacity())));
    };
    if(pS.doMgl.has_value())
    {
        std::snprintf(part, sizeof part, "%.2f mg/L", static_cast<double>(*pS.doMgl));
        add(part);
    }
    else
    {
        add("no reading");
    }
    if(pS.fault) add("probe not answering");
    if(pS.suspect) add("sudden drop");
    if(pS.alertsSuspended)
    {
        std::snprintf(part, sizeof part, "suspended %u min", static_cast<unsigned>((pS.alertsSuspendLeftS + 59) / 60));
        add(part);
    }
    if(pS.silenced) add("silenced");
    if(pS.maintenance) add("maintenance");
    return t;
}

void Agent::Series(const app::Status& pS, const app::Clock& pClock, Updates& pOut, bool pOnline, bool pBack)
{
    if(!pClock.unixS.has_value()) return; // a chart point needs a time
    const uint32_t bucket = *pClock.unixS / SERIES_BUCKET_S;
    const bool closed = mBucket.has_value() && bucket != *mBucket;
    if(closed)
    {
        if(pOnline) Medians(pOut);
        for(auto& series : mSeries)
            series.clear(); // offline, the bucket is lost: the board keeps every sample itself
    }
    mBucket = bucket;
    if(pS.doMgl.has_value()) // no reading this sample: nothing to add
    {
        mSeries[0].push_back(*pS.doMgl);
        mSeries[1].push_back(pS.satPct.value_or(0.0f));
        mSeries[2].push_back(pS.tempC.value_or(0.0f));
    }
    if(pBack && !closed) Medians(pOut); // just connected: this bucket so far, not the 0 the app starts with
}

void Agent::Medians(Updates& pOut) const
{
    for(std::size_t i = 0; i < mSeries.size(); ++i)
    {
        if(mSeries[i].empty()) continue;
        Update u = Of(SERIES[i]);
        u.f = std::round(Median(mSeries[i]) * SERIES_SCALE[i]) / SERIES_SCALE[i]; // as the app shows it
        pOut.push_back(u);
    }
}

void Agent::Merge(const app::Clock& pClock, uint32_t pTs, uint32_t pDurS, float pLo, float pHi)
{
    const int64_t now = static_cast<int64_t>(*pClock.unixS) + pClock.tzOffsetS;
    const int64_t at = static_cast<int64_t>(pTs) + pClock.tzOffsetS;
    for(std::size_t r = 0; r < TRENDS; ++r)
    {
        const TrendSpec& spec = TREND_SPECS[r];
        const int64_t last = now / spec.blockS;
        const int64_t first = std::max(at / spec.blockS, last - static_cast<int64_t>(spec.blocks) + 1);
        const int64_t end = std::min((at + pDurS - 1) / spec.blockS, last);
        for(int64_t n = first; n <= end; ++n)
        {
            Block& b = mBlocks[r][static_cast<std::size_t>(n) % spec.blocks];
            if(b.n != n)
            {
                b = Block{static_cast<uint32_t>(n), pLo, pHi, pTs}; // inside the window a slot holds one block
                continue;
            }
            if(pLo < b.lo)
            {
                b.lo = pLo;
                b.loTs = pTs;
            }
            b.hi = std::max(b.hi, pHi);
        }
    }
}

TrendText Agent::Row(std::size_t pRow, const app::App& pApp, const app::Clock& pClock) const
{
    const TrendSpec& spec = TREND_SPECS[pRow];
    const ladder::Config& lc = pApp.GetConfig().ladder;
    const float bottom = lc.red.mgl - 0.5f;
    const float span = lc.blue.mgl + 2.5f - bottom;
    // The window ends with the newest sample's block: right after a boundary the new block has none yet.
    const int64_t newest = static_cast<int64_t>(*pClock.unixS) - pApp.GetConfig().samplePeriodS;
    const int64_t last = (newest + pClock.tzOffsetS) / spec.blockS;
    char buf[TrendText::Capacity() + 1];
    std::size_t len = 0;
    const Block* low = nullptr;
    float high = 0.0f;
    for(int64_t n = last - static_cast<int64_t>(spec.blocks) + 1; n <= last; ++n)
    {
        const Block& b = mBlocks[pRow][static_cast<std::size_t>(n) % spec.blocks];
        const bool seen = b.n == n;
        const long bar = std::clamp(std::lround((b.lo - bottom) / span * 7.0f), 0L, 7L);
        len += static_cast<std::size_t>(std::snprintf(buf + len, sizeof buf - len, "%s", seen ? BARS[bar] : "·"));
        if(!seen) continue;
        high = low == nullptr ? b.hi : std::max(high, b.hi);
        if(low == nullptr || b.lo < low->lo) low = &b;
    }
    TrendText t;
    if(low == nullptr)
    {
        t.assign("no reading");
        return t;
    }
    const int64_t at = static_cast<int64_t>(low->loTs) + pClock.tzOffsetS;
    const int hh = static_cast<int>(at / 3600 % 24);
    const int mm = static_cast<int>(at / 60 % 60);
    const bool days = spec.blockS * spec.blocks > 86400u; // the week names the day
    std::snprintf(buf + len, sizeof buf - len, " low %.2f %s %02d:%02d · high %.2f", static_cast<double>(low->lo),
                  days ? DAYS[(at / 86400 + 4) % 7] : "at", hh, mm, static_cast<double>(high)); // 1970-01-01: Thu
    t.assign(buf);
    return t;
}

TrendUpdates Agent::Trends(const app::App& pApp, const app::Clock& pClock, bool pOnline)
{
    TrendUpdates out;
    if(!pClock.unixS.has_value()) return out; // the rows are drawn in time
    if(!mFilled)
    {
        // After a reboot: the 5-minute buckets of the longest window, newest first.
        mFilled = true;
        const TrendSpec& week = TREND_SPECS.back();
        const int64_t from = static_cast<int64_t>(*pClock.unixS) - static_cast<int64_t>(week.blockS * week.blocks);
        const log::AggregateLog& l = pApp.LogB();
        for(std::size_t i = l.Count(); i > 0; --i)
        {
            const std::optional<log::Aggregate> a = l.At(i - 1);
            if(!a.has_value()) continue;
            if(static_cast<int64_t>(a->ts) + AGGREGATE_S <= from) break;
            if(mLiveFromTs.has_value() && a->ts + AGGREGATE_S > *mLiveFromTs) continue; // the samples are exact
            Merge(pClock, a->ts, AGGREGATE_S, a->doMin / 1000.0f, a->doMax / 1000.0f);
        }
    }
    const bool back = pOnline && !mTrendsWereOnline;
    mTrendsWereOnline = pOnline;
    if(!pOnline) return out;
    for(std::size_t r = 0; r < TRENDS; ++r)
    {
        const TrendText t = Row(r, pApp, pClock);
        const bool quietLongEnough = pClock.nowMs - mRowAtMs[r] >= static_cast<uint64_t>(TREND_MIN_S) * 1000u;
        if(!back && (t == mRowShown[r] || !quietLongEnough)) continue;
        out.push_back(TrendUpdate{TREND_SPECS[r].param, t});
        mRowShown[r] = t;
        mRowAtMs[r] = pClock.nowMs;
    }
    return out;
}

Updates Agent::Step(const app::App& pApp, const app::Clock& pClock, std::span<const app::Notification> pNotes,
                    bool pOnline)
{
    Updates out;
    const app::Status& s = pApp.GetStatus();
    const bool back = pOnline && !mWasOnline;
    mWasOnline = pOnline;
    Series(s, pClock, out, pOnline, back);
    if(s.doMgl.has_value() && pClock.unixS.has_value())
    {
        if(!mLiveFromTs.has_value()) mLiveFromTs = *pClock.unixS;
        Merge(pClock, *pClock.unixS, 1, *s.doMgl, *s.doMgl);
    }
    if(!pOnline) return out;

    const Text level(s.fault ? "FAULT" : LEVELS[static_cast<int>(s.level)]);
    if(back || !(level == mLevel))
    {
        Update u = Of(Param::Level);
        u.text = level;
        out.push_back(u);
        mLevel = level;
        mLevelShown = s.fault ? std::nullopt : std::optional<ladder::Level>(s.level);
    }

    // Back online in a standing alarm: the push it started with may never have left the board.
    const bool alarm = (s.fault || s.level >= ladder::Level::Yellow) && !s.silenced && !s.alertsSuspended;
    bool push = back && alarm;
    for(const app::Notification& n : pNotes)
        push |= Pushed(n);
    const uint32_t key = static_cast<uint32_t>(s.level) | (s.fault ? 4u : 0u) | (s.suspect ? 8u : 0u) |
                         (s.alertsSuspended ? 16u : 0u) | (s.silenced ? 32u : 0u) | (s.maintenance ? 64u : 0u);
    const Text status = StatusLine(s);
    const bool quietLongEnough = pClock.nowMs - mStatusAtMs >= static_cast<uint64_t>(STATUS_MIN_S) * 1000u;
    if(back || push || key != mStateKey || (!(status == mStatus) && quietLongEnough))
    {
        Update u = Of(Param::Status);
        u.text = status;
        u.notify = push;
        out.push_back(u);
        mStatus = status;
        mStatusAtMs = pClock.nowMs;
        mPushes += push ? 1u : 0u;
    }
    mStateKey = key;

    const int32_t minutes = s.alertsSuspended ? static_cast<int32_t>((s.alertsSuspendLeftS + 59) / 60) : 0;
    if(back || minutes != mSuspendMin)
    {
        Update u = Of(Param::Suspend);
        u.i = minutes;
        out.push_back(u);
        mSuspendMin = minutes;
    }
    if(back) out.push_back(Of(Param::Ack)); // the button reads "not pressed"
    return out;
}

Updates Agent::Write(app::App& pApp, const app::Clock& pClock, IPlatform& pPlatform, Param pParam, int32_t pValue,
                     std::span<char> pScratch)
{
    Updates out;
    char body[64];
    if(pParam == Param::Ack && pValue != 0)
        std::snprintf(body, sizeof body, "{\"ack\":true}");
    else if(pParam == Param::Suspend && pValue >= 0 && pValue <= 60)
        std::snprintf(body, sizeof body, "{\"suspend_alerts\":{\"s\":%d,\"level\":\"%s\"}}",
                      static_cast<int>(pValue * 60),
                      mLevelShown.has_value() ? config::Name(*mLevelShown) : "red"); // FAULT shown: refused anyway
    else
        body[0] = '\0'; // read-only, released, or out of range: only the report goes back

    Answer answer;
    if(body[0] != '\0')
    {
        Request r;
        r.method = Method::Post;
        r.path = "/api/cmd";
        r.body = body;
        r.who = Who::Cloud;
        Handle(pApp, pClock, pPlatform, r, answer, pScratch);
    }
    if(pParam == Param::Ack) out.push_back(Of(Param::Ack));
    if(pParam == Param::Suspend)
    {
        // Accepted: what was asked (it applies at the next sample). Refused: what holds now.
        const app::Status& s = pApp.GetStatus();
        Update u = Of(Param::Suspend);
        u.i = answer.ok ? pValue : s.alertsSuspended ? static_cast<int32_t>((s.alertsSuspendLeftS + 59) / 60) : 0;
        out.push_back(u);
        mSuspendMin = -1; // the next Step reports what really holds, whatever arrives in between
    }
    return out;
}

} // namespace reefdo::gateway::cloud
