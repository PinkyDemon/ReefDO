// Tuya local protocol: framing, handshake and the switch transaction against a scriptable fake plug.
// The crypto here is a toy (invertible XOR, checksum "tags"): it exercises every path of the codec without a
// crypto library. The real ciphers are mbedTLS on the board.
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/tuya.hpp"

using namespace reefdo::tuya;

namespace
{

struct ToyCrypto : ICrypto
{
    bool failEcb = false;
    bool failGcm = false;
    uint8_t rnd = 0;

    static void Xor(std::span<const uint8_t> pKey, std::span<const uint8_t> pIn, std::span<uint8_t> pOut, uint8_t pSalt)
    {
        for(std::size_t i = 0; i < pIn.size(); ++i)
            pOut[i] = static_cast<uint8_t>(pIn[i] ^ pKey[i % pKey.size()] ^ pSalt ^ static_cast<uint8_t>(i / 16));
    }
    static void Tag(std::span<const uint8_t> pKey, std::span<const uint8_t> pA, std::span<const uint8_t> pB,
                    std::span<uint8_t> pOut)
    {
        uint32_t h = 2166136261u;
        auto mix = [&h](std::span<const uint8_t> pD)
        {
            for(const uint8_t b : pD)
                h = (h ^ b) * 16777619u;
        };
        mix(pKey);
        mix(pA);
        mix(pB);
        for(std::size_t i = 0; i < pOut.size(); ++i)
        {
            h = (h ^ static_cast<uint8_t>(i)) * 16777619u;
            pOut[i] = static_cast<uint8_t>(h >> 13);
        }
    }
    bool EcbEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                    std::span<uint8_t> pOut) override
    {
        if(failEcb) return false;
        Xor(pKey, pIn, pOut, 0x5A);
        return true;
    }
    bool EcbDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                    std::span<uint8_t> pOut) override
    {
        return EcbEncrypt(pKey, pIn, pOut);
    }
    bool GcmEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                    std::span<const uint8_t> pAad, std::span<const uint8_t> pIn, std::span<uint8_t> pOut,
                    std::span<uint8_t, 16> pTag) override
    {
        if(failGcm) return false;
        Xor(pKey, pIn, pOut, pIv[0]);
        Tag(pKey, pAad, std::span<const uint8_t>(pOut.data(), pIn.size()), pTag);
        return true;
    }
    bool GcmDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                    std::span<const uint8_t> pAad, std::span<const uint8_t> pIn, std::span<const uint8_t, 16> pTag,
                    std::span<uint8_t> pOut) override
    {
        std::array<uint8_t, 16> t{};
        Tag(pKey, pAad, pIn, t);
        if(std::memcmp(t.data(), pTag.data(), 16) != 0) return false;
        Xor(pKey, pIn, pOut, pIv[0]);
        return true;
    }
    void HmacSha256(std::span<const uint8_t> pKey, std::span<const uint8_t> pData, std::span<uint8_t, 32> pOut) override
    {
        Tag(pKey, pData, {}, pOut);
    }
    void Random(std::span<uint8_t> pOut) override
    {
        for(uint8_t& b : pOut)
            b = ++rnd;
    }
};

std::array<uint8_t, KEY_LEN> Key(const char* pK)
{
    std::array<uint8_t, KEY_LEN> k{};
    std::memcpy(k.data(), pK, KEY_LEN);
    return k;
}

std::span<const uint8_t> Bytes(const std::string& pS)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(pS.data()), pS.size());
}

Settings Plug(Version pV)
{
    Settings s;
    s.ip.assign("192.168.1.50");
    s.id.assign("bf0123456789abcdefgh");
    s.key.assign("0123456789abcdef");
    s.version = pV;
    s.dp = 1;
    return s;
}

// The device side of the protocol, on the same codec.
struct FakePlug : IConnection
{
    Version version;
    ToyCrypto& crypto;
    std::array<uint8_t, KEY_LEN> local = Key("0123456789abcdef");
    std::array<uint8_t, KEY_LEN> key = local;
    std::array<uint8_t, 16> clientNonce{};
    std::array<uint8_t, 16> remote{};
    std::vector<uint8_t> pending;
    std::size_t chunk = SIZE_MAX; // bytes handed out per Receive
    bool on = false;
    int sends = 0;
    // Behaviour switches
    int failSendAt = 0; // this Send (1-based) fails
    bool mute = false;
    bool badHmac = false;
    bool shortResp = false;
    uint32_t retcode = 0;
    bool statusInAck = false;
    bool noStatus = false;
    int chatter = 0;
    std::function<void(std::vector<uint8_t>&)> tamper; // edits the next reply

    FakePlug(Version pV, ToyCrypto& pC)
        : version(pV)
        , crypto(pC)
    {
    }

    void Reply(uint32_t pCmd, std::span<const uint8_t> pPlain, uint32_t pRet = 0)
    {
        Codec c{version, key, crypto};
        std::vector<uint8_t> f(FRAME_MAX);
        f.resize(c.Encode(1, pCmd, pPlain, f, pRet));
        if(tamper)
        {
            tamper(f);
            tamper = nullptr;
        }
        pending.insert(pending.end(), f.begin(), f.end());
    }
    std::string Status() const
    {
        return version == Version::V33
                   ? std::string(R"({"dps":{"1":)") + (on ? "true" : "false") + "}}"
                   : std::string(R"({"protocol":4,"data":{"dps":{"1":)") + (on ? "true" : "false") + "}}}";
    }

    bool Send(std::span<const uint8_t> pData) override
    {
        ++sends;
        if(sends == failSendAt) return false;
        if(mute) return true;
        Codec c{version, key, crypto};
        std::array<uint8_t, FRAME_MAX> scratch{};
        Error e = Error::None;
        const std::optional<Frame> f = c.Decode(pData, false, scratch, e);
        REQUIRE(f.has_value());
        if(f->cmd == CMD_SESS_KEY_NEG_START)
        {
            std::memcpy(clientNonce.data(), f->payload.data(), 16);
            for(std::size_t i = 0; i < 16; ++i)
                remote[i] = static_cast<uint8_t>(0xA0 + i);
            std::array<uint8_t, 48> resp{};
            std::memcpy(resp.data(), remote.data(), 16);
            crypto.HmacSha256(local, clientNonce, std::span<uint8_t, 32>(resp.data() + 16, 32));
            if(badHmac) resp[20] ^= 1;
            Reply(CMD_SESS_KEY_NEG_RESP, std::span<const uint8_t>(resp.data(), shortResp ? 47 : 48));
            return true;
        }
        if(f->cmd == CMD_SESS_KEY_NEG_FINISH)
        {
            std::array<uint8_t, 32> expect{};
            crypto.HmacSha256(local, remote, expect);
            REQUIRE(std::memcmp(expect.data(), f->payload.data(), 32) == 0);
            std::array<uint8_t, 16> x{};
            for(std::size_t i = 0; i < 16; ++i)
                x[i] = static_cast<uint8_t>(clientNonce[i] ^ remote[i]);
            std::array<uint8_t, 16> tag{};
            if(version == Version::V34)
                crypto.EcbEncrypt(local, x, key);
            else
                crypto.GcmEncrypt(local, std::span<const uint8_t, 12>(clientNonce.data(), 12), {}, x, key, tag);
            return true;
        }
        on = FindDp(f->payload, 1).value_or(on);
        for(int i = 0; i < chatter; ++i)
            Reply(CMD_STATUS, Bytes(R"({"dps":{"7":3}})"));
        Reply(f->cmd, statusInAck ? Bytes(Status()) : std::span<const uint8_t>(), retcode);
        if(!noStatus) Reply(CMD_STATUS, Bytes(Status()));
        return true;
    }
    std::size_t Receive(std::span<uint8_t> pOut, uint32_t) override
    {
        const std::size_t n = std::min({pending.size(), pOut.size(), chunk});
        std::memcpy(pOut.data(), pending.data(), n);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
};

} // namespace

TEST_CASE("CRC32 is the IEEE one (zlib, binascii.crc32)", "[tuya]")
{
    REQUIRE(Crc32(Bytes("123456789")) == 0xCBF43926u);
    REQUIRE(Crc32({}) == 0u);
}

TEST_CASE("All three protocol versions switch a plug on and off and read its state back", "[tuya]")
{
    for(const Version v : {Version::V33, Version::V34, Version::V35})
    {
        CAPTURE(static_cast<int>(v));
        ToyCrypto k;
        for(const bool on : {true, false})
        {
            FakePlug plug(v, k);
            plug.on = !on;
            const Result r = Switch(plug, k, Plug(v), on, 1790000000u);
            REQUIRE(r.Ok());
            REQUIRE(r.state == on);
            REQUIRE(plug.on == on);
        }
    }
}

TEST_CASE("The transaction copes with split delivery, pushed chatter, state in the ack, or no state at all", "[tuya]")
{
    ToyCrypto k;
    FakePlug a(Version::V34, k);
    a.chunk = 7;
    a.chatter = 2;
    REQUIRE(Switch(a, k, Plug(Version::V34), true, 1).Ok());

    FakePlug b(Version::V33, k);
    b.statusInAck = true;
    b.noStatus = true;
    const Result rb = Switch(b, k, Plug(Version::V33), true, 1);
    REQUIRE(rb.Ok());
    REQUIRE(rb.state == true);

    FakePlug c(Version::V35, k);
    c.noStatus = true; // acked, but the plug never tells: still a success, state unknown
    const Result rc = Switch(c, k, Plug(Version::V35), true, 1);
    REQUIRE(rc.Ok());
    REQUIRE_FALSE(rc.state.has_value());

    FakePlug d(Version::V33, k);
    d.chatter = 6; // the answer never comes within the frames we are willing to read
    REQUIRE(Switch(d, k, Plug(Version::V33), true, 1).error == Error::Timeout);
}

TEST_CASE("Every failure of the transaction is reported as what it is", "[tuya]")
{
    ToyCrypto k;
    const auto run = [&k](Version pV, const std::function<void(FakePlug&)>& pSetup)
    {
        FakePlug p(pV, k);
        pSetup(p);
        const Result r = Switch(p, k, Plug(pV), true, 1);
        k.failEcb = false;
        k.failGcm = false;
        return r.error;
    };
    REQUIRE(run(Version::V33, [](FakePlug& pP) { pP.mute = true; }) == Error::Timeout);
    REQUIRE(run(Version::V33, [](FakePlug& pP) { pP.failSendAt = 1; }) == Error::Send);
    REQUIRE(run(Version::V34, [](FakePlug& pP) { pP.failSendAt = 1; }) == Error::Send);
    REQUIRE(run(Version::V34, [](FakePlug& pP) { pP.failSendAt = 2; }) == Error::Send);
    REQUIRE(run(Version::V34, [](FakePlug& pP) { pP.mute = true; }) == Error::Timeout);
    REQUIRE(run(Version::V34, [](FakePlug& pP) { pP.badHmac = true; }) == Error::Auth);
    REQUIRE(run(Version::V35, [](FakePlug& pP) { pP.shortResp = true; }) == Error::Auth);
    REQUIRE(run(Version::V33, [](FakePlug& pP) { pP.retcode = 1; }) == Error::Refused);
    REQUIRE(run(Version::V33, [&k](FakePlug&) { k.failEcb = true; }) == Error::Crypto);
    REQUIRE(run(Version::V35, [&k](FakePlug&) { k.failGcm = true; }) == Error::Crypto);
    REQUIRE(run(Version::V33, [](FakePlug& pP) { pP.tamper = [](std::vector<uint8_t>& pF) { pF[20] ^= 0xFF; }; }) ==
            Error::Frame);
    REQUIRE(run(Version::V33, [](FakePlug& pP) { pP.tamper = [](std::vector<uint8_t>& pF) { pF[0] = 0x12; }; }) ==
            Error::Frame);
    REQUIRE(run(Version::V34, [](FakePlug& pP) { pP.tamper = [](std::vector<uint8_t>& pF) { pF[20] ^= 0xFF; }; }) ==
            Error::Auth);

    // The session key itself cannot be made: the client's fourth ECB operation (after the start frame, the plug's
    // answer and the finish frame) is the key derivation.
    struct FailOnSession : ToyCrypto
    {
        int calls = 0;
        bool EcbEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                        std::span<uint8_t> pOut) override
        {
            return ++calls != 4 && ToyCrypto::EcbEncrypt(pKey, pIn, pOut);
        }
    } client;
    FakePlug p(Version::V34, k);
    REQUIRE(Switch(p, client, Plug(Version::V34), true, 1).error == Error::Crypto);
}

TEST_CASE("A crypto provider is released through its interface", "[tuya]")
{
    std::unique_ptr<ICrypto> c = std::make_unique<ToyCrypto>();
    c.reset();
    REQUIRE(c == nullptr);
}

TEST_CASE("Frames: sizes, headers and every rejection", "[tuya]")
{
    ToyCrypto k;
    std::array<uint8_t, FRAME_MAX> out{};
    std::array<uint8_t, FRAME_MAX> scratch{};
    Error e = Error::None;
    const std::string json = R"({"dps":{"1":true}})";
    for(const Version v : {Version::V33, Version::V34, Version::V35})
    {
        CAPTURE(static_cast<int>(v));
        const Codec c{v, Key("0123456789abcdef"), k};
        const std::size_t n = c.Encode(9, CMD_CONTROL, Bytes(json), out, 0);
        REQUIRE(n > 0);
        REQUIRE(Codec::FrameSize(std::span<const uint8_t>(out.data(), 3)) == 0);
        REQUIRE(Codec::FrameSize(std::span<const uint8_t>(out.data(), 12)) == 0);
        REQUIRE(Codec::FrameSize(std::span<const uint8_t>(out.data(), n)) == n);
        const std::optional<Frame> f = c.Decode(std::span<const uint8_t>(out.data(), n), true, scratch, e);
        REQUIRE(f.has_value());
        REQUIRE(f->seq == 9);
        REQUIRE(f->cmd == CMD_CONTROL);
        REQUIRE(f->retcode == 0u);
        REQUIRE(std::string(reinterpret_cast<const char*>(f->payload.data()), f->payload.size()) == json);
        REQUIRE(e == Error::None);

        // too small an output buffer, too large a payload
        REQUIRE(c.Encode(1, CMD_CONTROL, Bytes(json), std::span<uint8_t>(out.data(), 40)) == 0);
        std::vector<uint8_t> big(FRAME_MAX, 'x');
        REQUIRE(c.Encode(1, CMD_CONTROL, big, out) == 0);

        // truncated, wrong suffix, a device frame without its return code
        REQUIRE_FALSE(c.Decode(std::span<const uint8_t>(out.data(), 20), true, scratch, e).has_value());
        REQUIRE(e == Error::Frame);
        const std::size_t m = c.Encode(9, CMD_CONTROL, {}, out);
        std::vector<uint8_t> bad(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(m));
        REQUIRE(c.Decode(bad, false, scratch, e).has_value());
        bad.back() ^= 1; // suffix
        REQUIRE_FALSE(c.Decode(bad, false, scratch, e).has_value());
        bad.back() ^= 1;
        bad[3] ^= 1; // prefix
        REQUIRE_FALSE(c.Decode(bad, false, scratch, e).has_value());
        bad[3] ^= 1;
        bad[v == Version::V35 ? 17 : 15] ^= 1; // the length no longer matches
        REQUIRE_FALSE(c.Decode(bad, false, scratch, e).has_value());
        REQUIRE(e == Error::Frame);
    }
    std::array<uint8_t, 20> junk{0x12, 0x34, 0x56, 0x78};
    REQUIRE(Codec::FrameSize(junk) == SIZE_MAX);
    std::array<uint8_t, 18> huge{0x00, 0x00, 0x55, 0xAA, 0, 0, 0, 1, 0, 0, 0, 7, 0xFF, 0xFF, 0xFF, 0xFF};
    REQUIRE(Codec::FrameSize(huge) == SIZE_MAX);
    std::array<uint8_t, 18> huge66{0x00, 0x00, 0x66, 0x99, 0, 0, 0, 0, 0, 1, 0, 0, 0, 7, 0xFF, 0xFF, 0xFF, 0xF0};
    REQUIRE(Codec::FrameSize(huge66) == SIZE_MAX);
}

TEST_CASE("Decryption failures: bad CRC, bad tag, ciphertext not in blocks, bad padding, crypto error", "[tuya]")
{
    ToyCrypto k;
    std::array<uint8_t, FRAME_MAX> out{};
    std::array<uint8_t, FRAME_MAX> scratch{};
    Error e = Error::None;
    const Codec c33{Version::V33, Key("0123456789abcdef"), k};
    const Codec c35{Version::V35, Key("0123456789abcdef"), k};

    // 3.3: an intact frame whose body is not whole blocks
    const auto frame33 = [&](std::vector<uint8_t> pBody)
    {
        std::vector<uint8_t> f = {0, 0, 0x55, 0xAA, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 0};
        f.insert(f.end(), pBody.begin(), pBody.end());
        const uint32_t len = static_cast<uint32_t>(pBody.size() + 8);
        f[15] = static_cast<uint8_t>(len);
        const uint32_t crc = Crc32(f);
        for(int s = 24; s >= 0; s -= 8)
            f.push_back(static_cast<uint8_t>(crc >> s));
        f.insert(f.end(), {0, 0, 0xAA, 0x55});
        return f;
    };
    REQUIRE_FALSE(c33.Decode(frame33(std::vector<uint8_t>(4 + 5, 0)), true, scratch, e).has_value());
    REQUIRE(e == Error::Frame);
    // whole blocks, but the padding comes out wrong: 0, >16, inconsistent
    for(const uint8_t last : {uint8_t{0}, uint8_t{17}, uint8_t{2}})
    {
        std::array<uint8_t, 16> plain{};
        plain[15] = last;
        std::array<uint8_t, 16> ct{};
        k.EcbEncrypt(Key("0123456789abcdef"), plain, ct);
        std::vector<uint8_t> body(4, 0);
        body.insert(body.end(), ct.begin(), ct.end());
        REQUIRE_FALSE(c33.Decode(frame33(body), true, scratch, e).has_value());
        REQUIRE(e == Error::Auth);
    }
    // the crypto library fails while decrypting
    std::vector<uint8_t> blocks(4 + 16, 0);
    k.failEcb = true;
    REQUIRE_FALSE(c33.Decode(frame33(blocks), true, scratch, e).has_value());
    REQUIRE(e == Error::Crypto);
    k.failEcb = false;
    // a CRC that does not match
    std::vector<uint8_t> f = frame33(blocks);
    f[f.size() - 5] ^= 1;
    REQUIRE_FALSE(c33.Decode(f, true, scratch, e).has_value());
    REQUIRE(e == Error::Frame);
    // 3.3 body with only the return code: an empty payload is fine
    const std::optional<Frame> ack = c33.Decode(frame33(std::vector<uint8_t>(4, 0)), true, scratch, e);
    REQUIRE(ack.has_value());
    REQUIRE(ack->payload.empty());
    REQUIRE_FALSE(c33.Decode(frame33({}), true, scratch, e).has_value()); // no room for the return code

    // 3.5: a wrong tag; a device frame too short for its return code
    std::size_t n = c35.Encode(1, CMD_STATUS, Bytes("{}"), out, 0);
    out[40] ^= 1;
    REQUIRE_FALSE(c35.Decode(std::span<const uint8_t>(out.data(), n), true, scratch, e).has_value());
    REQUIRE(e == Error::Auth);
    n = c35.Encode(1, CMD_SESS_KEY_NEG_START, Bytes("ab"), out);
    REQUIRE_FALSE(c35.Decode(std::span<const uint8_t>(out.data(), n), true, scratch, e).has_value());
    REQUIRE(e == Error::Frame);
    REQUIRE(c35.Decode(std::span<const uint8_t>(out.data(), n), false, scratch, e).has_value());
}

TEST_CASE("Control payloads, dp parsing and address checks", "[tuya]")
{
    std::array<char, 160> buf{};
    Settings s = Plug(Version::V33);
    s.dp = 20;
    std::size_t n = ControlJson(s, true, 1790000000u, buf);
    REQUIRE(std::string(buf.data(), n) ==
            R"({"devId":"bf0123456789abcdefgh","uid":"bf0123456789abcdefgh","t":"1790000000","dps":{"20":true}})");
    s.version = Version::V35;
    n = ControlJson(s, false, 7, buf);
    REQUIRE(std::string(buf.data(), n) == R"({"protocol":5,"t":7,"data":{"dps":{"20":false}}})");
    std::array<char, 10> tiny{};
    REQUIRE(ControlJson(s, false, 7, tiny) == 0);

    REQUIRE(FindDp(Bytes(R"({"dps":{"1": true, "20":false}})"), 1) == true);
    REQUIRE(FindDp(Bytes(R"({"dps":{"1": true, "20":false}})"), 20) == false);
    REQUIRE_FALSE(FindDp(Bytes(R"({"dps":{"20":5}})"), 20).has_value());
    REQUIRE_FALSE(FindDp(Bytes(R"({"dps":{"20":true}})"), 2).has_value());
    REQUIRE_FALSE(FindDp(Bytes(R"({"1":true})"), 1).has_value());
    REQUIRE_FALSE(FindDp(Bytes(R"({"dps":{"1":)"), 1).has_value()); // cut off

    for(const char* ok : {"192.168.1.50", "0.0.0.0", "255.255.255.255", "10.0.0.1"})
        REQUIRE(ValidIpv4(ok));
    for(const char* bad : {"", "192.168.1", "192.168.1.50.1", "192.168.1.256", "192.168..1", "1.2.3.4 ", "a.b.c.d",
                           "1.2.3.0004", "1.2.3.", "1234.1.1.1", "12345.1.1.1", "1.2-3.4"})
    {
        CAPTURE(bad);
        REQUIRE_FALSE(ValidIpv4(bad));
    }
}
