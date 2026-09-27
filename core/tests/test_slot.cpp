// Device outputs: kinds and their names, uniqueness (IsSame, Slot ==), and the value-held AnySlot.
#include "catch_amalgamated.hpp"

#include "reefdo/slot.hpp"

using namespace reefdo::slot;

namespace
{

Tuya Plug(const char* pId, const char* pIp, uint32_t pDp)
{
    Tuya t;
    t.id.assign(pId);
    t.ip.assign(pIp);
    t.key.assign("0123456789abcdef");
    t.dp = pDp;
    return t;
}

} // namespace

TEST_CASE("Slot kinds have a JSON name and a title; unknown names are no kind", "[slot]")
{
    for(const Kind k : {Kind::None, Kind::Relay, Kind::Tuya})
    {
        REQUIRE(KindNamed(Name(k)) == k);
        REQUIRE(Title(k)[0] != '\0');
    }
    REQUIRE(std::string_view(Name(Kind::Relay)) == "relay");
    REQUIRE_FALSE(KindNamed("lamp").has_value());
}

TEST_CASE("IsSame and Slot ==: the same relay channel, or the same plug outlet, whatever else differs", "[slot]")
{
    const Relay r1no(1, Wiring::No);
    const Relay r1nc(1, Wiring::Nc);
    const Relay r2(2, Wiring::No);
    REQUIRE(r1no.IsSame(r1nc));
    REQUIRE(r1no == r1nc); // Slot == is IsSame
    REQUIRE_FALSE(r1no == r2);

    const Tuya a = Plug("plug-a", "192.168.1.50", 1);
    REQUIRE(a == Plug("plug-a", "192.168.1.99", 1));       // the same plug at a new address
    REQUIRE(a == Plug("plug-b", "192.168.1.50", 1));       // the same address under a new id
    REQUIRE_FALSE(a == Plug("plug-a", "192.168.1.50", 2)); // another outlet of the same strip
    REQUIRE_FALSE(a == Plug("plug-b", "192.168.1.51", 1));

    REQUIRE_FALSE(r1no == a); // a relay is never a plug
    REQUIRE_FALSE(a == r1no);
}

TEST_CASE("AnySlot holds none, a relay or a plug by value; its == compares every parameter", "[slot]")
{
    AnySlot s;
    REQUIRE(s.GetKind() == Kind::None);
    REQUIRE(s.Get() == nullptr);
    REQUIRE(s.AsRelay() == nullptr);
    REQUIRE(s.AsTuya() == nullptr);
    REQUIRE(s == AnySlot());

    s = Relay(3, Wiring::Nc);
    REQUIRE(s.GetKind() == Kind::Relay);
    REQUIRE(s.Get()->GetKind() == Kind::Relay);
    REQUIRE(s.AsRelay()->channel == 3);
    REQUIRE(s.AsTuya() == nullptr);
    REQUIRE(s == AnySlot(Relay(3, Wiring::Nc)));
    REQUIRE_FALSE(s == AnySlot(Relay(3, Wiring::No))); // the same output, other settings
    REQUIRE_FALSE(s == AnySlot());

    AnySlot p = Plug("plug-a", "192.168.1.50", 1);
    REQUIRE(p.Get()->GetKind() == Kind::Tuya);
    REQUIRE(p == AnySlot(Plug("plug-a", "192.168.1.50", 1)));
    AnySlot rekeyed = p;
    rekeyed.AsTuya()->key.assign("fedcba9876543210");
    REQUIRE_FALSE(p == rekeyed); // a new key is a change, even to the same outlet
    REQUIRE(*p.Get() == *rekeyed.Get());

    const AnySlot& constant = p;
    REQUIRE(constant.Get() == p.Get());
    REQUIRE(constant.AsTuya() == p.AsTuya());
    REQUIRE(constant.AsRelay() == nullptr);

    p.Reset(Kind::Relay); // a new kind starts from its defaults
    REQUIRE(p.AsRelay()->channel == 1);
    REQUIRE(p.AsRelay()->wiring == Wiring::No);
    p.Reset(Kind::Tuya);
    REQUIRE(p.AsTuya()->id.empty());
    REQUIRE(p.AsTuya()->dp == 1);
    p.Reset(Kind::None);
    REQUIRE(p.Get() == nullptr);
}
