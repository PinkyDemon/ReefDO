#include "reefdo/slot.hpp"

#include "reefdo/fixed_vector.hpp"

namespace reefdo::slot
{

namespace
{

constexpr const char* KIND_NAMES[KINDS] = {"none", "relay", "tuya"};
constexpr const char* KIND_TITLES[KINDS] = {"no output", "relay", "Tuya plug"};
constexpr const char* WIRING_NAMES[] = {"NO", "NC"};
constexpr const char* VERSION_NAMES[] = {"3.3", "3.4", "3.5"};

constexpr Field RELAY_CHANNEL{.key = "channel",
                              .title = "relay",
                              .help = "The board's relay this device is wired to. Each relay belongs to one device.",
                              .type = FieldType::Number,
                              .min = 1,
                              .max = RELAYS};
constexpr Field RELAY_WIRING{
    .key = "wiring",
    .title = "wired",
    .help = "NC: the device is ON while the relay coil is off, so a dead controller leaves it running (fail-safe "
            "for aeration). NO: the device is on only while the coil is powered.",
    .type = FieldType::Choice,
    .choices = WIRING_NAMES};
constexpr Field TUYA_ID{.key = "id",
                        .title = "device id",
                        .help = "The plug's device id (from the Tuya app or tinytuya).",
                        .type = FieldType::Text,
                        .min = 1,
                        .max = 31};
constexpr Field TUYA_IP{
    .key = "ip",
    .title = "address",
    .help = "The plug's address on your network. Give it a fixed address (a DHCP reservation in your router) — "
            "ReefDO does not search for it.",
    .type = FieldType::Text,
    .min = 7,
    .max = 15,
    .ipv4 = true};
constexpr Field TUYA_KEY{
    .key = "key",
    .title = "local key",
    .help = "The plug's 16-character local key (tinytuya wizard). Shown as ******** once saved; type a new one to "
            "replace it.",
    .type = FieldType::Text,
    .min = 16,
    .max = 16,
    .secret = true};
constexpr Field TUYA_VERSION{
    .key = "version",
    .title = "version",
    .help = "The plug's protocol version: 3.3 for most older plugs, 3.4 or 3.5 for newer firmware. tinytuya scan "
            "tells you.",
    .type = FieldType::Choice,
    .choices = VERSION_NAMES};
constexpr Field TUYA_DP{
    .key = "dp",
    .title = "dp",
    .help = "The data point that switches the outlet: 1 on almost every single plug; 1..4 for the outlets of a power "
            "strip (two outlets of one strip are two devices).",
    .type = FieldType::Number,
    .min = 1,
    .max = 255};

// Every parameter value of a slot, in field order: what makes two slots of one kind equal.
class Values final : public IFields
{
public:
    void Number(const Field&, uint32_t& pV) override { mNumbers.push_back(pV); }
    void String(const Field&, Text& pV) override { mTexts.push_back(pV); }
    void Choice(const Field&, uint8_t& pV) override { mNumbers.push_back(pV); }
    bool operator==(const Values& pO) const { return mNumbers == pO.mNumbers && mTexts == pO.mTexts; }

private:
    FixedVector<uint32_t, 8> mNumbers;
    FixedVector<Text, 4> mTexts;
};

} // namespace

const char* Name(Kind pK)
{
    return KIND_NAMES[static_cast<uint8_t>(pK)];
}

const char* Title(Kind pK)
{
    return KIND_TITLES[static_cast<uint8_t>(pK)];
}

std::optional<Kind> KindNamed(std::string_view pName)
{
    for(uint8_t k = 0; k < KINDS; ++k)
    {
        if(pName == KIND_NAMES[k]) return static_cast<Kind>(k);
    }
    return std::nullopt;
}

bool operator==(const Slot& pA, const Slot& pB)
{
    return pA.IsSame(pB);
}

bool Relay::IsSame(const Slot& pOther) const
{
    return pOther.GetKind() == Kind::Relay && static_cast<const Relay&>(pOther).channel == channel;
}

void Relay::Fields(IFields& pF)
{
    pF.Number(RELAY_CHANNEL, channel);
    uint8_t w = static_cast<uint8_t>(wiring);
    pF.Choice(RELAY_WIRING, w);
    wiring = static_cast<Wiring>(w);
}

bool Tuya::IsSame(const Slot& pOther) const
{
    if(pOther.GetKind() != Kind::Tuya) return false;
    const Tuya& o = static_cast<const Tuya&>(pOther);
    return (o.id == id || o.ip == ip) && o.dp == dp;
}

void Tuya::Fields(IFields& pF)
{
    pF.String(TUYA_ID, id);
    pF.String(TUYA_IP, ip);
    pF.String(TUYA_KEY, key);
    uint8_t v = static_cast<uint8_t>(version);
    pF.Choice(TUYA_VERSION, v);
    version = static_cast<tuya::Version>(v);
    pF.Number(TUYA_DP, dp);
}

const Slot* AnySlot::Get() const
{
    if(const Relay* r = AsRelay()) return r;
    return AsTuya(); // nullptr: none
}

Slot* AnySlot::Get()
{
    return const_cast<Slot*>(static_cast<const AnySlot&>(*this).Get());
}

void AnySlot::Reset(Kind pK)
{
    if(pK == Kind::Relay)
    {
        mV = Relay{};
    }
    else if(pK == Kind::Tuya)
    {
        mV = Tuya{};
    }
    else
    {
        mV = std::monostate{};
    }
}

bool AnySlot::operator==(const AnySlot& pO) const
{
    if(GetKind() != pO.GetKind()) return false;
    AnySlot a = *this; // Fields() walks mutable parameters: walk copies
    AnySlot b = pO;
    Values va;
    Values vb;
    if(a.Get() != nullptr)
    {
        a.Get()->Fields(va);
        b.Get()->Fields(vb);
    }
    return va == vb;
}

} // namespace reefdo::slot
