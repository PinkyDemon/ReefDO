#include "reefdo/app.hpp"

#include <algorithm>

namespace reefdo::app
{

using ladder::Level;

namespace
{

constexpr uint32_t YELLOW_REPEAT_MS = 600'000;
constexpr uint32_t RED_REPEAT_MS = 120'000;

uint32_t Max3(uint32_t pA, uint32_t pB, uint32_t pC)
{
    const uint32_t ab = pA > pB ? pA : pB;
    return ab > pC ? ab : pC;
}

uint32_t LastSeq(const log::RecordLog& pL)
{
    const std::optional<log::Record> r = pL.last();
    return r.has_value() ? r->seq : 0;
}

} // namespace

App::App(const config::Config& pCfg, probe::IProbe& pProbe, Stores pStores)
    : mCfg(pCfg)
    , mProbe(pProbe)
    , mLogA(pStores.a)
    , mLogB(pStores.b)
    , mLogE(pStores.e)
    , mLogD(pStores.d)
{
    ApplyConfig();
}

void App::ApplyConfig()
{
    mLadderCfg = mCfg.ladder;
    mServiceCfg = service::Config{};
    mServiceCfg.windowStartMin = mCfg.service.windowStartMin;
    mServiceCfg.windowEndMin = mCfg.service.windowEndMin;
    mServiceCfg.settleS = mCfg.service.settleS;
    mServiceCfg.tailS = mCfg.service.tailS;
    mServiceCfg.minHeadroomPct = mCfg.service.minHeadroomPct;
    mServiceCfg.inconclusiveDays = mCfg.service.inconclusiveDays;
    mServiceCfg.chirp = mCfg.service.chirp;
    mServiceCfg.induceDeficitS = mCfg.service.induceDeficitS;
    mServiceCfg.induceDeficitDevice = mCfg.service.induceDeficitDevice;
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        mServiceCfg.devices[i].serviceS = mCfg.devices[i].serviceS;
        mServiceCfg.devices[i].minResponsePct = mCfg.devices[i].minResponsePct;
    }
}

uint32_t App::UptimeS(const Clock& pClock) const
{
    return static_cast<uint32_t>((pClock.nowMs - mBootMs) / 1000u);
}

std::optional<service::LocalTime> App::Local(const Clock& pClock) const
{
    if(!pClock.unixS.has_value()) return std::nullopt;
    const int64_t l = static_cast<int64_t>(*pClock.unixS) + pClock.tzOffsetS;
    const int64_t day = l / 86400;
    const int64_t sec = l - day * 86400;
    return service::LocalTime{static_cast<uint16_t>(sec / 60), static_cast<uint32_t>(day)};
}

void App::Write(log::Record& pR, bool pAlsoEvents)
{
    pR.seq = mNextSeq++;
    mLogA.Append(pR);
    if(pAlsoEvents) mLogE.Append(pR);
}

void App::LogSimple(const Clock& pClock, log::Type pType, uint32_t pAux, float pF0, float pF1)
{
    log::Record r;
    r.ts = pClock.unixS.value_or(0);
    r.uptimeS = UptimeS(pClock);
    r.type = pType;
    r.level = static_cast<uint8_t>(mStatus.effective);
    r.aux = pAux;
    r.f0 = pF0;
    r.f1 = pF1;
    Write(r, true);
}

void App::Start(const Clock& pClock, uint32_t pBootReason)
{
    mBootMs = pClock.nowMs;
    mLogA.Open();
    mLogB.Open();
    mLogE.Open();
    mLogD.Open();
    mNextSeq = Max3(LastSeq(mLogA), LastSeq(mLogE), LastSeq(mLogD)) + 1;
    mStarted = true;
    LogSimple(pClock, log::Type::Boot, pBootReason, 0.0f, 0.0f);
    LogSimple(pClock, log::Type::Correction, 0, mCfg.correction.scale, mCfg.correction.offset);
    LogSimple(pClock, log::Type::Config, mCfg.schema, 0.0f, 0.0f);
    Notification n{NotifyKind::Boot};
    n.value = static_cast<float>(pBootReason);
    Notify(n);
    TimeSync(pClock);
}

void App::Notify(Notification pN)
{
    mNotifications.push_back(pN);
}

FixedVector<Notification, 8> App::TakeNotifications()
{
    FixedVector<Notification, 8> out = mNotifications;
    mNotifications.clear();
    return out;
}

void App::TimeSync(const Clock& pClock)
{
    if(!pClock.unixS.has_value()) return;
    // Expected wall clock from the last known one plus the monotonic time elapsed since.
    float delta = 0.0f;
    bool logIt = !mLastUnix.has_value();
    if(mLastUnix.has_value())
    {
        const int64_t expected =
            static_cast<int64_t>(*mLastUnix) + static_cast<int64_t>((pClock.nowMs - mLastUnixAtMs) / 1000u);
        const int64_t d = static_cast<int64_t>(*pClock.unixS) - expected;
        delta = static_cast<float>(d);
        logIt = d > 5 || d < -5;
    }
    mLastUnix = *pClock.unixS;
    mLastUnixAtMs = pClock.nowMs;
    if(logIt) LogSimple(pClock, log::Type::TimeSync, 0, delta, 0.0f);
}

void App::LogLadderEvents(const Clock& pClock, const ladder::Output& pOut, float pDoNow)
{
    for(const ladder::Event& e : pOut.events)
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = UptimeS(pClock);
        r.type = log::Type::Event;
        r.level = static_cast<uint8_t>(pOut.effective);
        r.flags = static_cast<uint16_t>(static_cast<uint8_t>(e.from) | (static_cast<uint8_t>(e.to) << 8));
        r.aux = static_cast<uint32_t>(e.type) | (static_cast<uint32_t>(e.device) << 8);
        r.f0 = pDoNow;
        Write(r, true);
        ++mDaily.events;

        Notification n{NotifyKind::Level};
        switch(e.type)
        {
            case ladder::EventType::EffectiveChange:
                if(e.to == Level::Normal)
                {
                    n.kind = NotifyKind::Recovered;
                }
                else if(e.to > e.from)
                {
                    n.level = e.to;
                    n.urgent = e.to >= Level::Yellow;
                    mLastAlertNotifyMs = pClock.nowMs;
                    mLastNotifiedLevel = e.to;
                }
                else
                {
                    n.level = e.to; // stepping down: informational
                }
                Notify(n);
                break;
            case ladder::EventType::FaultEnter:
                n.kind = NotifyKind::Fault;
                n.urgent = true;
                Notify(n);
                break;
            case ladder::EventType::FaultClear:
                n.kind = NotifyKind::FaultCleared;
                Notify(n);
                break;
            default: break;
        }
    }
}

void App::LogServiceEvents(const Clock& pClock, const service::Output& pOut)
{
    for(const service::Event& e : pOut.events)
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = UptimeS(pClock);
        r.type = log::Type::Service;
        r.level = static_cast<uint8_t>(mStatus.effective);
        r.flags = static_cast<uint16_t>((e.judged ? 1 : 0) | (e.clockUnknown ? 2 : 0) | (e.aborted ? 4 : 0));
        r.aux = static_cast<uint32_t>(e.type) | (static_cast<uint32_t>(e.device) << 8) |
                (static_cast<uint32_t>(e.outcome) << 16) | (static_cast<uint32_t>(e.skip) << 24);
        r.f0 = e.response;
        Write(r, true);
        ++mDaily.events;
        if(e.type == service::EventType::FailAlert)
        {
            Notification n{NotifyKind::ServiceFail};
            n.device = e.device;
            n.value = e.response;
            Notify(n);
        }
        else if(e.type == service::EventType::InconclusiveAlert)
        {
            Notify(Notification{NotifyKind::ServiceInconclusive});
        }
    }
}

void App::AlertRepeat(const Clock& pClock)
{
    if(mStatus.effective < Level::Yellow || mStatus.silenced || mStatus.maintenance) return;
    const uint32_t interval = mStatus.effective == Level::Red ? RED_REPEAT_MS : YELLOW_REPEAT_MS;
    if(pClock.nowMs - mLastAlertNotifyMs < interval) return;
    mLastAlertNotifyMs = pClock.nowMs;
    Notification n{mStatus.fault ? NotifyKind::Fault : NotifyKind::Level};
    n.level = mStatus.effective;
    n.urgent = true;
    n.repeat = true;
    Notify(n);
}

void App::Aggregate(const Clock& pClock, const probe::Reading& pR, float pDoC, Level pEff)
{
    if(!pClock.unixS.has_value()) return; // aggregates need a real timestamp
    const uint32_t bucket = *pClock.unixS / 300u;
    if(mAggregateBucket.has_value() && bucket != *mAggregateBucket)
    {
        mLogB.Append(*mAggregator.Flush(*mAggregateBucket * 300u)); // never empty: every call adds a sample below
    }
    mAggregateBucket = bucket;
    mAggregator.Add(pDoC, pR.satPct, pR.tempC, static_cast<uint8_t>(pEff));
}

void App::DailyRollover(const Clock& pClock, const std::optional<service::LocalTime>& pLt)
{
    if(!pLt.has_value()) return;
    if(mDailyDay.has_value() && pLt->dayIndex != *mDailyDay && mDaily.samples > 0)
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = UptimeS(pClock);
        r.type = log::Type::Daily;
        r.level = mDaily.levelMax;
        r.f0 = mDaily.doMin;
        r.f1 = mDaily.doSum / static_cast<float>(mDaily.samples);
        r.f2 = mDaily.doMax;
        r.f3 = mDaily.tempSum / static_cast<float>(mDaily.samples);
        r.aux = mDaily.minuteOfMin | (static_cast<uint32_t>(mDaily.levelMax) << 16) |
                (std::min<uint32_t>(mDaily.events, 255u) << 24);
        r.seq = mNextSeq++;
        mLogA.Append(r);
        mLogD.Append(r);
        mDaily = Daily{};
    }
    mDailyDay = pLt->dayIndex;
}

void App::Tick(const Clock& pClock)
{
    const std::optional<service::LocalTime> lt = Local(pClock);
    TimeSync(pClock);
    DailyRollover(pClock, lt);
    mStatus.clockKnown = lt.has_value();
    mStatus.uptimeS = UptimeS(pClock);
    ++mStatus.ticks;

    // 1. Probe → correction → filters
    const probe::PollResult pr = mProbe.Poll();
    mStatus.probe = pr.status;
    std::optional<float> doMed;
    std::optional<float> satMed;
    // A frozen reading (Stuck) is a sensor failure too: it is logged, but it counts towards FAULT.
    if(pr.reading.has_value())
    {
        const float doC = mCfg.correction.Apply(pr.reading->doMgl);
        doMed = mMedDo.Push(doC);
        satMed = mMedSat.Push(pr.reading->satPct);
        mSlopeDo.Push(pClock.nowMs, *doMed);
        mStatus.tempC = pr.reading->tempC;
    }
    if(pr.reading.has_value() && pr.status != probe::Status::Stuck)
        mStatus.consecutiveFailures = 0;
    else
        ++mStatus.consecutiveFailures;
    mStatus.doMgl = doMed;
    mStatus.satPct = satMed;
    mStatus.slopeMglPer10min = mSlopeDo.SlopePer10min();

    // 2. Ladder
    ladder::Input li;
    li.doMgl = pr.status == probe::Status::Stuck ? std::nullopt : doMed; // a frozen value is not a reading
    li.slopeMglPer10min = mStatus.slopeMglPer10min;
    li.tempC = pr.reading.has_value() ? std::optional<float>(pr.reading->tempC) : std::nullopt;
    li.nowMs = pClock.nowMs;
    if(lt.has_value()) li.minuteOfDay = lt->minuteOfDay;
    li.ackPressed = mAckPending;
    li.maintenance = mStatus.maintenance;
    li.deviceFailed = mService.p.failActive;
    mAckPending = false;
    const ladder::Output lo = ladder::Step(mLadder, li, mLadderCfg);
    mStatus.level = lo.level;
    mStatus.effective = lo.effective;
    mStatus.fault = lo.fault;
    mStatus.silenced = lo.silenced;
    mStatus.heat = lo.heat;
    mStatus.buzzer = lo.buzzer;
    mStatus.led = lo.led;

    // 3. Service
    service::Input si;
    si.nowMs = pClock.nowMs;
    si.local = lt;
    si.satPct = satMed;
    si.level = lo.level;
    si.fault = lo.fault;
    si.maintenance = mStatus.maintenance;
    si.runNow = mRunServicePending;
    mRunServicePending = false;
    const service::Output so = service::Step(mService, si, mServiceCfg);
    mStatus.serviceRunning = so.running;
    mStatus.servicePhase = mService.phase;
    mStatus.serviceDevice = static_cast<uint8_t>(mService.device);
    mStatus.deviceFailed = so.failed;
    mStatus.inconclusiveAlert = so.inconclusiveAlert;
    mStatus.chirp = so.chirp; // runs start only at Normal, where the buzzer is silent

    // 4. Merge device demands; apply polarity
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        bool on = lo.deviceOn[i] || so.deviceOn[i];
        if(so.cutDevice.has_value() && *so.cutDevice == i) on = false;
        mAutoOn[i] = on;
        if(mStatus.maintenance && mOverride[i].has_value()) on = *mOverride[i] || on; // automatic demands win
        mStatus.deviceOn[i] = on;
        mStatus.relayEnergised[i] = mCfg.devices[i].wiredNc ? !on : on;
    }

    // 5. Log
    const float doNow = doMed.value_or(0.0f);
    if(pr.reading.has_value())
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = mStatus.uptimeS;
        r.type = log::Type::Measurement;
        r.level = static_cast<uint8_t>(lo.effective);
        r.flags = static_cast<uint16_t>((pr.status == probe::Status::Stuck ? FLAG_STUCK : 0) |
                                        (mStatus.maintenance ? FLAG_MAINTENANCE : 0) |
                                        (lo.silenced ? FLAG_SILENCED : 0) | (so.running ? FLAG_SERVICE_RUNNING : 0) |
                                        (lo.heat ? FLAG_HEAT : 0) | (lt.has_value() ? 0 : FLAG_CLOCK_UNKNOWN));
        r.f0 = *doMed;
        r.f1 = *satMed;
        r.f2 = pr.reading->tempC;
        r.f3 = mStatus.slopeMglPer10min;
        r.aux = static_cast<uint32_t>(pr.reading->doMgl * 1000.0f); // the probe's own number, for the record
        Write(r, false);
        Aggregate(pClock, *pr.reading, *doMed, lo.effective);
        // Daily accumulation
        if(mDaily.samples == 0 || *doMed < mDaily.doMin)
        {
            mDaily.doMin = *doMed;
            mDaily.minuteOfMin = lt.has_value() ? lt->minuteOfDay : 0;
        }
        if(mDaily.samples == 0 || *doMed > mDaily.doMax) mDaily.doMax = *doMed;
        mDaily.doSum += *doMed;
        mDaily.tempSum += pr.reading->tempC;
        if(static_cast<uint8_t>(lo.effective) > mDaily.levelMax) mDaily.levelMax = static_cast<uint8_t>(lo.effective);
        ++mDaily.samples;
    }
    LogLadderEvents(pClock, lo, doNow);
    LogServiceEvents(pClock, so);
    AlertRepeat(pClock);

    // Correction parameters go into the log once a day, just before the service window opens.
    if(lt.has_value() && lt->minuteOfDay + 1 == mServiceCfg.windowStartMin && mCorrectionLoggedDay != lt->dayIndex)
    {
        mCorrectionLoggedDay = lt->dayIndex;
        LogSimple(pClock, log::Type::Correction, 1, mCfg.correction.scale, mCfg.correction.offset);
    }
    mStatus.nextSeq = mNextSeq;
}

void App::Ack()
{
    mAckPending = true;
}

void App::SetMaintenance(bool pOn, const Clock& pClock)
{
    if(pOn == mStatus.maintenance) return;
    mStatus.maintenance = pOn;
    if(!pOn)
    {
        for(auto& o : mOverride)
            o.reset();
    }
    LogSimple(pClock, log::Type::Command, pOn ? 1u : 2u, 0.0f, 0.0f);
}

void App::RunService()
{
    mRunServicePending = true;
}

bool App::SetConfig(const config::Config& pCfg, const Clock& pClock)
{
    const bool correctionChanged = !(pCfg.correction == mCfg.correction);
    mCfg = pCfg;
    ApplyConfig();
    LogSimple(pClock, log::Type::Config, mCfg.schema, 0.0f, 0.0f);
    if(correctionChanged) LogSimple(pClock, log::Type::Correction, 2, mCfg.correction.scale, mCfg.correction.offset);
    return true;
}

bool App::SetDeviceOverride(std::size_t pDevice, std::optional<bool> pOn)
{
    if(!mStatus.maintenance || pDevice >= DEVICES) return false;
    mOverride[pDevice] = pOn;
    // Takes effect now, not at the next sample: a relay test should click when the button is pressed.
    const bool on = pOn.has_value() ? (*pOn || mAutoOn[pDevice]) : mAutoOn[pDevice];
    mStatus.deviceOn[pDevice] = on;
    mStatus.relayEnergised[pDevice] = mCfg.devices[pDevice].wiredNc ? !on : on;
    return true;
}

probe::CalResult App::AirCalibrate(const Clock& pClock)
{
    if(!mStatus.maintenance) return probe::CalResult::Refused;
    const probe::CalResult r = mProbe.AirCalibrate();
    LogSimple(pClock, log::Type::Command, 3, static_cast<float>(r), 0.0f);
    return r;
}

bool App::ServicePersistentChanged()
{
    const bool changed = !(mService.p == mServiceSaved);
    if(changed) mServiceSaved = mService.p;
    return changed;
}

} // namespace reefdo::app
