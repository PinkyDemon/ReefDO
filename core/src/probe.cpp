#include "reefdo/probe.hpp"

#include <array>
#include <cmath>
#include <cstring>

#include "reefdo/modbus.hpp"

namespace reefdo::probe
{

namespace
{

uint32_t BitsOf(float pF)
{
    uint32_t b = 0;
    std::memcpy(&b, &pF, sizeof b);
    return b;
}

bool Plausible(const Reading& pR)
{
    if(!std::isfinite(pR.doMgl) || !std::isfinite(pR.satPct) || !std::isfinite(pR.tempC)) return false;
    if(pR.tempC < -5.0f || pR.tempC > 60.0f) return false; // the probe's own operating range
    if(pR.satPct < 0.0f || pR.satPct > 200.0f) return false;
    if(pR.doMgl < 0.0f || pR.doMgl > 50.0f) return false; // Type C full scale
    return true;
}

} // namespace

std::size_t Rk500::ReadFrame(std::span<uint8_t> pBuf)
{
    // Gather until the buffer is full or a read returns nothing; the UART's own inter-byte timeout
    // ends a short frame (e.g. an exception reply) before the buffer fills.
    std::size_t total = 0;
    while(total < pBuf.size())
    {
        const std::size_t n = mUart.Read(pBuf.subspan(total), mCfg.responseTimeoutMs);
        if(n == 0) break;
        total += n;
    }
    return total;
}

PollResult Rk500::Poll()
{
    PollResult res;
    std::array<uint8_t, modbus::REQUEST_SIZE> req{};
    modbus::BuildReadHolding(mCfg.address, REG_DO, READ_COUNT, req);
    mUart.DiscardInput();
    mUart.Write(req);

    std::array<uint8_t, READ_RESPONSE_SIZE> buf{};
    const std::size_t n = ReadFrame(buf);
    if(n == 0)
    {
        res.status = Status::Timeout;
        return res;
    }
    const modbus::Parsed p = modbus::ParseReadResponse(mCfg.address, READ_COUNT, std::span(buf).first(n));
    if(!p.Ok())
    {
        if(*p.error == modbus::Error::Exception)
        {
            res.status = Status::ModbusException;
            res.exceptionCode = p.exceptionCode;
        }
        else
        {
            res.status = Status::FrameError;
        }
        return res;
    }

    Reading r;
    r.doMgl = modbus::DecodeFloatAbcd(p.payload.subspan<0, 4>());
    r.satPct = modbus::DecodeFloatAbcd(p.payload.subspan<4, 4>());
    r.tempC = modbus::DecodeFloatAbcd(p.payload.subspan<8, 4>());
    if(!Plausible(r))
    {
        res.status = Status::Implausible;
        return res;
    }

    // Stuck detection: a live optical probe never returns bit-identical DO *and* temperature for half an hour.
    const uint32_t db = BitsOf(r.doMgl);
    const uint32_t tb = BitsOf(r.tempC);
    if(mHaveLast && db == mLastDoBits && tb == mLastTempBits)
    {
        ++mIdentical;
    }
    else
    {
        mIdentical = 0;
    }
    mHaveLast = true;
    mLastDoBits = db;
    mLastTempBits = tb;

    res.reading = r;
    res.status = mIdentical >= mCfg.stuckSamples ? Status::Stuck : Status::Ok;
    return res;
}

CalResult Rk500::WriteOne(uint16_t pReg)
{
    std::array<uint8_t, modbus::REQUEST_SIZE> req{};
    modbus::BuildWriteSingle(mCfg.address, pReg, 1, req);
    mUart.DiscardInput();
    mUart.Write(req);

    std::array<uint8_t, modbus::REQUEST_SIZE> buf{};
    const std::size_t n = ReadFrame(buf);
    const modbus::Parsed p = modbus::ParseWriteResponse(mCfg.address, pReg, 1, std::span(buf).first(n));
    return p.Ok() ? CalResult::Ok : CalResult::Failed;
}

CalResult Rk500::AirCalibrate()
{
    return WriteOne(REG_AIR_CAL);
}

CalResult Rk500::ZeroCalibrate()
{
    if(!mCfg.allowZeroCal) return CalResult::Refused;
    return WriteOne(REG_ZERO_CAL);
}

} // namespace reefdo::probe
