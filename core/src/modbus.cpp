#include "reefdo/modbus.hpp"

#include <cstring>

namespace reefdo::modbus
{

uint16_t Crc16(std::span<const uint8_t> pData)
{
    uint16_t crc = 0xFFFF;
    for(uint8_t b : pData)
    {
        crc ^= b;
        for(int i = 0; i < 8; ++i)
        {
            const bool lsb = (crc & 1u) != 0;
            crc >>= 1;
            if(lsb) crc ^= 0xA001;
        }
    }
    return crc;
}

namespace
{

std::size_t BuildRequest(uint8_t pAddr, uint8_t pFn, uint16_t pA, uint16_t pB, std::span<uint8_t> pOut)
{
    if(pOut.size() < REQUEST_SIZE) return 0;
    pOut[0] = pAddr;
    pOut[1] = pFn;
    pOut[2] = static_cast<uint8_t>(pA >> 8);
    pOut[3] = static_cast<uint8_t>(pA & 0xFF);
    pOut[4] = static_cast<uint8_t>(pB >> 8);
    pOut[5] = static_cast<uint8_t>(pB & 0xFF);
    const uint16_t crc = Crc16(pOut.first(6));
    pOut[6] = static_cast<uint8_t>(crc & 0xFF); // low byte first
    pOut[7] = static_cast<uint8_t>(crc >> 8);
    return REQUEST_SIZE;
}

// Checks shared by both response types. Returns a Parsed with `error` set on failure;
// on success `error` is empty and the caller continues with function-specific checks.
Parsed CommonChecks(uint8_t pAddr, uint8_t pFn, std::span<const uint8_t> pFrame)
{
    Parsed p;
    if(pFrame.size() < 5)
    {
        p.error = Error::TooShort;
        return p;
    }
    const uint16_t crc = Crc16(pFrame.first(pFrame.size() - 2));
    const uint16_t wire = static_cast<uint16_t>(pFrame[pFrame.size() - 2] | (pFrame[pFrame.size() - 1] << 8));
    if(crc != wire)
    {
        p.error = Error::BadCrc;
        return p;
    }
    if(pFrame[0] != pAddr)
    {
        p.error = Error::WrongAddress;
        return p;
    }
    if(pFrame[1] == static_cast<uint8_t>(pFn | 0x80))
    {
        p.error = Error::Exception;
        p.exceptionCode = pFrame[2];
        return p;
    }
    if(pFrame[1] != pFn)
    {
        p.error = Error::WrongFunction;
        return p;
    }
    return p;
}

} // namespace

std::size_t BuildReadHolding(uint8_t pAddr, uint16_t pReg, uint16_t pCount, std::span<uint8_t> pOut)
{
    return BuildRequest(pAddr, FN_READ_HOLDING, pReg, pCount, pOut);
}

std::size_t BuildWriteSingle(uint8_t pAddr, uint16_t pReg, uint16_t pValue, std::span<uint8_t> pOut)
{
    return BuildRequest(pAddr, FN_WRITE_SINGLE, pReg, pValue, pOut);
}

Parsed ParseReadResponse(uint8_t pAddr, uint16_t pCount, std::span<const uint8_t> pFrame)
{
    Parsed p = CommonChecks(pAddr, FN_READ_HOLDING, pFrame);
    if(!p.Ok()) return p;
    const std::size_t byteCount = pFrame[2];
    if(byteCount != static_cast<std::size_t>(pCount) * 2 || pFrame.size() != byteCount + 5)
    {
        p.error = Error::WrongLength;
        return p;
    }
    p.payload = pFrame.subspan(3, byteCount);
    return p;
}

Parsed ParseWriteResponse(uint8_t pAddr, uint16_t pReg, uint16_t pValue, std::span<const uint8_t> pFrame)
{
    Parsed p = CommonChecks(pAddr, FN_WRITE_SINGLE, pFrame);
    if(!p.Ok()) return p;
    if(pFrame.size() != REQUEST_SIZE)
    {
        p.error = Error::WrongLength;
        return p;
    }
    const uint16_t echoReg = static_cast<uint16_t>((pFrame[2] << 8) | pFrame[3]);
    const uint16_t echoVal = static_cast<uint16_t>((pFrame[4] << 8) | pFrame[5]);
    if(echoReg != pReg || echoVal != pValue)
    {
        p.error = Error::EchoMismatch;
        return p;
    }
    return p;
}

float DecodeFloatAbcd(std::span<const uint8_t, 4> pB)
{
    const uint32_t bits = (static_cast<uint32_t>(pB[0]) << 24) | (static_cast<uint32_t>(pB[1]) << 16) |
                          (static_cast<uint32_t>(pB[2]) << 8) | static_cast<uint32_t>(pB[3]);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

} // namespace reefdo::modbus
