#include "file_store.hpp"

#include <cstring>

namespace sim
{

FileStore::FileStore(std::string pPath, std::size_t pSegments, std::size_t pSegmentBytes)
    : mPath(std::move(pPath))
    , mSegments(pSegments)
    , mSegmentBytes(pSegmentBytes)
    , mBytes(pSegments * pSegmentBytes, 0xFF)
{
    mFile = std::fopen(mPath.c_str(), "r+b");
    if(mFile != nullptr)
    {
        const std::size_t got = std::fread(mBytes.data(), 1, mBytes.size(), mFile);
        if(got == mBytes.size()) return;
        std::fclose(mFile); // another geometry: start over
    }
    mFile = std::fopen(mPath.c_str(), "w+b");
    if(mFile != nullptr) Persist(0, mBytes.size());
}

FileStore::~FileStore()
{
    if(mFile != nullptr) std::fclose(mFile);
}

bool FileStore::InRange(std::size_t pSegment, std::size_t pOffset, std::size_t pLen) const
{
    return pSegment < mSegments && pOffset <= mSegmentBytes && pLen <= mSegmentBytes - pOffset;
}

void FileStore::Persist(std::size_t pAt, std::size_t pLen)
{
    if(mFile == nullptr) return;
    std::fseek(mFile, static_cast<long>(pAt), SEEK_SET);
    std::fwrite(mBytes.data() + pAt, 1, pLen, mFile);
    std::fflush(mFile);
}

bool FileStore::Read(std::size_t pSegment, std::size_t pOffset, std::span<uint8_t> pOut)
{
    if(!InRange(pSegment, pOffset, pOut.size())) return false;
    std::memcpy(pOut.data(), mBytes.data() + pSegment * mSegmentBytes + pOffset, pOut.size());
    return true;
}

bool FileStore::Write(std::size_t pSegment, std::size_t pOffset, std::span<const uint8_t> pData)
{
    if(!InRange(pSegment, pOffset, pData.size())) return false;
    const std::size_t at = pSegment * mSegmentBytes + pOffset;
    for(std::size_t i = 0; i < pData.size(); ++i)
        mBytes[at + i] &= pData[i]; // NOR: bits only go 1 → 0
    Persist(at, pData.size());
    return true;
}

bool FileStore::Erase(std::size_t pSegment)
{
    if(pSegment >= mSegments) return false;
    std::memset(mBytes.data() + pSegment * mSegmentBytes, 0xFF, mSegmentBytes);
    Persist(pSegment * mSegmentBytes, mSegmentBytes);
    return true;
}

} // namespace sim
