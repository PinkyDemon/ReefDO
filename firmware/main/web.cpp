#include "web.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "reefdo/gateway/router.hpp"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "hal/nvs_store.hpp"
#include "platform.hpp"
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

namespace gw = reefdo::gateway;

const char* const TAG = "web";
const char* const KEY_PASSWORD = "pw";
const char* const DEFAULT_PASSWORD = "reefdo";
constexpr std::size_t BODY_MAX = reefdo::config::DOC_MAX; // a whole config document

httpd_handle_t sServer = nullptr;
char sPassword[33] = {};
portMUX_TYPE sPasswordLock = portMUX_INITIALIZER_UNLOCKED; // written by the console's passwd, read per request
char sBody[BODY_MAX + 1];                                  // request bodies (the httpd task only)
char sJson[gw::JSON_MAX];                                  // the router's scratch
std::atomic<bool> sBusy{false};                            // a handler is running: the board's own check waits

// A request in flight, for the self-check in net.cpp: a server busy with a long download is not a dead one.
class BusyScope
{
public:
    BusyScope() { sBusy = true; }
    ~BusyScope() { sBusy = false; }
};

// Cross-origin access, so a copy of the page served from elsewhere can talk to the board.
void Cors(httpd_req_t* pReq)
{
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
    httpd_resp_set_hdr(pReq, "Access-Control-Allow-Headers", "Authorization, Content-Type");
}

// Chunked HTTP for every router response. Sends happen with the App unlocked, so a slow client never stalls
// the sampler; a CSV ring may rotate meanwhile, which can repeat or skip a few rows at a segment boundary.
class HttpResponse final : public gw::IResponse
{
public:
    HttpResponse(httpd_req_t* pReq, sampler::Guard& pGuard)
        : mReq(pReq)
        , mGuard(pGuard)
    {
    }
    void Begin(gw::Code pCode, gw::ContentType pType, const char* pFilename) override
    {
        httpd_resp_set_status(mReq, pCode == gw::Code::Ok             ? "200 OK"
                                    : pCode == gw::Code::BadRequest   ? "400 Bad Request"
                                    : pCode == gw::Code::Unauthorised ? "401 Unauthorized"
                                    : pCode == gw::Code::Forbidden    ? "403 Forbidden"
                                                                      : "404 Not Found");
        httpd_resp_set_type(mReq, pType == gw::ContentType::Json  ? "application/json"
                                  : pType == gw::ContentType::Csv ? "text/csv"
                                                                  : "text/plain");
        Cors(mReq);
        httpd_resp_set_hdr(mReq, "Cache-Control", "no-store");
        if(pFilename != nullptr)
        {
            std::snprintf(mDisposition, sizeof mDisposition, "attachment; filename=\"%s\"", pFilename);
            httpd_resp_set_hdr(mReq, "Content-Disposition", mDisposition);
        }
    }
    bool Write(std::string_view pChunk) override
    {
        while(!pChunk.empty())
        {
            if(mLen == sizeof mBuf && !Flush()) return false;
            const std::size_t n = std::min(pChunk.size(), sizeof mBuf - mLen);
            std::memcpy(mBuf + mLen, pChunk.data(), n);
            mLen += n;
            pChunk.remove_prefix(n);
        }
        return mOk;
    }
    esp_err_t Finish()
    {
        Flush();
        mGuard.Unlock(); // the terminator too: a stalled client must never hold up the sampler
        const esp_err_t err = httpd_resp_send_chunk(mReq, nullptr, 0);
        mGuard.Lock();
        return err;
    }

private:
    bool Flush()
    {
        if(mLen == 0 || !mOk) return mOk;
        mGuard.Unlock();
        mOk = httpd_resp_send_chunk(mReq, mBuf, static_cast<ssize_t>(mLen)) == ESP_OK;
        mGuard.Lock();
        mLen = 0;
        return mOk;
    }

    httpd_req_t* mReq;
    sampler::Guard& mGuard;
    char mBuf[1400];
    std::size_t mLen = 0;
    bool mOk = true;
    char mDisposition[64] = {};
};

esp_err_t Options(httpd_req_t* pReq)
{
    Cors(pReq);
    return httpd_resp_send(pReq, nullptr, 0);
}

void LoadPassword()
{
    char pw[sizeof sPassword] = {};
    std::size_t n = 0;
    if(!hal::nvs::GetStr(KEY_PASSWORD, pw, n) || n == 0) std::strcpy(pw, DEFAULT_PASSWORD);
    taskENTER_CRITICAL(&sPasswordLock);
    std::memcpy(sPassword, pw, sizeof sPassword);
    taskEXIT_CRITICAL(&sPasswordLock);
}

gw::Who Caller(httpd_req_t* pReq)
{
    char header[160] = {};
    if(httpd_req_get_hdr_value_str(pReq, "Authorization", header, sizeof header) != ESP_OK) return gw::Who::Anonymous;
    char pw[sizeof sPassword];
    taskENTER_CRITICAL(&sPasswordLock);
    std::memcpy(pw, sPassword, sizeof pw);
    taskEXIT_CRITICAL(&sPasswordLock);
    return gw::Authorise(header, pw);
}

// Reads the whole body into sBody (NUL-terminated); false and a 413 when it does not fit. The caller then returns
// ESP_FAIL: the server closes the socket instead of draining the rest of a large body 32 bytes at a time.
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

// Every /api/* route but the OTA upload: the gateway decides.
esp_err_t Api(httpd_req_t* pReq)
{
    const BusyScope busy;
    std::size_t len = 0;
    if(!ReadBody(pReq, len)) return ESP_FAIL;
    const std::string_view uri(pReq->uri); // the server's copy, alive for the whole request
    const std::size_t q = uri.find('?');
    gw::Request r;
    r.method = pReq->method == HTTP_PUT    ? gw::Method::Put
               : pReq->method == HTTP_POST ? gw::Method::Post
                                           : gw::Method::Get;
    r.path = uri.substr(0, q);
    r.query = q == std::string_view::npos ? std::string_view() : uri.substr(q + 1);
    r.body = std::string_view(sBody, len);
    r.who = Caller(pReq);
    sampler::Guard guard;
    HttpResponse out(pReq, guard);
    gw::Handle(sampler::App(), sampler::ClockNow(), platform::Get(), r, out, sJson);
    return out.Finish();
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

esp_err_t SendJson(httpd_req_t* pReq, int pLen)
{
    Cors(pReq);
    httpd_resp_set_type(pReq, "application/json");
    httpd_resp_set_hdr(pReq, "Cache-Control", "no-store");
    return httpd_resp_send(pReq, sJson, pLen);
}

// Raw firmware image in the body, streamed to flash: no router, it cannot hold a 1 MB body. Maintenance only;
// the bootloader's rollback guards the result.
esp_err_t PostOta(httpd_req_t* pReq)
{
    const BusyScope busy;
    // A refusal returns ESP_FAIL: the socket closes instead of swallowing the megabytes of an image nobody wanted.
    if(Caller(pReq) != gw::Who::User)
    {
        Cors(pReq);
        // No WWW-Authenticate on purpose: browsers would cache a native login for the whole site.
        httpd_resp_set_status(pReq, "401 Unauthorized");
        httpd_resp_send(pReq, "unauthorised", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    {
        sampler::Guard guard;
        if(!sampler::App().GetStatus().maintenance)
        {
            httpd_resp_send_err(pReq, HTTPD_400_BAD_REQUEST, "maintenance mode required");
            return ESP_FAIL;
        }
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if(target == nullptr || pReq->content_len == 0 || pReq->content_len > target->size)
    {
        httpd_resp_send_err(pReq, HTTPD_400_BAD_REQUEST, "no OTA partition or bad size");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota = 0;
    if(esp_ota_begin(target, pReq->content_len, &ota) != ESP_OK)
    {
        httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    std::size_t got = 0;
    while(got < pReq->content_len)
    {
        const int n = httpd_req_recv(pReq, sBody, BODY_MAX);
        if(n <= 0 || esp_ota_write(ota, sBody, static_cast<std::size_t>(n)) != ESP_OK)
        {
            esp_ota_abort(ota);
            httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_FAIL;
        }
        got += static_cast<std::size_t>(n);
    }
    if(esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK)
    {
        httpd_resp_send_err(pReq, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OTA image written to %s (%u bytes): rebooting", target->label, static_cast<unsigned>(got));
    SendJson(pReq, std::snprintf(sJson, sizeof sJson, "{\"ok\":true,\"partition\":\"%s\"}", target->label));
    sampler::Restart(reefdo::app::RestartWhy::Update); // logged, a second for the answer to leave, then the reboot
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
    cfg.max_uri_handlers = 12;
    // lwIP has CONFIG_LWIP_MAX_SOCKETS (16): the server's 3 own plus these 6 clients, the phone app's MQTT link, a
    // plug and the board's own check stay under it. Without the budget a few half-open phone sessions exhaust the
    // sockets, accept() fails for everyone and the page is dead while everything else runs; the keep-alive probes
    // reap a client that left without closing.
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&sServer, &cfg));
    Add("/", HTTP_GET, Index);
    Add("/logo.svg", HTTP_GET, Logo);
    Add("/apple-touch-icon.png", HTTP_GET, TouchIcon);
    Add("/api/ota", HTTP_POST, PostOta); // before the wildcard: the first match wins
    Add("/api/*", HTTP_GET, Api);
    Add("/api/*", HTTP_PUT, Api);
    Add("/api/*", HTTP_POST, Api);
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

bool Busy()
{
    return sBusy;
}

} // namespace web
