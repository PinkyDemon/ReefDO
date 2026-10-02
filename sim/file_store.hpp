#pragma once
// A log partition as a file, with NOR semantics like the board's: a write only clears bits, an erase sets a
// segment back to 0xFF. The whole partition lives in memory; every change is written through, so the history
// survives a restart of the simulator.
#include <cstdio>
#include <string>
#include <vector>

#include "reefdo/log.hpp"

namespace sim
{

class FileStore final : public reefdo::log::IBlockStore
{
public:
    FileStore(std::string pPath, std::size_t pSegments, std::size_t pSegmentBytes);
    ~FileStore() override;
    FileStore(const FileStore&) = delete;
    FileStore& operator=(const FileStore&) = delete;

    std::size_t Segments() const override { return mSegments; }
    std::size_t SegmentBytes() const override { return mSegmentBytes; }
    bool Read(std::size_t pSegment, std::size_t pOffset, std::span<uint8_t> pOut) override;
    bool Write(std::size_t pSegment, std::size_t pOffset, std::span<const uint8_t> pData) override;
    bool Erase(std::size_t pSegment) override;

    bool Ok() const { return mFile != nullptr; }

private:
    bool InRange(std::size_t pSegment, std::size_t pOffset, std::size_t pLen) const;
    void Persist(std::size_t pAt, std::size_t pLen);

    std::string mPath;
    std::size_t mSegments;
    std::size_t mSegmentBytes;
    std::vector<uint8_t> mBytes;
    std::FILE* mFile = nullptr;
};

} // namespace sim
