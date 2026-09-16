#include "catch_amalgamated.hpp"

#include "reefdo/solubility.hpp"

using namespace reefdo::solubility;
using Catch::Matchers::WithinAbs;

TEST_CASE("O2 solubility matches the reference tables (Benson-Krause / USGS)", "[solubility]")
{
    // fresh / 35 ppt, mg/L at 1 atm
    REQUIRE_THAT(O2SaturationMgl(20.0f, 0.0f), WithinAbs(9.09, 0.05));
    REQUIRE_THAT(O2SaturationMgl(25.0f, 0.0f), WithinAbs(8.26, 0.05));
    REQUIRE_THAT(O2SaturationMgl(26.0f, 0.0f), WithinAbs(8.09, 0.05));
    REQUIRE_THAT(O2SaturationMgl(20.0f, 35.0f), WithinAbs(7.38, 0.05));
    REQUIRE_THAT(O2SaturationMgl(25.0f, 35.0f), WithinAbs(6.75, 0.05));
    REQUIRE_THAT(O2SaturationMgl(26.0f, 35.0f), WithinAbs(6.63, 0.05));
    // A few more anchors from the USGS table
    REQUIRE_THAT(O2SaturationMgl(0.0f, 0.0f), WithinAbs(14.62, 0.05));
    REQUIRE_THAT(O2SaturationMgl(30.0f, 0.0f), WithinAbs(7.56, 0.05));
}

TEST_CASE("Seawater holds roughly 18-20 % less oxygen than freshwater across the reef range", "[solubility]")
{
    for(float t = 22.0f; t <= 30.0f; t += 1.0f)
    {
        const float ratio = O2SaturationMgl(t, 35.0f) / O2SaturationMgl(t, 0.0f);
        REQUIRE(ratio > 0.80f);
        REQUIRE(ratio < 0.83f);
    }
}

TEST_CASE("Pressure scales linearly; inputs are clamped, not extrapolated", "[solubility]")
{
    const float base = O2SaturationMgl(26.0f, 35.0f, 101.325f);
    REQUIRE_THAT(O2SaturationMgl(26.0f, 35.0f, 90.0f), WithinAbs(base * 90.0 / 101.325, 0.001));

    // Clamps: below/above each axis lands on the boundary value.
    REQUIRE(O2SaturationMgl(-10.0f, 0.0f) == O2SaturationMgl(0.0f, 0.0f));
    REQUIRE(O2SaturationMgl(80.0f, 0.0f) == O2SaturationMgl(50.0f, 0.0f));
    REQUIRE(O2SaturationMgl(25.0f, -3.0f) == O2SaturationMgl(25.0f, 0.0f));
    REQUIRE(O2SaturationMgl(25.0f, 90.0f) == O2SaturationMgl(25.0f, 50.0f));
    REQUIRE(O2SaturationMgl(25.0f, 35.0f, 10.0f) == O2SaturationMgl(25.0f, 35.0f, 50.0f));
    REQUIRE(O2SaturationMgl(25.0f, 35.0f, 300.0f) == O2SaturationMgl(25.0f, 35.0f, 120.0f));
}

TEST_CASE("mg/L from saturation is the inverse of the table", "[solubility]")
{
    REQUIRE_THAT(MglFromSaturation(100.0f, 26.0f, 35.0f), WithinAbs(O2SaturationMgl(26.0f, 35.0f), 1e-5));
    REQUIRE_THAT(MglFromSaturation(50.0f, 26.0f, 35.0f), WithinAbs(O2SaturationMgl(26.0f, 35.0f) / 2, 1e-5));
    REQUIRE(MglFromSaturation(0.0f, 26.0f, 35.0f) == 0.0f);
}
