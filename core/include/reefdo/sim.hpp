#pragma once
// Virtual tank and probe model: d(sat)/dt = (kFlow + Σ kDevice) · (100 − sat) − R + P in
// %/h, respiration R higher at night, photosynthesis P with the lights on; the probe adds lag, noise, faults.
#include <array>
#include <cstdint>
#include <span>

#include "reefdo/probe.hpp"

namespace reefdo::sim
{

constexpr std::size_t DEVICES = 6;

struct TankConfig
{
    float kFlowPerH = 0.6f;                   // surface exchange with the return pump running
    std::array<float, DEVICES> kDevicePerH{}; // extra exchange per device when it is on (bubblers, pumps)
    float respirationDayPctPerH = 2.0f;
    float respirationNightPctPerH = 6.0f;
    float photosynthesisPctPerH = 5.0f; // lights on
    uint16_t lightsOnMin = 9 * 60;
    uint16_t lightsOffMin = 20 * 60;
    float tempDayC = 26.5f;
    float tempNightC = 25.6f;
    float salinityPsu = 35.0f;
    uint8_t returnPumpDevice = 0xFF; // device whose power state gates k_flow (0xFF = always flowing)
    float sat0Pct = 100.0f;
};

class Tank
{
public:
    explicit Tank(const TankConfig& pCfg)
        : mCfg(pCfg)
        , mSat(pCfg.sat0Pct)
    {
    }

    // Advance dt seconds. `device_on` is the merged, semantic "powered" state per device.
    void Step(float pDtS, uint16_t pMinuteOfDay, std::span<const bool, DEVICES> pDeviceOn);

    float SatPct() const { return mSat; }
    float TempC() const { return mTemp; }
    float DoMgl() const; // true seawater mg/L
    bool LightsOn(uint16_t pMinuteOfDay) const;

    // Scenario knobs
    void StallReturnPump(bool pStalled)
    {
        mStalled = pStalled;
    } // powered but not moving water; a power cycle clears it
    bool ReturnPumpStalled() const { return mStalled; }
    void SetDeviceK(std::size_t pI, float pKPerH) { mCfg.kDevicePerH[pI] = pKPerH; }
    void SetSat(float pSatPct) { mSat = pSatPct; }

private:
    TankConfig mCfg;
    float mSat;
    float mTemp = 26.0f;
    bool mStalled = false;
    bool mPumpWasOn = true;
};

struct ProbeModel
{
    float tauS = 17.0f;               // first-order lag (T90 < 40 s)
    float noisePct = 0.25f;           // uniform ± on saturation
    float dtS = 10.0f;                // time between polls
    bool freshwaterReference = false; // report mg/L against freshwater solubility
    // Fault injection
    bool dropout = false;       // every poll times out
    bool stuck = false;         // repeats the last reading bit for bit
    bool implausible = false;   // reports NaN
    float bubblePct = 0.0f;     // added to the reported saturation (a bubble sitting on the cap)
    uint32_t crcErrorEvery = 0; // every Nth poll is a frame error
};

class Probe : public probe::IProbe
{
public:
    Probe(Tank& pTank, const ProbeModel& pModel, uint64_t pSeed = 1);

    probe::PollResult Poll() override;
    probe::CalResult AirCalibrate() override
    {
        ++mCalibrations;
        return probe::CalResult::Ok;
    }

    ProbeModel& Model() { return mModel; }
    uint32_t Calibrations() const { return mCalibrations; }

private:
    float Uniform(float pLo, float pHi);

    Tank& mTank;
    ProbeModel mModel;
    uint64_t mRng;
    float mLaggedSat;
    uint32_t mPolls = 0;
    uint32_t mCalibrations = 0;
    probe::Reading mLast{};
    bool mHaveLast = false;
};

} // namespace reefdo::sim
