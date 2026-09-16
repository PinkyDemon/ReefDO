#pragma once
// The DO correction: an affine map on the probe's mg/L, always applied, identity by default.
// No on/off switch: every consumer sees corrected values only.

namespace reefdo
{

struct Correction
{
    float scale = 1.0f;
    float offset = 0.0f;

    constexpr float Apply(float pProbeMgl) const { return pProbeMgl * scale + offset; }
    constexpr bool IsFactory() const { return scale == 1.0f && offset == 0.0f; }
    bool operator==(const Correction&) const = default;
};

} // namespace reefdo
