#pragma once
// What ReefDO shows and accepts in a phone app through a cloud (ESP RainMaker Neo on the board, a fake in the
// simulator), without the cloud's SDK: a handful of parameters, their updates within a small message budget,
// and the two things the app may do — acknowledge and suspend the current alert. The board never waits for it.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "reefdo/app.hpp"
#include "reefdo/fixed_string.hpp"
#include "reefdo/fixed_vector.hpp"
#include "reefdo/gateway/router.hpp"

namespace reefdo::gateway::cloud
{

constexpr uint32_t SERIES_BUCKET_S = 300; // one chart point per series per 5 minutes: the bucket's median
constexpr uint32_t STATUS_MIN_S = 60;     // the status line changes at most this often, unless the state changes
constexpr std::size_t BUCKET_MAX = 128;   // samples per bucket kept for the median (sample periods down to 3 s)

enum class Param : uint8_t
{
    Do,          // mg/L, time series (primary)
    Saturation,  // %, time series
    Temperature, // °C, time series
    Level,       // "Normal", "Blue", "Yellow", "Red", "FAULT"
    Status,      // one line for a person; carries the phone push
    Ack,         // write true: acknowledge; reads false
    Suspend,     // minutes the current alert stays suspended; write 1..60 to suspend, 0 to resume
    Hour,        // the trend rows (Agent::Trends): the last hour, a bar per 2.5 min
    HalfDay,     // the last 12 h, a bar per 30 min
    Day,         // the last 24 h, a bar per hour
    Week,        // the last 7 days, a bar per 6 h
};
constexpr std::size_t PARAMS = 11;
constexpr std::size_t TRENDS = 4;

enum class Kind : uint8_t
{
    Bool,
    Int,
    Float,
    Text
};

// How the firmware declares a parameter; the ids are what the app shows.
struct ParamInfo
{
    const char* id;
    Kind kind;
    bool writable;
    bool timeSeries;
    const char* ui; // RainMaker UI type
    int32_t min = 0, max = 0, step = 0;
    const char* type = nullptr; // RainMaker param type, when a standard one fits (the app picks its widget by it)
};
extern const std::array<ParamInfo, PARAMS> PARAM_INFO;

using Text = FixedString<95>;

struct Update
{
    Param param;
    bool b = false;
    int32_t i = 0;
    float f = 0.0f;
    Text text;
    bool notify = false; // push to the phone (Status only)
};
using Updates = FixedVector<Update, PARAMS - TRENDS + 2>; // Step and Write never carry a trend row

// A trend row draws its window as text (the app charts only daily averages): a bar per block, its lowest DO, then
// the low with its time and the high. No quote, backslash or newline: the SDK puts strings into JSON unescaped.
struct TrendSpec
{
    Param param;
    uint32_t blockS; // seconds per bar, aligned to local time
    std::size_t blocks;
};
inline constexpr std::array<TrendSpec, TRENDS> TREND_SPECS = {{
    {Param::Hour, 150, 24},
    {Param::HalfDay, 1800, 24},
    {Param::Day, 3600, 24},
    {Param::Week, 21600, 28}, // the longest window: the refill after a reboot covers it
}};
constexpr std::size_t TREND_BLOCKS_MAX = 28;
constexpr uint32_t TREND_MIN_S = 60; // a row changes at most this often
using TrendText = FixedString<159>;  // 28 bars of 3 bytes and the numbers
static_assert(TREND_BLOCKS_MAX * 3 + 48 <= TrendText::Capacity());
static_assert(std::all_of(TREND_SPECS.begin(), TREND_SPECS.end(),
                          [](const TrendSpec& pS)
                          {
                              return pS.blocks <= TREND_BLOCKS_MAX &&
                                     pS.blockS * pS.blocks <= TREND_SPECS.back().blockS * TREND_SPECS.back().blocks;
                          }));

struct TrendUpdate
{
    Param param;
    TrendText text;
};
using TrendUpdates = FixedVector<TrendUpdate, TRENDS>;

class Agent
{
public:
    // Once per sample, with that sample's notifications, under the App's lock. pOnline: the link is up; going up
    // again sends everything once more.
    Updates Step(const app::App& pApp, const app::Clock& pClock, std::span<const app::Notification> pNotes,
                 bool pOnline);

    // A write from the app, answered like any API request (router, Who::Cloud). Returns the updates that report
    // the result back (the Ack button resets, the suspend shows its minutes).
    Updates Write(app::App& pApp, const app::Clock& pClock, IPlatform& pPlatform, Param pParam, int32_t pValue,
                  std::span<char> pScratch);

    // Every few seconds, under the App's lock: the trend rows that changed (each at most once per TREND_MIN_S, all
    // again when the link comes back). The first call with a clock fills them from the 5-minute log.
    TrendUpdates Trends(const app::App& pApp, const app::Clock& pClock, bool pOnline);

    uint32_t Pushes() const { return mPushes; }

private:
    struct Block
    {
        uint32_t n = 0; // which block it holds (local seconds / blockS); 0: none
        float lo = 0.0f;
        float hi = 0.0f;
        uint32_t loTs = 0; // when the low was seen (unix)
    };

    void Series(const app::Status& pS, const app::Clock& pClock, Updates& pOut, bool pOnline, bool pBack);
    void Medians(Updates& pOut) const; // one rounded point per series that has samples
    // DO seen from pTs for pDurS seconds, into every row's blocks inside its window.
    void Merge(const app::Clock& pClock, uint32_t pTs, uint32_t pDurS, float pLo, float pHi);
    TrendText Row(std::size_t pRow, const app::App& pApp, const app::Clock& pClock) const;

    std::array<std::array<Block, TREND_BLOCKS_MAX>, TRENDS> mBlocks{};
    std::array<TrendText, TRENDS> mRowShown;
    std::array<uint64_t, TRENDS> mRowAtMs{};
    std::optional<uint32_t> mLiveFromTs; // the first sample seen: the refill stops short of it
    bool mFilled = false;
    bool mTrendsWereOnline = false;

    bool mWasOnline = false;
    std::optional<uint32_t> mBucket;                       // start of the bucket being filled (unix / SERIES_BUCKET_S)
    std::array<FixedVector<float, BUCKET_MAX>, 3> mSeries; // DO, saturation, temperature of the bucket
    Text mLevel;
    Text mStatus;
    uint64_t mStatusAtMs = 0;
    uint32_t mStateKey = 0xFFFFFFFFu; // level, fault, suspect, suspended, silenced: a change reports at once
    int32_t mSuspendMin = -1;
    std::optional<ladder::Level> mLevelShown; // what the app shows: a suspend from it carries this (FAULT: none)
    uint32_t mPushes = 0;
};

// The status line: "Yellow · 4.31 mg/L · sudden drop · suspended 23 min".
Text StatusLine(const app::Status& pS);

} // namespace reefdo::gateway::cloud
