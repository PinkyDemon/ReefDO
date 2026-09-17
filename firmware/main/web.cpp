#include "web.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reefdo/api.hpp"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/nvs_store.hpp"
#include "indicator.hpp"
#include "mbedtls/base64.h"
#include "net.hpp"
#include "notify.hpp"
#include "sampler.hpp"

extern const uint8_t sIndexStart[] asm("_binary_index_html_start");
extern const uint8_t sIndexEnd[] asm("_binary_index_html_end");
extern const uint8_t sLogoStart[] asm("_binary_logo_ReefDO_svg_start");
extern const uint8_t sLogoEnd[] asm("_binary_logo_ReefDO_svg_end");
extern const uint8_t sTouchStart[] asm("_binary_apple_touch_icon_png_start");
extern const uint8_t sTouchEnd[] asm("_binary_apple_touch_icon_png_end");

namespace web
{

namespace
{

const char* const TAG = "web";
const char* const KEY_PASSWORD = "pw";
const char* const DEFAULT_PASSWORD = "reefdo";
const char* const USER = "reef";
constexpr std::size_t BODY_MAX = 4096;

httpd_handle_t sServer = nullptr;
char sPassword[33] = {};
char sBody[BODY_MAX + 1]; // request bodies (the httpd task only)
char sJson[4096];         // responses

// Streams CSV into chunked responses. Sends happen with the App unlocked, so a slow client never stalls the
// sampler; the ring may rotate meanwhile, which can repeat or skip a few rows at a segment boundary.
class HttpSink : public reefdo::api::ISink
{
public:
    HttpSink(httpd_req_t* pReq, sampler::Guard& pGuard)
        : mReq(pReq)
        , mGuard(pGuard)
    {
    }
    bool Write(std::string_view pChunk) override
    {
        if(mLen + pChunk.size() > sizeof mBuf && !Flush()) return false;
        std::memcpy(mBuf + mLen, pChunk.data(), pChunk.size());
        mLen += pChunk.size();
        return true;
    }
    bool Flush()
    {
        if(mLen == 0) return true;
        mGuard.Unlock();
        const bool ok = httpd_resp_send_chunk(mReq, mBuf, static_cast<ssize_t>(mLen)) == ESP_OK;
        mGuard.Lock();
        mLen = 0;
        return ok;
    }

private:
    httpd_req_t* mReq;
    sampler::Guard& mGuard;
    char mBuf[1400];
    std::size_t mLen = 0;
};

// Cross-origin access, so a copy of the page served from elsewhere can talk to the board.
void Cors(httpd_req_t* pReq)
{
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Headers", "Authorization, Content-Type");
}

esp_err_t Options(httpd_req_t* pReq)
{
    Cors(pReq);
    return httpd_resp_send(pReq, nullptr, 0);
}

void LoadPassword()
{
    std::size_t n = 0;
    if(!hal::nvs::GetStr(KEY_PASSWORD, sPassword, n) || n == 0) std::strcpy(sPassword, DEFAULT_PASSWORD);
}

// Basic auth against reef:<password>; sends the 401 itself when it fails.
bool Authorised(httpd_req_t* pReq)
{
    char header[160] = {};
    bool ok = false;
    if(httpd_req_get_hdr_value_str(pReq, "Authorization", header, sizeof header) == ESP_OK &&
       std::strncmp(header, "Basic ", 6) == 0)
    {
        unsigned char decoded[96] = {};
        std::size_t n = 0;
        if(mbedtls_base64_decode(decoded, sizeof decoded - 1, &n, reinterpret_cast<unsigned char*>(header + 6),
                                 std::strlen(header + 6)) == 0)
        {
            char expected[64];
            std::snprintf(expected, sizeof expected, "%s:%s", USER, sPassword);
            ok = std::strcmp(reinterpret_cast<char*>(decoded), expected) == 0;
        }
    }
    if(!ok)
    {
        Cors(pReq);
        // No WWW-Authenticate on purpose: browsers would cache a native login for the whole site.
        httpd_resp_set_status(pReq, "401 Unauthorized");
        httpd_resp_send(pReq, "unauthorised", HTTPD_RESP_USE_STRLEN);
    }
    return ok;
}

// Reads the whole body into sBody (NUL-terminated); false and a 413 when it does not fit.
bool ReadBody(httpd_req_t* pReq, std::size_t& pLen)
{
    if(pReq->content_len > BODY_MAX)
    {
        httpd_resp_send_err(pReq, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return false;
    }
    std::size_t got = 0;
    while(got < pReq->content_len)
    {
        const int n = httpd_req_recv(pReq, sBody + got, pReq->content_len - got);
        if(n <= 0)
        {
            httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    sBody[got] = '\0';
    pLen = got;
    return true;
}

uint32_t QueryU32(httpd_req_t* pReq, const char* pKey, uint32_t pDefault)
{
    char query[128] = {};
    char value[24] = {};
    if(httpd_req_get_url_query_str(pReq, query, sizeof query) != ESP_OK) return pDefault;
    if(httpd_query_key_value(query, pKey, value, sizeof value) != ESP_OK) return pDefault;
    return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

bool QueryStr(httpd_req_t* pReq, const char* pKey, std::span<char> pOut)
{
    char query[128] = {};
    if(httpd_req_get_url_query_str(pReq, query, sizeof query) != ESP_OK) return false;
    return httpd_query_key_value(query, pKey, pOut.data(), pOut.size()) == ESP_OK;
}

esp_err_t SendJson(httpd_req_t* pReq, std::size_t pLen)
{
    Cors(pReq);
    httpd_resp_set_type(pReq, "application/json");
    httpd_resp_set_hdr(pReq, "Cache-Control", "no-store");
    return httpd_resp_send(pReq, sJson, static_cast<ssize_t>(pLen));
}

esp_err_t Index(httpd_req_t* pReq)
{
    httpd_resp_set_type(pReq, "text/html; charset=utf-8");
    httpd_resp_set_hdr(pReq, "Cache-Control", "no-cache");
    return httpd_resp_send(pReq, reinterpret_cast<const char*>(sIndexStart), sIndexEnd - sIndexStart);
}

esp_err_t Logo(httpd_req_t* pReq)
{
    httpd_resp_set_type(pReq, "image/svg+xml");
    httpd_resp_set_hdr(pReq, "Cache-Control", "max-age=86400");
    return httpd_resp_send(pReq, reinterpret_cast<const char*>(sLogoStart), sLogoEnd - sLogoStart);
}

// iOS home-screen icon: PNG only, transparency would render black
esp_err_t TouchIcon(httpd_req_t* pReq)
{
    httpd_resp_set_type(pReq, "image/png");
    httpd_resp_set_hdr(pReq, "Cache-Control", "max-age=86400");
    return httpd_resp_send(pReq, reinterpret_cast<const char*>(sTouchStart), sTouchEnd - sTouchStart);
}

esp_err_t GetStatus(httpd_req_t* pReq)
{
    std::size_t n = 0;
    {
        sampler::Guard guard;
        n = reefdo::api::StatusJson(sampler::App(), sampler::ClockNow(), sJson);
    }
    return SendJson(pReq, n);
}

esp_err_t GetService(httpd_req_t* pReq)
{
    std::size_t n = 0;
    {
        sampler::Guard guard;
        n = reefdo::api::ServiceJson(sampler::App(), sJson);
    }
    return SendJson(pReq, n);
}

// Lets the page check credentials before it claims to be signed in.
esp_err_t GetAuth(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":true}");
    return SendJson(pReq, static_cast<std::size_t>(n));
}

esp_err_t GetConfig(httpd_req_t* pReq)
{
    return SendJson(pReq, sampler::ConfigJson(sJson));
}

esp_err_t PutConfig(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_OK;
    reefdo::config::LoadResult r;
    sampler::ApplyConfigJson(std::string_view(sBody, len), r);
    return SendJson(pReq, reefdo::api::ResultJson(r, sJson));
}

esp_err_t PostCmd(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_OK;
    reefdo::api::Command c;
    {
        sampler::Guard guard;
        c = reefdo::api::ApplyCommand(sampler::App(), std::string_view(sBody, len), sampler::ClockNow());
    }
    if(c.setUnix.has_value()) sampler::SetTime(*c.setUnix, c.setTz);
    sampler::ApplyRelays();
    const int n =
        std::snprintf(sJson, sizeof sJson, "{\"ok\":%s,\"message\":\"%s\"}", c.ok ? "true" : "false", c.message);
    return SendJson(pReq, static_cast<std::size_t>(n));
}

esp_err_t GetSeries(httpd_req_t* pReq)
{
    char tier[4] = "A";
    QueryStr(pReq, "tier", tier);
    const uint32_t from = QueryU32(pReq, "from", 0);
    const uint32_t to = QueryU32(pReq, "to", 0xFFFFFFFFu);
    const uint32_t every = QueryU32(pReq, "every", 1);
    httpd_resp_set_type(pReq, "text/csv");
    Cors(pReq);
    httpd_resp_set_hdr(pReq, "Cache-Control", "no-store");
    {
        sampler::Guard guard;
        HttpSink sink(pReq, guard);
        reefdo::api::SeriesCsv(sampler::App(), tier[0] == 'B' ? 'B' : 'A', from, to, every > 0 ? every : 1, sink);
        sink.Flush();
    }
    return httpd_resp_send_chunk(pReq, nullptr, 0);
}

esp_err_t GetEvents(httpd_req_t* pReq)
{
    httpd_resp_set_type(pReq, "text/csv");
    Cors(pReq);
    httpd_resp_set_hdr(pReq, "Cache-Control", "no-store");
    {
        sampler::Guard guard;
        HttpSink sink(pReq, guard);
        reefdo::api::EventsCsv(sampler::App(), QueryU32(pReq, "since", 0), sink);
        sink.Flush();
    }
    return httpd_resp_send_chunk(pReq, nullptr, 0);
}

esp_err_t GetExport(httpd_req_t* pReq)
{
    httpd_resp_set_type(pReq, "text/csv");
    Cors(pReq);
    httpd_resp_set_hdr(pReq, "Content-Disposition", "attachment; filename=\"reefdo.csv\"");
    {
        sampler::Guard guard;
        HttpSink sink(pReq, guard);
        reefdo::api::ExportCsv(sampler::App(), QueryU32(pReq, "since", 0), sink);
        sink.Flush();
    }
    return httpd_resp_send_chunk(pReq, nullptr, 0);
}

esp_err_t GetSys(httpd_req_t* pReq)
{
    const net::Status s = net::GetStatus();
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);
    const char* image = state == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
                        : state == ESP_OTA_IMG_VALID        ? "valid"
                                                            : "undefined";
    const int n = std::snprintf(
        sJson, sizeof sJson,
        "{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d,\"ap\":%s,\"ntp\":%s,\"ntfy_sent\":%lu,"
        "\"ntfy_failed\":%lu,\"partition\":\"%s\",\"image\":\"%s\",\"heap\":%lu,\"heap_min\":%lu,"
        "\"probe\":\"%s\",\"muted\":%s,\"config\":\"%s\"}",
        s.ssid.c_str(), s.connected ? "true" : "false", s.ip.c_str(), s.rssi, s.apActive ? "true" : "false",
        s.timeSynced ? "true" : "false", static_cast<unsigned long>(notify::Sent()),
        static_cast<unsigned long>(notify::Failed()), running->label, image,
        static_cast<unsigned long>(esp_get_free_heap_size()),
        static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
        sampler::GetProbeSource() == sampler::ProbeSource::Sim ? "sim" : "rk500", indicator::Muted() ? "true" : "false",
        sampler::ConfigState());
    return SendJson(pReq, static_cast<std::size_t>(n));
}

// Form-encoded ssid=&pass= (a password may legitimately contain JSON-hostile characters).
esp_err_t PostWifi(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_OK;
    char ssid[33] = {};
    char pass[65] = {};
    httpd_query_key_value(sBody, "ssid", ssid, sizeof ssid);
    httpd_query_key_value(sBody, "pass", pass, sizeof pass);
    const bool ok = net::SetCredentials(ssid, pass);
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":%s}", ok ? "true" : "false");
    return SendJson(pReq, static_cast<std::size_t>(n));
}

esp_err_t PostPassword(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_OK;
    char pw[33] = {};
    httpd_query_key_value(sBody, "password", pw, sizeof pw);
    const bool ok = SetPassword(pw);
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":%s}", ok ? "true" : "false");
    return SendJson(pReq, static_cast<std::size_t>(n));
}

esp_err_t PostNtfyTest(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    reefdo::config::Config cfg;
    {
        sampler::Guard guard;
        cfg = sampler::App().GetConfig();
    }
    const bool ok = notify::SendTest(cfg);
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":%s}", ok ? "true" : "false");
    return SendJson(pReq, static_cast<std::size_t>(n));
}

// Form-encoded pattern=&volume=&hz=&s=: plays the buzzer for a few seconds, mute or not.
esp_err_t PostBuzzerTest(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_OK;
    char pattern[16] = "beep";
    char value[8] = {};
    httpd_query_key_value(sBody, "pattern", pattern, sizeof pattern);
    const uint32_t volume = httpd_query_key_value(sBody, "volume", value, sizeof value) == ESP_OK
                                ? static_cast<uint32_t>(std::atoi(value))
                                : 2;
    const uint32_t hz = httpd_query_key_value(sBody, "hz", value, sizeof value) == ESP_OK
                            ? static_cast<uint32_t>(std::atoi(value))
                            : 2400;
    const uint32_t seconds =
        httpd_query_key_value(sBody, "s", value, sizeof value) == ESP_OK ? static_cast<uint32_t>(std::atoi(value)) : 5;
    reefdo::config::BuzzerPattern p = reefdo::config::BuzzerPattern::Off;
    const bool ok = indicator::ParseBuzzerPattern(pattern, p);
    if(ok) indicator::TestBuzzer(p, volume, hz, seconds);
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":%s}", ok ? "true" : "false");
    return SendJson(pReq, static_cast<std::size_t>(n));
}

void RestartLater(void*)
{
    esp_restart();
}

// Raw firmware image in the body. Maintenance only; the bootloader's rollback guards the result.
esp_err_t PostOta(httpd_req_t* pReq)
{
    if(!Authorised(pReq)) return ESP_OK;
    {
        sampler::Guard guard;
        if(!sampler::App().GetStatus().maintenance)
        {
            httpd_resp_send_err(pReq, HTTPD_400_BAD_REQUEST, "maintenance mode required");
            return ESP_OK;
        }
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if(target == nullptr || pReq->content_len == 0 || pReq->content_len > target->size)
    {
        httpd_resp_send_err(pReq, HTTPD_400_BAD_REQUEST, "no OTA partition or bad size");
        return ESP_OK;
    }
    esp_ota_handle_t ota = 0;
    if(esp_ota_begin(target, pReq->content_len, &ota) != ESP_OK)
    {
        httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }
    std::size_t got = 0;
    while(got < pReq->content_len)
    {
        const int n = httpd_req_recv(pReq, sBody, BODY_MAX);
        if(n <= 0 || esp_ota_write(ota, sBody, static_cast<std::size_t>(n)) != ESP_OK)
        {
            esp_ota_abort(ota);
            httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_OK;
        }
        got += static_cast<std::size_t>(n);
    }
    if(esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK)
    {
        httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OTA image written to %s (%u bytes): rebooting", target->label, static_cast<unsigned>(got));
    const int n = std::snprintf(sJson, sizeof sJson, "{\"ok\":true,\"partition\":\"%s\"}", target->label);
    SendJson(pReq, static_cast<std::size_t>(n));
    esp_timer_create_args_t args = {};
    args.callback = &RestartLater;
    args.name = "ota-restart";
    esp_timer_handle_t timer = nullptr;
    esp_timer_create(&args, &timer);
    esp_timer_start_once(timer, 1500000);
    return ESP_OK;
}

void Add(const char* pUri, httpd_method_t pMethod, esp_err_t (*pHandler)(httpd_req_t*))
{
    httpd_uri_t u = {};
    u.uri = pUri;
    u.method = pMethod;
    u.handler = pHandler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(sServer, &u));
}

} // namespace

void Start()
{
    LoadPassword();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 24;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&sServer, &cfg));
    Add("/", HTTP_GET, Index);
    Add("/logo.svg", HTTP_GET, Logo);
    Add("/apple-touch-icon.png", HTTP_GET, TouchIcon);
    Add("/api/status", HTTP_GET, GetStatus);
    Add("/api/service", HTTP_GET, GetService);
    Add("/api/auth", HTTP_GET, GetAuth);
    Add("/api/config", HTTP_GET, GetConfig);
    Add("/api/config", HTTP_PUT, PutConfig);
    Add("/api/cmd", HTTP_POST, PostCmd);
    Add("/api/series", HTTP_GET, GetSeries);
    Add("/api/events", HTTP_GET, GetEvents);
    Add("/api/export.csv", HTTP_GET, GetExport);
    Add("/api/sys", HTTP_GET, GetSys);
    Add("/api/wifi", HTTP_POST, PostWifi);
    Add("/api/passwd", HTTP_POST, PostPassword);
    Add("/api/ntfy-test", HTTP_POST, PostNtfyTest);
    Add("/api/buzzer-test", HTTP_POST, PostBuzzerTest);
    Add("/api/ota", HTTP_POST, PostOta);
    Add("/api/*", HTTP_OPTIONS, Options);
    ESP_LOGI(TAG, "http://reefdo.local/ up");
}

bool SetPassword(std::string_view pPassword)
{
    if(pPassword.size() > 32) return false;
    const bool ok = pPassword.empty() ? hal::nvs::EraseKey(KEY_PASSWORD) : hal::nvs::SetStr(KEY_PASSWORD, pPassword);
    if(ok) LoadPassword();
    return ok;
}

} // namespace web
