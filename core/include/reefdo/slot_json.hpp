#pragma once
// A device's slot as JSON, shared by the config document (config.cpp) and the gateway's status document
// (gateway/src/views.cpp).
#include <ArduinoJson.h>

#include "reefdo/slot.hpp"

namespace reefdo::config
{

// {"type": "relay", "channel": 1, ...}; pRedact writes secret parameters as KEY_REDACTED.
void PutSlot(JsonObject pO, const slot::AnySlot& pS, bool pRedact);

} // namespace reefdo::config
