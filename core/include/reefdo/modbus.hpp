#pragma once
// Modbus RTU codec — exactly the subset the RK500-04 speaks (FC 0x03 read holding, FC 0x06 write single).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace reefdo::modbus
{

constexpr uint8_t FN_READ_HOLDING = 0x03;
constexpr uint8_t FN_WRITE_SINGLE = 0x06;
constexpr std::size_t REQUEST_SIZE = 8; // both request types are 8 bytes on the wire

// CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF). On the wire the LOW byte goes first.
uint16_t Crc16(std::span<const uint8_t> pData);

// Build request frames. Return the number of bytes written (8), or 0 if `out` is too small.
std::size_t BuildReadHolding(uint8_t pAddr, uint16_t pReg, uint16_t pCount, std::span<uint8_t> pOut);
std::size_t BuildWriteSingle(uint8_t pAddr, uint16_t pReg, uint16_t pValue, std::span<uint8_t> pOut);

enum class Error : uint8_t
{
    TooShort, // fewer than 5 bytes — can't even be an exception frame
    BadCrc,
    WrongAddress,
    Exception, // slave answered fn|0x80; see `exception_code`
    WrongFunction,
    WrongLength,  // byte count / frame length disagree with what was asked
    EchoMismatch, // FC 0x06 echo carries a different register or value
};

struct Parsed
{
    std::optional<Error> error;
    uint8_t exceptionCode = 0;
    std::span<const uint8_t> payload; // register bytes of a read response (2 * count)
    bool Ok() const { return !error.has_value(); }
};

// Validate a response to build_read_holding(addr, ..., count). `frame` is the raw bytes received.
Parsed ParseReadResponse(uint8_t pAddr, uint16_t pCount, std::span<const uint8_t> pFrame);
// Validate the echo of build_write_single(addr, reg, value).
Parsed ParseWriteResponse(uint8_t pAddr, uint16_t pReg, uint16_t pValue, std::span<const uint8_t> pFrame);

// Big-endian IEEE-754 single, high word first ("ABCD") — the RK500-04's float layout.
float DecodeFloatAbcd(std::span<const uint8_t, 4> pBytes);

} // namespace reefdo::modbus
