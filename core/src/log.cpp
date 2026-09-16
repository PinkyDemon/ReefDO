#include "reefdo/log.hpp"

#include <cstdio>
#include <cstring>

#include "reefdo/modbus.hpp" // crc16: the same CRC-16/MODBUS serves the log records

namespace reefdo::log
{

namespace
{

void PutU16(uint8_t* p_, uint16_t pV)
{
    p_[0] = static_cast<uint8_t>(pV);
    p_[1] = static_cast<uint8_t>(pV >> 8);
}
void PutU32(uint8_t* p_, uint32_t pV)
{
    PutU16(p_, static_cast<uint16_t>(pV));
    PutU16(p_ + 2, static_cast<uint16_t>(pV >> 16));
}
void PutF32(uint8_t* p_, float pF)
{
    uint32_t b = 0;
    std::memcpy(&b, &pF, 4);
    PutU32(p_, b);
}
uint16_t GetU16(const uint8_t* p_)
{
    return static_cast<uint16_t>(p_[0] | (p_[1] << 8));
}
uint32_t GetU32(const uint8_t* p_)
{
    return GetU16(p_) | (static_cast<uint32_t>(GetU16(p_ + 2)) << 16);
}
float GetF32(const uint8_t* p_)
{
    const uint32_t b = GetU32(p_);
    float f = 0.0f;
    std::memcpy(&f, &b, 4);
    return f;
}

// A record whose bytes are all 0xFF is erased flash, not data; its CRC would not match anyway, but
// treating it explicitly keeps the intent readable.
bool AllFf(std::span<const uint8_t> pB)
{
    for(const uint8_t x : pB)
        if(x != 0xFF) return false;
    return true;
}

bool CrcOk(std::span<const uint8_t> pB)
{
    // payload | crc16 | pad(2): the CRC covers the payload
    const std::size_t n = pB.size() - 4;
    return !AllFf(pB) && modbus::Crc16(pB.first(n)) == GetU16(pB.data() + n);
}

bool RecordValid(std::span<const uint8_t> pB)
{
    return CrcOk(pB);
}
bool AggregateValid(std::span<const uint8_t> pB)
{
    return CrcOk(pB);
}

uint16_t SatU16(float pV)
{
    if(pV < 0.0f) pV = 0.0f;
    if(pV > 655.0f) pV = 655.0f;
    return static_cast<uint16_t>(pV * 100.0f + 0.5f);
}
uint16_t DoU16(float pV)
{
    if(pV < 0.0f) pV = 0.0f;
    if(pV > 65.0f) pV = 65.0f;
    return static_cast<uint16_t>(pV * 1000.0f + 0.5f);
}
int16_t TempI16(float pV)
{
    if(pV < -300.0f) pV = -300.0f;
    if(pV > 300.0f) pV = 300.0f;
    return static_cast<int16_t>(pV * 100.0f + (pV >= 0.0f ? 0.5f : -0.5f));
}

} // namespace

void Encode(const Record& pR, std::span<uint8_t, RECORD_SIZE> pOut)
{
    uint8_t* p = pOut.data();
    PutU32(p + 0, pR.seq);
    PutU32(p + 4, pR.ts);
    PutU32(p + 8, pR.uptimeS);
    p[12] = static_cast<uint8_t>(pR.type);
    p[13] = pR.level;
    PutU16(p + 14, pR.flags);
    PutF32(p + 16, pR.f0);
    PutF32(p + 20, pR.f1);
    PutF32(p + 24, pR.f2);
    PutF32(p + 28, pR.f3);
    PutU32(p + 32, pR.aux);
    PutU16(p + 36, modbus::Crc16(pOut.first(36)));
    PutU16(p + 38, 0);
}

std::optional<Record> Decode(std::span<const uint8_t, RECORD_SIZE> pIn)
{
    if(!RecordValid(pIn)) return std::nullopt;
    const uint8_t* p = pIn.data();
    Record r;
    r.seq = GetU32(p + 0);
    r.ts = GetU32(p + 4);
    r.uptimeS = GetU32(p + 8);
    r.type = static_cast<Type>(p[12]);
    r.level = p[13];
    r.flags = GetU16(p + 14);
    r.f0 = GetF32(p + 16);
    r.f1 = GetF32(p + 20);
    r.f2 = GetF32(p + 24);
    r.f3 = GetF32(p + 28);
    r.aux = GetU32(p + 32);
    return r;
}

void Encode(const Aggregate& pA, std::span<uint8_t, AGGREGATE_SIZE> pOut)
{
    uint8_t* p = pOut.data();
    PutU32(p + 0, pA.ts);
    PutU16(p + 4, pA.doMin);
    PutU16(p + 6, pA.doAvg);
    PutU16(p + 8, pA.doMax);
    PutU16(p + 10, pA.satAvg);
    PutU16(p + 12, static_cast<uint16_t>(pA.tempMin));
    PutU16(p + 14, static_cast<uint16_t>(pA.tempMax));
    p[16] = pA.levelMax;
    p[17] = pA.samples;
    PutU16(p + 18, 0);
    PutU16(p + 20, modbus::Crc16(pOut.first(20)));
    PutU16(p + 22, 0);
}

std::optional<Aggregate> Decode(std::span<const uint8_t, AGGREGATE_SIZE> pIn)
{
    if(!AggregateValid(pIn)) return std::nullopt;
    const uint8_t* p = pIn.data();
    Aggregate a;
    a.ts = GetU32(p + 0);
    a.doMin = GetU16(p + 4);
    a.doAvg = GetU16(p + 6);
    a.doMax = GetU16(p + 8);
    a.satAvg = GetU16(p + 10);
    a.tempMin = static_cast<int16_t>(GetU16(p + 12));
    a.tempMax = static_cast<int16_t>(GetU16(p + 14));
    a.levelMax = p[16];
    a.samples = p[17];
    return a;
}

std::size_t CsvHeader(std::span<char> pOut)
{
    static const char HEADER[] = "seq,ts,uptime_s,type,level,flags,f0,f1,f2,f3,aux\n";
    const std::size_t n = sizeof HEADER - 1;
    if(pOut.size() < n) return 0;
    std::memcpy(pOut.data(), HEADER, n);
    return n;
}

std::size_t CsvLine(const Record& pR, std::span<char> pOut)
{
    const int n = std::snprintf(
        pOut.data(), pOut.size(), "%lu,%lu,%lu,%u,%u,%u,%.3f,%.2f,%.2f,%.3f,%lu\n", static_cast<unsigned long>(pR.seq),
        static_cast<unsigned long>(pR.ts), static_cast<unsigned long>(pR.uptimeS), static_cast<unsigned>(pR.type),
        pR.level, pR.flags, static_cast<double>(pR.f0), static_cast<double>(pR.f1), static_cast<double>(pR.f2),
        static_cast<double>(pR.f3), static_cast<unsigned long>(pR.aux));
    return static_cast<std::size_t>(n) < pOut.size() ? static_cast<std::size_t>(n) : 0;
}

// ---------------------------------------------------------------------------------------------- Ring

namespace
{
constexpr uint16_t HEADER_MAGIC = 0x5244; // "RD"
constexpr uint16_t HEADER_VERSION = 1;
} // namespace

Ring::Ring(IBlockStore& pStore, std::size_t pRecordSize, Validator pValid)
    : mStore(pStore)
    , mRecordSize(pRecordSize)
    , mValid(pValid)
    , mPerSegment(pStore.SegmentBytes() / pRecordSize - 1)
{
}

bool Ring::ReadHeader(std::size_t pSegment, uint32_t& pEpoch) const
{
    uint8_t h[HEADER_BYTES];
    if(!mStore.Read(pSegment, 0, h)) return false;
    if(GetU16(h) != HEADER_MAGIC || GetU16(h + 2) != HEADER_VERSION) return false;
    if(modbus::Crc16(std::span<const uint8_t>(h, 8)) != GetU16(h + 8)) return false;
    pEpoch = GetU32(h + 4);
    return true;
}

bool Ring::SlotErased(std::size_t pSegment, std::size_t pSlot) const
{
    uint8_t buf[64];
    const std::span<uint8_t> b(buf, mRecordSize);
    return mStore.Read(pSegment, pSlot * mRecordSize, b) && AllFf(b);
}

std::size_t Ring::ValidLength(std::size_t pSegment) const
{
    // Live records form a prefix of slots 1..mPerSegment (append-only; a torn tail and erased flash both fail
    // the validator), so binary-search its end: a boot over a 5 MB log costs a few dozen reads per segment.
    uint8_t buf[64];
    const std::span<uint8_t> b(buf, mRecordSize);
    std::size_t lo = 0;
    std::size_t hi = mPerSegment; // records [0, lo) valid, [hi, mPerSegment) invalid
    while(lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        if(mStore.Read(pSegment, (mid + 1) * mRecordSize, b) && mValid(b))
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

void Ring::Open()
{
    const std::size_t nseg = mStore.Segments();
    bool live[MAX_SEGMENTS] = {};
    uint32_t epoch[MAX_SEGMENTS] = {};
    std::optional<std::size_t> newest;
    for(std::size_t s = 0; s < nseg; ++s)
    {
        live[s] = ReadHeader(s, epoch[s]);
        mLens[s] = 0;
        mClean[s] = false;
        if(live[s] && (!newest || epoch[s] > epoch[*newest])) newest = s;
    }
    mUsed = 0;
    mCount = 0;
    mTail = 0;
    mHeadSealed = false;
    if(!newest) return;

    // The live run: consecutive epochs walking back from the newest segment. Anything else with a header is
    // a stray (an erase that stopped at the header, bit rot): erase it so it can never be mistaken for data.
    std::size_t s = *newest;
    mUsed = 1;
    while(mUsed < nseg)
    {
        const std::size_t p = (s + nseg - 1) % nseg;
        if(!live[p] || epoch[p] + 1 != epoch[s]) break;
        s = p;
        ++mUsed;
    }
    mTail = s;
    mEpoch = epoch[*newest];
    for(std::size_t i = 0; i < nseg; ++i)
    {
        const bool inRun = ((i + nseg - mTail) % nseg) < mUsed;
        if(live[i] && !inRun) mClean[i] = mStore.Erase(i);
    }
    for(std::size_t i = 0; i < mUsed; ++i)
    {
        const std::size_t seg = (mTail + i) % nseg;
        mLens[seg] = static_cast<uint32_t>(ValidLength(seg));
    }
    for(std::size_t i = 0; i < mUsed; ++i)
        mCount += mLens[(mTail + i) % nseg];
    // A partial tail is an interrupted drop or a sealed segment that became the oldest: finish dropping it.
    if(mUsed > 1 && mLens[mTail] < mPerSegment) DropTail();
    const std::size_t h = Head();
    mHeadSealed = mLens[h] < mPerSegment && !SlotErased(h, mLens[h] + 1);
}

bool Ring::StartSegment(std::size_t pSegment)
{
    if(!mClean[pSegment])
    {
        if(!mStore.Erase(pSegment)) return false;
        mClean[pSegment] = true;
    }
    uint8_t h[HEADER_BYTES];
    PutU16(h, HEADER_MAGIC);
    PutU16(h + 2, HEADER_VERSION);
    PutU32(h + 4, mEpoch + 1);
    PutU16(h + 8, modbus::Crc16(std::span<const uint8_t>(h, 8)));
    if(!mStore.Write(pSegment, 0, h)) return false; // a retry rewrites the same bits: safe on NOR
    ++mEpoch;
    ++mUsed;
    mLens[pSegment] = 0;
    mClean[pSegment] = false;
    mHeadSealed = false;
    return true;
}

bool Ring::Append(std::span<const uint8_t> pRecord)
{
    if(pRecord.size() != mRecordSize) return false;
    const std::size_t nseg = mStore.Segments();
    if(mUsed == 0)
    {
        mTail = 0;
        if(!StartSegment(0)) return false;
    }
    else if(mLens[Head()] == mPerSegment || mHeadSealed)
    {
        if(mUsed == nseg && !DropTail()) return false;
        if(!StartSegment((Head() + 1) % nseg)) return false;
    }
    const std::size_t h = Head();
    if(!mStore.Write(h, (mLens[h] + 1) * mRecordSize, pRecord))
    {
        mHeadSealed = true; // whatever landed cannot be rewritten; carry on in a fresh segment
        return false;
    }
    ++mLens[h];
    ++mCount;
    return true;
}

bool Ring::DropTail()
{
    if(!mStore.Erase(mTail)) return false;
    mCount -= mLens[mTail];
    mLens[mTail] = 0;
    mClean[mTail] = true;
    mTail = (mTail + 1) % mStore.Segments();
    --mUsed;
    return true;
}

bool Ring::ReadAt(std::size_t pIndex, std::span<uint8_t> pOut) const
{
    if(pIndex >= mCount || pOut.size() < mRecordSize) return false;
    std::size_t seg = mTail;
    while(pIndex >= mLens[seg])
    {
        pIndex -= mLens[seg];
        seg = (seg + 1) % mStore.Segments();
    }
    return mStore.Read(seg, (pIndex + 1) * mRecordSize, pOut.first(mRecordSize));
}

// ------------------------------------------------------------------------------------------ RecordLog

RecordLog::RecordLog(IBlockStore& pStore)
    : mRing(pStore, RECORD_SIZE, RecordValid)
{
}

bool RecordLog::Append(const Record& pR)
{
    std::array<uint8_t, RECORD_SIZE> buf{};
    Encode(pR, buf);
    return mRing.Append(buf);
}

std::optional<Record> RecordLog::At(std::size_t pIndex) const
{
    std::array<uint8_t, RECORD_SIZE> buf{};
    if(!mRing.ReadAt(pIndex, buf)) return std::nullopt;
    return Decode(buf);
}

std::optional<Record> RecordLog::last() const
{
    if(mRing.Count() == 0) return std::nullopt;
    return At(mRing.Count() - 1);
}

std::size_t RecordLog::LowerBound(uint32_t pSeq) const
{
    std::size_t lo = 0;
    std::size_t hi = mRing.Count();
    while(lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::optional<Record> r = At(mid);
        if(r.has_value() && r->seq < pSeq)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

// --------------------------------------------------------------------------------------- AggregateLog

AggregateLog::AggregateLog(IBlockStore& pStore)
    : mRing(pStore, AGGREGATE_SIZE, AggregateValid)
{
}

bool AggregateLog::Append(const Aggregate& pA)
{
    std::array<uint8_t, AGGREGATE_SIZE> buf{};
    Encode(pA, buf);
    return mRing.Append(buf);
}

std::optional<Aggregate> AggregateLog::At(std::size_t pIndex) const
{
    std::array<uint8_t, AGGREGATE_SIZE> buf{};
    if(!mRing.ReadAt(pIndex, buf)) return std::nullopt;
    return Decode(buf);
}

// ----------------------------------------------------------------------------------------- Aggregator

void Aggregator::Add(float pDoMgl, float pSatPct, float pTempC, uint8_t pLevel)
{
    if(mN == 0)
    {
        mDoMin = mDoMax = pDoMgl;
        mTempMin = mTempMax = pTempC;
        mDoSum = mSatSum = 0.0f;
        mLevelMax = 0;
    }
    if(pDoMgl < mDoMin) mDoMin = pDoMgl;
    if(pDoMgl > mDoMax) mDoMax = pDoMgl;
    if(pTempC < mTempMin) mTempMin = pTempC;
    if(pTempC > mTempMax) mTempMax = pTempC;
    if(pLevel > mLevelMax) mLevelMax = pLevel;
    mDoSum += pDoMgl;
    mSatSum += pSatPct;
    ++mN;
}

std::optional<Aggregate> Aggregator::Flush(uint32_t pBucketTs)
{
    if(mN == 0) return std::nullopt;
    Aggregate a;
    a.ts = pBucketTs;
    a.doMin = DoU16(mDoMin);
    a.doAvg = DoU16(mDoSum / static_cast<float>(mN));
    a.doMax = DoU16(mDoMax);
    a.satAvg = SatU16(mSatSum / static_cast<float>(mN));
    a.tempMin = TempI16(mTempMin);
    a.tempMax = TempI16(mTempMax);
    a.levelMax = mLevelMax;
    a.samples = static_cast<uint8_t>(mN > 255 ? 255 : mN);
    mN = 0;
    return a;
}

} // namespace reefdo::log
