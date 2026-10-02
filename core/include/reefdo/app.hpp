#pragma once
// The orchestrator: one Tick() per sample period: probe → correction → filters → ladder → self test → log.
// No hardware here: the probe is an IProbe, storage IBlockStores, time a Clock snapshot.
#include <array>
#include <cstdint>
#include <optional>

#include "reefdo/boost.hpp"
#include "reefdo/config.hpp"
#include "reefdo/filter.hpp"
#include "reefdo/fixed_vector.hpp"
#include "reefdo/ladder.hpp"
#include "reefdo/log.hpp"
#include "reefdo/probe.hpp"
#include "reefdo/selftest.hpp"

namespace reefdo::app
{

constexpr uint32_t SUSPEND_MAX_S = 2 * 3600; // device maintenance: out of order at most this long per command
constexpr uint32_t MAINTENANCE_S = 30 * 60;  // ReefDO maintenance mode ends by itself this long after it is entered
constexpr uint32_t TUYA_LOST_AFTER = 3;      // failed transactions in a row before a plug counts as lost

// A Tuya plug's link, as its task last reported it (Status::tuyaLink).
enum class Link : uint8_t
{
    None,    // a relay, not a plug
    Pending, // configured, not reached yet
    Ok,
    Lost // TUYA_LOST_AFTER failures in a row: logged and pushed
};

// Why a device is on (Status::deviceWhy). Any bit keeps it on; only a suspension turns it off regardless.
enum Demand : uint8_t
{
    DEMAND_LADDER = 1, // its level, or a pulse device's normal power
    DEMAND_TEST = 2,
    DEMAND_BOOST = 4,
    DEMAND_WINDOW = 8,
    DEMAND_MANUAL = 16, // switched on by hand (App::SetManual; the page's device test too)
};

struct Clock
{
    uint64_t nowMs = 0;            // monotonic
    std::optional<uint32_t> unixS; // wall clock, when known
    int32_t tzOffsetS = 0;         // local = unix + offset
};

struct Stores
{
    log::IBlockStore& a; // every sample + event
    log::IBlockStore& b; // 5-minute aggregates
    log::IBlockStore& e; // events only
    log::IBlockStore& d; // daily summaries
};

enum class NotifyKind : uint8_t
{
    Level,
    Fault,
    FaultCleared,
    Recovered,
    TestFail,
    TestInconclusive,
    Boot,
    DeviceLost, // a Tuya plug stopped answering
    DeviceBack
};

struct Notification
{
    NotifyKind kind;
    ladder::Level level = ladder::Level::Normal;
    uint8_t device = 0;
    float value = 0.0f;
    bool urgent = false;
    bool repeat = false; // a re-send of a standing alarm
};
// Collected between two TakeNotifications(): room for every plug going lost at once, plus the level and test ones.
using Notifications = FixedVector<Notification, DEVICES + 8>;

// Bits in Measurement records' `flags`
enum MeasurementFlag : uint16_t
{
    FLAG_STUCK = 1,
    FLAG_MAINTENANCE = 2,
    FLAG_SILENCED = 4,
    FLAG_TEST_RUNNING = 8,
    FLAG_HEAT = 16, // up to 1.1 only (the heat trigger); never set now
    FLAG_CLOCK_UNKNOWN = 32,
    FLAG_SUSPENDED = 64,         // a device is out of order (device maintenance)
    FLAG_SUSPECT = 128,          // a sudden drop: the ladder's dwells are extended
    FLAG_ALERTS_SUSPENDED = 256, // an alert suspend: alert-driven devices and the alarm as at Normal
};

// Command records: aux = 1 maintenance on, 2 maintenance off (f1 = 0 command, 1 time ran out), 3 air calibration
//   (f0 = result),
//   4 out of order (| device << 8, f0 = seconds), 5 back in order (| device << 8, f1 = 0 command, 1 time ran out),
//   6 manual on / 7 manual off / 8 back to automatic (| device << 8; 8: f1 = 0 command, 1 a schedule or trigger
//   took over, 2 an exclusive test started), 9 plug lost (| device << 8, f0 = tuya::Error), 10 plug back,
//   11 a failed test verdict cleared by hand (| device << 8)
// Test records: aux = event type | device << 8 | outcome << 16 | skip << 24, flags = judged 1 | clock unknown 2
//   | aborted 4 | high DO 8 (a missed response let off: f1 = DO at the device's start, f2 = the limit)
// Ladder event records: aux = ladder::EventType, flags = from | to << 8, aux device in bits 8..15 for Pulse,
//   f0 = DO; SuddenDrop: f1 = the 2 min slope (mg/L per 10 min); AlertSuspend: f1 = seconds; AlertResume: f1 =
//   ladder::ResumeReason

struct Status
{
    std::optional<float> doMgl;  // corrected, median-filtered
    std::optional<float> satPct; // median-filtered
    std::optional<float> tempC;
    float slopeMglPer10min = 0.0f;
    float fastSlopeMglPer10min = 0.0f; // over 2 min: sudden-drop detection
    probe::Status probe = probe::Status::Timeout;
    uint32_t consecutiveFailures = 0;
    ladder::Level level = ladder::Level::Normal;
    bool fault = false;
    bool silenced = false;
    bool suppressed = false;      // day_alarm suppress in effect
    bool suspect = false;         // a sudden drop: dwells extended
    bool alertsSuspended = false; // the current alert suspended (App::SuspendAlerts)
    uint32_t alertsSuspendLeftS = 0;
    bool maintenance = false;
    uint32_t maintenanceLeftS = 0;                     // until maintenance mode ends by itself
    std::array<bool, DEVICES> deviceOn{};              // semantic "powered"
    std::array<bool, RELAYS> relayEnergised{};         // per relay channel, after NO/NC polarity; unassigned = off
    std::array<uint8_t, DEVICES> deviceWhy{};          // Demand bits (a suspended device keeps them, but is off)
    std::array<uint32_t, DEVICES> suspendLeftS{};      // > 0: out of order (device maintenance), seconds left
    std::array<std::optional<bool>, DEVICES> manual{}; // switched on / off by hand; nullopt = automatic
    std::array<Link, DEVICES> tuyaLink{};
    std::array<tuya::Error, DEVICES> tuyaError{}; // the last transaction's
    bool sound = false;                           // the ladder lets the alarm sound (config signals shape it)
    bool chirp = false;                           // this tick only
    bool testRunning = false;
    selftest::Phase testPhase = selftest::Phase::Idle;
    bool boostRunning = false;
    uint8_t testDevice = 0;
    std::array<bool, DEVICES> deviceFailed{};
    bool inconclusiveAlert = false;
    uint32_t nextSeq = 1;
    uint32_t uptimeS = 0;
    bool clockKnown = false;
    uint32_t ticks = 0;
};

class App
{
public:
    App(const config::Config& pCfg, probe::IProbe& pProbe, Stores pStores);

    // Opens the logs, recovers seq, logs Boot / Correction / Config. Call once before tick().
    void Start(const Clock& pClock, uint32_t pBootReason);
    void Tick(const Clock& pClock);

    // Commands (from the console, the web UI, the button)
    void Ack();
    // Alert suspend: the alert-driven devices and the alarm act as at Normal for 1..ladder::ALERT_SUSPEND_MAX_S
    // seconds; 0 resumes. Only during an alert, never in a FAULT; a deeper level, a FAULT or the alert ending
    // resumes by itself. Applied at the next tick, like Ack. pSeen: the level the person saw when they asked (a phone
    // may show an older one); refused if it is deeper now.
    bool SuspendAlerts(uint32_t pSeconds, std::optional<ladder::Level> pSeen = std::nullopt);
    // ReefDO maintenance mode, for MAINTENANCE_S; entering it again restarts the countdown.
    void SetMaintenance(bool pOn, const Clock& pClock);
    void RunTest();
    bool SetConfig(const config::Config& pCfg, const Clock& pClock);

    // Device maintenance, independent of ReefDO maintenance: out of order for 1..SUSPEND_MAX_S seconds — off
    // whatever wants it on, alerts included. 0 puts it back in order.
    bool SuspendDevice(std::size_t pDevice, uint32_t pSeconds, const Clock& pClock);
    // Manual switch: on or off by hand, nullopt = back to automatic. It holds until the next scheduled or triggered
    // change — the device's demands change (a window opens or closes, the test reaches it...), the level or FAULT
    // changes, or an exclusive test starts — and is refused while an exclusive test runs. Device maintenance still
    // wins.
    bool SetManual(std::size_t pDevice, std::optional<bool> pState, const Clock& pClock);
    probe::CalResult AirCalibrate(const Clock& pClock); // maintenance only
    // Clears a device's failed test verdict by hand. The verdict is only shown and pushed; nothing runs because of it.
    bool ClearFailure(std::size_t pDevice, const Clock& pClock);
    // The Tuya task's result for one transaction with a plug. False if that slot is not a plug.
    bool ReportTuya(std::size_t pDevice, tuya::Error pError, const Clock& pClock);

    const Status& GetStatus() const { return mStatus; }
    const config::Config& GetConfig() const { return mCfg; }
    Notifications TakeNotifications();

    const log::RecordLog& LogA() const { return mLogA; }
    const log::AggregateLog& LogB() const { return mLogB; }
    const log::RecordLog& LogE() const { return mLogE; }
    const log::RecordLog& LogD() const { return mLogD; }

    const selftest::State& TestState() const { return mTest; }
    const selftest::Persistent& TestPersistent() const { return mTest.p; }
    void RestoreTest(const selftest::Persistent& p_) { mTest.p = p_; }
    bool TestPersistentChanged(); // true once after a change; the firmware then saves to NVS

private:
    struct Daily
    {
        uint32_t samples = 0;
        float doMin = 0, doMax = 0, doSum = 0, tempSum = 0;
        uint16_t minuteOfMin = 0;
        uint8_t levelMax = 0;
        uint32_t events = 0;
    };

    std::optional<selftest::LocalTime> Local(const Clock& pClock) const;
    // Plugs whose settings changed (all of them without pOld) start over as Pending.
    void ResetLinks(const std::array<slot::AnySlot, DEVICES>* pOld);
    void Write(log::Record& pR, bool pAlsoEvents);
    void LogSimple(const Clock& pClock, log::Type pType, uint32_t pAux, float pF0, float pF1);
    void LogLadderEvents(const Clock& pClock, const ladder::Output& pOut, float pDoNow);
    void LogTestEvents(const Clock& pClock, const selftest::Output& pOut);
    void LogBoostEvents(const Clock& pClock, const boost::Output& pOut);
    void Notify(Notification pN);
    void Aggregate(const Clock& pClock, const probe::Reading& pR, float pDoC, ladder::Level pEff);
    void DailyRollover(const Clock& pClock, const std::optional<selftest::LocalTime>& pLt);
    void TimeSync(const Clock& pClock);
    void AlertRepeat(const Clock& pClock);
    uint32_t UptimeS(const Clock& pClock) const;
    void Resolve(std::size_t pI, const Clock& pClock); // demands + manual + test - suspension → deviceOn, relays
    void UpdateRelays();                               // every channel from its device (unassigned: de-energised)
    void Expire(std::size_t pI, const Clock& pClock);  // puts a device back in order when its time is up
    void EndSuspension(std::size_t pI, const Clock& pClock, uint32_t pWhy);
    void EndMaintenance(const Clock& pClock, uint32_t pWhy); // pWhy: 0 command, 1 time ran out
    void EndManual(std::size_t pI, const Clock& pClock, uint32_t pWhy);
    bool AnySuspended() const;

    config::Config mCfg;
    probe::IProbe& mProbe;
    log::RecordLog mLogA;
    log::AggregateLog mLogB;
    log::RecordLog mLogE;
    log::RecordLog mLogD;

    ladder::State mLadder;
    selftest::State mTest;
    boost::State mBoost;
    selftest::Persistent mTestSaved;

    filter::Median5 mMedDo;
    filter::Median5 mMedSat;
    filter::SlopeEstimator mSlopeDo{600'000};
    filter::SlopeEstimator mFastSlopeDo{120'000};
    log::Aggregator mAggregator;
    std::optional<uint32_t> mAggregateBucket;
    Daily mDaily;
    std::optional<uint32_t> mDailyDay;

    Status mStatus;
    Notifications mNotifications;

    std::array<uint8_t, DEVICES> mAutoWhy{}; // ladder / test / boost / window, as of the last tick
    std::array<std::optional<uint64_t>, DEVICES> mSuspendUntilMs{};
    std::optional<uint64_t> mMaintenanceUntilMs;
    std::array<std::optional<bool>, DEVICES> mManual{};
    std::array<uint32_t, DEVICES> mTuyaFails{};
    bool mAckPending = false;
    std::optional<uint32_t> mSuspendAlertsPending;
    bool mRunTestPending = false;
    bool mStarted = false;
    uint64_t mBootMs = 0;
    uint32_t mNextSeq = 1;
    std::optional<uint32_t> mLastUnix;
    uint64_t mLastUnixAtMs = 0;
    std::optional<uint32_t> mCorrectionLoggedDay;
    uint64_t mLastAlertNotifyMs = 0;
};

} // namespace reefdo::app
