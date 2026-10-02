#pragma once
// The phone app through ESP RainMaker Neo: the gateway's cloud agent on the board. An observer like the web page —
// its own task, never inside the sampler's lock during network work; the ladder does not know it exists. A board
// without a device certificate (not linked, or an older partition table) runs with it off.
#include <cstdint>
#include <span>

#include "reefdo/app.hpp"
#include "reefdo/gateway/router.hpp"

namespace cloud_link
{

void Start(); // after net::Start
// From the sampler, under its lock, once per sample, with that sample's notifications.
void OnSample(const reefdo::app::App& pApp, const reefdo::app::Clock& pClock,
              std::span<const reefdo::app::Notification> pNotes);
// Opens a Bluetooth window for the ESP RainMaker Home app to link the board (and, optionally, set its Wi-Fi).
reefdo::gateway::CloudLink StartLink();
const char* State(); // "off", "not linked", "linking", "offline", "online"
// Erases the device certificate (fctry) so that the board can be linked again, to another account; the caller
// reboots. Costs one of the account's 20 lifetime node IDs at the next link. Console only.
bool Forget();
uint32_t Pushes();

} // namespace cloud_link
