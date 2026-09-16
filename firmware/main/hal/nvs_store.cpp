#include "hal/nvs_store.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace hal::nvs
{

namespace
{

const char* const TAG = "nvs";
const char* const NAMESPACE = "reefdo";

// Opens the namespace for one operation; commits on close when writing.
class Handle
{
public:
    explicit Handle(nvs_open_mode_t pMode) { mOk = nvs_open(NAMESPACE, pMode, &mH) == ESP_OK; }
    ~Handle()
    {
        if(mOk) nvs_close(mH);
    }
    bool Ok() const { return mOk; }
    nvs_handle_t Get() const { return mH; }
    bool Commit() { return nvs_commit(mH) == ESP_OK; }

private:
    nvs_handle_t mH = 0;
    bool mOk = false;
};

} // namespace

bool Init()
{
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS unusable (%s): erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool GetStr(const char* pKey, std::span<char> pOut, std::size_t& pLen)
{
    Handle h(NVS_READONLY);
    if(!h.Ok()) return false;
    std::size_t n = pOut.size();
    if(nvs_get_str(h.Get(), pKey, pOut.data(), &n) != ESP_OK) return false;
    pLen = n > 0 ? n - 1 : 0;
    return true;
}

bool SetStr(const char* pKey, std::string_view pValue)
{
    char buf[256]; // credentials, topics, names; long documents go through set_blob
    if(pValue.size() >= sizeof buf) return false;
    pValue.copy(buf, pValue.size());
    buf[pValue.size()] = '\0';
    Handle h(NVS_READWRITE);
    return h.Ok() && nvs_set_str(h.Get(), pKey, buf) == ESP_OK && h.Commit();
}

bool GetBlob(const char* pKey, std::span<uint8_t> pOut, std::size_t& pLen)
{
    Handle h(NVS_READONLY);
    if(!h.Ok()) return false;
    std::size_t n = pOut.size();
    if(nvs_get_blob(h.Get(), pKey, pOut.data(), &n) != ESP_OK) return false;
    pLen = n;
    return true;
}

bool SetBlob(const char* pKey, std::span<const uint8_t> pValue)
{
    Handle h(NVS_READWRITE);
    return h.Ok() && nvs_set_blob(h.Get(), pKey, pValue.data(), pValue.size()) == ESP_OK && h.Commit();
}

bool GetI32(const char* pKey, int32_t& pOut)
{
    Handle h(NVS_READONLY);
    return h.Ok() && nvs_get_i32(h.Get(), pKey, &pOut) == ESP_OK;
}

bool SetI32(const char* pKey, int32_t pValue)
{
    Handle h(NVS_READWRITE);
    return h.Ok() && nvs_set_i32(h.Get(), pKey, pValue) == ESP_OK && h.Commit();
}

bool EraseKey(const char* pKey)
{
    Handle h(NVS_READWRITE);
    if(!h.Ok()) return false;
    const esp_err_t err = nvs_erase_key(h.Get(), pKey);
    return (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && h.Commit();
}

bool EraseAll()
{
    Handle h(NVS_READWRITE);
    return h.Ok() && nvs_erase_all(h.Get()) == ESP_OK && h.Commit();
}

} // namespace hal::nvs
