#pragma once
// reefdo::log::IBlockStore over a raw pData partition: a pSegment is one erase block, writes program in place.
#include <cstddef>
#include <cstdint>
#include <span>

#include "reefdo/log.hpp"

#include "esp_partition.h"

namespace hal
{

class FlashStore : public reefdo::log::IBlockStore
{
public:
    // pSegmentBytes: a multiple of 4096. False if the partition is missing or too small.
    bool Open(const char* pLabel, std::size_t pSegmentBytes);

    std::size_t Segments() const override { return mSegments; }
    std::size_t SegmentBytes() const override { return mSegmentBytes; }
    bool Read(std::size_t pSegment, std::size_t pOffset, std::span<uint8_t> pOut) override;
    bool Write(std::size_t pSegment, std::size_t pOffset, std::span<const uint8_t> pData) override;
    bool Erase(std::size_t pSegment) override;

    const char* Label() const { return mPart ? mPart->label : "?"; }
    uint32_t Errors() const { return mErrors; } // failed flash operations since boot

private:
    bool InRange(std::size_t pSegment, std::size_t pOffset, std::size_t pLen) const;

    const esp_partition_t* mPart = nullptr;
    std::size_t mSegmentBytes = 0;
    std::size_t mSegments = 0;
    uint32_t mErrors = 0;
};

} // namespace hal
