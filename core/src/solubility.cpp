#include "reefdo/solubility.hpp"

#include <cmath>

namespace reefdo::solubility
{

namespace
{

float Clampf(float pV, float pLo, float pHi)
{
    if(pV < pLo) return pLo;
    if(pV > pHi) return pHi;
    return pV;
}

} // namespace

float O2SaturationMgl(float pTempC, float pSalinityPpt, float pPressureKpa)
{
    const double t = Clampf(pTempC, 0.0f, 50.0f);
    const double s = Clampf(pSalinityPpt, 0.0f, 50.0f);
    const double p = Clampf(pPressureKpa, 50.0f, 120.0f);

    // Benson & Krause (1984), freshwater term, T in kelvin, result in mg/L at 1 atm.
    const double tk = t + 273.15;
    const double lnC = -139.34411 + 1.575701e5 / tk - 6.642308e7 / (tk * tk) + 1.243800e10 / (tk * tk * tk) -
                       8.621949e11 / (tk * tk * tk * tk);
    // Salinity term (ppt).
    const double lnS = s * (1.7674e-2 - 10.754 / tk + 2140.7 / (tk * tk));
    const double c = std::exp(lnC - lnS);

    // Pressure: proportional to total pressure (vapour-pressure refinement is < 0.5 % over the reef range).
    return static_cast<float>(c * (p / 101.325));
}

float MglFromSaturation(float pSatPct, float pTempC, float pSalinityPpt, float pPressureKpa)
{
    return pSatPct / 100.0f * O2SaturationMgl(pTempC, pSalinityPpt, pPressureKpa);
}

float SeawaterScale(float pTempC, float pSalinityPpt)
{
    return O2SaturationMgl(pTempC, pSalinityPpt) / O2SaturationMgl(pTempC, 0.0f);
}

} // namespace reefdo::solubility
