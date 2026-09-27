#pragma once
// The device registry's size. Each device drives at most one output — a relay or a Tuya plug — and no two
// devices share one; more devices than relays leaves room for plugs. Change DEVICES here, nowhere else.
#include <cstddef>

namespace reefdo
{

constexpr std::size_t DEVICES = 8;
constexpr std::size_t RELAYS = 6; // the board's relay channels

static_assert(DEVICES >= 1 && DEVICES <= 99, "device numbers are one or two digits in the config");

} // namespace reefdo
