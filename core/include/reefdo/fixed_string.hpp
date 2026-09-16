#pragma once
// Fixed-capacity, always NUL-terminated string for names, topics, error paths. No heap.
#include <cstddef>
#include <cstring>
#include <string_view>

namespace reefdo
{

template <std::size_t N>
class FixedString
{
public:
    FixedString() = default;
    explicit FixedString(std::string_view pS) { assign(pS); }

    // Copies up to N bytes. Returns false if the input was truncated.
    bool assign(std::string_view pS)
    {
        const bool fits = pS.size() <= N;
        mLen = fits ? pS.size() : N;
        std::memcpy(mData, pS.data(), mLen);
        mData[mLen] = '\0';
        return fits;
    }
    void clear() { assign({}); }

    std::string_view view() const { return std::string_view(mData, mLen); }
    const char* c_str() const { return mData; }
    std::size_t size() const { return mLen; }
    bool empty() const { return mLen == 0; }
    static constexpr std::size_t Capacity() { return N; }

    bool operator==(const FixedString& pO) const { return view() == pO.view(); }

private:
    char mData[N + 1]{};
    std::size_t mLen = 0;
};

} // namespace reefdo
