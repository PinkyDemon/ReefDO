#include "reefdo/filter.hpp"

namespace reefdo::filter
{

float Median5::Push(float pV)
{
    mBuf[mHead] = pV;
    mHead = (mHead + 1) % mBuf.size();
    if(mN < mBuf.size()) ++mN;

    // Sort a copy of the held samples (≤ 5 elements: insertion sort is the right tool).
    std::array<float, 5> s = mBuf;
    for(std::size_t i = 1; i < mN; ++i)
    {
        const float x = s[i];
        std::size_t j = i;
        while(j > 0 && s[j - 1] > x)
        {
            s[j] = s[j - 1];
            --j;
        }
        s[j] = x;
    }
    // Even counts (2, 4) return the lower-middle element: deterministic, no averaging of the two.
    return s[(mN - 1) / 2];
}

void Median5::Reset()
{
    mN = 0;
    mHead = 0;
}

void SlopeEstimator::Push(uint64_t pTMs, float pV)
{
    // Evict samples older than the window relative to the new sample.
    while(mN > 0 && pTMs - mT[mHead] > mWindowMs)
    {
        mHead = (mHead + 1) % CAPACITY;
        --mN;
    }
    // Ring full: drop the oldest.
    if(mN == CAPACITY)
    {
        mHead = (mHead + 1) % CAPACITY;
        --mN;
    }
    const std::size_t slot = (mHead + mN) % CAPACITY;
    mT[slot] = pTMs;
    mV[slot] = pV;
    ++mN;
}

void SlopeEstimator::Reset()
{
    mN = 0;
    mHead = 0;
}

float SlopeEstimator::SlopePer10min() const
{
    if(mN < 2) return 0.0f;
    const uint64_t t0 = mT[mHead];
    const uint64_t tLast = mT[(mHead + mN - 1) % CAPACITY];
    if(tLast - t0 < MIN_SPAN_MS) return 0.0f;

    // Ordinary least squares, times relative to the oldest sample (seconds), in double.
    double sx = 0;
    double sy = 0;
    double sxx = 0;
    double sxy = 0;
    for(std::size_t i = 0; i < mN; ++i)
    {
        const std::size_t k = (mHead + i) % CAPACITY;
        const double x = static_cast<double>(mT[k] - t0) / 1000.0;
        const double y = mV[k];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double n = static_cast<double>(mN);
    const double denom = n * sxx - sx * sx; // > 0 whenever the span check above passed
    const double perSecond = (n * sxy - sx * sy) / denom;
    return static_cast<float>(perSecond * 600.0);
}

} // namespace reefdo::filter
