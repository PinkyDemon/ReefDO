#include <utility>

#include "catch_amalgamated.hpp"

#include "reefdo/filter.hpp"

#include "proptest.hpp"

using namespace reefdo::filter;
using Catch::Matchers::WithinAbs;

TEST_CASE("Median5 warms up, then rejects single-sample spikes", "[filter]")
{
    Median5 m;
    REQUIRE(m.Count() == 0);
    REQUIRE(m.Push(6.0f) == 6.0f); // 1 sample: itself
    REQUIRE(m.Push(9.0f) == 6.0f); // 2 samples: lower-middle
    REQUIRE(m.Push(5.0f) == 6.0f); // 3: {5,6,9}
    REQUIRE(m.Push(7.0f) == 6.0f); // 4: {5,6,7,9} lower-middle
    REQUIRE(m.Push(6.5f) == 6.5f); // 5: {5,6,6.5,7,9}
    REQUIRE(m.Count() == 5);
    REQUIRE(m.Push(0.1f) == 6.5f); // bubble spike replaces 6.0: {0.1,5,6.5,7,9}
    REQUIRE(m.Push(6.4f) == 6.4f); // {0.1,5,6.4,6.5,7}
    REQUIRE(m.Count() == 5);
    m.Reset();
    REQUIRE(m.Count() == 0);
    REQUIRE(m.Push(1.0f) == 1.0f);
}

TEST_CASE("Median5 equals a sorted-middle reference for random input", "[filter][property]")
{
    proptest::Forall(200,
                     [](proptest::Rng& pRng)
                     {
                         Median5 m;
                         float window[5] = {};
                         std::size_t n = 0;
                         std::size_t head = 0;
                         for(int i = 0; i < 20; ++i)
                         {
                             const float v = pRng.Uniform(0.0f, 10.0f);
                             window[head] = v;
                             head = (head + 1) % 5;
                             if(n < 5) ++n;
                             float s[5];
                             for(std::size_t k = 0; k < n; ++k)
                                 s[k] = window[k];
                             for(std::size_t a = 0; a < n; ++a)
                                 for(std::size_t b = a + 1; b < n; ++b)
                                     if(s[b] < s[a]) std::swap(s[a], s[b]);
                             REQUIRE(m.Push(v) == s[(n - 1) / 2]);
                         }
                     });
}

TEST_CASE("SlopeEstimator reports 0 until two samples span a minute", "[filter]")
{
    SlopeEstimator s;
    REQUIRE(s.SlopePer10min() == 0.0f);
    s.Push(0, 6.0f);
    REQUIRE(s.SlopePer10min() == 0.0f);
    s.Push(30'000, 5.0f);
    REQUIRE(s.SlopePer10min() == 0.0f); // 30 s span: too short
    s.Push(60'000, 4.0f);
    REQUIRE_THAT(s.SlopePer10min(), WithinAbs(-20.0, 1e-3)); // -1 per 30 s = -20 per 10 min
    REQUIRE(s.Count() == 3);
    s.Reset();
    REQUIRE(s.Count() == 0);
    REQUIRE(s.SlopePer10min() == 0.0f);
}

TEST_CASE("SlopeEstimator recovers the slope of a noiseless line, in units per 10 min", "[filter]")
{
    SlopeEstimator s;
    // 6.60 falling 0.3 mg/L per 10 min, sampled every 10 s for 10 min
    for(int i = 0; i <= 60; ++i)
        s.Push(static_cast<uint64_t>(i) * 10'000, 6.6f - 0.3f * (i / 60.0f));
    REQUIRE_THAT(s.SlopePer10min(), WithinAbs(-0.3, 1e-4));
    REQUIRE(s.Count() == 61);
}

TEST_CASE("SlopeEstimator evicts samples older than the window", "[filter]")
{
    SlopeEstimator s(100'000); // 100 s window
    s.Push(0, 10.0f);
    s.Push(50'000, 10.0f);
    s.Push(100'000, 10.0f);
    REQUIRE(s.Count() == 3);
    s.Push(150'000, 5.0f); // t=0 is now 150 s old: evicted
    REQUIRE(s.Count() == 3);
    s.Push(400'000, 5.0f); // everything older than 300 s: evicted, only this one remains
    REQUIRE(s.Count() == 1);
    REQUIRE(s.SlopePer10min() == 0.0f);
}

TEST_CASE("SlopeEstimator ring drops the oldest sample when full", "[filter]")
{
    SlopeEstimator s(10'000'000); // huge window so nothing is evicted by age
    for(int i = 0; i < 100; ++i)
        s.Push(static_cast<uint64_t>(i) * 1000, 1.0f + i * 0.01f);
    REQUIRE(s.Count() == SlopeEstimator::CAPACITY);
    REQUIRE_THAT(s.SlopePer10min(), WithinAbs(6.0, 1e-3)); // 0.01/s = 6 per 10 min
}

TEST_CASE("SlopeEstimator sign follows the data for random lines", "[filter][property]")
{
    proptest::Forall(200,
                     [](proptest::Rng& pRng)
                     {
                         SlopeEstimator s;
                         const float a = pRng.Uniform(4.0f, 8.0f);
                         const float b = pRng.Uniform(-1.0f, 1.0f); // per 10 min
                         for(int i = 0; i <= 60; ++i)
                             s.Push(static_cast<uint64_t>(i) * 10'000, a + b * (i / 60.0f));
                         REQUIRE_THAT(s.SlopePer10min(), WithinAbs(b, 1e-3));
                     });
}
