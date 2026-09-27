#include "reefdo/tuya.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace reefdo::tuya
{

namespace
{

constexpr uint32_t PREFIX_55AA = 0x000055AAu;
constexpr uint32_t SUFFIX_55AA = 0x0000AA55u;
constexpr uint32_t PREFIX_6699 = 0x00006699u;
constexpr uint32_t SUFFIX_6699 = 0x00009966u;
constexpr std::size_t HEADER_55AA = 16;    // prefix, seq, cmd, length
constexpr std::size_t HEADER_6699 = 18;    // prefix, 2 reserved bytes, seq, cmd, length
constexpr std::size_t VERSION_HEADER = 15; // "3.x" + 12 zero bytes, in front of most payloads
constexpr std::size_t IV_LEN = 12;
constexpr std::size_t TAG_LEN = 16;
constexpr std::size_t HMAC_LEN = 32;
constexpr uint32_t NO_HEADER_CMDS[] = {3, 4, 5, 9, 10, 16, 18, 64}; // tinytuya's NO_PROTOCOL_HEADER_CMDS
constexpr std::size_t PLAIN_MAX = FRAME_MAX - 96;                   // leaves room for framing and padding

void Put32(uint8_t* pP, uint32_t pV)
{
    pP[0] = static_cast<uint8_t>(pV >> 24);
    pP[1] = static_cast<uint8_t>(pV >> 16);
    pP[2] = static_cast<uint8_t>(pV >> 8);
    pP[3] = static_cast<uint8_t>(pV);
}

uint32_t Get32(const uint8_t* pP)
{
    return static_cast<uint32_t>(pP[0]) << 24 | static_cast<uint32_t>(pP[1]) << 16 | static_cast<uint32_t>(pP[2]) << 8 |
           pP[3];
}

bool NeedsHeader(uint32_t pCmd)
{
    for(const uint32_t c : NO_HEADER_CMDS)
    {
        if(c == pCmd) return false;
    }
    return true;
}

void VersionHeader(Version pV, uint8_t* pOut)
{
    std::memset(pOut, 0, VERSION_HEADER);
    const char* const names[] = {"3.3", "3.4", "3.5"};
    std::memcpy(pOut, names[static_cast<uint8_t>(pV)], 3);
}

// Drops a version header at the front of a decrypted (or, in 3.3, still encrypted) payload.
std::span<const uint8_t> StripHeader(Version pV, std::span<const uint8_t> pP)
{
    uint8_t h[VERSION_HEADER];
    VersionHeader(pV, h);
    if(pP.size() >= VERSION_HEADER && std::memcmp(pP.data(), h, 3) == 0) return pP.subspan(VERSION_HEADER);
    return pP;
}

bool SameBytes(std::span<const uint8_t> pA, std::span<const uint8_t> pB)
{
    uint8_t diff = 0; // constant time: the length is public, the contents are not
    for(std::size_t i = 0; i < pA.size(); ++i)
        diff = static_cast<uint8_t>(diff | (pA[i] ^ pB[i]));
    return diff == 0;
}

// PKCS#7 pad and ECB-encrypt pIn into pOut; returns the ciphertext size, 0 on crypto failure.
std::size_t EcbPadded(ICrypto& pC, std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn, uint8_t* pOut)
{
    uint8_t padded[FRAME_MAX];
    const std::size_t pad = 16 - pIn.size() % 16;
    std::memcpy(padded, pIn.data(), pIn.size());
    std::memset(padded + pIn.size(), static_cast<int>(pad), pad);
    const std::size_t n = pIn.size() + pad;
    if(!pC.EcbEncrypt(pKey, std::span<const uint8_t>(padded, n), std::span<uint8_t>(pOut, n))) return 0;
    return n;
}

// ECB-decrypt and unpad pIn into pOut; nullopt with pError set on failure.
std::optional<std::span<const uint8_t>> EcbUnpadded(ICrypto& pC, std::span<const uint8_t, KEY_LEN> pKey,
                                                    std::span<const uint8_t> pIn, std::span<uint8_t> pOut,
                                                    Error& pError)
{
    if(pIn.empty()) return std::span<const uint8_t>();
    if(pIn.size() % 16 != 0)
    {
        pError = Error::Frame;
        return std::nullopt;
    }
    if(!pC.EcbDecrypt(pKey, pIn, pOut.first(pIn.size())))
    {
        pError = Error::Crypto;
        return std::nullopt;
    }
    const uint8_t pad = pOut[pIn.size() - 1];
    bool ok = pad >= 1 && pad <= 16;
    for(std::size_t i = 0; ok && i < pad; ++i)
        ok = pOut[pIn.size() - 1 - i] == pad;
    if(!ok)
    {
        pError = Error::Auth; // garbage after decryption: the wrong key
        return std::nullopt;
    }
    return std::span<const uint8_t>(pOut.data(), pIn.size() - pad);
}

// Accumulates bytes from the stream and hands out one complete frame at a time.
class Reader
{
public:
    std::optional<std::span<const uint8_t>> Next(IConnection& pConn, uint32_t pTimeoutMs, Error& pError)
    {
        if(mUsed > 0)
        {
            std::memmove(mBuf, mBuf + mUsed, mFill - mUsed);
            mFill -= mUsed;
            mUsed = 0;
        }
        for(;;)
        {
            const std::size_t size = Codec::FrameSize(std::span<const uint8_t>(mBuf, mFill));
            if(size == SIZE_MAX)
            {
                pError = Error::Frame;
                return std::nullopt;
            }
            if(size > 0 && mFill >= size)
            {
                mUsed = size;
                return std::span<const uint8_t>(mBuf, size);
            }
            const std::size_t got = pConn.Receive(std::span<uint8_t>(mBuf + mFill, FRAME_MAX - mFill), pTimeoutMs);
            if(got == 0)
            {
                pError = Error::Timeout;
                return std::nullopt;
            }
            mFill += got;
        }
    }

private:
    uint8_t mBuf[FRAME_MAX]{};
    std::size_t mFill = 0;
    std::size_t mUsed = 0;
};

// Reads frames until one with command pCmd arrives (others — pushed status updates — are skimmed for the dp).
std::optional<Frame> Await(Reader& pR, IConnection& pConn, const Codec& pC, uint32_t pCmd, uint32_t pTimeoutMs,
                           std::span<uint8_t> pScratch, const Settings& pS, Result& pRes)
{
    for(int i = 0; i < 6; ++i)
    {
        const std::optional<std::span<const uint8_t>> raw = pR.Next(pConn, pTimeoutMs, pRes.error);
        if(!raw.has_value()) return std::nullopt;
        const std::optional<Frame> f = pC.Decode(*raw, true, pScratch, pRes.error);
        if(!f.has_value()) return std::nullopt;
        const std::optional<bool> dp = FindDp(f->payload, pS.dp);
        if(dp.has_value()) pRes.state = dp;
        if(f->cmd == pCmd) return f;
    }
    pRes.error = Error::Timeout; // chatter, but never the answer
    return std::nullopt;
}

bool Send(IConnection& pConn, const Codec& pC, uint32_t pSeq, uint32_t pCmd, std::span<const uint8_t> pPlain,
          Result& pRes)
{
    uint8_t out[FRAME_MAX];
    const std::size_t n = pC.Encode(pSeq, pCmd, pPlain, out);
    if(n == 0)
    {
        pRes.error = Error::Crypto;
        return false;
    }
    if(!pConn.Send(std::span<const uint8_t>(out, n)))
    {
        pRes.error = Error::Send;
        return false;
    }
    return true;
}

// 3.4 / 3.5: agree a session key; on success pC.key holds it.
bool Negotiate(IConnection& pConn, Codec& pC, Reader& pR, uint32_t& pSeq, const Settings& pS, uint32_t pTimeoutMs,
               Result& pRes)
{
    uint8_t scratch[FRAME_MAX];
    std::array<uint8_t, 16> local{};
    pC.crypto.Random(local);
    if(!Send(pConn, pC, pSeq++, CMD_SESS_KEY_NEG_START, local, pRes)) return false;
    const std::optional<Frame> f = Await(pR, pConn, pC, CMD_SESS_KEY_NEG_RESP, pTimeoutMs, scratch, pS, pRes);
    if(!f.has_value()) return false;
    std::array<uint8_t, HMAC_LEN> expect{};
    pC.crypto.HmacSha256(pC.key, local, expect);
    if(f->payload.size() != 16 + HMAC_LEN || !SameBytes(f->payload.subspan(16), expect))
    {
        pRes.error = Error::Auth;
        return false;
    }
    std::array<uint8_t, 16> remote{};
    std::memcpy(remote.data(), f->payload.data(), 16);
    std::array<uint8_t, HMAC_LEN> finish{};
    pC.crypto.HmacSha256(pC.key, remote, finish);
    if(!Send(pConn, pC, pSeq++, CMD_SESS_KEY_NEG_FINISH, finish, pRes)) return false;

    std::array<uint8_t, KEY_LEN> mixed{};
    for(std::size_t i = 0; i < KEY_LEN; ++i)
        mixed[i] = static_cast<uint8_t>(local[i] ^ remote[i]);
    std::array<uint8_t, KEY_LEN> session{};
    bool ok = false;
    if(pC.version == Version::V34)
    {
        ok = pC.crypto.EcbEncrypt(pC.key, mixed, session);
    }
    else
    {
        std::array<uint8_t, TAG_LEN> tag{};
        ok = pC.crypto.GcmEncrypt(pC.key, std::span<const uint8_t, IV_LEN>(local.data(), IV_LEN), {}, mixed, session,
                                  tag);
    }
    if(!ok)
    {
        pRes.error = Error::Crypto;
        return false;
    }
    pC.key = session;
    return true;
}

} // namespace

uint32_t Crc32(std::span<const uint8_t> pData)
{
    uint32_t crc = 0xFFFFFFFFu;
    for(const uint8_t b : pData)
    {
        crc ^= b;
        for(int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::size_t Codec::Encode(uint32_t pSeq, uint32_t pCmd, std::span<const uint8_t> pPlain, std::span<uint8_t> pOut,
                          std::optional<uint32_t> pRetcode) const
{
    if(pPlain.size() > PLAIN_MAX) return 0;
    // What goes under the cipher (3.4 / 3.5), or after it (3.3's header).
    uint8_t inner[FRAME_MAX];
    std::size_t m = 0;
    const bool header = NeedsHeader(pCmd);
    if(version == Version::V35 && pRetcode.has_value())
    {
        Put32(inner, *pRetcode);
        m = 4;
    }
    if(header && version != Version::V33)
    {
        VersionHeader(version, inner + m);
        m += VERSION_HEADER;
    }
    std::memcpy(inner + m, pPlain.data(), pPlain.size());
    m += pPlain.size();

    if(version == Version::V35)
    {
        const std::size_t length = IV_LEN + m + TAG_LEN;
        const std::size_t total = HEADER_6699 + length + 4;
        if(total > pOut.size()) return 0;
        uint8_t* p = pOut.data();
        Put32(p, PREFIX_6699);
        p[4] = 0;
        p[5] = 0;
        Put32(p + 6, pSeq);
        Put32(p + 10, pCmd);
        Put32(p + 14, static_cast<uint32_t>(length));
        uint8_t* iv = p + HEADER_6699;
        crypto.Random(std::span<uint8_t>(iv, IV_LEN));
        if(!crypto.GcmEncrypt(key, std::span<const uint8_t, IV_LEN>(iv, IV_LEN),
                              std::span<const uint8_t>(p + 4, HEADER_6699 - 4), std::span<const uint8_t>(inner, m),
                              std::span<uint8_t>(iv + IV_LEN, m),
                              std::span<uint8_t, TAG_LEN>(iv + IV_LEN + m, TAG_LEN)))
            return 0;
        Put32(iv + IV_LEN + m + TAG_LEN, SUFFIX_6699);
        return total;
    }

    // 55AA: [retcode] [3.3 header] ciphertext, then CRC32 (3.3) or HMAC-SHA256 (3.4)
    uint8_t body[FRAME_MAX];
    std::size_t n = 0;
    if(pRetcode.has_value())
    {
        Put32(body, *pRetcode);
        n = 4;
    }
    if(header && version == Version::V33)
    {
        VersionHeader(version, body + n);
        n += VERSION_HEADER;
    }
    const std::size_t c = EcbPadded(crypto, key, std::span<const uint8_t>(inner, m), body + n);
    if(c == 0) return 0;
    n += c;
    const bool hmac = version == Version::V34;
    const std::size_t end = (hmac ? HMAC_LEN : 4) + 4;
    const std::size_t total = HEADER_55AA + n + end;
    if(total > pOut.size()) return 0;
    uint8_t* p = pOut.data();
    Put32(p, PREFIX_55AA);
    Put32(p + 4, pSeq);
    Put32(p + 8, pCmd);
    Put32(p + 12, static_cast<uint32_t>(n + end));
    std::memcpy(p + HEADER_55AA, body, n);
    const std::span<const uint8_t> covered(p, HEADER_55AA + n);
    if(hmac)
    {
        crypto.HmacSha256(key, covered, std::span<uint8_t, HMAC_LEN>(p + HEADER_55AA + n, HMAC_LEN));
    }
    else
    {
        Put32(p + HEADER_55AA + n, Crc32(covered));
    }
    Put32(p + total - 4, SUFFIX_55AA);
    return total;
}

std::size_t Codec::FrameSize(std::span<const uint8_t> pData)
{
    if(pData.size() < 4) return 0;
    const uint32_t prefix = Get32(pData.data());
    uint64_t size = UINT64_MAX; // 64 bits: a hostile length must not wrap on a 32-bit target
    if(prefix == PREFIX_55AA)
    {
        if(pData.size() < HEADER_55AA) return 0;
        size = HEADER_55AA + static_cast<uint64_t>(Get32(pData.data() + 12));
    }
    else if(prefix == PREFIX_6699)
    {
        if(pData.size() < HEADER_6699) return 0;
        size = HEADER_6699 + static_cast<uint64_t>(Get32(pData.data() + 14)) + 4;
    }
    return size > FRAME_MAX ? SIZE_MAX : static_cast<std::size_t>(size);
}

std::optional<Frame> Codec::Decode(std::span<const uint8_t> pFrame, bool pFromDevice, std::span<uint8_t> pScratch,
                                   Error& pError) const
{
    pError = Error::Frame;
    Frame f;
    std::span<const uint8_t> plain;
    if(version == Version::V35)
    {
        if(pFrame.size() < HEADER_6699 + IV_LEN + TAG_LEN + 4 || Get32(pFrame.data()) != PREFIX_6699 ||
           Get32(pFrame.data() + pFrame.size() - 4) != SUFFIX_6699 || FrameSize(pFrame) != pFrame.size())
            return std::nullopt;
        f.seq = Get32(pFrame.data() + 6);
        f.cmd = Get32(pFrame.data() + 10);
        const std::size_t m = pFrame.size() - HEADER_6699 - IV_LEN - TAG_LEN - 4;
        const uint8_t* iv = pFrame.data() + HEADER_6699;
        if(!crypto.GcmDecrypt(key, std::span<const uint8_t, IV_LEN>(iv, IV_LEN), pFrame.subspan(4, HEADER_6699 - 4),
                              std::span<const uint8_t>(iv + IV_LEN, m),
                              std::span<const uint8_t, TAG_LEN>(iv + IV_LEN + m, TAG_LEN), pScratch.first(m)))
        {
            pError = Error::Auth;
            return std::nullopt;
        }
        plain = std::span<const uint8_t>(pScratch.data(), m);
        if(pFromDevice)
        {
            if(plain.size() < 4) return std::nullopt;
            f.retcode = Get32(plain.data());
            plain = plain.subspan(4);
        }
        f.payload = StripHeader(version, plain);
        pError = Error::None;
        return f;
    }

    const bool hmac = version == Version::V34;
    const std::size_t end = (hmac ? HMAC_LEN : 4) + 4;
    if(pFrame.size() < HEADER_55AA + end || Get32(pFrame.data()) != PREFIX_55AA ||
       Get32(pFrame.data() + pFrame.size() - 4) != SUFFIX_55AA || FrameSize(pFrame) != pFrame.size())
        return std::nullopt;
    f.seq = Get32(pFrame.data() + 4);
    f.cmd = Get32(pFrame.data() + 8);
    const std::size_t n = pFrame.size() - HEADER_55AA - end;
    const std::span<const uint8_t> covered = pFrame.first(HEADER_55AA + n);
    const std::span<const uint8_t> check = pFrame.subspan(HEADER_55AA + n, end - 4);
    if(hmac)
    {
        std::array<uint8_t, HMAC_LEN> mac{};
        crypto.HmacSha256(key, covered, mac);
        if(!SameBytes(check, mac))
        {
            pError = Error::Auth;
            return std::nullopt;
        }
    }
    else if(Get32(check.data()) != Crc32(covered))
    {
        return std::nullopt;
    }
    std::span<const uint8_t> body = pFrame.subspan(HEADER_55AA, n);
    if(pFromDevice)
    {
        if(body.size() < 4) return std::nullopt;
        f.retcode = Get32(body.data());
        body = body.subspan(4);
    }
    if(version == Version::V33) body = StripHeader(version, body); // 3.3 puts it in front of the ciphertext
    const std::optional<std::span<const uint8_t>> clear = EcbUnpadded(crypto, key, body, pScratch, pError);
    if(!clear.has_value()) return std::nullopt;
    f.payload = StripHeader(version, *clear);
    pError = Error::None;
    return f;
}

std::size_t ControlJson(const Settings& pS, bool pOn, uint32_t pUnixS, std::span<char> pOut)
{
    const char* const v = pOn ? "true" : "false";
    const unsigned long t = pUnixS;
    const unsigned long dp = pS.dp;
    const int n =
        pS.version == Version::V33
            ? std::snprintf(pOut.data(), pOut.size(), R"({"devId":"%s","uid":"%s","t":"%lu","dps":{"%lu":%s}})",
                            pS.id.c_str(), pS.id.c_str(), t, dp, v)
            : std::snprintf(pOut.data(), pOut.size(), R"({"protocol":5,"t":%lu,"data":{"dps":{"%lu":%s}}})", t, dp, v);
    return static_cast<std::size_t>(n) < pOut.size() ? static_cast<std::size_t>(n) : 0; // (a negative n is huge)
}

std::optional<bool> FindDp(std::span<const uint8_t> pPayload, uint32_t pDp)
{
    const std::string_view s(reinterpret_cast<const char*>(pPayload.data()), pPayload.size());
    const std::size_t dps = s.find("\"dps\"");
    if(dps == std::string_view::npos) return std::nullopt;
    char key[16];
    std::snprintf(key, sizeof key, "\"%lu\"", static_cast<unsigned long>(pDp));
    std::size_t at = s.find(key, dps);
    if(at == std::string_view::npos) return std::nullopt;
    at += std::strlen(key);
    while(at < s.size() && (s[at] == ' ' || s[at] == ':'))
        ++at;
    const std::string_view rest = s.substr(at);
    if(rest.starts_with("true")) return true;
    if(rest.starts_with("false")) return false;
    return std::nullopt;
}

Result Switch(IConnection& pConn, ICrypto& pCrypto, const Settings& pS, bool pOn, uint32_t pUnixS, uint32_t pTimeoutMs)
{
    Result res;
    Codec c{pS.version, {}, pCrypto};
    std::memcpy(c.key.data(), pS.key.c_str(), std::min(pS.key.size(), KEY_LEN));
    Reader reader;
    uint32_t seq = 1;
    if(pS.version != Version::V33 && !Negotiate(pConn, c, reader, seq, pS, pTimeoutMs, res)) return res;

    char json[160];
    const std::size_t n = ControlJson(pS, pOn, pUnixS, json);
    const uint32_t cmd = pS.version == Version::V33 ? CMD_CONTROL : CMD_CONTROL_NEW;
    if(!Send(pConn, c, seq, cmd, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json), n), res)) return res;
    uint8_t scratch[FRAME_MAX];
    const std::optional<Frame> ack = Await(reader, pConn, c, cmd, pTimeoutMs, scratch, pS, res);
    if(!ack.has_value()) return res;
    if(ack->retcode.value_or(0) != 0)
    {
        res.error = Error::Refused;
        return res;
    }
    if(!res.state.has_value())
    {
        // The new state usually follows as a pushed status; wait a little for it, but the ack is what counts.
        Await(reader, pConn, c, CMD_STATUS, pTimeoutMs / 3, scratch, pS, res);
        res.error = Error::None;
    }
    return res;
}

bool ValidIpv4(std::string_view pS)
{
    int parts = 0;
    std::size_t i = 0;
    while(parts < 4)
    {
        std::size_t digits = 0;
        unsigned value = 0;
        while(i < pS.size() && pS[i] >= '0' && pS[i] <= '9' && digits < 4)
        {
            value = value * 10 + static_cast<unsigned>(pS[i] - '0');
            ++i;
            ++digits;
        }
        if(digits == 0 || digits > 3 || value > 255) return false;
        ++parts;
        if(parts < 4)
        {
            if(i >= pS.size() || pS[i] != '.') return false;
            ++i;
        }
    }
    return i == pS.size();
}

} // namespace reefdo::tuya
