#pragma once
// A deliberately tiny property-testing helper: a seeded xorshift generator and a `forall` loop that
// reports the failing iteration's seed through Catch2's INFO. No shrinking — on a failure, re-run
// with `Rng rng(seed)` in a plain TEST_CASE and bisect by hand. Keeps the test build dependency-free.
#include <cstdint>

#include "catch_amalgamated.hpp"

namespace proptest
{

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t pSeed)
        : s(pSeed ? pSeed : 0x9E3779B97F4A7C15ull)
    {
    }
    uint64_t Next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double Unit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    float Uniform(float pLo, float pHi) { return pLo + static_cast<float>(Unit()) * (pHi - pLo); }
    uint32_t Below(uint32_t pN) { return static_cast<uint32_t>(Next() % pN); } // [0,n)
    bool Coin(double p_ = 0.5) { return Unit() < p_; }
};

template <class Body>
void Forall(int pIterations, Body&& pBody)
{
    for(int i = 0; i < pIterations; ++i)
    {
        const uint64_t seed = 0x2545F4914F6CDD1Dull * static_cast<uint64_t>(i + 1);
        INFO("property iteration " << i << " (seed " << seed << ")");
        Rng rng(seed);
        pBody(rng);
    }
}

} // namespace proptest
