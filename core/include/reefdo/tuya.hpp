#pragma once
// Tuya Wi-Fi plugs over the LAN, without the cloud: the local protocol 3.3 / 3.4 / 3.5 on TCP 6668.
// Framing, session negotiation and one switch transaction. Crypto and the socket come in through interfaces
// (mbedTLS and lwIP on the board), so all of this runs and is tested on the host.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/fixed_string.hpp"

namespace reefdo::tuya
{

constexpr uint16_t PORT = 6668;
constexpr std::size_t KEY_LEN = 16;
constexpr std::size_t FRAME_MAX = 1024; // our frames are small; a device status is a few hundred bytes

enum class Version : uint8_t
{
    V33,
    V34,
    V35
};

// One plug outlet (a device's slot::Tuya carries these). Lengths are checked where the config is validated.
struct Settings
{
    FixedString<31> ip; // dotted IPv4 — give the plug a DHCP reservation
    FixedString<31> id;
    FixedString<31> key; // the local key, 16 characters
    Version version = Version::V33;
    uint32_t dp = 1; // the switch data point, 1 on almost every plug
};

class ICrypto
{
public:
    virtual ~ICrypto() = default;
    // Whole blocks only (the codec pads). Return false on any library failure.
    virtual bool EcbEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                            std::span<uint8_t> pOut) = 0;
    virtual bool EcbDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                            std::span<uint8_t> pOut) = 0;
    virtual bool GcmEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                            std::span<const uint8_t> pAad, std::span<const uint8_t> pIn, std::span<uint8_t> pOut,
                            std::span<uint8_t, 16> pTag) = 0;
    // False when the tag does not match.
    virtual bool GcmDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                            std::span<const uint8_t> pAad, std::span<const uint8_t> pIn,
                            std::span<const uint8_t, 16> pTag, std::span<uint8_t> pOut) = 0;
    virtual void HmacSha256(std::span<const uint8_t> pKey, std::span<const uint8_t> pData,
                            std::span<uint8_t, 32> pOut) = 0;
    virtual void Random(std::span<uint8_t> pOut) = 0;
};

// A connected TCP stream to the plug.
class IConnection
{
public:
    virtual ~IConnection() = default;
    virtual bool Send(std::span<const uint8_t> pData) = 0;
    // Up to pOut.size() bytes, waiting at most pTimeoutMs; 0 = nothing (timeout or closed).
    virtual std::size_t Receive(std::span<uint8_t> pOut, uint32_t pTimeoutMs) = 0;
};

enum class Error : uint8_t
{
    None,
    Connect, // set by the caller: no TCP connection
    Send,    // the stream refused our bytes
    Timeout, // no (complete) answer
    Frame,   // an answer we cannot read: bad prefix, length, CRC, suffix or padding
    Auth,    // HMAC or GCM tag wrong, or the session handshake did not verify: the local key is wrong
    Refused, // the plug answered the command with a non-zero return code
    Crypto,  // the crypto library failed
};

struct Result
{
    Error error = Error::None;
    std::optional<bool> state; // the switch state the plug reported back, when it did
    bool Ok() const { return error == Error::None; }
};

// Commands we use (tinytuya names)
enum Command : uint32_t
{
    CMD_SESS_KEY_NEG_START = 3,
    CMD_SESS_KEY_NEG_RESP = 4,
    CMD_SESS_KEY_NEG_FINISH = 5,
    CMD_CONTROL = 7,
    CMD_STATUS = 8,
    CMD_CONTROL_NEW = 13,
};

// ---- the codec, exposed for tests and for a device emulator ----

uint32_t Crc32(std::span<const uint8_t> pData);

struct Frame
{
    uint32_t seq = 0;
    uint32_t cmd = 0;
    std::optional<uint32_t> retcode;  // device frames carry one
    std::span<const uint8_t> payload; // decrypted, version header removed; points into the caller's scratch
};

struct Codec
{
    Version version = Version::V33;
    std::array<uint8_t, KEY_LEN> key{}; // the local key, or the session key once negotiated
    ICrypto& crypto;

    // A complete frame into pOut; returns its size, 0 if it does not fit or the crypto failed.
    // pRetcode: set when encoding as the device does.
    std::size_t Encode(uint32_t pSeq, uint32_t pCmd, std::span<const uint8_t> pPlain, std::span<uint8_t> pOut,
                       std::optional<uint32_t> pRetcode = std::nullopt) const;
    // Size of the frame at the start of pData once its header is in; 0 = need more bytes; SIZE_MAX = not a frame.
    static std::size_t FrameSize(std::span<const uint8_t> pData);
    // Checks and decrypts one complete frame. pFromDevice: a return code precedes the payload.
    std::optional<Frame> Decode(std::span<const uint8_t> pFrame, bool pFromDevice, std::span<uint8_t> pScratch,
                                Error& pError) const;
};

// The "t" and dps payload for switching data point `dp`.
std::size_t ControlJson(const Settings& pS, bool pOn, uint32_t pUnixS, std::span<char> pOut);
// The switch state for `dp` in a status payload, if it is there.
std::optional<bool> FindDp(std::span<const uint8_t> pPayload, uint32_t pDp);

// One transaction on an open connection: negotiate a session (3.4 / 3.5), switch the dp, wait for the ack.
Result Switch(IConnection& pConn, ICrypto& pCrypto, const Settings& pS, bool pOn, uint32_t pUnixS,
              uint32_t pTimeoutMs = 3000);

// "192.168.1.50" → true when it is a dotted IPv4 address.
bool ValidIpv4(std::string_view pS);

} // namespace reefdo::tuya
