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

bool InWindows(const FixedVector<config::TimeWindow, config::WINDOWS>& pWindows, uint16_t pMinute)
{
    for(const config::TimeWindow& w : pWindows)
    {
        if(w.Contains(pMinute)) return true;
    }
    return false;
}

// Whole seconds left, rounded up; the caller has already ended anything at or past its time.
uint32_t LeftS(const std::optional<uint64_t>& pUntilMs, uint64_t pNowMs)
{
    if(!pUntilMs.has_value()) return 0;
    return static_cast<uint32_t>((*pUntilMs - pNowMs + 999u) / 1000u);
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
    ResetLinks(nullptr);
}

void App::ResetLinks(const std::array<slot::AnySlot, DEVICES>* pOld)
{
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const slot::AnySlot& s = mCfg.devices[i].slot;
        if(pOld != nullptr && (*pOld)[i] == s) continue;
        mStatus.tuyaLink[i] = s.AsTuya() != nullptr ? Link::Pending : Link::None;
        mStatus.tuyaError[i] = tuya::Error::None;
        mTuyaFails[i] = 0;
    }
}

uint32_t App::UptimeS(const Clock& pClock) const
{
    return static_cast<uint32_t>((pClock.nowMs - mBootMs) / 1000u);
}

std::optional<selftest::LocalTime> App::Local(const Clock& pClock) const
{
    if(!pClock.unixS.has_value()) return std::nullopt;
    const int64_t l = static_cast<int64_t>(*pClock.unixS) + pClock.tzOffsetS;
    const int64_t day = l / 86400;
    const int64_t sec = l - day * 86400;
    return selftest::LocalTime{static_cast<uint16_t>(sec / 60), static_cast<uint32_t>(day)};
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
    r.level = static_cast<uint8_t>(mStatus.level);
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

Notifications App::TakeNotifications()
{
    Notifications out = mNotifications;
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
        r.level = static_cast<uint8_t>(pOut.level);
        r.flags = static_cast<uint16_t>(static_cast<uint8_t>(e.from) | (static_cast<uint8_t>(e.to) << 8));
        r.aux = static_cast<uint32_t>(e.type) | (static_cast<uint32_t>(e.device) << 8);
        r.f0 = pDoNow;
        if(e.type == ladder::EventType::SuddenDrop) r.f1 = mStatus.fastSlopeMglPer10min;
        if(e.type == ladder::EventType::AlertSuspend || e.type == ladder::EventType::AlertResume)
            r.f1 = static_cast<float>(e.detail);
        Write(r, true);
        ++mDaily.events;

        Notification n{NotifyKind::Level};
        n.value = pDoNow; // the DO the event happened at
        switch(e.type)
        {
            case ladder::EventType::LevelChange:
                if(e.to == Level::Normal)
                {
                    n.kind = NotifyKind::Recovered;
                }
                else if(e.to > e.from)
                {
                    n.level = e.to;
                    n.urgent = e.to >= Level::Yellow;
                    mLastAlertNotifyMs = pClock.nowMs;
                }
                else
                {
                    n.level = e.to; // stepping down: informational
                }
                Notify(n);
                break;
            case ladder::EventType::FaultEnter:
                n.kind = NotifyKind::Fault;
                n.level = mCfg.ladder.faultLevel; // whose devices it runs
                n.urgent = true;
                mLastAlertNotifyMs = pClock.nowMs;
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

void App::LogBoostEvents(const Clock& pClock, const boost::Output& pOut)
{
    for(const boost::Event& e : pOut.events)
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = UptimeS(pClock);
        r.type = log::Type::Boost;
        r.level = static_cast<uint8_t>(mStatus.level);
        r.aux = static_cast<uint32_t>(e.type);
        r.f0 = e.doMgl;
        r.f1 = mCfg.boost.targetMgl;
        Write(r, true);
        ++mDaily.events;
    }
}

void App::LogTestEvents(const Clock& pClock, const selftest::Output& pOut)
{
    for(const selftest::Event& e : pOut.events)
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = UptimeS(pClock);
        r.type = log::Type::Test;
        r.level = static_cast<uint8_t>(mStatus.level);
        r.flags = static_cast<uint16_t>((e.judged ? 1 : 0) | (e.clockUnknown ? 2 : 0) | (e.aborted ? 4 : 0) |
                                        (e.highDo ? 8 : 0));
        r.aux = static_cast<uint32_t>(e.type) | (static_cast<uint32_t>(e.device) << 8) |
                (static_cast<uint32_t>(e.outcome) << 16) | (static_cast<uint32_t>(e.skip) << 24);
        r.f0 = e.response;
        r.f1 = e.doMgl;
        r.f2 = e.highDo ? mCfg.test.noFailAboveMgl : 0.0f;
        Write(r, true);
        ++mDaily.events;
        if(e.type == selftest::EventType::FailAlert)
        {
            Notification n{NotifyKind::TestFail};
            n.device = e.device;
            n.value = e.response;
            Notify(n);
        }
        else if(e.type == selftest::EventType::InconclusiveAlert)
        {
            Notify(Notification{NotifyKind::TestInconclusive});
        }
    }
}

void App::AlertRepeat(const Clock& pClock)
{
    const bool alarm = mStatus.fault || mStatus.level >= Level::Yellow;
    if(!alarm || mStatus.silenced || mStatus.maintenance || mStatus.alertsSuspended) return;
    const bool loud = mStatus.fault || mStatus.level == Level::Red; // FAULT repeats like Red
    if(pClock.nowMs - mLastAlertNotifyMs < (loud ? RED_REPEAT_MS : YELLOW_REPEAT_MS)) return;
    mLastAlertNotifyMs = pClock.nowMs;
    Notification n{mStatus.fault ? NotifyKind::Fault : NotifyKind::Level};
    n.level = mStatus.fault ? mCfg.ladder.faultLevel : mStatus.level;
    n.value = mStatus.doMgl.value_or(0.0f);
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

void App::DailyRollover(const Clock& pClock, const std::optional<selftest::LocalTime>& pLt)
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
    const std::optional<selftest::LocalTime> lt = Local(pClock);
    for(std::size_t i = 0; i < DEVICES; ++i)
        Expire(i, pClock);
    if(mMaintenanceUntilMs.has_value() && pClock.nowMs >= *mMaintenanceUntilMs) EndMaintenance(pClock, 1);
    mStatus.maintenanceLeftS = LeftS(mMaintenanceUntilMs, pClock.nowMs);
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
        mFastSlopeDo.Push(pClock.nowMs, *doMed);
        mStatus.tempC = pr.reading->tempC;
    }
    if(pr.reading.has_value() && pr.status != probe::Status::Stuck)
        mStatus.consecutiveFailures = 0;
    else
        ++mStatus.consecutiveFailures;
    mStatus.doMgl = doMed;
    mStatus.satPct = satMed;
    mStatus.slopeMglPer10min = mSlopeDo.SlopePer10min();
    mStatus.fastSlopeMglPer10min = mFastSlopeDo.SlopePer10min();

    // 2. Ladder
    const Level wasLevel = mStatus.level;
    const bool wasFault = mStatus.fault;
    ladder::Input li;
    li.doMgl = pr.status == probe::Status::Stuck ? std::nullopt : doMed; // a frozen value is not a reading
    li.slopeMglPer10min = mStatus.slopeMglPer10min;
    li.fastSlopeMglPer10min = mStatus.fastSlopeMglPer10min;
    li.nowMs = pClock.nowMs;
    if(lt.has_value()) li.minuteOfDay = lt->minuteOfDay;
    li.ackPressed = mAckPending;
    li.suspendAlertsS = mSuspendAlertsPending;
    li.maintenance = mStatus.maintenance;
    mAckPending = false;
    mSuspendAlertsPending.reset();
    const ladder::Output lo = ladder::Step(mLadder, li, mCfg.ladder);
    mStatus.level = lo.level;
    mStatus.fault = lo.fault;
    mStatus.silenced = lo.silenced;
    mStatus.suppressed = lo.suppressed;
    mStatus.suspect = lo.suspect;
    mStatus.alertsSuspended = lo.alertsSuspended;
    mStatus.alertsSuspendLeftS = lo.alertsSuspendLeftS;
    mStatus.sound = lo.sound;

    // 3. Self test
    selftest::Input si;
    si.nowMs = pClock.nowMs;
    si.local = lt;
    si.satPct = satMed;
    si.doMgl = doMed;
    si.level = lo.level;
    si.fault = lo.fault;
    si.maintenance = mStatus.maintenance;
    for(std::size_t i = 0; i < DEVICES; ++i)
        si.deviceOut[i] = mSuspendUntilMs[i].has_value();
    const bool wasTesting = mStatus.testRunning;
    si.runNow = mRunTestPending;
    mRunTestPending = false;
    const selftest::Output so = selftest::Step(mTest, si, mCfg.test);
    mStatus.testRunning = so.running;
    mStatus.testPhase = mTest.phase;
    mStatus.testDevice = static_cast<uint8_t>(mTest.device);
    mStatus.deviceFailed = so.failed;
    mStatus.inconclusiveAlert = so.inconclusiveAlert;
    mStatus.chirp = so.chirp; // runs start only at Normal, where the buzzer is silent

    // 3b. Boost: yields to a test run and to maintenance
    boost::Input bi;
    bi.local = lt;
    bi.doMgl = li.doMgl; // a stuck reading is no reading here either
    bi.testRunning = so.running;
    bi.maintenance = mStatus.maintenance;
    const boost::Output bo = boost::Step(mBoost, bi, mCfg.boost);
    mStatus.boostRunning = bo.running;

    // 4. Merge device demands: any one keeps a device on. An exclusive test run holds the windows off.
    // A manual switch lasts until the next scheduled or triggered change, and an exclusive test starts clean.
    const bool holdOff = mCfg.test.exclusive && so.running;
    const bool testStarts = holdOff && !wasTesting;
    const bool levelChanged = lo.level != wasLevel || lo.fault != wasFault; // what runs because of the ladder
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const bool window = !holdOff && lt.has_value() && InWindows(mCfg.devices[i].windows, lt->minuteOfDay);
        uint8_t why = static_cast<uint8_t>((lo.deviceOn[i] ? DEMAND_LADDER : 0) | (so.deviceOn[i] ? DEMAND_TEST : 0) |
                                           (bo.deviceOn[i] ? DEMAND_BOOST : 0) | (window ? DEMAND_WINDOW : 0));
        if(so.cutDevice.has_value() && *so.cutDevice == i) why = 0;
        if(mManual[i].has_value() && testStarts) EndManual(i, pClock, 2);
        if(mManual[i].has_value() && (why != mAutoWhy[i] || levelChanged)) EndManual(i, pClock, 1);
        mAutoWhy[i] = why;
        Resolve(i, pClock);
    }

    // 5. Log
    const float doNow = doMed.value_or(0.0f);
    if(pr.reading.has_value())
    {
        log::Record r;
        r.ts = pClock.unixS.value_or(0);
        r.uptimeS = mStatus.uptimeS;
        r.type = log::Type::Measurement;
        r.level = static_cast<uint8_t>(lo.level);
        r.flags = static_cast<uint16_t>(
            (pr.status == probe::Status::Stuck ? FLAG_STUCK : 0) | (mStatus.maintenance ? FLAG_MAINTENANCE : 0) |
            (lo.silenced ? FLAG_SILENCED : 0) | (so.running ? FLAG_TEST_RUNNING : 0) |
            (lt.has_value() ? 0 : FLAG_CLOCK_UNKNOWN) | (AnySuspended() ? FLAG_SUSPENDED : 0) |
            (lo.suspect ? FLAG_SUSPECT : 0) | (lo.alertsSuspended ? FLAG_ALERTS_SUSPENDED : 0));
        r.f0 = *doMed;
        r.f1 = *satMed;
        r.f2 = pr.reading->tempC;
        r.f3 = mStatus.slopeMglPer10min;
        r.aux = static_cast<uint32_t>(pr.reading->doMgl * 1000.0f); // the probe's own number, for the record
        Write(r, false);
        Aggregate(pClock, *pr.reading, *doMed, lo.level);
        // Daily accumulation
        if(mDaily.samples == 0 || *doMed < mDaily.doMin)
        {
            mDaily.doMin = *doMed;
            mDaily.minuteOfMin = lt.has_value() ? lt->minuteOfDay : 0;
        }
        if(mDaily.samples == 0 || *doMed > mDaily.doMax) mDaily.doMax = *doMed;
        mDaily.doSum += *doMed;
        mDaily.tempSum += pr.reading->tempC;
        if(static_cast<uint8_t>(lo.level) > mDaily.levelMax) mDaily.levelMax = static_cast<uint8_t>(lo.level);
        ++mDaily.samples;
    }
    LogLadderEvents(pClock, lo, doNow);
    LogTestEvents(pClock, so);
    LogBoostEvents(pClock, bo);
    AlertRepeat(pClock);

    // Correction parameters go into the log once a day, just before the test window opens.
    if(lt.has_value() && lt->minuteOfDay + 1 == mCfg.test.windowStartMin && mCorrectionLoggedDay != lt->dayIndex)
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

bool App::SuspendAlerts(uint32_t pSeconds, std::optional<ladder::Level> pSeen)
{
    if(pSeconds > ladder::ALERT_SUSPEND_MAX_S) return false;
    if(pSeconds > 0 && pSeen.has_value() && mStatus.level > *pSeen) return false;       // worse than what was seen
    if(pSeconds > 0 && (mStatus.fault || mStatus.level == Level::Normal)) return false; // nothing to suspend
    if(pSeconds == 0 && !mStatus.alertsSuspended) return false;                         // nothing to resume
    mSuspendAlertsPending = pSeconds;
    return true;
}

void App::SetMaintenance(bool pOn, const Clock& pClock)
{
    if(pOn)
    {
        if(!mStatus.maintenance) LogSimple(pClock, log::Type::Command, 1u, 0.0f, 0.0f); // a restart is not logged
        mStatus.maintenance = true;
        mMaintenanceUntilMs = pClock.nowMs + static_cast<uint64_t>(MAINTENANCE_S) * 1000u;
    }
    else if(mStatus.maintenance)
    {
        EndMaintenance(pClock, 0);
    }
    mStatus.maintenanceLeftS = LeftS(mMaintenanceUntilMs, pClock.nowMs);
}

void App::EndMaintenance(const Clock& pClock, uint32_t pWhy)
{
    mStatus.maintenance = false;
    mMaintenanceUntilMs.reset();
    LogSimple(pClock, log::Type::Command, 2u, 0.0f, static_cast<float>(pWhy));
}

void App::RunTest()
{
    mRunTestPending = true;
}

bool App::SetConfig(const config::Config& pCfg, const Clock& pClock)
{
    const bool correctionChanged = !(pCfg.correction == mCfg.correction);
    std::array<slot::AnySlot, DEVICES> old; // not the whole config: this runs on the web server's small stack
    for(std::size_t i = 0; i < DEVICES; ++i)
        old[i] = mCfg.devices[i].slot;
    mCfg = pCfg;
    ResetLinks(&old);
    UpdateRelays(); // a device may have moved to another relay, or to a plug
    LogSimple(pClock, log::Type::Config, mCfg.schema, 0.0f, 0.0f);
    if(correctionChanged) LogSimple(pClock, log::Type::Correction, 2, mCfg.correction.scale, mCfg.correction.offset);
    return true;
}

bool App::SuspendDevice(std::size_t pDevice, uint32_t pSeconds, const Clock& pClock)
{
    if(pDevice >= DEVICES || pSeconds > SUSPEND_MAX_S) return false;
    if(pSeconds == 0)
    {
        if(mSuspendUntilMs[pDevice].has_value()) EndSuspension(pDevice, pClock, 0);
    }
    else
    {
        mSuspendUntilMs[pDevice] = pClock.nowMs + static_cast<uint64_t>(pSeconds) * 1000u;
        LogSimple(pClock, log::Type::Command, 4u | static_cast<uint32_t>(pDevice) << 8, static_cast<float>(pSeconds),
                  0.0f);
    }
    Resolve(pDevice, pClock);
    return true;
}

bool App::SetManual(std::size_t pDevice, std::optional<bool> pState, const Clock& pClock)
{
    if(pDevice >= DEVICES) return false;
    if(pState.has_value() && mCfg.test.exclusive && mStatus.testRunning) return false; // the test owns the tank
    if(!pState.has_value())
    {
        if(mManual[pDevice].has_value()) EndManual(pDevice, pClock, 0);
    }
    else if(mManual[pDevice] != pState)
    {
        mManual[pDevice] = pState;
        LogSimple(pClock, log::Type::Command, (*pState ? 6u : 7u) | static_cast<uint32_t>(pDevice) << 8, 0.0f, 0.0f);
    }
    Resolve(pDevice, pClock);
    return true;
}

void App::EndSuspension(std::size_t pI, const Clock& pClock, uint32_t pWhy)
{
    mSuspendUntilMs[pI].reset();
    LogSimple(pClock, log::Type::Command, 5u | static_cast<uint32_t>(pI) << 8, 0.0f, static_cast<float>(pWhy));
}

void App::EndManual(std::size_t pI, const Clock& pClock, uint32_t pWhy)
{
    mManual[pI].reset();
    LogSimple(pClock, log::Type::Command, 8u | static_cast<uint32_t>(pI) << 8, 0.0f, static_cast<float>(pWhy));
}

void App::Expire(std::size_t pI, const Clock& pClock)
{
    if(mSuspendUntilMs[pI].has_value() && pClock.nowMs >= *mSuspendUntilMs[pI]) EndSuspension(pI, pClock, 1);
}

bool App::AnySuspended() const
{
    for(const std::optional<uint64_t>& u : mSuspendUntilMs)
    {
        if(u.has_value()) return true;
    }
    return false;
}

void App::Resolve(std::size_t pI, const Clock& pClock)
{
    Expire(pI, pClock);
    uint8_t why = mAutoWhy[pI];
    if(mManual[pI].value_or(false)) why = static_cast<uint8_t>(why | DEMAND_MANUAL);
    bool on = mManual[pI].value_or(why != 0); // switched off by hand: off until the next change, whatever wants it
    if(mSuspendUntilMs[pI].has_value()) on = false; // out of order beats everything
    mStatus.deviceWhy[pI] = why;
    mStatus.suspendLeftS[pI] = LeftS(mSuspendUntilMs[pI], pClock.nowMs);
    mStatus.manual[pI] = mManual[pI];
    mStatus.deviceOn[pI] = on;
    UpdateRelays(); // a plug is switched by the Tuya task from deviceOn
}

void App::UpdateRelays()
{
    mStatus.relayEnergised.fill(false);
    for(std::size_t i = 0; i < DEVICES; ++i)
    {
        const slot::Relay* r = mCfg.devices[i].slot.AsRelay();
        if(r == nullptr || r->channel - 1u >= RELAYS) continue; // channel 0 wraps out of range too
        mStatus.relayEnergised[r->channel - 1] =
            r->wiring == slot::Wiring::Nc ? !mStatus.deviceOn[i] : mStatus.deviceOn[i];
    }
}

bool App::ReportTuya(std::size_t pDevice, tuya::Error pError, const Clock& pClock)
{
    if(pDevice >= DEVICES || mCfg.devices[pDevice].slot.AsTuya() == nullptr) return false;
    mStatus.tuyaError[pDevice] = pError;
    Notification n{NotifyKind::DeviceBack};
    n.device = static_cast<uint8_t>(pDevice);
    if(pError == tuya::Error::None)
    {
        mTuyaFails[pDevice] = 0;
        if(mStatus.tuyaLink[pDevice] == Link::Lost)
        {
            LogSimple(pClock, log::Type::Command, 10u | static_cast<uint32_t>(pDevice) << 8, 0.0f, 0.0f);
            Notify(n);
        }
        mStatus.tuyaLink[pDevice] = Link::Ok;
        return true;
    }
    ++mTuyaFails[pDevice];
    if(mTuyaFails[pDevice] >= TUYA_LOST_AFTER && mStatus.tuyaLink[pDevice] != Link::Lost)
    {
        mStatus.tuyaLink[pDevice] = Link::Lost;
        LogSimple(pClock, log::Type::Command, 9u | static_cast<uint32_t>(pDevice) << 8, static_cast<float>(pError),
                  0.0f);
        n.kind = NotifyKind::DeviceLost;
        n.urgent = true;
        n.value = static_cast<float>(pError);
        Notify(n);
    }
    return true;
}

probe::CalResult App::AirCalibrate(const Clock& pClock)
{
    if(!mStatus.maintenance) return probe::CalResult::Refused;
    const probe::CalResult r = mProbe.AirCalibrate();
    LogSimple(pClock, log::Type::Command, 3, static_cast<float>(r), 0.0f);
    return r;
}

bool App::ClearFailure(std::size_t pDevice, const Clock& pClock)
{
    if(pDevice >= DEVICES) return false;
    if(mTest.p.failActive[pDevice])
    {
        mTest.p.failActive[pDevice] = false;
        mStatus.deviceFailed[pDevice] = false;
        LogSimple(pClock, log::Type::Command, 11u | static_cast<uint32_t>(pDevice) << 8, 0.0f, 0.0f);
    }
    return true;
}

bool App::TestPersistentChanged()
{
    const bool changed = !(mTest.p == mTestSaved);
    if(changed) mTestSaved = mTest.p;
    return changed;
}

} // namespace reefdo::app
