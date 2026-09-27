#pragma once
// Device outputs ("slots"): a relay channel on the board or a Tuya plug outlet on the LAN. Every slot type lists its
// own parameters (Fields): the config reads, writes and checks them from that list, and the page builds its form
// from the JSON schema made of it. No two devices may share an output: see IsSame.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "reefdo/devices.hpp"
#include "reefdo/fixed_string.hpp"
#include "reefdo/tuya.hpp"

namespace reefdo::slot
{

enum class Kind : uint8_t // in the order of AnySlot's alternatives
{
    None,
    Relay,
    Tuya
};
constexpr std::size_t KINDS = 3;
const char* Name(Kind pK);                             // "none", "relay", "tuya": the JSON "type"
const char* Title(Kind pK);                            // for the page
std::optional<Kind> KindNamed(std::string_view pName); // nullopt: not a slot type

using Text = FixedString<31>;
static_assert(std::is_same_v<Text, decltype(tuya::Settings::id)>, "plug settings are slot texts");

enum class FieldType : uint8_t
{
    Number,
    Text,
    Choice
};

// One parameter of a slot type: what it is called, what it may hold, how the page shows it.
struct Field
{
    const char* key;   // JSON key
    const char* title; // label on the page
    const char* help;  // tooltip
    FieldType type = FieldType::Number;
    uint32_t min = 0;                          // Number: the smallest value; Text: the shortest length
    uint32_t max = 0;                          // Number: the largest value; Text: the longest length
    std::span<const char* const> choices = {}; // Choice: the names; the value is the index
    bool ipv4 = false;                         // Text: a dotted IPv4 address
    bool secret = false;                       // Text: never leaves the board in clear
};

// A walk over a slot's parameters. Reading, writing, checking, comparing and describing a slot all use it.
class IFields
{
public:
    virtual void Number(const Field& pF, uint32_t& pV) = 0;
    virtual void String(const Field& pF, Text& pV) = 0;
    virtual void Choice(const Field& pF, uint8_t& pV) = 0;

protected:
    ~IFields() = default; // walkers live on the stack; nothing owns one through this interface
};

class Slot
{
public:
    virtual ~Slot() = default;
    virtual Kind GetKind() const = 0;
    // The same physical output — a relay channel, a plug outlet — whatever the other parameters say.
    virtual bool IsSame(const Slot& pOther) const = 0;
    virtual void Fields(IFields& pF) = 0; // every parameter, in order
};

// Uniqueness, not equal settings: two devices whose slots compare equal would switch the same output.
bool operator==(const Slot& pA, const Slot& pB);

enum class Wiring : uint8_t
{
    No,
    Nc // on while the coil is off = on when the controller is dead
};

class Relay final : public Slot
{
public:
    Relay() = default;
    Relay(uint32_t pChannel, Wiring pWiring)
        : channel(pChannel)
        , wiring(pWiring)
    {
    }
    Kind GetKind() const override { return Kind::Relay; }
    bool IsSame(const Slot& pOther) const override; // the same channel
    void Fields(IFields& pF) override;

    uint32_t channel = 1; // 1..RELAYS
    Wiring wiring = Wiring::No;
};

// A Tuya plug outlet: the protocol's settings, as a slot.
class Tuya final : public Slot, public tuya::Settings
{
public:
    Kind GetKind() const override { return Kind::Tuya; }
    bool IsSame(const Slot& pOther) const override; // the same plug (by id or address) and data point
    void Fields(IFields& pF) override;
};

// A device's slot, held by value (no heap): none, a relay or a plug.
class AnySlot
{
public:
    AnySlot() = default;
    AnySlot(const Relay& pR)
        : mV(pR)
    {
    }
    AnySlot(const Tuya& pT)
        : mV(pT)
    {
    }
    Kind GetKind() const { return static_cast<Kind>(mV.index()); }
    const Slot* Get() const; // nullptr: none
    Slot* Get();
    const Relay* AsRelay() const { return std::get_if<Relay>(&mV); }
    Relay* AsRelay() { return std::get_if<Relay>(&mV); }
    const Tuya* AsTuya() const { return std::get_if<Tuya>(&mV); }
    Tuya* AsTuya() { return std::get_if<Tuya>(&mV); }
    void Reset(Kind pK); // a slot of that kind with its defaults

    // The same kind with the same parameters (a new key counts): configuration equality, not uniqueness.
    bool operator==(const AnySlot& pO) const;

private:
    using Variant = std::variant<std::monostate, Relay, Tuya>;
    static_assert(std::variant_size_v<Variant> == KINDS, "one alternative per Kind");
    static_assert(std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(Kind::Relay), Variant>, Relay>,
                  "GetKind() is the alternative's index");
    static_assert(std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(Kind::Tuya), Variant>, Tuya>,
                  "GetKind() is the alternative's index");
    Variant mV;
};

} // namespace reefdo::slot
