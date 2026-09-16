#pragma once
// Typed access to the "reefdo" NVS namespace. Every getter returns false when the pKey is absent.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace hal::nvs
{

bool Init(); // nvs_flash_init; a full-or-outdated partition is erased and re-initialised

bool GetStr(const char* pKey, std::span<char> pOut, std::size_t& pLen); // pLen excludes the terminator
bool SetStr(const char* pKey, std::string_view pValue);
bool GetBlob(const char* pKey, std::span<uint8_t> pOut, std::size_t& pLen);
bool SetBlob(const char* pKey, std::span<const uint8_t> pValue);
bool GetI32(const char* pKey, int32_t& pOut);
bool SetI32(const char* pKey, int32_t pValue);
bool EraseKey(const char* pKey);
bool EraseAll();

} // namespace hal::nvs
