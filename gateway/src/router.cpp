#include "reefdo/gateway/router.hpp"

#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

namespace reefdo::gateway
{

namespace
{

constexpr std::string_view USER = "reef";
constexpr std::size_t PASSWORD_MAX = 32;
constexpr const char* BUZZER_NAMES[] = {"off", "chirp", "beep", "double", "triple", "continuous"};

struct Context
{
    app::App& app;
    const app::Clock& clock;
    IPlatform& platform;
    const Request& req;
    IResponse& out;
    std::span<char> scratch;
};

int Hex(char pC)
{
    if(pC >= '0' && pC <= '9') return pC - '0';
    if(pC >= 'a' && pC <= 'f') return pC - 'a' + 10;
    if(pC >= 'A' && pC <= 'F') return pC - 'A' + 10;
    return -1;
}

// 0..63 for a base64 digit, -1 otherwise
int Base64Digit(char pC)
{
    if(pC >= 'A' && pC <= 'Z') return pC - 'A';
    if(pC >= 'a' && pC <= 'z') return pC - 'a' + 26;
    if(pC >= '0' && pC <= '9') return pC - '0' + 52;
    if(pC == '+') return 62;
    if(pC == '/') return 63;
    return -1;
}

// Standard base64 with '=' padding; false on anything else or when it does not fit.
bool Base64Decode(std::string_view pIn, std::span<char> pOut, std::size_t& pLen)
{
    pLen = 0;
    uint32_t acc = 0;
    int bits = 0;
    std::size_t i = 0;
    for(; i < pIn.size() && pIn[i] != '='; ++i)
    {
        const int d = Base64Digit(pIn[i]);
        if(d < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if(bits >= 8)
        {
            bits -= 8;
            if(pLen == pOut.size()) return false;
            pOut[pLen++] = static_cast<char>((acc >> bits) & 0xFFu);
        }
    }
    for(; i < pIn.size(); ++i)
        if(pIn[i] != '=') return false;
    return true;
}

// Compares every byte whatever the first mismatch: no timing hint about the password.
bool SameSecret(std::string_view pA, std::string_view pB)
{
    uint32_t diff = static_cast<uint32_t>(pA.size() ^ pB.size());
    for(std::size_t i = 0; i < pA.size() && i < pB.size(); ++i)
        diff |= static_cast<uint32_t>(static_cast<uint8_t>(pA[i]) ^ static_cast<uint8_t>(pB[i]));
    return diff == 0;
}

void Json(Context& pC, std::size_t pLen)
{
    pC.out.Begin(Code::Ok, ContentType::Json, nullptr);
    pC.out.Write(std::string_view(pC.scratch.data(), pLen));
}

void JsonOk(Context& pC, bool pOk)
{
    const int n = std::snprintf(pC.scratch.data(), pC.scratch.size(), "{\"ok\":%s}", pOk ? "true" : "false");
    Json(pC, static_cast<std::size_t>(n));
}

void Text(IResponse& pOut, Code pCode, std::string_view pText)
{
    pOut.Begin(pCode, ContentType::Text, nullptr);
    pOut.Write(pText);
}

uint32_t Number(const Context& pC, std::string_view pKey, uint32_t pDefault)
{
    return FormNumber(pC.req.query, pKey).value_or(pDefault);
}

void GetStatus(Context& pC)
{
    Json(pC, StatusJson(pC.app, pC.clock, pC.scratch));
}

void GetTest(Context& pC)
{
    Json(pC, TestJson(pC.app, pC.scratch));
}

void GetSlots(Context& pC)
{
    Json(pC, config::SlotSchema(pC.scratch));
}

void GetAuth(Context& pC)
{
    JsonOk(pC, true); // only signed-in requests get here
}

void GetConfig(Context& pC)
{
    Json(pC, config::Write(pC.app.GetConfig(), pC.scratch, true)); // local keys stay on the board
}

void PutConfig(Context& pC)
{
    Json(pC, ResultJson(pC.platform.ApplyConfig(pC.req.body), pC.scratch));
}

void PostCmd(Context& pC)
{
    const Command c = ApplyCommand(pC.app, pC.req.body, pC.clock);
    if(c.setUnix.has_value()) pC.platform.SetTime(*c.setUnix, c.setTz);
    pC.platform.OutputsChanged();
    const int n = std::snprintf(pC.scratch.data(), pC.scratch.size(), "{\"ok\":%s,\"message\":\"%s\"}",
                                c.ok ? "true" : "false", c.message);
    Json(pC, static_cast<std::size_t>(n));
}

void GetSeries(Context& pC)
{
    FixedString<1> tier;
    FormValue(pC.req.query, "tier", tier);
    const uint32_t every = Number(pC, "every", 1);
    pC.out.Begin(Code::Ok, ContentType::Csv, nullptr);
    SeriesCsv(pC.app, tier.view() == "B" ? 'B' : 'A', Number(pC, "from", 0), Number(pC, "to", 0xFFFFFFFFu),
              every > 0 ? every : 1, pC.out);
}

void GetEvents(Context& pC)
{
    pC.out.Begin(Code::Ok, ContentType::Csv, nullptr);
    EventsCsv(pC.app, Number(pC, "since", 0), pC.out);
}

void GetExport(Context& pC)
{
    pC.out.Begin(Code::Ok, ContentType::Csv, "reefdo.csv");
    ExportCsv(pC.app, Number(pC, "since", 0), pC.out);
}

void GetSys(Context& pC)
{
    const SysInfo s = pC.platform.Sys();
    JsonDocument doc;
    doc["ssid"] = s.ssid.view();
    doc["connected"] = s.connected;
    doc["ip"] = s.ip.view();
    doc["rssi"] = s.rssi;
    doc["ap"] = s.apActive;
    doc["ntp"] = s.timeSynced;
    doc["partition"] = s.partition.view();
    doc["image"] = s.image.view();
    doc["heap"] = s.heap;
    doc["heap_min"] = s.heapMin;
    doc["probe"] = s.probe.view();
    doc["muted"] = s.muted;
    doc["config"] = s.config.view();
    doc["cloud"] = s.cloud.view();
    doc["cloud_pushes"] = s.cloudPushes;
    Json(pC, serializeJson(doc, pC.scratch.data(), pC.scratch.size()));
}

// Form ssid=&pass= (a password may legitimately contain JSON-hostile characters). No pass: an open network.
void PostWifi(Context& pC)
{
    FixedString<32> ssid;
    FixedString<64> pass;
    const bool ok = FormValue(pC.req.body, "ssid", ssid) == Form::Ok &&
                    FormValue(pC.req.body, "pass", pass) != Form::TooLong &&
                    pC.platform.SetWifi(ssid.view(), pass.view());
    JsonOk(pC, ok);
}

// Form password= ; empty: back to the default.
void PostPassword(Context& pC)
{
    FixedString<PASSWORD_MAX> pw;
    const bool ok = FormValue(pC.req.body, "password", pw) == Form::Ok && pC.platform.SetPassword(pw.view());
    JsonOk(pC, ok);
}

// Form muted=1|0: every alarm tone silent, for testing.
void PostMute(Context& pC)
{
    const std::optional<uint32_t> muted = FormNumber(pC.req.body, "muted");
    if(muted.has_value()) pC.platform.SetMuted(*muted != 0);
    const int n = std::snprintf(pC.scratch.data(), pC.scratch.size(), "{\"ok\":%s,\"muted\":%s}",
                                muted.has_value() ? "true" : "false", pC.platform.Sys().muted ? "true" : "false");
    Json(pC, static_cast<std::size_t>(n));
}

// Form pattern=&volume=&hz=&s=: plays the buzzer for a few seconds, muted or not.
void PostBuzzerTest(Context& pC)
{
    FixedString<16> name;
    if(FormValue(pC.req.body, "pattern", name) == Form::Missing) name.assign("beep");
    bool ok = false;
    for(std::size_t i = 0; i < std::size(BUZZER_NAMES) && !ok; ++i)
    {
        if(name.view() != BUZZER_NAMES[i]) continue;
        ok = true;
        pC.platform.TestBuzzer(static_cast<config::BuzzerPattern>(i), FormNumber(pC.req.body, "volume").value_or(2),
                               FormNumber(pC.req.body, "hz").value_or(2400), FormNumber(pC.req.body, "s").value_or(5));
    }
    JsonOk(pC, ok);
}

// Opens the Bluetooth window for linking the board in the phone app (ESP RainMaker Home).
void PostCloudLink(Context& pC)
{
    const CloudLink l = pC.platform.StartCloudLink();
    JsonDocument doc;
    doc["ok"] = l.ok;
    doc["service"] = l.service.view();
    doc["pop"] = l.pop.view();
    doc["message"] = l.message.view();
    Json(pC, serializeJson(doc, pC.scratch.data(), pC.scratch.size()));
}

struct Route
{
    Method method;
    std::string_view path;
    bool signedIn; // writes, and the credential check itself
    void (*handler)(Context&);
};

constexpr Route ROUTES[] = {
    {Method::Get, "/api/status", false, GetStatus},
    {Method::Get, "/api/test", false, GetTest},
    {Method::Get, "/api/slots", false, GetSlots},
    {Method::Get, "/api/auth", true, GetAuth},
    {Method::Get, "/api/config", false, GetConfig},
    {Method::Put, "/api/config", true, PutConfig},
    {Method::Post, "/api/cmd", true, PostCmd},
    {Method::Get, "/api/series", false, GetSeries},
    {Method::Get, "/api/events", false, GetEvents},
    {Method::Get, "/api/export.csv", false, GetExport},
    {Method::Get, "/api/sys", false, GetSys},
    {Method::Post, "/api/wifi", true, PostWifi},
    {Method::Post, "/api/passwd", true, PostPassword},
    {Method::Post, "/api/mute", true, PostMute},
    {Method::Post, "/api/buzzer-test", true, PostBuzzerTest},
    {Method::Post, "/api/cloud-link", true, PostCloudLink},
};

// The cloud may send exactly one command, and only one of these: config and maintenance stay on the local page.
bool CloudMay(std::string_view pBody)
{
    JsonDocument doc;
    if(deserializeJson(doc, pBody.data(), pBody.size())) return false;
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    if(root.size() != 1) return false;
    const std::string_view key = root.begin()->key().c_str();
    return key == "ack" || key == "suspend_alerts";
}

} // namespace

void Handle(app::App& pApp, const app::Clock& pClock, IPlatform& pPlatform, const Request& pReq, IResponse& pOut,
            std::span<char> pScratch)
{
    for(const Route& r : ROUTES)
    {
        if(r.method != pReq.method || r.path != pReq.path) continue;
        // No WWW-Authenticate on purpose: browsers would cache a native login for the whole site.
        const bool cloudCommand = pReq.who == Who::Cloud && r.handler == PostCmd && CloudMay(pReq.body);
        if(r.signedIn && pReq.who == Who::Cloud && !cloudCommand)
            return Text(pOut, Code::Forbidden, "not from the cloud: use the local page");
        if(r.signedIn && pReq.who == Who::Anonymous) return Text(pOut, Code::Unauthorised, "unauthorised");
        Context c{pApp, pClock, pPlatform, pReq, pOut, pScratch};
        return r.handler(c);
    }
    Text(pOut, Code::NotFound, "not found");
}

Who Authorise(std::string_view pHeader, std::string_view pPassword)
{
    constexpr std::string_view BASIC = "Basic ";
    if(!pHeader.starts_with(BASIC)) return Who::Anonymous;
    char decoded[USER.size() + 1 + PASSWORD_MAX];
    std::size_t n = 0;
    if(!Base64Decode(pHeader.substr(BASIC.size()), decoded, n)) return Who::Anonymous;
    const std::string_view got(decoded, n);
    const bool user = got.starts_with(USER) && got.size() > USER.size() && got[USER.size()] == ':';
    return user && SameSecret(got.substr(USER.size() + 1), pPassword) ? Who::User : Who::Anonymous;
}

Form FormValue(std::string_view pEncoded, std::string_view pKey, std::span<char> pOut, std::size_t& pLen)
{
    pLen = 0;
    while(!pEncoded.empty())
    {
        const std::size_t amp = pEncoded.find('&');
        const std::string_view pair = pEncoded.substr(0, amp);
        pEncoded = amp == std::string_view::npos ? std::string_view() : pEncoded.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        if(pair.substr(0, eq) != pKey) continue;
        const std::string_view v = eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
        for(std::size_t i = 0; i < v.size(); ++i)
        {
            char c = v[i];
            if(c == '+') c = ' ';
            const int hi = c == '%' && i + 2 < v.size() ? Hex(v[i + 1]) : -1; // a stray '%' stays itself
            const int lo = hi >= 0 ? Hex(v[i + 2]) : -1;
            if(lo >= 0)
            {
                c = static_cast<char>(hi * 16 + lo);
                i += 2;
            }
            if(pLen == pOut.size()) return Form::TooLong;
            pOut[pLen++] = c;
        }
        return Form::Ok;
    }
    return Form::Missing;
}

std::optional<uint32_t> FormNumber(std::string_view pEncoded, std::string_view pKey)
{
    FixedString<10> digits;
    if(FormValue(pEncoded, pKey, digits) != Form::Ok || digits.empty()) return std::nullopt;
    uint64_t v = 0;
    for(const char c : digits.view())
    {
        if(c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if(v > UINT32_MAX) return std::nullopt;
    return static_cast<uint32_t>(v);
}

} // namespace reefdo::gateway
