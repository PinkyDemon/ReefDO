#include "reefdo/sim.hpp"

#include <limits>

#include "reefdo/solubility.hpp"

namespace reefdo::sim
{

bool Tank::LightsOn(uint16_t pM) const
{
    return pM >= mCfg.lightsOnMin && pM < mCfg.lightsOffMin;
}

float Tank::DoMgl() const
{
    return solubility::MglFromSaturation(mSat, mTemp, mCfg.salinityPsu);
}

void Tank::Step(float pDtS, uint16_t pMinuteOfDay, std::span<const bool, DEVICES> pDeviceOn)
{
    const bool day = LightsOn(pMinuteOfDay);
    mTemp = day ? mCfg.tempDayC : mCfg.tempNightC;

    // A power cycle (off → on) of the return pump clears a stall: that is what the ladder's pulse is for.
    bool flowing = true;
    if(mCfg.returnPumpDevice < DEVICES)
    {
        const bool on = pDeviceOn[mCfg.returnPumpDevice];
        if(on && !mPumpWasOn) mStalled = false;
        mPumpWasOn = on;
        flowing = on && !mStalled;
    }

    float k = flowing ? mCfg.kFlowPerH : 0.0f;
    for(std::size_t i = 0; i < DEVICES; ++i)
        k += pDeviceOn[i] ? mCfg.kDevicePerH[i] : 0.0f;

    const float source = day ? mCfg.photosynthesisPctPerH - mCfg.respirationDayPctPerH : -mCfg.respirationNightPctPerH;
    const float dsat = (k * (100.0f - mSat) + source) * (pDtS / 3600.0f);
    mSat += dsat;
    if(mSat < 0.0f) mSat = 0.0f;
}

Probe::Probe(Tank& pTank, const ProbeModel& pModel, uint64_t pSeed)
    : mTank(pTank)
    , mModel(pModel)
    , mRng(pSeed ? pSeed : 0x9E3779B97F4A7C15ull)
    , mLaggedSat(pTank.SatPct())
{
}

float Probe::Uniform(float pLo, float pHi)
{
    mRng ^= mRng << 13;
    mRng ^= mRng >> 7;
    mRng ^= mRng << 17;
    const float u = static_cast<float>(mRng >> 40) / 16777216.0f;
    return pLo + u * (pHi - pLo);
}

probe::PollResult Probe::Poll()
{
    ++mPolls;
    // The lag runs even when the electronics fail: the optics keep tracking the water.
    mLaggedSat += (mTank.SatPct() - mLaggedSat) * (mModel.dtS / (mModel.tauS + mModel.dtS));

    probe::PollResult r;
    if(mModel.dropout)
    {
        r.status = probe::Status::Timeout;
        return r;
    }
    if(mModel.crcErrorEvery > 0 && mPolls % mModel.crcErrorEvery == 0)
    {
        r.status = probe::Status::FrameError;
        return r;
    }
    if(mModel.implausible)
    {
        r.status = probe::Status::Implausible;
        return r;
    }
    if(mModel.stuck && mHaveLast)
    {
        r.status = probe::Status::Stuck;
        r.reading = mLast;
        return r;
    }
    probe::Reading rd;
    rd.satPct = mLaggedSat + mModel.bubblePct + Uniform(-mModel.noisePct, mModel.noisePct);
    rd.tempC = mTank.TempC() + Uniform(-0.05f, 0.05f);
    const float salinity = mModel.freshwaterReference ? 0.0f : 35.0f;
    rd.doMgl = solubility::MglFromSaturation(rd.satPct, rd.tempC, salinity);
    mLast = rd;
    mHaveLast = true;
    r.status = probe::Status::Ok;
    r.reading = rd;
    return r;
}

} // namespace reefdo::sim
