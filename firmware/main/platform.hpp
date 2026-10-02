#pragma once
// The board's side of the gateway (reefdo::gateway::IPlatform): network, flash, buzzer, the phone app link.
// One instance, for every transport that goes through the router (the web server, the cloud link).
#include "reefdo/gateway/router.hpp"

namespace platform
{

reefdo::gateway::IPlatform& Get();

} // namespace platform
