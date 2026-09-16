#pragma once
// The web UI's contract, without the web. The HTTP handlers only parse the URL, call these,
// and stream what comes back. JSON documents are built into caller buffers; bulk data is CSV through a sink.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/app.hpp"
#include "reefdo/config.hpp"

namespace reefdo::api
{

// Streaming output for CSV. Return false to stop (the client went away).
class ISink
{
public:
    virtual ~ISink() = default;
    virtual bool Write(std::string_view pChunk) = 0;
};

// JSON builders. Return bytes written (no terminator), 0 if the buffer is too small.
std::size_t StatusJson(const app::App& pApp, const app::Clock& pClock, std::span<char> pOut);
std::size_t ServiceJson(const app::App& pApp, std::span<char> pOut);

// PUT /api/config: parse, validate, apply. `out` receives {"ok":true} or {"ok":false,"path":..,"message":..}.
config::LoadResult ApplyConfig(app::App& pApp, std::string_view pJson, const app::Clock& pClock);
std::size_t ResultJson(const config::LoadResult& pR, std::span<char> pOut);

// POST /api/cmd, one command per document: {"ack":true} · {"maintenance":b} · {"service":"run"} · {"cal":"air"}
// · {"relay":{"device":1..6,"on":b|null}} · {"time":{"unix":s,"tz":s}}
struct Command
{
    bool ok = false;
    const char* message = "";
    std::optional<uint32_t> setUnix; // the firmware applies these to the system clock
    std::optional<int32_t> setTz;
};
Command ApplyCommand(app::App& pApp, std::string_view pJson, const app::Clock& pClock);

// Every tier-A record with seq >= since, as CSV with a header. Returns records written.
std::size_t ExportCsv(const app::App& pApp, uint32_t pSinceSeq, ISink& pSink);
// Events (tier E) with seq >= since.
std::size_t EventsCsv(const app::App& pApp, uint32_t pSinceSeq, ISink& pSink);
// Chart series: tier A "ts,do,sat,temp,level" or tier B "ts,do_min,do_avg,do_max,sat_avg,temp_min,temp_max,level_max"
// for ts in [from, to], keeping every `every`-th row (>= 1).
std::size_t SeriesCsv(const app::App& pApp, char pTier, uint32_t pFromTs, uint32_t pToTs, uint32_t pEvery,
                      ISink& pSink);

} // namespace reefdo::api
