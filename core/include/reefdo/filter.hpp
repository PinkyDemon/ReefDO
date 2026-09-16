#pragma once
// Sample conditioning in front of the ladder: a median-of-5 (kills single-sample spikes such as a bubble
// on the cap) and a least-squares slope over a sliding time window (the rate-of-change early warning).
#include <array>
#include <cstddef>
#include <cstdint>

namespace reefdo::filter
{

class Median5
{
public:
    // Push a sample, get the median of the last (up to) five. During warm-up it is the median of what exists.
    float Push(float pV);
    void Reset();
    std::size_t Count() const { return mN; }

private:
    std::array<float, 5> mBuf{};
    std::size_t mN = 0;    // samples held (≤ 5)
    std::size_t mHead = 0; // next write slot
};

class SlopeEstimator
{
public:
    static constexpr std::size_t CAPACITY = 64;     // 60 samples = 10 min at 10 s, plus slack
    static constexpr uint32_t MIN_SPAN_MS = 60'000; // below this span the slope is reported as 0

    explicit SlopeEstimator(uint32_t pWindowMs = 600'000)
        : mWindowMs(pWindowMs)
    {
    }

    void Push(uint64_t pTMs, float pV);
    void Reset();
    std::size_t Count() const { return mN; }

    // Least-squares slope of the samples inside the window, in units per 10 minutes.
    // Negative = falling. 0 until at least two samples span MIN_SPAN_MS.
    float SlopePer10min() const;

private:
    std::array<uint64_t, CAPACITY> mT{};
    std::array<float, CAPACITY> mV{};
    std::size_t mN = 0;
    std::size_t mHead = 0; // index of the oldest sample
    uint32_t mWindowMs;
};

} // namespace reefdo::filter
