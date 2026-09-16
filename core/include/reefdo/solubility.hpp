#pragma once
// Oxygen solubility: Benson & Krause (1984), as in APHA Standard Methods / USGS DOTABLES.

namespace reefdo::solubility
{

// Air-saturated O2 in mg/L; inputs clamped to the equation's range (0–50 °C, 0–50 ppt, 50–120 kPa).
float O2SaturationMgl(float pTempC, float pSalinityPpt, float pPressureKpa = 101.325f);

// mg/L implied by a saturation percentage under the given conditions.
float MglFromSaturation(float pSatPct, float pTempC, float pSalinityPpt, float pPressureKpa = 101.325f);

// Freshwater-referenced mg/L → this salinity (≈ 0.82 at 35 ppt): the Correction::scale to use if the
// probe turns out to report freshwater values.
float SeawaterScale(float pTempC, float pSalinityPpt);

} // namespace reefdo::solubility
