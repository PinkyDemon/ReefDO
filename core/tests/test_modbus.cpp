#include <array>
#include <cmath>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/modbus.hpp"

using namespace reefdo::modbus;
using Catch::Matchers::WithinAbs;

namespace
{
// Frames straight from the Rika documents (user manual §7, protocol doc §2–5).
const std::vector<uint8_t> READ_REQ = {0x0A, 0x03, 0x00, 0x00, 0x00, 0x06, 0xC4, 0xB3};
const std::vector<uint8_t> READ_RESP = {0x0A, 0x03, 0x0C, 0x40, 0xF2, 0x8D, 0x18, 0x42, 0xC8,
                                        0xC2, 0xC2, 0x41, 0xF1, 0x5C, 0x29, 0xF5, 0xE6};
const std::vector<uint8_t> SET_ADDR1 = {0x0A, 0x06, 0x00, 0x14, 0x00, 0x01, 0x09, 0x75};
const std::vector<uint8_t> SET_ADDR2 = {0x0A, 0x06, 0x00, 0x14, 0x00, 0x02, 0x49, 0x74};
const std::vector<uint8_t> AIR_CAL = {0x0A, 0x06, 0x00, 0x1A, 0x00, 0x01, 0x68, 0xB6};
const std::vector<uint8_t> ZERO_CAL = {0x0A, 0x06, 0x00, 0x1C, 0x00, 0x01, 0x88, 0xB7};

std::vector<uint8_t> WithCrc(std::vector<uint8_t> pBody)
{
    const uint16_t c = Crc16(pBody);
    pBody.push_back(static_cast<uint8_t>(c & 0xFF));
    pBody.push_back(static_cast<uint8_t>(c >> 8));
    return pBody;
}
} // namespace

TEST_CASE("CRC-16/MODBUS matches the datasheet frames, low byte first on the wire", "[modbus]")
{
    REQUIRE(Crc16(std::span(READ_REQ).first(6)) == 0xB3C4);
    REQUIRE(Crc16(std::span(READ_RESP).first(15)) == 0xE6F5);
    REQUIRE(Crc16(std::span(SET_ADDR1).first(6)) == 0x7509);
    REQUIRE(Crc16(std::span(SET_ADDR2).first(6)) == 0x7449);
    REQUIRE(Crc16(std::span(AIR_CAL).first(6)) == 0xB668);
    REQUIRE(Crc16(std::span(ZERO_CAL).first(6)) == 0xB788);
    REQUIRE(Crc16(std::span<const uint8_t>{}) == 0xFFFF);
}

TEST_CASE("Request builders reproduce the datasheet frames byte for byte", "[modbus]")
{
    std::array<uint8_t, 8> out{};
    REQUIRE(BuildReadHolding(0x0A, 0x0000, 6, out) == 8);
    REQUIRE(std::vector<uint8_t>(out.begin(), out.end()) == READ_REQ);

    REQUIRE(BuildWriteSingle(0x0A, 0x0014, 0x0001, out) == 8);
    REQUIRE(std::vector<uint8_t>(out.begin(), out.end()) == SET_ADDR1);
    REQUIRE(BuildWriteSingle(0x0A, 0x0014, 0x0002, out) == 8);
    REQUIRE(std::vector<uint8_t>(out.begin(), out.end()) == SET_ADDR2);
    REQUIRE(BuildWriteSingle(0x0A, 0x001A, 1, out) == 8);
    REQUIRE(std::vector<uint8_t>(out.begin(), out.end()) == AIR_CAL);
    REQUIRE(BuildWriteSingle(0x0A, 0x001C, 1, out) == 8);
    REQUIRE(std::vector<uint8_t>(out.begin(), out.end()) == ZERO_CAL);

    std::array<uint8_t, 7> small{};
    REQUIRE(BuildReadHolding(0x0A, 0, 6, small) == 0);
    REQUIRE(BuildWriteSingle(0x0A, 0, 1, small) == 0);
}

TEST_CASE(
    "Read response: the datasheet reply decodes to 7.58 mg/L, 100 %, 30.17 C (the manual rounds 0x40F28D18 to 7.57)",
    "[modbus]")
{
    const Parsed p = ParseReadResponse(0x0A, 6, READ_RESP);
    REQUIRE(p.Ok());
    REQUIRE(p.payload.size() == 12);
    REQUIRE_THAT(DecodeFloatAbcd(p.payload.subspan<0, 4>()), WithinAbs(7.5797, 0.0005));
    REQUIRE_THAT(DecodeFloatAbcd(p.payload.subspan<4, 4>()), WithinAbs(100.38, 0.01)); // 0x42C8C2C2
    REQUIRE_THAT(DecodeFloatAbcd(p.payload.subspan<8, 4>()), WithinAbs(30.17, 0.005));
}

TEST_CASE("Read response: every rejection path", "[modbus]")
{
    SECTION("too short")
    {
        const std::vector<uint8_t> f = {0x0A, 0x03, 0x0C, 0x40};
        REQUIRE(ParseReadResponse(0x0A, 6, f).error == Error::TooShort);
        REQUIRE(ParseReadResponse(0x0A, 6, std::span<const uint8_t>{}).error == Error::TooShort);
    }
    SECTION("bad crc")
    {
        std::vector<uint8_t> f = READ_RESP;
        f[5] ^= 0x01;
        REQUIRE(ParseReadResponse(0x0A, 6, f).error == Error::BadCrc);
    }
    SECTION("wrong address")
    {
        REQUIRE(ParseReadResponse(0x0B, 6, READ_RESP).error == Error::WrongAddress);
    }
    SECTION("exception frame carries its code")
    {
        const std::vector<uint8_t> f = WithCrc({0x0A, 0x83, 0x02});
        const Parsed p = ParseReadResponse(0x0A, 6, f);
        REQUIRE(p.error == Error::Exception);
        REQUIRE(p.exceptionCode == 0x02);
    }
    SECTION("wrong function")
    {
        const std::vector<uint8_t> f = WithCrc({0x0A, 0x04, 0x02, 0x00, 0x00});
        REQUIRE(ParseReadResponse(0x0A, 6, f).error == Error::WrongFunction);
    }
    SECTION("byte count disagrees with the request")
    {
        REQUIRE(ParseReadResponse(0x0A, 5, READ_RESP).error == Error::WrongLength);
    }
    SECTION("byte count disagrees with the frame length")
    {
        std::vector<uint8_t> body(READ_RESP.begin(), READ_RESP.end() - 2);
        body.push_back(0x00); // one extra byte, re-CRC'd
        REQUIRE(ParseReadResponse(0x0A, 6, WithCrc(body)).error == Error::WrongLength);
    }
}

TEST_CASE("Write response: echo validation", "[modbus]")
{
    REQUIRE(ParseWriteResponse(0x0A, 0x001A, 1, AIR_CAL).Ok());
    REQUIRE(ParseWriteResponse(0x0A, 0x0014, 2, SET_ADDR2).Ok());

    SECTION("wrong length")
    {
        const std::vector<uint8_t> f = WithCrc({0x0A, 0x06, 0x00, 0x1A, 0x00, 0x01, 0x00});
        REQUIRE(ParseWriteResponse(0x0A, 0x001A, 1, f).error == Error::WrongLength);
    }
    SECTION("register mismatch")
    {
        REQUIRE(ParseWriteResponse(0x0A, 0x001C, 1, AIR_CAL).error == Error::EchoMismatch);
    }
    SECTION("value mismatch")
    {
        REQUIRE(ParseWriteResponse(0x0A, 0x0014, 1, SET_ADDR2).error == Error::EchoMismatch);
    }
    SECTION("exception on write")
    {
        const std::vector<uint8_t> f = WithCrc({0x0A, 0x86, 0x03});
        const Parsed p = ParseWriteResponse(0x0A, 0x001A, 1, f);
        REQUIRE(p.error == Error::Exception);
        REQUIRE(p.exceptionCode == 0x03);
    }
    SECTION("common checks apply to writes too")
    {
        REQUIRE(ParseWriteResponse(0x0A, 0x001A, 1, std::span(AIR_CAL).first(4)).error == Error::TooShort);
        REQUIRE(ParseWriteResponse(0x0B, 0x001A, 1, AIR_CAL).error == Error::WrongAddress);
    }
}

TEST_CASE("Float decoding is big-endian, high word first", "[modbus]")
{
    const std::array<uint8_t, 4> one = {0x3F, 0x80, 0x00, 0x00};
    REQUIRE(DecodeFloatAbcd(one) == 1.0f);
    const std::array<uint8_t, 4> neg = {0xC0, 0x00, 0x00, 0x00};
    REQUIRE(DecodeFloatAbcd(neg) == -2.0f);
    const std::array<uint8_t, 4> nan = {0x7F, 0xC0, 0x00, 0x00};
    REQUIRE(std::isnan(DecodeFloatAbcd(nan)));
}
