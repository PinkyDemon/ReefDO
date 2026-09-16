#pragma once
// Everything on flash: fixed-size CRC'd records (Record 40 B: tiers A/E/D, Aggregate 24 B: tier B) in
// sliding-window rings of flash segments (README "Log rings").
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "reefdo/ladder.hpp"

namespace reefdo::log
{

enum class Type : uint8_t
{
    Measurement = 1,
    Event = 2, // ladder / service events; `aux` carries the event code, `flags` the detail
    Boot = 3,
    Fault = 4,
    Config = 5,
    Correction = 6, // f0 = scale, f1 = offset
    Command = 7,
    TimeSync = 8, // f0 = offset applied (s), aux = source
    Daily = 9,    // f0 do_min, f1 do_avg, f2 do_max, f3 temp_avg; aux = minute_of_min | level_max << 16 | events << 24
    Service = 10, // aux = service event code | device << 8 | outcome << 16; f0 = response
};

constexpr std::size_t RECORD_SIZE = 40;
constexpr std::size_t AGGREGATE_SIZE = 24;
constexpr std::size_t MAX_SEGMENTS = 128; // a store may not have more segments than this

struct Record
{
    uint32_t seq = 0;
    uint32_t ts = 0; // unix seconds, 0 = clock unknown when written
    uint32_t uptimeS = 0;
    Type type = Type::Measurement;
    uint8_t level = 0; // effective ladder level
    uint16_t flags = 0;
    float f0 = 0.0f; // Measurement: do (corrected, mg/L)
    float f1 = 0.0f; // Measurement: saturation %
    float f2 = 0.0f; // Measurement: temperature °C
    float f3 = 0.0f; // Measurement: slope mg/L per 10 min
    uint32_t aux = 0;
    bool operator==(const Record&) const = default;
};

struct Aggregate
{
    uint32_t ts = 0;    // start of the 5-minute bucket
    uint16_t doMin = 0; // mg/L × 1000
    uint16_t doAvg = 0;
    uint16_t doMax = 0;
    uint16_t satAvg = 0; // % × 100
    int16_t tempMin = 0; // °C × 100
    int16_t tempMax = 0;
    uint8_t levelMax = 0;
    uint8_t samples = 0;
    bool operator==(const Aggregate&) const = default;
};

void Encode(const Record& pR, std::span<uint8_t, RECORD_SIZE> pOut);
std::optional<Record> Decode(std::span<const uint8_t, RECORD_SIZE> pIn); // nullopt: CRC mismatch or erased
void Encode(const Aggregate& pA, std::span<uint8_t, AGGREGATE_SIZE> pOut);
std::optional<Aggregate> Decode(std::span<const uint8_t, AGGREGATE_SIZE> pIn);

// One line of CSV for a record ("seq,ts,uptime_s,type,level,flags,f0,f1,f2,f3,aux\n"); returns bytes written, 0 if it
// doesn't fit.
std::size_t CsvHeader(std::span<char> pOut);
std::size_t CsvLine(const Record& pR, std::span<char> pOut);

// N equal segments of raw NOR flash (a partition on the board, vectors in tests): writes clear bits in
// place, Erase() returns a segment to 0xFF, and either can be cut short by a power loss.
class IBlockStore
{
public:
    virtual ~IBlockStore() = default;
    virtual std::size_t Segments() const = 0;
    virtual std::size_t SegmentBytes() const = 0;
    virtual bool Read(std::size_t pSegment, std::size_t pOffset, std::span<uint8_t> pOut) = 0;
    virtual bool Write(std::size_t pSegment, std::size_t pOffset, std::span<const uint8_t> pData) = 0;
    virtual bool Erase(std::size_t pSegment) = 0;
};

// Ring of fixed-size records over an IBlockStore; slot 0 of each segment is an epoch header, records follow.
// Power-loss rules (sealing, partial tails, erase-before-use) are in the README; record size >= HEADER_BYTES.
class Ring
{
public:
    using Validator = bool (*)(std::span<const uint8_t>);
    static constexpr std::size_t HEADER_BYTES = 10;

    Ring(IBlockStore& pStore, std::size_t pRecordSize, Validator pValid);

    void Open();                                   // recover from whatever is on the store
    bool Append(std::span<const uint8_t> pRecord); // record_size bytes; drops the oldest segment when full
    std::size_t Count() const { return mCount; }
    std::size_t Capacity() const { return mPerSegment * mStore.Segments(); }
    bool ReadAt(std::size_t pIndex, std::span<uint8_t> pOut) const; // index 0 = oldest live record

private:
    bool ReadHeader(std::size_t pSegment, uint32_t& pEpoch) const;
    bool SlotErased(std::size_t pSegment, std::size_t pSlot) const;
    std::size_t ValidLength(std::size_t pSegment) const; // live records at the start of a segment
    bool StartSegment(std::size_t pSegment);             // erase (unless known clean) and write the header
    bool DropTail();                                     // erase the oldest segment
    std::size_t Head() const { return (mTail + mUsed - 1) % mStore.Segments(); }

    IBlockStore& mStore;
    std::size_t mRecordSize;
    Validator mValid;
    std::size_t mPerSegment; // records per segment (the header slot excluded)
    std::size_t mTail = 0;   // oldest used segment
    std::size_t mUsed = 0;   // segments in use (0 = empty)
    std::size_t mCount = 0;
    uint32_t mEpoch = 0;               // of the head
    bool mHeadSealed = false;          // a torn write: no more records into the head
    uint32_t mLens[MAX_SEGMENTS] = {}; // live records per segment, by physical index
    bool mClean[MAX_SEGMENTS] = {};    // erased by this Ring since open: start_segment may skip the erase
};

// Typed ring of Records with seq lookup.
class RecordLog
{
public:
    explicit RecordLog(IBlockStore& pStore);
    void Open() { mRing.Open(); }
    bool Append(const Record& pR);
    std::size_t Count() const { return mRing.Count(); }
    std::optional<Record> At(std::size_t pIndex) const;
    std::optional<Record> last() const;
    std::size_t LowerBound(uint32_t pSeq) const; // first index with record.seq >= seq (== count() if none)
    std::size_t Capacity() const { return mRing.Capacity(); }

private:
    Ring mRing;
};

class AggregateLog
{
public:
    explicit AggregateLog(IBlockStore& pStore);
    void Open() { mRing.Open(); }
    bool Append(const Aggregate& pA);
    std::size_t Count() const { return mRing.Count(); }
    std::optional<Aggregate> At(std::size_t pIndex) const;
    std::size_t Capacity() const { return mRing.Capacity(); }

private:
    Ring mRing;
};

// Folds samples into 5-minute aggregates.
class Aggregator
{
public:
    void Add(float pDoMgl, float pSatPct, float pTempC, uint8_t pLevel);
    std::optional<Aggregate> Flush(uint32_t pBucketTs); // nullopt when nothing was added
    uint32_t Samples() const { return mN; }

private:
    uint32_t mN = 0;
    float mDoMin = 0, mDoSum = 0, mDoMax = 0, mSatSum = 0, mTempMin = 0, mTempMax = 0;
    uint8_t mLevelMax = 0;
};

} // namespace reefdo::log
