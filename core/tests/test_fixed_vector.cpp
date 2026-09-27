#include "catch_amalgamated.hpp"

#include "reefdo/fixed_vector.hpp"

using reefdo::FixedVector;

TEST_CASE("FixedVector holds up to N and refuses the rest", "[fixed_vector]")
{
    FixedVector<int, 3> v;
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
    REQUIRE(FixedVector<int, 3>::Capacity() == 3);

    REQUIRE(v.push_back(10));
    REQUIRE(v.push_back(20));
    REQUIRE(v.push_back(30));
    REQUIRE_FALSE(v.push_back(40)); // full: dropped, not overwritten

    REQUIRE(v.size() == 3);
    REQUIRE_FALSE(v.empty());
    REQUIRE(v[0] == 10);
    REQUIRE(v[2] == 30);

    v[1] = 21;
    int sum = 0;
    for(int x : v)
        sum += x;
    REQUIRE(sum == 10 + 21 + 30);

    v.clear();
    REQUIRE(v.empty());
    REQUIRE(v.push_back(1));
    REQUIRE(v.size() == 1);
}

TEST_CASE("FixedVector equality compares the stored elements only", "[fixed_vector]")
{
    FixedVector<int, 3> a;
    FixedVector<int, 3> b;
    REQUIRE(a == b);
    a.push_back(1);
    REQUIRE_FALSE(a == b); // sizes differ
    b.push_back(2);
    REQUIRE_FALSE(a == b); // contents differ
    b[0] = 1;
    REQUIRE(a == b);
    a.push_back(7);
    a.clear();
    a.push_back(1);
    REQUIRE(a == b); // a stale slot beyond size() does not count
}
