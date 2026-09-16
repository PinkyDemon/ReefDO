#pragma once
// In-memory IBlockStore that behaves like NOR flash: writes only clear bits (dst &= src), erase sets 0xFF.
// Power-cut simulation: `torn` truncates the next write to that many bytes; `torn_erase` stops the next erase
// after that many bytes (so the segment header survives over a half-erased tail — the nasty case).
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "reefdo/log.hpp"

struct FakeStore : reefdo::log::IBlockStore
{
    std::vector<std::vector<uint8_t>> segs;
    std::size_t bytes;
    std::optional<std::size_t> torn;
    std::optional<std::size_t> tornErase;
    bool failErase = false;
    bool failWrite = false;
    bool failRead = false;
    std::optional<int> failReadAfter; // reads succeed this many more times, then fail
    int erases = 0;

    FakeStore(std::size_t pN, std::size_t pSegBytes)
        : segs(pN, std::vector<uint8_t>(pSegBytes, 0xFF))
        , bytes(pSegBytes)
    {
    }
    std::size_t Segments() const override { return segs.size(); }
    std::size_t SegmentBytes() const override { return bytes; }
    bool Read(std::size_t pS, std::size_t pOff, std::span<uint8_t> pOut) override
    {
        if(failReadAfter.has_value() && (*failReadAfter)-- <= 0) return false;
        if(failRead || pS >= segs.size() || pOff + pOut.size() > bytes) return false;
        std::memcpy(pOut.data(), segs[pS].data() + pOff, pOut.size());
        return true;
    }
    bool Write(std::size_t pS, std::size_t pOff, std::span<const uint8_t> pData) override
    {
        if(failWrite || pS >= segs.size() || pOff + pData.size() > bytes) return false;
        std::size_t n = pData.size();
        if(torn.has_value())
        {
            n = *torn < n ? *torn : n;
            torn.reset();
        }
        for(std::size_t i = 0; i < n; ++i)
            segs[pS][pOff + i] &= pData[i];
        return n == pData.size();
    }
    bool Erase(std::size_t pS) override
    {
        if(failErase || pS >= segs.size()) return false;
        ++erases;
        std::size_t from = 0; // the erase runs from the end towards the header, and stops early when torn
        if(tornErase.has_value())
        {
            from = *tornErase < bytes ? bytes - *tornErase : 0;
            tornErase.reset();
        }
        std::fill(segs[pS].begin() + static_cast<std::ptrdiff_t>(from), segs[pS].end(), 0xFF);
        return from == 0;
    }
};
