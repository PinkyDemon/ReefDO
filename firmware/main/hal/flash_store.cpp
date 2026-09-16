#include "hal/flash_store.hpp"

#include "esp_log.h"

namespace hal
{

namespace
{
const char* const TAG = "flash";
}

bool FlashStore::Open(const char* pLabel, std::size_t pSegmentBytes)
{
    mPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, pLabel);
    if(mPart == nullptr || pSegmentBytes == 0 || pSegmentBytes % 4096 != 0)
    {
        ESP_LOGE(TAG, "partition %s: not found or bad pSegment size", pLabel);
        mPart = nullptr;
        return false;
    }
    mSegmentBytes = pSegmentBytes;
    mSegments = mPart->size / pSegmentBytes;
    if(mSegments > reefdo::log::MAX_SEGMENTS) mSegments = reefdo::log::MAX_SEGMENTS;
    if(mSegments == 0)
    {
        ESP_LOGE(TAG, "partition %s: smaller than one segment", pLabel);
        mPart = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "%s: %u segments of %u KB", pLabel, static_cast<unsigned>(mSegments),
             static_cast<unsigned>(pSegmentBytes / 1024));
    return true;
}

bool FlashStore::InRange(std::size_t pSegment, std::size_t pOffset, std::size_t pLen) const
{
    return mPart != nullptr && pSegment < mSegments && pOffset + pLen <= mSegmentBytes;
}

bool FlashStore::Read(std::size_t pSegment, std::size_t pOffset, std::span<uint8_t> out)
{
    if(!InRange(pSegment, pOffset, out.size())) return false;
    const esp_err_t err = esp_partition_read(mPart, pSegment * mSegmentBytes + pOffset, out.data(), out.size());
    if(err != ESP_OK) ++mErrors;
    return err == ESP_OK;
}

bool FlashStore::Write(std::size_t pSegment, std::size_t pOffset, std::span<const uint8_t> data)
{
    if(!InRange(pSegment, pOffset, data.size())) return false;
    const esp_err_t err = esp_partition_write(mPart, pSegment * mSegmentBytes + pOffset, data.data(), data.size());
    if(err != ESP_OK)
    {
        ++mErrors;
        ESP_LOGE(TAG, "%s: write failed: %s", mPart->label, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool FlashStore::Erase(std::size_t pSegment)
{
    if(!InRange(pSegment, 0, mSegmentBytes)) return false;
    const esp_err_t err = esp_partition_erase_range(mPart, pSegment * mSegmentBytes, mSegmentBytes);
    if(err != ESP_OK)
    {
        ++mErrors;
        ESP_LOGE(TAG, "%s: erase failed: %s", mPart->label, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

} // namespace hal
