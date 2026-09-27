#include "tuya_link.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"
#include "sampler.hpp"

namespace tuya_link
{

namespace
{

using reefdo::tuya::Error;
using reefdo::tuya::KEY_LEN;

const char* const TAG = "tuya";
constexpr int64_t REFRESH_MS = 60'000; // re-assert a plug's state: it may have been switched by hand or rebooted
constexpr int64_t RETRY_MS = 5'000;    // after a failed transaction
constexpr uint32_t CONNECT_TIMEOUT_MS = 3'000;
constexpr uint32_t ANSWER_TIMEOUT_MS = 3'000;

// mbedTLS 4 (ESP-IDF 6): PSA Crypto only. Keys are imported per operation — a plug talks once a minute.
class PsaCrypto : public reefdo::tuya::ICrypto
{
public:
    bool EcbEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                    std::span<uint8_t> pOut) override
    {
        return Ecb(true, pKey, pIn, pOut);
    }
    bool EcbDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                    std::span<uint8_t> pOut) override
    {
        return Ecb(false, pKey, pIn, pOut);
    }
    bool GcmEncrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                    std::span<const uint8_t> pAad, std::span<const uint8_t> pIn, std::span<uint8_t> pOut,
                    std::span<uint8_t, 16> pTag) override
    {
        mbedtls_svc_key_id_t id;
        if(!Import(PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_ENCRYPT, pKey, id)) return false;
        std::size_t n = 0;
        const psa_status_t st = psa_aead_encrypt(id, PSA_ALG_GCM, pIv.data(), pIv.size(), pAad.data(), pAad.size(),
                                                 pIn.data(), pIn.size(), mTmp, sizeof mTmp, &n);
        psa_destroy_key(id);
        if(st != PSA_SUCCESS || n != pIn.size() + 16) return false;
        std::memcpy(pOut.data(), mTmp, pIn.size()); // PSA writes ciphertext || tag
        std::memcpy(pTag.data(), mTmp + pIn.size(), 16);
        return true;
    }
    bool GcmDecrypt(std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t, 12> pIv,
                    std::span<const uint8_t> pAad, std::span<const uint8_t> pIn, std::span<const uint8_t, 16> pTag,
                    std::span<uint8_t> pOut) override
    {
        if(pIn.size() + 16 > sizeof mTmp) return false;
        std::memcpy(mTmp, pIn.data(), pIn.size());
        std::memcpy(mTmp + pIn.size(), pTag.data(), 16);
        mbedtls_svc_key_id_t id;
        if(!Import(PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_DECRYPT, pKey, id)) return false;
        std::size_t n = 0;
        const psa_status_t st = psa_aead_decrypt(id, PSA_ALG_GCM, pIv.data(), pIv.size(), pAad.data(), pAad.size(),
                                                 mTmp, pIn.size() + 16, pOut.data(), pOut.size(), &n);
        psa_destroy_key(id);
        return st == PSA_SUCCESS && n == pIn.size();
    }
    void HmacSha256(std::span<const uint8_t> pKey, std::span<const uint8_t> pData, std::span<uint8_t, 32> pOut) override
    {
        std::memset(pOut.data(), 0, pOut.size()); // on failure: a MAC that matches nothing
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&a, pKey.size() * 8);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        mbedtls_svc_key_id_t id;
        if(psa_import_key(&a, pKey.data(), pKey.size(), &id) != PSA_SUCCESS) return;
        std::size_t n = 0;
        psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), pData.data(), pData.size(), pOut.data(), pOut.size(), &n);
        psa_destroy_key(id);
    }
    void Random(std::span<uint8_t> pOut) override { esp_fill_random(pOut.data(), pOut.size()); }

private:
    static bool Import(psa_key_type_t pType, psa_algorithm_t pAlg, psa_key_usage_t pUsage,
                       std::span<const uint8_t, KEY_LEN> pKey, mbedtls_svc_key_id_t& pId)
    {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, pType);
        psa_set_key_bits(&a, KEY_LEN * 8);
        psa_set_key_usage_flags(&a, pUsage);
        psa_set_key_algorithm(&a, pAlg);
        return psa_import_key(&a, pKey.data(), pKey.size(), &pId) == PSA_SUCCESS;
    }
    static bool Ecb(bool pEncrypt, std::span<const uint8_t, KEY_LEN> pKey, std::span<const uint8_t> pIn,
                    std::span<uint8_t> pOut)
    {
        mbedtls_svc_key_id_t id;
        if(!Import(PSA_KEY_TYPE_AES, PSA_ALG_ECB_NO_PADDING, pEncrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT,
                   pKey, id))
            return false;
        std::size_t n = 0;
        const psa_status_t st =
            pEncrypt
                ? psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, pIn.data(), pIn.size(), pOut.data(), pOut.size(), &n)
                : psa_cipher_decrypt(id, PSA_ALG_ECB_NO_PADDING, pIn.data(), pIn.size(), pOut.data(), pOut.size(), &n);
        psa_destroy_key(id);
        return st == PSA_SUCCESS && n == pIn.size();
    }

    uint8_t mTmp[reefdo::tuya::FRAME_MAX + 16]{};
};

// One TCP connection per transaction: plugs accept few connections, and a fresh one never goes stale.
class Tcp : public reefdo::tuya::IConnection
{
public:
    ~Tcp()
    {
        if(mFd >= 0) close(mFd);
    }
    bool Open(const char* pIp)
    {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(reefdo::tuya::PORT);
        if(inet_pton(AF_INET, pIp, &a.sin_addr) != 1) return false;
        mFd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(mFd < 0) return false;
        const int flags = fcntl(mFd, F_GETFL, 0);
        fcntl(mFd, F_SETFL, flags | O_NONBLOCK); // connect with our own timeout, not lwIP's minutes
        if(connect(mFd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        {
            if(errno != EINPROGRESS) return false;
            fd_set w;
            FD_ZERO(&w);
            FD_SET(mFd, &w);
            timeval tv = {static_cast<time_t>(CONNECT_TIMEOUT_MS / 1000), 0};
            if(select(mFd + 1, nullptr, &w, nullptr, &tv) <= 0) return false;
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(mFd, SOL_SOCKET, SO_ERROR, &err, &len);
            if(err != 0) return false;
        }
        fcntl(mFd, F_SETFL, flags);
        timeval stv = {3, 0};
        setsockopt(mFd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
        return true;
    }
    bool Send(std::span<const uint8_t> pData) override
    {
        std::size_t off = 0;
        while(off < pData.size())
        {
            const int n = send(mFd, pData.data() + off, pData.size() - off, 0);
            if(n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }
    std::size_t Receive(std::span<uint8_t> pOut, uint32_t pTimeoutMs) override
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(mFd, &r);
        timeval tv = {static_cast<time_t>(pTimeoutMs / 1000), static_cast<suseconds_t>((pTimeoutMs % 1000) * 1000)};
        if(select(mFd + 1, &r, nullptr, nullptr, &tv) <= 0) return 0;
        const int n = recv(mFd, pOut.data(), pOut.size(), 0);
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

private:
    int mFd = -1;
};

// One transaction; runs on the plug task only (16 KB stack: the codec's frame buffers live there).
reefdo::tuya::Result Transact(const reefdo::tuya::Settings& pS, bool pOn)
{
    static PsaCrypto sCrypto; // the plug task is its only user
    Tcp conn;
    if(!conn.Open(pS.ip.c_str())) return {Error::Connect, std::nullopt};
    const uint32_t unixS = sampler::ClockNow().unixS.value_or(0);
    return reefdo::tuya::Switch(conn, sCrypto, pS, pOn, unixS, ANSWER_TIMEOUT_MS);
}

// A console bench test, handed to the plug task. Static: a caller that gave up must not leave a dangling target.
struct TestRequest
{
    reefdo::tuya::Settings settings;
    bool on = false;
    reefdo::tuya::Result result;
};
TestRequest sTest;
SemaphoreHandle_t sTestPending = nullptr;
SemaphoreHandle_t sTestDone = nullptr;

struct Track
{
    reefdo::slot::AnySlot slot;
    std::optional<bool> lastWant; // what we last tried to set
    bool lastOk = false;
    int64_t lastMs = 0;
};

void Task(void*)
{
    std::array<Track, reefdo::DEVICES> track{};
    for(;;)
    {
        if(xSemaphoreTake(sTestPending, 0) == pdTRUE)
        {
            sTest.result = Transact(sTest.settings, sTest.on);
            xSemaphoreGive(sTestDone);
        }
        std::array<reefdo::slot::AnySlot, reefdo::DEVICES> s;
        std::array<bool, reefdo::DEVICES> want{};
        {
            sampler::Guard guard;
            for(std::size_t i = 0; i < reefdo::DEVICES; ++i)
            {
                s[i] = sampler::App().GetConfig().devices[i].slot;
                want[i] = sampler::App().GetStatus().deviceOn[i];
            }
        }
        for(std::size_t i = 0; i < reefdo::DEVICES; ++i)
        {
            Track& t = track[i];
            const reefdo::slot::Tuya* plug = s[i].AsTuya();
            if(plug == nullptr) continue;
            if(!(s[i] == t.slot)) // new plug or new settings: start over
            {
                t = Track{};
                t.slot = s[i];
            }
            const int64_t now = esp_timer_get_time() / 1000;
            const bool due = t.lastWant != want[i] || now - t.lastMs >= (t.lastOk ? REFRESH_MS : RETRY_MS);
            if(!due) continue;
            const reefdo::tuya::Result r = Transact(*plug, want[i]);
            t.lastWant = want[i];
            t.lastOk = r.Ok();
            t.lastMs = now;
            if(!r.Ok())
                ESP_LOGW(TAG, "device %u at %s: %s", static_cast<unsigned>(i + 1), plug->ip.c_str(),
                         ErrorName(r.error));
            sampler::Guard guard;
            sampler::App().ReportTuya(i, r.error, sampler::ClockNow());
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

} // namespace

void Start()
{
    if(psa_crypto_init() != PSA_SUCCESS) ESP_LOGE(TAG, "PSA crypto init failed: plugs will not be reached");
    sTestPending = xSemaphoreCreateBinary();
    sTestDone = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(Task, "tuya", 16384, nullptr, 3, nullptr, 0);
}

reefdo::tuya::Result Test(const reefdo::tuya::Settings& pS, bool pOn)
{
    if(sTestPending == nullptr) return {Error::Connect, std::nullopt};
    xSemaphoreTake(sTestDone, 0); // a stale answer from a test that timed out
    sTest.settings = pS;
    sTest.on = pOn;
    xSemaphoreGive(sTestPending);
    if(xSemaphoreTake(sTestDone, pdMS_TO_TICKS(12'000)) != pdTRUE) return {Error::Timeout, std::nullopt};
    return sTest.result;
}

const char* ErrorName(Error pE)
{
    static const char* const NAMES[] = {"ok",
                                        "no connection (address? plug powered? Wi-Fi?)",
                                        "send failed",
                                        "no answer (wrong local key or version?)",
                                        "unreadable answer (wrong version?)",
                                        "authentication failed (wrong local key)",
                                        "refused by the plug",
                                        "crypto failure"};
    return NAMES[static_cast<int>(pE)];
}

} // namespace tuya_link
