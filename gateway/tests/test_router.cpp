#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

#include "reefdo/gateway/router.hpp"

#include "scenario.hpp"
#include "test_platform.hpp"

using namespace scenario;
using testing::Platform;
using namespace reefdo::gateway;
using reefdo::FixedString;

namespace
{

struct Reply : IResponse
{
    int begins = 0;
    Code code = Code::Ok;
    ContentType type = ContentType::Text;
    std::string filename;
    std::string body;
    void Begin(Code pCode, ContentType pType, const char* pFilename) override
    {
        ++begins;
        code = pCode;
        type = pType;
        filename = pFilename != nullptr ? pFilename : "";
    }
    bool Write(std::string_view pChunk) override
    {
        body.append(pChunk);
        return true;
    }
};

struct Bench
{
    Scenario s;
    Platform platform;
    std::vector<char> scratch = std::vector<char>(JSON_MAX);
    Bench()
    {
        s.Start();
        s.RunS(60);
        platform.app = &s.app;
        platform.clock = &s.clock;
    }
    Reply Call(Method pM, std::string_view pPath, std::string_view pQuery = {}, std::string_view pBody = {},
               Who pWho = Who::User)
    {
        Reply r;
        Handle(s.app, s.clock, platform, Request{pM, pPath, pQuery, pBody, pWho}, r, scratch);
        REQUIRE(r.begins == 1);
        return r;
    }
};

std::string Base64(std::string_view pS)
{
    static const char* const DIGITS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for(const char c : pS)
    {
        acc = (acc << 8) | static_cast<uint8_t>(c);
        bits += 8;
        while(bits >= 6)
        {
            bits -= 6;
            out += DIGITS[(acc >> bits) & 63u];
        }
    }
    if(bits > 0) out += DIGITS[(acc << (6 - bits)) & 63u];
    while(out.size() % 4 != 0)
        out += '=';
    return out;
}

} // namespace

TEST_CASE("Authorise accepts exactly Basic reef:<password>", "[router]")
{
    REQUIRE(Authorise("Basic " + Base64("reef:s3cr+t/Pw!"), "s3cr+t/Pw!") == Who::User);
    REQUIRE(Authorise("Basic " + Base64("reef:"), "") == Who::User); // an empty password is still a password
    REQUIRE(Authorise("Basic " + Base64("reef:wrong"), "s3cret") == Who::Anonymous);
    REQUIRE(Authorise("Basic " + Base64("reef:s3cre"), "s3cret") == Who::Anonymous);   // a prefix is not enough
    REQUIRE(Authorise("Basic " + Base64("reef:s3cretX"), "s3cret") == Who::Anonymous); // nor a longer one
    const std::string binary = "x\xfb\xef\xbe\xff\xff\xff"; // encodes to "++++////" after "reef:x"
    REQUIRE(Base64("reef:" + binary).find("++++////") != std::string::npos);
    REQUIRE(Authorise("Basic " + Base64("reef:" + binary), binary) == Who::User);
    REQUIRE(Authorise("Basic cmVl{", "") == Who::Anonymous); // past 'z'
    REQUIRE(Authorise("Basic cmVl:", "") == Who::Anonymous); // between '9' and 'A'
    REQUIRE(Authorise("Basic " + Base64("root:s3cret"), "s3cret") == Who::Anonymous);
    REQUIRE(Authorise("Basic " + Base64("reef"), "") == Who::Anonymous);
    REQUIRE(Authorise("Basic " + Base64("reefs3cret"), "s3cret") == Who::Anonymous);
    REQUIRE(Authorise("Bearer " + Base64("reef:s3cret"), "s3cret") == Who::Anonymous);
    REQUIRE(Authorise("", "s3cret") == Who::Anonymous);
    REQUIRE(Authorise("Basic cmVlZjp*", "") == Who::Anonymous);  // not base64
    REQUIRE(Authorise("Basic cmVlZjo=x", "") == Who::Anonymous); // data after the padding
    REQUIRE(Authorise("Basic cmVlZjo==", "") == Who::User);      // padding is fine
    const std::string huge(100, 'x');
    REQUIRE(Authorise("Basic " + Base64("reef:" + huge), huge) == Who::Anonymous); // longer than any password
}

TEST_CASE("FormValue decodes '+' and %XX and tells missing from too long", "[router]")
{
    FixedString<16> v;
    REQUIRE(FormValue("a=1&pw=x%21y+z%2B&b", "pw", v) == Form::Ok);
    REQUIRE(v.view() == "x!y z+");
    REQUIRE(FormValue("a=1&pw=%41%61%7e", "pw", v) == Form::Ok);
    REQUIRE(v.view() == "Aa~");
    REQUIRE(FormValue("pw=100%&x", "pw", v) == Form::Ok); // stray '%' at the end
    REQUIRE(v.view() == "100%");
    REQUIRE(FormValue("pw=%zz%4g", "pw", v) == Form::Ok); // not hex: taken as it is
    REQUIRE(v.view() == "%zz%4g");
    REQUIRE(FormValue("pw=%#0%:0", "pw", v) == Form::Ok); // below '0', between '9' and 'A'
    REQUIRE(v.view() == "%#0%:0");
    REQUIRE(FormValue("flag&pw", "pw", v) == Form::Ok); // a key without '=' is an empty value
    REQUIRE(v.empty());
    REQUIRE(FormValue("a=1&b=2", "pw", v) == Form::Missing);
    REQUIRE(FormValue("", "pw", v) == Form::Missing);
    REQUIRE(FormValue("pw=0123456789abcdefX", "pw", v) == Form::TooLong);
    REQUIRE(v.empty());
    REQUIRE(FormValue("pw=0123456789abcdef", "pw", v) == Form::Ok); // exactly the capacity
}

TEST_CASE("FormNumber takes decimal digits that fit 32 bits", "[router]")
{
    REQUIRE(FormNumber("from=0&to=4294967295", "to") == 4294967295u);
    REQUIRE(FormNumber("from=0", "from") == 0u);
    REQUIRE_FALSE(FormNumber("to=4294967296", "to").has_value());
    REQUIRE_FALSE(FormNumber("to=12345678901", "to").has_value()); // too long to even read
    REQUIRE_FALSE(FormNumber("to=12a", "to").has_value());
    REQUIRE_FALSE(FormNumber("to=-1", "to").has_value());
    REQUIRE_FALSE(FormNumber("to=", "to").has_value());
    REQUIRE_FALSE(FormNumber("from=1", "to").has_value());
}

TEST_CASE("Reads are open, writes need the password, unknown routes are 404", "[router]")
{
    Bench b;
    const char* reads[] = {"/api/status", "/api/test", "/api/slots", "/api/config", "/api/sys"};
    for(const char* path : reads)
    {
        INFO(path);
        const Reply r = b.Call(Method::Get, path, {}, {}, Who::Anonymous);
        REQUIRE(r.code == Code::Ok);
        REQUIRE(r.type == ContentType::Json);
        REQUIRE(r.body.front() == '{');
    }
    struct Write
    {
        Method method;
        const char* path;
    };
    const Write writes[] = {{Method::Get, "/api/auth"},        {Method::Put, "/api/config"},
                            {Method::Post, "/api/cmd"},        {Method::Post, "/api/wifi"},
                            {Method::Post, "/api/passwd"},     {Method::Post, "/api/mute"},
                            {Method::Post, "/api/buzzer-test"}};
    for(const Write& w : writes)
    {
        INFO(w.path);
        const Reply r = b.Call(w.method, w.path, {}, R"({"ack":true})", Who::Anonymous);
        REQUIRE(r.code == Code::Unauthorised);
        REQUIRE(r.type == ContentType::Text);
    }
    REQUIRE(b.platform.configs.empty());
    REQUIRE(b.platform.outputsChanged == 0);
    REQUIRE_FALSE(b.s.app.GetStatus().silenced);

    REQUIRE(b.Call(Method::Get, "/api/auth").body == R"({"ok":true})");
    REQUIRE(b.Call(Method::Get, "/api/nothing").code == Code::NotFound);
    REQUIRE(b.Call(Method::Post, "/api/status").code == Code::NotFound); // right path, wrong method
}

TEST_CASE("Config reads redacted and writes through the platform", "[router]")
{
    Bench b;
    const Reply get = b.Call(Method::Get, "/api/config", {}, {}, Who::Anonymous);
    REQUIRE(get.body.find("\"sample_period_s\":10") != std::string::npos);
    const Reply bad = b.Call(Method::Put, "/api/config", {}, R"({"sample_period_s":0})");
    REQUIRE(bad.body.find("\"ok\":false") != std::string::npos);
    const Reply good = b.Call(Method::Put, "/api/config", {}, R"({"ack_silence_s":600})");
    REQUIRE(good.body == R"({"ok":true})");
    REQUIRE(b.platform.configs.size() == 2);
    REQUIRE(b.s.app.GetConfig().ladder.ackSilenceS == 600);
}

TEST_CASE("Commands apply, switch the outputs now and pass the time on", "[router]")
{
    Bench b;
    const Reply ack = b.Call(Method::Post, "/api/cmd", {}, R"({"ack":true})");
    REQUIRE(ack.body == R"({"ok":true,"message":"acknowledged"})");
    REQUIRE(b.platform.outputsChanged == 1);
    REQUIRE_FALSE(b.platform.unix.has_value());
    const Reply time = b.Call(Method::Post, "/api/cmd", {}, R"({"time":{"unix":1789400000,"tz":3600}})");
    REQUIRE(time.body.find("\"ok\":true") != std::string::npos);
    REQUIRE(b.platform.unix == 1789400000u);
    REQUIRE(b.platform.tz == 3600);
    REQUIRE(b.Call(Method::Post, "/api/cmd", {}, "{").body.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("CSV routes stream series, events and the export", "[router]")
{
    Bench b;
    const Reply a = b.Call(Method::Get, "/api/series", {}, {}, Who::Anonymous);
    REQUIRE(a.type == ContentType::Csv);
    REQUIRE(a.body.starts_with("ts,do,sat,temp,level"));
    const Reply every = b.Call(Method::Get, "/api/series", "every=0&from=0&to=4294967295", {}, Who::Anonymous);
    REQUIRE(every.body == a.body); // every=0 is every row
    const Reply tierB = b.Call(Method::Get, "/api/series", "tier=B", {}, Who::Anonymous);
    REQUIRE(tierB.body.starts_with("ts,do_min"));
    const Reply late = b.Call(Method::Get, "/api/series", "from=4000000000", {}, Who::Anonymous);
    REQUIRE(late.body.find('\n') == late.body.size() - 1); // the header only
    const Reply ev = b.Call(Method::Get, "/api/events", "since=0", {}, Who::Anonymous);
    REQUIRE(ev.type == ContentType::Csv);
    REQUIRE(ev.filename.empty());
    const Reply ex = b.Call(Method::Get, "/api/export.csv", {}, {}, Who::Anonymous);
    REQUIRE(ex.filename == "reefdo.csv");
    REQUIRE(ex.body.starts_with("seq,ts"));
}

TEST_CASE("System info comes from the platform", "[router]")
{
    Bench b;
    b.platform.sys.ssid.assign("reef \"net\"");
    b.platform.sys.connected = true;
    b.platform.sys.rssi = -61;
    b.platform.sys.config.assign("rejected at levels: \"x\"");
    b.platform.sys.cloud.assign("online");
    b.platform.sys.cloudPushes = 3;
    const Reply r = b.Call(Method::Get, "/api/sys", {}, {}, Who::Anonymous);
    REQUIRE(r.body.find(R"("ssid":"reef \"net\"")") != std::string::npos); // escaped properly
    REQUIRE(r.body.find(R"("connected":true)") != std::string::npos);
    REQUIRE(r.body.find(R"("rssi":-61)") != std::string::npos);
    REQUIRE(r.body.find(R"("muted":false)") != std::string::npos);
    REQUIRE(r.body.find(R"("cloud":"online","cloud_pushes":3)") != std::string::npos);
}

TEST_CASE("Wi-Fi and password forms are decoded before they are stored", "[router]")
{
    Bench b;
    REQUIRE(b.Call(Method::Post, "/api/wifi", {}, "ssid=My+Net&pass=p%26ss%3Dw%21rd").body == R"({"ok":true})");
    REQUIRE(b.platform.ssid == "My Net");
    REQUIRE(b.platform.pass == "p&ss=w!rd");
    REQUIRE(b.Call(Method::Post, "/api/wifi", {}, "ssid=Open").body == R"({"ok":true})"); // no password
    REQUIRE(b.platform.pass.empty());
    REQUIRE(b.Call(Method::Post, "/api/wifi", {}, "pass=x").body == R"({"ok":false})"); // no ssid
    REQUIRE(b.Call(Method::Post, "/api/wifi", {}, "ssid=a&pass=" + std::string(65, 'x')).body == R"({"ok":false})");
    b.platform.acceptWifi = false;
    REQUIRE(b.Call(Method::Post, "/api/wifi", {}, "ssid=a").body == R"({"ok":false})");

    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "password=S3cr%21t").body == R"({"ok":true})");
    REQUIRE(b.platform.password == "S3cr!t");
    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "password=").body == R"({"ok":true})"); // back to the default
    REQUIRE(b.platform.password.empty());
    b.platform.password = "kept";
    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "other=1").body == R"({"ok":false})");
    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "password=" + std::string(33, 'x')).body == R"({"ok":false})");
    REQUIRE(b.platform.password == "kept");
    b.platform.acceptPassword = false;
    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "password=abc").body == R"({"ok":false})");
}

TEST_CASE("Mute and the buzzer test", "[router]")
{
    Bench b;
    REQUIRE(b.Call(Method::Post, "/api/mute", {}, "muted=1").body == R"({"ok":true,"muted":true})");
    REQUIRE(b.Call(Method::Post, "/api/mute", {}, "muted=0").body == R"({"ok":true,"muted":false})");
    REQUIRE(b.Call(Method::Post, "/api/mute", {}, "muted=yes").body == R"({"ok":false,"muted":false})");

    REQUIRE(b.Call(Method::Post, "/api/buzzer-test", {}, "").body == R"({"ok":true})"); // the defaults
    REQUIRE(b.platform.pattern == reefdo::config::BuzzerPattern::Beep);
    REQUIRE(b.platform.volume == 2);
    REQUIRE(b.platform.hz == 2400);
    REQUIRE(b.platform.seconds == 5);
    REQUIRE(b.Call(Method::Post, "/api/buzzer-test", {}, "pattern=continuous&volume=3&hz=3000&s=2").body ==
            R"({"ok":true})");
    REQUIRE(b.platform.pattern == reefdo::config::BuzzerPattern::Continuous);
    REQUIRE(b.platform.volume == 3);
    REQUIRE(b.platform.hz == 3000);
    REQUIRE(b.platform.seconds == 2);
    b.platform.pattern.reset();
    REQUIRE(b.Call(Method::Post, "/api/buzzer-test", {}, "pattern=siren").body == R"({"ok":false})");
    REQUIRE(b.Call(Method::Post, "/api/buzzer-test", {}, "pattern=" + std::string(20, 'x')).body == R"({"ok":false})");
    REQUIRE_FALSE(b.platform.pattern.has_value());
}

TEST_CASE("The cloud reads, acknowledges and suspends alerts — nothing else", "[router][cloud]")
{
    Bench b;
    REQUIRE(b.Call(Method::Get, "/api/status", {}, {}, Who::Cloud).code == Code::Ok);
    REQUIRE(b.Call(Method::Post, "/api/cmd", {}, R"({"ack":true})", Who::Cloud).body.find("\"ok\":true") !=
            std::string::npos);
    REQUIRE(b.Call(Method::Post, "/api/cmd", {}, R"({"suspend_alerts":{"s":0}})", Who::Cloud).code == Code::Ok);
    const char* refused[] = {R"({"maintenance":true})", R"({"ack":true,"maintenance":true})", "{", "[1]",
                             R"({"manual":{"device":1,"on":true}})"};
    for(const char* body : refused)
    {
        INFO(body);
        const Reply r = b.Call(Method::Post, "/api/cmd", {}, body, Who::Cloud);
        REQUIRE(r.code == Code::Forbidden);
    }
    REQUIRE(b.Call(Method::Put, "/api/config", {}, R"({"ack_silence_s":60})", Who::Cloud).code == Code::Forbidden);
    REQUIRE(b.Call(Method::Get, "/api/auth", {}, {}, Who::Cloud).code == Code::Forbidden);
    REQUIRE(b.Call(Method::Post, "/api/passwd", {}, "password=x", Who::Cloud).code == Code::Forbidden);
    REQUIRE_FALSE(b.s.app.GetStatus().maintenance);
    REQUIRE(b.platform.configs.empty());
}

TEST_CASE("Linking to the phone app: signed in only, the name and the code come back", "[router][cloud]")
{
    Bench b;
    REQUIRE(b.Call(Method::Post, "/api/cloud-link", {}, {}, Who::Anonymous).code == Code::Unauthorised);
    REQUIRE(b.Call(Method::Post, "/api/cloud-link", {}, {}, Who::Cloud).code == Code::Forbidden);
    REQUIRE(b.platform.links == 0);
    b.platform.link.ok = true;
    b.platform.link.service.assign("REEFDO_1A2B");
    b.platform.link.pop.assign("5f3c9a01");
    b.platform.link.message.assign("open the app");
    const Reply r = b.Call(Method::Post, "/api/cloud-link");
    REQUIRE(r.body == R"({"ok":true,"service":"REEFDO_1A2B","pop":"5f3c9a01","message":"open the app"})");
    REQUIRE(b.platform.links == 1);
}
