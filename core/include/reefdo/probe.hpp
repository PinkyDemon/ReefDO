#pragma once
// RK500-04 driver: one Modbus read of six registers → DO mg/L, saturation %, temperature °C.
// Transport-agnostic (IUart); the firmware wraps the ESP-IDF UART, the tests use a fake.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace reefdo::probe
{

class IUart
{
public:
    virtual ~IUart() = default;
    virtual void Write(std::span<const uint8_t> pData) = 0;
    // Read up to out.size() bytes, waiting at most timeout_ms for the first byte. Returns bytes read (0 = nothing).
    virtual std::size_t Read(std::span<uint8_t> pOut, uint32_t pTimeoutMs) = 0;
    // Drop anything still sitting in the receive buffer (stale half-frames from a previous poll).
    virtual void DiscardInput() = 0;
};

enum class Status : uint8_t
{
    Ok,
    Stuck,           // valid reading, but bit-identical DO+temperature for `stuck_samples` polls — soft fault
    Timeout,         // nothing came back
    FrameError,      // bytes came back but not a valid response (CRC, address, function, length)
    ModbusException, // the probe answered with an exception frame
    Implausible,     // decoded, but NaN/Inf or outside the physically possible range
};

struct Reading
{
    float doMgl;
    float satPct;
    float tempC;
};

struct PollResult
{
    Status status = Status::Timeout;
    std::optional<Reading> reading; // present for Ok and Stuck only
    uint8_t exceptionCode = 0;      // for ModbusException
};

struct Config
{
    uint8_t address = 0x0A; // factory default
    uint32_t responseTimeoutMs = 100;
    uint32_t stuckSamples = 180; // 30 min at 10 s
    bool allowZeroCal = false;   // zero-O2 calibration is refused unless this is set
};

// Register map (FC 0x03 reads, FC 0x06 writes)
constexpr uint16_t REG_DO = 0x0000;                            // float, mg/L
constexpr uint16_t REG_SATURATION = 0x0002;                    // float, %
constexpr uint16_t REG_TEMPERATURE = 0x0004;                   // float, °C
constexpr uint16_t REG_AIR_CAL = 0x001A;                       // write 1
constexpr uint16_t REG_ZERO_CAL = 0x001C;                      // write 1 — dangerous without true anoxic water
constexpr uint16_t READ_COUNT = 6;                             // three floats
constexpr std::size_t READ_RESPONSE_SIZE = 5 + 2 * READ_COUNT; // 17 bytes

enum class CalResult : uint8_t
{
    Ok,
    Refused,
    Failed
};

// What the app polls: the real RK500 driver, or the simulator's probe model.
class IProbe
{
public:
    virtual ~IProbe() = default;
    virtual PollResult Poll() = 0;
    virtual CalResult AirCalibrate() = 0;
};

class Rk500 : public IProbe
{
public:
    Rk500(IUart& pUart, const Config& pCfg)
        : mUart(pUart)
        , mCfg(pCfg)
    {
    }

    PollResult Poll() override;
    CalResult AirCalibrate() override;
    CalResult ZeroCalibrate();

    uint32_t IdenticalCount() const { return mIdentical; }

private:
    CalResult WriteOne(uint16_t pReg);
    std::size_t ReadFrame(std::span<uint8_t> pBuf);

    IUart& mUart;
    Config mCfg;
    bool mHaveLast = false;
    uint32_t mLastDoBits = 0;
    uint32_t mLastTempBits = 0;
    uint32_t mIdentical = 0;
};

} // namespace reefdo::probe
