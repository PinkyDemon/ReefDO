#pragma once
// The API as one function: a request in, a response out, whatever carried it (HTTP on the board or in the
// simulator; later the cloud). Transports only move bytes; what each route does and who may call it is here.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/app.hpp"
#include "reefdo/config.hpp"
#include "reefdo/fixed_string.hpp"
#include "reefdo/gateway/views.hpp"

namespace reefdo::gateway
{

enum class Method : uint8_t
{
    Get,
    Put,
    Post
};

// Who is asking; the transport decides it (the HTTP adapter from the Authorization header, via Authorise).
enum class Who : uint8_t
{
    Anonymous, // reads only
    User,      // signed in with the password: everything
    Cloud      // the phone app through the cloud: reads, and of the commands only ack and suspend_alerts
};

enum class Code : uint16_t
{
    Ok = 200,
    BadRequest = 400,
    Unauthorised = 401,
    Forbidden = 403,
    NotFound = 404
};

enum class ContentType : uint8_t
{
    Json,
    Csv,
    Text
};

struct Request
{
    Method method = Method::Get;
    std::string_view path;  // "/api/status"
    std::string_view query; // "tier=A&from=0", without the '?'
    std::string_view body;
    Who who = Who::Anonymous;
};

// Begin() once, then the body through Write(). A false Write means the client went away.
class IResponse : public ISink
{
public:
    // pFilename: offer the body as a download under this name (the CSV export)
    virtual void Begin(Code pCode, ContentType pType, const char* pFilename) = 0;
};

// What the board has and the core does not: the network, the flash, the buzzer. The simulator fakes it.
struct SysInfo
{
    FixedString<32> ssid;
    bool connected = false;
    FixedString<16> ip;
    int32_t rssi = 0;
    bool apActive = false;
    bool timeSynced = false;
    FixedString<16> partition; // the running OTA slot
    FixedString<16> image;     // "valid", "pending", "undefined"
    uint32_t heap = 0;
    uint32_t heapMin = 0;
    FixedString<8> probe; // "rk500", "sim"
    bool muted = false;
    FixedString<96> config;                         // "stored", "defaults" or "rejected at <path>: <message>"
    FixedString<16> cloud = FixedString<16>("off"); // "off", "not linked", "offline", "online"
    uint32_t cloudPushes = 0;                       // phone pushes sent since boot
};

// Linking the board to a phone app account: a Bluetooth window the app connects to.
struct CloudLink
{
    bool ok = false;
    FixedString<32> service; // the name the app lists
    FixedString<16> pop;     // the code to type into the app
    FixedString<64> message; // why not, or what to do
};

class IPlatform
{
public:
    virtual SysInfo Sys() = 0;
    // Validate, apply to the App and persist; the result says why not.
    virtual config::LoadResult ApplyConfig(std::string_view pJson) = 0;
    virtual void SetTime(uint32_t pUnixS, std::optional<int32_t> pTzOffsetS) = 0;
    virtual void OutputsChanged() = 0; // a command changed device states: switch now, not at the next sample
    virtual bool SetWifi(std::string_view pSsid, std::string_view pPassword) = 0;
    virtual bool SetPassword(std::string_view pPassword) = 0; // empty: back to the default
    virtual void SetMuted(bool pMuted) = 0;
    virtual void TestBuzzer(config::BuzzerPattern pPattern, uint32_t pVolume, uint32_t pHz, uint32_t pSeconds) = 0;
    virtual CloudLink StartCloudLink() = 0; // opens the window (about 10 min) and returns at once

protected:
    ~IPlatform() = default;
};

// Answers one request. pScratch: JSON_MAX bytes for the JSON documents.
void Handle(app::App& pApp, const app::Clock& pClock, IPlatform& pPlatform, const Request& pReq, IResponse& pOut,
            std::span<char> pScratch);

// Basic auth: User if pHeader is "Basic base64(reef:<pPassword>)", else Anonymous.
Who Authorise(std::string_view pHeader, std::string_view pPassword);

// One value out of "a=1&b=x%20y" (a query string or a form body), percent- and '+'-decoded into pOut.
enum class Form : uint8_t
{
    Ok,
    Missing,
    TooLong // pOut holds nothing usable
};
Form FormValue(std::string_view pEncoded, std::string_view pKey, std::span<char> pOut, std::size_t& pLen);
std::optional<uint32_t> FormNumber(std::string_view pEncoded, std::string_view pKey); // decimal digits only

template <std::size_t N>
Form FormValue(std::string_view pEncoded, std::string_view pKey, FixedString<N>& pOut)
{
    char buf[N];
    std::size_t len = 0;
    const Form f = FormValue(pEncoded, pKey, buf, len);
    pOut.assign(std::string_view(buf, f == Form::Ok ? len : 0));
    return f;
}

} // namespace reefdo::gateway
