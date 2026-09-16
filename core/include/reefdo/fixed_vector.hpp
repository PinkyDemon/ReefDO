#pragma once
// Fixed-capacity vector: the only "container" the core uses. No heap, no exceptions.
#include <array>
#include <cstddef>

namespace reefdo
{

template <class T, std::size_t N>
class FixedVector
{
public:
    // Returns false (and drops the value) when full — callers that care check it.
    bool push_back(const T& pV)
    {
        if(mSize == N) return false;
        mData[mSize++] = pV;
        return true;
    }
    void clear() { mSize = 0; }
    std::size_t size() const { return mSize; }
    bool empty() const { return mSize == 0; }
    static constexpr std::size_t Capacity() { return N; }
    const T& operator[](std::size_t pI) const { return mData[pI]; }
    T& operator[](std::size_t pI) { return mData[pI]; }
    const T* begin() const { return mData.data(); }
    const T* end() const { return mData.data() + mSize; }

private:
    std::array<T, N> mData{};
    std::size_t mSize = 0;
};

} // namespace reefdo
