#include <cstring>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/log.hpp"

#include "proptest.hpp"

using namespace reefdo::log;
using Catch::Matchers::WithinAbs;

#include "fake_store.hpp"

namespace
{

Record Sample(uint32_t pSeq)
{
    Record r;
    r.seq = pSeq;
    r.ts = 1'700'000'000u + pSeq * 10;
    r.uptimeS = pSeq * 10;
    r.type = Type::Measurement;
    r.level = static_cast<uint8_t>(pSeq % 4);
    r.flags = 0x0102;
    r.f0 = 6.5f - static_cast<float>(pSeq % 100) * 0.01f;
    r.f1 = 97.25f;
    r.f2 = 26.1f;
    r.f3 = -0.05f;
    r.aux = pSeq * 3;
    return r;
}

} // namespace

TEST_CASE("Record codec round-trips and rejects damage", "[log]")
{
    const Record r = Sample(42);
    std::array<uint8_t, RECORD_SIZE> buf{};
    Encode(r, buf);
    REQUIRE(Decode(buf) == r);

    std::array<uint8_t, RECORD_SIZE> bad = buf;
    bad[17] ^= 0x01;
    REQUIRE_FALSE(Decode(bad).has_value());
    std::array<uint8_t, RECORD_SIZE> erased{};
    std::fill(erased.begin(), erased.end(), 0xFF);
    REQUIRE_FALSE(Decode(erased).has_value());
    std::array<uint8_t, RECORD_SIZE> zeros{}; // a zeroed record: CRC of zeros is not zero → invalid
    REQUIRE_FALSE(Decode(zeros).has_value());
}

TEST_CASE("Aggregate codec round-trips, clamps, and rejects damage", "[log]")
{
    Aggregate a;
    a.ts = 1'700'000'000u;
    a.doMin = 6120;
    a.doAvg = 6300;
    a.doMax = 6480;
    a.satAvg = 9725;
    a.tempMin = -150;
    a.tempMax = 2810;
    a.levelMax = 2;
    a.samples = 30;
    std::array<uint8_t, AGGREGATE_SIZE> buf{};
    Encode(a, buf);
    REQUIRE(Decode(buf) == a);
    buf[5] ^= 0x80;
    REQUIRE_FALSE(Decode(buf).has_value());

    Aggregator g;
    REQUIRE_FALSE(g.Flush(0).has_value()); // nothing added
    g.Add(6.5f, 95.0f, 26.0f, 0);
    g.Add(7.0f, 105.0f, 25.0f, 2);
    g.Add(6.0f, 100.0f, 27.0f, 1);
    REQUIRE(g.Samples() == 3);
    const std::optional<Aggregate> f = g.Flush(1000);
    REQUIRE(f.has_value());
    REQUIRE(f->ts == 1000);
    REQUIRE(f->doMin == 6000);
    REQUIRE(f->doAvg == 6500);
    REQUIRE(f->doMax == 7000);
    REQUIRE(f->satAvg == 10000);
    REQUIRE(f->tempMin == 2500);
    REQUIRE(f->tempMax == 2700);
    REQUIRE(f->levelMax == 2);
    REQUIRE(f->samples == 3);
    REQUIRE(g.Samples() == 0);

    Aggregator clamp; // out-of-range values saturate instead of wrapping
    for(int i = 0; i < 300; ++i)
        clamp.Add(-1.0f, -5.0f, -400.0f, 0);
    clamp.Add(99.0f, 999.0f, 400.0f, 3);
    const Aggregate c = *clamp.Flush(0);
    REQUIRE(c.doMin == 0);
    REQUIRE(c.doMax == 65000);
    REQUIRE(c.tempMin == -30000);
    REQUIRE(c.tempMax == 30000);
    REQUIRE(c.samples == 255);
    REQUIRE(c.satAvg == 0); // avg of 300 × -5 and one 999 is negative → 0
    Aggregator hi;
    hi.Add(1.0f, 999.0f, 20.0f, 0);
    REQUIRE(hi.Flush(0)->satAvg == 65500);
}

TEST_CASE("CSV export of a record", "[log]")
{
    char buf[160];
    REQUIRE(CsvHeader(buf) > 0);
    REQUIRE(std::string(buf, CsvHeader(buf)) == "seq,ts,uptime_s,type,level,flags,f0,f1,f2,f3,aux\n");
    const std::size_t n = CsvLine(Sample(7), buf);
    REQUIRE(std::string(buf, n) == "7,1700000070,70,1,3,258,6.430,97.25,26.10,-0.050,21\n");
    char tiny[8];
    REQUIRE(CsvLine(Sample(7), tiny) == 0);
    REQUIRE(CsvHeader(tiny) == 0);
}

TEST_CASE("Ring: append and read back across segments, then wrap by erasing the oldest", "[log]")
{
    FakeStore store(4, 4 * RECORD_SIZE); // 4 segments × (1 header + 3 records)
    RecordLog log(store);
    log.Open();
    REQUIRE(log.Count() == 0);
    REQUIRE(log.Capacity() == 12);
    REQUIRE_FALSE(log.last().has_value());
    REQUIRE_FALSE(log.At(0).has_value());

    for(uint32_t i = 1; i <= 10; ++i)
        REQUIRE(log.Append(Sample(i)));
    REQUIRE(log.Count() == 10);
    REQUIRE(log.At(0)->seq == 1);
    REQUIRE(log.At(9)->seq == 10);
    REQUIRE(log.last()->seq == 10);
    REQUIRE(store.erases == 4); // every segment is erased once, right before its first use

    for(uint32_t i = 11; i <= 12; ++i)
        REQUIRE(log.Append(Sample(i))); // all four segments full
    REQUIRE(log.Count() == 12);
    REQUIRE(store.erases == 4);
    REQUIRE(log.Append(Sample(13))); // the oldest segment is dropped to make room
    REQUIRE(store.erases == 5);
    REQUIRE(log.Count() == 10); // 12 - 3 dropped + 1
    REQUIRE(log.At(0)->seq == 4);
    REQUIRE(log.last()->seq == 13);
    for(uint32_t i = 14; i <= 40; ++i)
        REQUIRE(log.Append(Sample(i)));
    REQUIRE(log.last()->seq == 40);
    REQUIRE(log.Count() >= 10);
    REQUIRE(log.Count() <= 12);
    // Everything readable is contiguous and in order.
    for(std::size_t i = 1; i < log.Count(); ++i)
        REQUIRE(log.At(i)->seq == log.At(i - 1)->seq + 1);
}

TEST_CASE("Ring: recovery after a clean stop, a torn write, and a wrapped ring", "[log]")
{
    FakeStore store(3, 4 * RECORD_SIZE); // 3 records per segment
    {
        RecordLog log(store);
        log.Open();
        for(uint32_t i = 1; i <= 4; ++i)
            REQUIRE(log.Append(Sample(i))); // segment 0 full, 4 in segment 1
    }
    {
        RecordLog log(store); // clean reopen
        log.Open();
        REQUIRE(log.Count() == 4);
        REQUIRE(log.At(0)->seq == 1);
        REQUIRE(log.last()->seq == 4);
        store.torn = 17; // power cut half-way through the next record
        REQUIRE_FALSE(log.Append(Sample(5)));
    }
    {
        RecordLog log(store);
        log.Open();
        REQUIRE(log.Count() == 4);      // the torn record is not a record
        REQUIRE(log.Append(Sample(5))); // ...and its slot is never rewritten: segment 1 is sealed, 5 starts segment 2
        REQUIRE(log.last()->seq == 5);
        REQUIRE(log.Count() == 5);
        REQUIRE(store.erases == 3);
        for(uint32_t i = 6; i <= 7; ++i)
            REQUIRE(log.Append(Sample(i))); // segment 2 full
        REQUIRE(store.erases == 3);
        REQUIRE(log.Append(Sample(8))); // segment 0 dropped and reused: the ring has wrapped
        REQUIRE(store.erases == 4);
        REQUIRE(log.Count() == 5);
        REQUIRE(log.At(0)->seq == 4);
    }
    {
        RecordLog log(store); // recovery of a wrapped ring: the sealed segment 1 is now the tail and is dropped, head
                              // is segment 0
        log.Open();
        REQUIRE(store.erases == 5);
        REQUIRE(log.Count() == 4);
        REQUIRE(log.At(0)->seq == 5);
        REQUIRE(log.last()->seq == 8);
        REQUIRE(log.LowerBound(5) == 0);
        REQUIRE(log.LowerBound(7) == 2);
        REQUIRE(log.LowerBound(8) == 3);
        REQUIRE(log.LowerBound(9) == 4);
        REQUIRE(log.LowerBound(1) == 0);
        REQUIRE(log.Append(Sample(9))); // continues in the head
        REQUIRE(log.Count() == 5);
        REQUIRE(store.erases == 5);
    }
}

TEST_CASE("Ring: a torn write that left nothing behind still seals the segment", "[log]")
{
    FakeStore store(2, 3 * RECORD_SIZE);
    RecordLog log(store);
    log.Open();
    REQUIRE(log.Append(Sample(1)));
    store.torn = 0;
    REQUIRE_FALSE(log.Append(Sample(2)));
    REQUIRE(log.Append(Sample(3))); // the ring cannot know that nothing landed: it seals and starts segment 1
    REQUIRE(log.Count() == 2);
    REQUIRE(store.erases == 2);
    RecordLog again(store);
    again.Open(); // segment 0 is partial and it is the tail: dropped, whatever sealed it
    REQUIRE(again.Count() == 1);
    REQUIRE(again.At(0)->seq == 3);
}

TEST_CASE("Ring: an interrupted erase of the oldest segment is finished on open", "[log]")
{
    FakeStore store(3, 4 * RECORD_SIZE);
    {
        RecordLog log(store);
        log.Open();
        for(uint32_t i = 1; i <= 9; ++i)
            REQUIRE(log.Append(Sample(i))); // full
        store.tornErase = 2 * RECORD_SIZE; // power cut: the header of segment 0 survives, its last two records are gone
        REQUIRE_FALSE(log.Append(Sample(10)));
        REQUIRE(log.Count() == 9); // nothing changed from the ring's point of view
    }
    {
        RecordLog log(store);
        log.Open(); // segment 0 has a live header and one readable record left: a partial tail is dropped
        REQUIRE(store.erases == 5);
        REQUIRE(log.Count() == 6);
        REQUIRE(log.At(0)->seq == 4);
        REQUIRE(log.Append(Sample(10))); // reuses the freshly erased segment 0 without another erase
        REQUIRE(store.erases == 5);
        REQUIRE(log.At(0)->seq == 4);
        REQUIRE(log.last()->seq == 10);
    }
    {
        FakeStore ascending(2,
                            3 * RECORD_SIZE); // the other order: the header goes first, the segment is simply not live
        RecordLog log(ascending);
        log.Open();
        for(uint32_t i = 1; i <= 4; ++i)
            REQUIRE(log.Append(Sample(i)));
        std::fill(ascending.segs[0].begin(), ascending.segs[0].begin() + RECORD_SIZE, 0xFF);
        RecordLog again(ascending);
        again.Open();
        REQUIRE(again.Count() == 2);
        REQUIRE(again.At(0)->seq == 3);
    }
}

TEST_CASE("Ring: records without a segment header are not data; a stray header is erased", "[log]")
{
    FakeStore store(2, 3 * RECORD_SIZE);
    for(uint32_t i = 1; i <= 4; ++i)
    {
        std::array<uint8_t, RECORD_SIZE> b{};
        Encode(Sample(i), b);
        store.Write((i - 1) / 2, ((i - 1) % 2 + 1) * RECORD_SIZE, b); // behind the ring: no headers
    }
    RecordLog log(store);
    log.Open();
    REQUIRE(log.Count() == 0);
    REQUIRE(log.Append(Sample(5))); // the segment is erased before its first use
    REQUIRE(store.erases == 1);
    REQUIRE(log.Count() == 1);
    REQUIRE(log.At(0)->seq == 5);

    FakeStore stray(3, 3 * RECORD_SIZE);
    {
        RecordLog w(stray);
        w.Open();
        for(uint32_t i = 1; i <= 4; ++i)
            REQUIRE(w.Append(Sample(i))); // segments 0 (epoch 1) and 1 (epoch 2)
    }
    std::vector<uint8_t> header(stray.segs[0].begin(), stray.segs[0].begin() + RECORD_SIZE);
    std::copy(header.begin(), header.end(), stray.segs[2].begin()); // segment 2 now claims epoch 1 as well
    RecordLog r(stray);
    r.Open();
    REQUIRE(stray.erases == 3); // the stray was erased on open
    REQUIRE(r.Count() == 4);
    REQUIRE(r.At(0)->seq == 1);
    REQUIRE(r.last()->seq == 4);
}

TEST_CASE("Ring: store failures are reported, not hidden", "[log]")
{
    FakeStore store(2, 2 * RECORD_SIZE); // one record per segment
    RecordLog log(store);
    log.Open();
    store.failWrite = true;
    REQUIRE_FALSE(log.Append(Sample(1))); // the header could not be written
    store.failWrite = false;
    REQUIRE(log.Append(Sample(1)));
    REQUIRE(store.erases == 1); // the retry did not erase again
    REQUIRE(log.Append(Sample(2)));
    store.failErase = true;
    REQUIRE_FALSE(log.Append(Sample(3))); // full: the tail cannot be dropped
    store.failErase = false;
    REQUIRE(log.Append(Sample(3)));
    REQUIRE(log.At(0)->seq == 2);

    FakeStore one(1, 3 * RECORD_SIZE); // a single-segment ring simply starts over when it fills
    RecordLog single(one);
    single.Open();
    for(uint32_t i = 1; i <= 2; ++i)
        REQUIRE(single.Append(Sample(i)));
    REQUIRE(single.Count() == 2);
    REQUIRE(single.Append(Sample(3)));
    REQUIRE(single.Count() == 1);
    REQUIRE(single.At(0)->seq == 3);

    FakeStore unreadable(2, 2 * RECORD_SIZE);
    RecordLog u(unreadable);
    u.Open();
    REQUIRE(u.Append(Sample(1)));
    unreadable.failRead = true;
    REQUIRE_FALSE(u.At(0).has_value());
    REQUIRE(u.LowerBound(1) == 0); // unreadable records sort as "not less than"

    FakeStore dead(2, 2 * RECORD_SIZE); // a store that cannot be read at all opens as empty
    dead.failRead = true;
    RecordLog d(dead);
    d.Open();
    REQUIRE(d.Count() == 0);

    FakeStore dying(2, 3 * RECORD_SIZE); // ...and one that dies after the headers were read: live, but no records
    {
        RecordLog w(dying);
        w.Open();
        REQUIRE(w.Append(Sample(1)));
    }
    dying.failReadAfter = 2;
    RecordLog dy(dying);
    dy.Open();
    REQUIRE(dy.Count() == 0);

    FakeStore stuck(3, 3 * RECORD_SIZE); // the interrupted-drop repair on open cannot erase: the tail stays
    {
        RecordLog w(stuck);
        w.Open();
        for(uint32_t i = 1; i <= 6; ++i)
            REQUIRE(w.Append(Sample(i)));
        stuck.tornErase = RECORD_SIZE;
        REQUIRE_FALSE(w.Append(Sample(7)));
    }
    stuck.failErase = true;
    RecordLog s(stuck);
    s.Open();
    REQUIRE(s.Count() == 5);
    REQUIRE(s.At(0)->seq == 1);

    // The raw ring rejects wrong record sizes and short read buffers.
    FakeStore raw(2, 2 * RECORD_SIZE);
    Ring ring(raw, RECORD_SIZE, [](std::span<const uint8_t>) { return true; });
    ring.Open();
    std::array<uint8_t, 8> wrong{};
    REQUIRE_FALSE(ring.Append(wrong));
    std::array<uint8_t, RECORD_SIZE> full{};
    REQUIRE(ring.Append(full));
    REQUIRE_FALSE(ring.ReadAt(0, wrong));
    REQUIRE(ring.ReadAt(0, full));
}

TEST_CASE("Ring: recovery orders segments by epoch, not by position", "[log]")
{
    FakeStore store(4, 3 * RECORD_SIZE);
    {
        RecordLog log(store);
        log.Open();
        for(uint32_t i = 1; i <= 10; ++i)
            REQUIRE(log.Append(Sample(i))); // live: 1, 2, 3, 0 (epochs 2..5); 0 was reused
    }
    RecordLog log(store);
    log.Open();
    REQUIRE(log.Count() == 8);
    REQUIRE(log.At(0)->seq == 3);
    REQUIRE(log.last()->seq == 10);
    REQUIRE(log.Append(Sample(11))); // the head (segment 0) is full: segment 1 is dropped and reused
    REQUIRE(store.erases == 6);
    REQUIRE(log.At(0)->seq == 5);
}

TEST_CASE("AggregateLog stores 24-byte records on its own ring", "[log]")
{
    FakeStore store(2, 5 * AGGREGATE_SIZE);
    AggregateLog log(store);
    log.Open();
    REQUIRE(log.Capacity() == 8);
    for(uint32_t i = 0; i < 7; ++i)
    {
        Aggregate a;
        a.ts = 300 * i;
        a.doAvg = static_cast<uint16_t>(6000 + i);
        a.samples = 30;
        REQUIRE(log.Append(a));
    }
    REQUIRE(log.Count() == 7);
    REQUIRE(log.At(6)->ts == 1800);
    REQUIRE_FALSE(log.At(7).has_value());
    RecordLog wrong(store); // reading 40-byte records off a 24-byte ring is garbage: reported as invalid, never a crash
    wrong.Open();
    REQUIRE(wrong.Count() == 0);
}

TEST_CASE("Ring survives random appends and power cuts: valid prefix, order kept, seq never reused", "[log][property]")
{
    proptest::Forall(80,
                     [](proptest::Rng& pRng)
                     {
                         // Two segments or more: a single-segment ring whose one erase is interrupted keeps the
                         // *oldest* records (a partial lone segment is indistinguishable from a normal head), which is
                         // fine for a wipe but not a suffix.
                         FakeStore store(2 + pRng.Below(5), (2 + pRng.Below(6)) * RECORD_SIZE);
                         uint32_t nextSeq = 1;
                         std::vector<uint32_t> expected; // seqs believed durable
                         for(int round = 0; round < 8; ++round)
                         {
                             RecordLog log(store);
                             log.Open();
                             // What survived must be a contiguous, in-order tail of what was written.
                             REQUIRE(log.Count() <= expected.size());
                             for(std::size_t i = 0; i < log.Count(); ++i)
                             {
                                 const std::optional<Record> r = log.At(i);
                                 REQUIRE(r.has_value());
                                 REQUIRE(r->seq == expected[expected.size() - log.Count() + i]);
                             }
                             while(expected.size() > log.Count())
                                 expected.erase(expected.begin());
                             const int writes = 1 + static_cast<int>(pRng.Below(20));
                             for(int w = 0; w < writes; ++w)
                             {
                                 if(pRng.Coin(0.1))
                                     store.torn =
                                         pRng.Below(36); // power cut before the CRC lands: the record is not durable
                                 if(pRng.Coin(0.1))
                                     store.tornErase =
                                         pRng.Below(static_cast<uint32_t>(store.bytes) + 1); // ...or during an erase
                                 const bool ok = log.Append(Sample(nextSeq));
                                 if(ok) expected.push_back(nextSeq);
                                 ++nextSeq; // a seq is never reused, even after a failed write
                                 if(!ok) break;
                             }
                             store.torn.reset();
                             store.tornErase.reset();
                             // Keep `expected` to what the ring can still hold (the oldest are dropped on wrap).
                             while(expected.size() > log.Count())
                                 expected.erase(expected.begin());
                         }
                     });
}
