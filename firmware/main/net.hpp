#pragma once
// WiFi station with a setup access point as fallback, mDNS (reefdo.local) and SNTP. Nothing in the
// ladder depends on any of it.
#include <cstdint>
#include <string_view>

#include "reefdo/fixed_string.hpp"

namespace net
{

struct Status
{
    reefdo::FixedString<32> ssid;
    bool connected = false;
    reefdo::FixedString<16> ip;
    int8_t rssi = 0;
    bool apActive = false;
    bool timeSynced = false;
};

void Start();
Status GetStatus();
bool SetCredentials(std::string_view pSsid, std::string_view pPassword); // persisted; reconnects
void Forget();                                                           // drop credentials, raise the AP
bool HasCredentials();
// Keeps credentials the phone app's provisioning already joined with: persisted, no reconnect.
bool RememberCredentials(std::string_view pSsid, std::string_view pPassword);
// Around the phone app's link window: the provisioning manager drives the station meanwhile. pJoined: it joined
// a network with credentials the app sent (already remembered); otherwise ReefDO's own are restored.
void PauseStation();
void ResumeStation(bool pJoined);

} // namespace net
