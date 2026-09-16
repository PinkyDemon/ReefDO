#include "catch_amalgamated.hpp"

#include "reefdo/correction.hpp"
#include "reefdo/solubility.hpp"

using Catch::Matchers::WithinAbs;
using reefdo::Correction;

TEST_CASE("The default correction is the identity: factory values pass through untouched", "[correction]")
{
    constexpr Correction C;
    STATIC_REQUIRE(C.IsFactory()); // usable at compile time...
    STATIC_REQUIRE(C.Apply(6.41f) == 6.41f);
    const Correction r; // ...and the same at run time (the coverage counters only see this one)
    REQUIRE(r.IsFactory());
    REQUIRE(r.Apply(6.41f) == 6.41f);
    REQUIRE(r.Apply(0.0f) == 0.0f);
}

TEST_CASE("Scale and offset are applied as raw * scale + offset", "[correction]")
{
    const Correction c{0.82f, -0.05f};
    REQUIRE_FALSE(c.IsFactory());
    REQUIRE_THAT(c.Apply(8.0f), WithinAbs(8.0 * 0.82 - 0.05, 1e-6));
    REQUIRE_FALSE(Correction{1.0f, 0.1f}.IsFactory());  // offset alone is a deviation
    REQUIRE_FALSE(Correction{0.99f, 0.0f}.IsFactory()); // scale alone is a deviation
}

TEST_CASE("seawater_scale is the solubility ratio", "[correction][solubility]")
{
    using reefdo::solubility::O2SaturationMgl;
    using reefdo::solubility::SeawaterScale;
    REQUIRE_THAT(SeawaterScale(26.0f, 35.0f),
                 WithinAbs(O2SaturationMgl(26.0f, 35.0f) / O2SaturationMgl(26.0f, 0.0f), 1e-6));
    REQUIRE_THAT(SeawaterScale(26.0f, 35.0f), WithinAbs(0.82, 0.01));
    REQUIRE_THAT(SeawaterScale(26.0f, 0.0f), WithinAbs(1.0, 1e-6)); // freshwater: no change
    // Applying it to a freshwater-referenced 8.09 mg/L (100 % at 26 °C) gives the seawater 6.63.
    const Correction c{SeawaterScale(26.0f, 35.0f), 0.0f};
    REQUIRE_THAT(c.Apply(O2SaturationMgl(26.0f, 0.0f)), WithinAbs(6.63, 0.05));
}
