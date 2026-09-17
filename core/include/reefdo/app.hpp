#pragma once
// The orchestrator: one Tick() per sample period runs probe → correction → filters → ladder → service → log.
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
#include "reefdo/service.hpp"

namespace reefdo::app
{

constexpr std::size_t DEVICES = ladder::DEVICES;

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
    ServiceFail,
    ServiceInconclusive,
    Boot
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

// Bits in Measurement records' `flags`
enum MeasurementFlag : uint16_t
{
    FLAG_STUCK = 1,
    FLAG_MAINTENANCE = 2,
    FLAG_SILENCED = 4,
    FLAG_SERVICE_RUNNING = 8,
    FLAG_HEAT = 16,
    FLAG_CLOCK_UNKNOWN = 32,
};

// Service records: aux = event type | device << 8 | outcome << 16 | skip << 24
// Ladder event records: aux = ladder::EventType, flags = from | to << 8, aux device in bits 8..15 for Pulse

struct Status
{
    std::optional<float> doMgl;  // corrected, median-filtered
    std::optional<float> satPct; // median-filtered
    std::optional<float> tempC;
    float slopeMglPer10min = 0.0f;
    probe::Status probe = probe::Status::Timeout;
    uint32_t consecutiveFailures = 0;
    ladder::Level level = ladder::Level::Normal;
    ladder::Level effective = ladder::Level::Normal;
    bool fault = false;
    bool silenced = false;
    bool heat = false;
    bool maintenance = false;
    std::array<bool, DEVICES> deviceOn{};       // semantic "powered"
    std::array<bool, DEVICES> relayEnergised{}; // after NO/NC polarity — what the coils get
    ladder::Buzzer buzzer = ladder::Buzzer::Off;
    ladder::Led led = ladder::Led::Green;
    bool chirp = false; // this tick only
    bool serviceRunning = false;
    service::Phase servicePhase = service::Phase::Idle;
    bool boostRunning = false;
    uint8_t serviceDevice = 0;
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
    void SetMaintenance(bool pOn, const Clock& pClock);
    void RunService();
    bool SetConfig(const config::Config& pCfg, const Clock& pClock);
    // Maintenance-only: force a device on/off for the relay test; the ladder's demands still win. nullopt clears.
    bool SetDeviceOverride(std::size_t pDevice, std::optional<bool> pOn);
    probe::CalResult AirCalibrate(const Clock& pClock); // maintenance only

    const Status& GetStatus() const { return mStatus; }
    const config::Config& GetConfig() const { return mCfg; }
    FixedVector<Notification, 8> TakeNotifications();

    const log::RecordLog& LogA() const { return mLogA; }
    const log::AggregateLog& LogB() const { return mLogB; }
    const log::RecordLog& LogE() const { return mLogE; }
    const log::RecordLog& LogD() const { return mLogD; }

    const service::State& ServiceState() const { return mService; }
    const service::Persistent& ServicePersistent() const { return mService.p; }
    void RestoreService(const service::Persistent& p_) { mService.p = p_; }
    bool ServicePersistentChanged(); // true once after a change; the firmware then saves to NVS

private:
    struct Daily
    {
        uint32_t samples = 0;
        float doMin = 0, doMax = 0, doSum = 0, tempSum = 0;
        uint16_t minuteOfMin = 0;
        uint8_t levelMax = 0;
        uint32_t events = 0;
    };

    std::optional<service::LocalTime> Local(const Clock& pClock) const;
    void ApplyConfig();
    void Write(log::Record& pR, bool pAlsoEvents);
    void LogSimple(const Clock& pClock, log::Type pType, uint32_t pAux, float pF0, float pF1);
    void LogLadderEvents(const Clock& pClock, const ladder::Output& pOut, float pDoNow);
    void LogServiceEvents(const Clock& pClock, const service::Output& pOut);
    void LogBoostEvents(const Clock& pClock, const boost::Output& pOut);
    void Notify(Notification pN);
    void Aggregate(const Clock& pClock, const probe::Reading& pR, float pDoC, ladder::Level pEff);
    void DailyRollover(const Clock& pClock, const std::optional<service::LocalTime>& pLt);
    void TimeSync(const Clock& pClock);
    void AlertRepeat(const Clock& pClock);
    uint32_t UptimeS(const Clock& pClock) const;

    config::Config mCfg;
    probe::IProbe& mProbe;
    log::RecordLog mLogA;
    log::AggregateLog mLogB;
    log::RecordLog mLogE;
    log::RecordLog mLogD;

    ladder::Config mLadderCfg;
    ladder::State mLadder;
    service::Config mServiceCfg;
    service::State mService;
    boost::State mBoost;
    service::Persistent mServiceSaved;

    filter::Median5 mMedDo;
    filter::Median5 mMedSat;
    filter::SlopeEstimator mSlopeDo{600'000};
    log::Aggregator mAggregator;
    std::optional<uint32_t> mAggregateBucket;
    Daily mDaily;
    std::optional<uint32_t> mDailyDay;

    Status mStatus;
    FixedVector<Notification, 8> mNotifications;
    std::array<std::optional<bool>, DEVICES> mOverride{};
    std::array<bool, DEVICES> mAutoOn{}; // the merged demand before any override, for instant override changes
    bool mAckPending = false;
    bool mRunServicePending = false;
    bool mStarted = false;
    uint64_t mBootMs = 0;
    uint32_t mNextSeq = 1;
    std::optional<uint32_t> mLastUnix;
    uint64_t mLastUnixAtMs = 0;
    std::optional<uint32_t> mCorrectionLoggedDay;
    uint64_t mLastAlertNotifyMs = 0;
    ladder::Level mLastNotifiedLevel = ladder::Level::Normal;
};

} // namespace reefdo::app
