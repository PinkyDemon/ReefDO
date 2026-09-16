#include <cstring>
#include <deque>
#include <limits>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/modbus.hpp"
#include "reefdo/probe.hpp"

using namespace reefdo::probe;
using Catch::Matchers::WithinAbs;

namespace
{

// Scripted UART: each queued chunk is handed out by one read() call; an empty queue reads 0 (timeout).
struct FakeUart : IUart
{
    std::deque<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> written;
    int discards = 0;

    void Write(std::span<const uint8_t> pData) override { written.assign(pData.begin(), pData.end()); }
    std::size_t Read(std::span<uint8_t> pOut, uint32_t) override
    {
        if(chunks.empty()) return 0;
        std::vector<uint8_t> c = std::move(chunks.front());
        chunks.pop_front();
        const std::size_t n = c.size() < pOut.size() ? c.size() : pOut.size();
        std::memcpy(pOut.data(), c.data(), n);
        return n;
    }
    void DiscardInput() override { ++discards; }
};

std::vector<uint8_t> Be(float pF)
{
    uint32_t b = 0;
    std::memcpy(&b, &pF, 4);
    return {static_cast<uint8_t>(b >> 24), static_cast<uint8_t>(b >> 16), static_cast<uint8_t>(b >> 8),
            static_cast<uint8_t>(b)};
}

std::vector<uint8_t> Reply(float pDoMgl, float pSat, float pTemp, uint8_t pAddr = 0x0A)
{
    std::vector<uint8_t> f = {pAddr, 0x03, 0x0C};
    for(const auto& v : {Be(pDoMgl), Be(pSat), Be(pTemp)})
        f.insert(f.end(), v.begin(), v.end());
    const uint16_t c = reefdo::modbus::Crc16(f);
    f.push_back(static_cast<uint8_t>(c & 0xFF));
    f.push_back(static_cast<uint8_t>(c >> 8));
    return f;
}

const std::vector<uint8_t> DATASHEET_REPLY = {0x0A, 0x03, 0x0C, 0x40, 0xF2, 0x8D, 0x18, 0x42, 0xC8,
                                              0xC2, 0xC2, 0x41, 0xF1, 0x5C, 0x29, 0xF5, 0xE6};

} // namespace

TEST_CASE("poll sends the datasheet request and decodes the datasheet reply", "[probe]")
{
    FakeUart uart;
    Rk500 probe(uart, Config{});
    uart.chunks.push_back(DATASHEET_REPLY);

    const PollResult r = probe.Poll();
    REQUIRE(uart.discards == 1);
    REQUIRE(uart.written == std::vector<uint8_t>{0x0A, 0x03, 0x00, 0x00, 0x00, 0x06, 0xC4, 0xB3});
    REQUIRE(r.status == Status::Ok);
    REQUIRE(r.reading.has_value());
    REQUIRE_THAT(r.reading->doMgl, WithinAbs(7.5797, 0.0005));
    REQUIRE_THAT(r.reading->satPct, WithinAbs(100.38, 0.01));
    REQUIRE_THAT(r.reading->tempC, WithinAbs(30.17, 0.005));
}

TEST_CASE("poll reassembles a reply that arrives in pieces", "[probe]")
{
    FakeUart uart;
    Rk500 probe(uart, Config{});
    uart.chunks.push_back({DATASHEET_REPLY.begin(), DATASHEET_REPLY.begin() + 5});
    uart.chunks.push_back({DATASHEET_REPLY.begin() + 5, DATASHEET_REPLY.begin() + 11});
    uart.chunks.push_back({DATASHEET_REPLY.begin() + 11, DATASHEET_REPLY.end()});
    REQUIRE(probe.Poll().status == Status::Ok);
}

TEST_CASE("poll: timeout, frame errors, exception", "[probe]")
{
    FakeUart uart;
    Rk500 probe(uart, Config{});

    SECTION("nothing back")
    {
        const PollResult r = probe.Poll();
        REQUIRE(r.status == Status::Timeout);
        REQUIRE_FALSE(r.reading.has_value());
    }
    SECTION("garbled (bad CRC)")
    {
        std::vector<uint8_t> f = DATASHEET_REPLY;
        f[7] ^= 0xFF;
        uart.chunks.push_back(f);
        REQUIRE(probe.Poll().status == Status::FrameError);
    }
    SECTION("short frame from another address")
    {
        uart.chunks.push_back(Reply(7.0f, 100.0f, 25.0f, 0x0B));
        REQUIRE(probe.Poll().status == Status::FrameError);
    }
    SECTION("exception frame")
    {
        std::vector<uint8_t> f = {0x0A, 0x83, 0x02};
        const uint16_t c = reefdo::modbus::Crc16(f);
        f.push_back(static_cast<uint8_t>(c & 0xFF));
        f.push_back(static_cast<uint8_t>(c >> 8));
        uart.chunks.push_back(f);
        const PollResult r = probe.Poll();
        REQUIRE(r.status == Status::ModbusException);
        REQUIRE(r.exceptionCode == 0x02);
    }
}

TEST_CASE("poll rejects implausible values on every axis", "[probe]")
{
    FakeUart uart;
    Rk500 probe(uart, Config{});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const std::vector<std::vector<uint8_t>> bad = {
        Reply(nan, 100.0f, 25.0f),  Reply(7.0f, nan, 25.0f),     Reply(7.0f, 100.0f, inf),
        Reply(7.0f, 100.0f, -6.0f), Reply(7.0f, 100.0f, 61.0f),  Reply(7.0f, -0.1f, 25.0f),
        Reply(7.0f, 200.1f, 25.0f), Reply(-0.1f, 100.0f, 25.0f), Reply(50.1f, 100.0f, 25.0f),
    };
    for(const auto& f : bad)
    {
        uart.chunks.push_back(f);
        const PollResult r = probe.Poll();
        REQUIRE(r.status == Status::Implausible);
        REQUIRE_FALSE(r.reading.has_value());
    }
    // Boundaries are inclusive.
    uart.chunks.push_back(Reply(0.0f, 0.0f, -5.0f));
    REQUIRE(probe.Poll().status == Status::Ok);
    uart.chunks.push_back(Reply(50.0f, 200.0f, 60.0f));
    REQUIRE(probe.Poll().status == Status::Ok);
}

TEST_CASE("poll flags a bit-identical DO+temperature stream as Stuck, still returning the reading", "[probe]")
{
    FakeUart uart;
    Config cfg;
    cfg.stuckSamples = 3;
    Rk500 probe(uart, cfg);

    uart.chunks.push_back(Reply(6.5f, 98.0f, 26.0f));
    REQUIRE(probe.Poll().status == Status::Ok); // first: nothing to compare with
    REQUIRE(probe.IdenticalCount() == 0);
    uart.chunks.push_back(Reply(6.5f, 97.0f, 26.0f)); // saturation may differ; DO+temp identical
    REQUIRE(probe.Poll().status == Status::Ok);
    REQUIRE(probe.IdenticalCount() == 1);
    uart.chunks.push_back(Reply(6.5f, 98.0f, 26.0f));
    REQUIRE(probe.Poll().status == Status::Ok);
    REQUIRE(probe.IdenticalCount() == 2);
    uart.chunks.push_back(Reply(6.5f, 98.0f, 26.0f));
    const PollResult r = probe.Poll();
    REQUIRE(r.status == Status::Stuck);
    REQUIRE(r.reading.has_value());
    REQUIRE(probe.IdenticalCount() == 3);

    uart.chunks.push_back(Reply(6.5f, 98.0f, 26.01f)); // temperature moved: not stuck
    REQUIRE(probe.Poll().status == Status::Ok);
    REQUIRE(probe.IdenticalCount() == 0);
    uart.chunks.push_back(Reply(6.51f, 98.0f, 26.01f)); // DO moved
    REQUIRE(probe.Poll().status == Status::Ok);
    REQUIRE(probe.IdenticalCount() == 0);
}

TEST_CASE("air calibration writes 0x001A=1 and checks the echo", "[probe]")
{
    FakeUart uart;
    Rk500 probe(uart, Config{});
    const std::vector<uint8_t> echo = {0x0A, 0x06, 0x00, 0x1A, 0x00, 0x01, 0x68, 0xB6};

    uart.chunks.push_back(echo);
    REQUIRE(probe.AirCalibrate() == CalResult::Ok);
    REQUIRE(uart.written == echo);

    REQUIRE(probe.AirCalibrate() == CalResult::Failed); // no echo → failed
}

TEST_CASE("zero calibration is refused unless explicitly allowed", "[probe]")
{
    FakeUart uart;
    Rk500 locked(uart, Config{});
    REQUIRE(locked.ZeroCalibrate() == CalResult::Refused);
    REQUIRE(uart.written.empty()); // nothing was sent to the probe

    Config cfg;
    cfg.allowZeroCal = true;
    Rk500 open(uart, cfg);
    uart.chunks.push_back({0x0A, 0x06, 0x00, 0x1C, 0x00, 0x01, 0x88, 0xB7});
    REQUIRE(open.ZeroCalibrate() == CalResult::Ok);
    REQUIRE(uart.written == std::vector<uint8_t>{0x0A, 0x06, 0x00, 0x1C, 0x00, 0x01, 0x88, 0xB7});
}
