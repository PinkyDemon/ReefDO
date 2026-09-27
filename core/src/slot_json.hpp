#pragma once
// A device's slot as JSON, shared by the config document and the status document (core-internal: config.cpp
// writes it, api.cpp embeds it).
#include <ArduinoJson.h>

#include "reefdo/slot.hpp"

namespace reefdo::config
{

// {"type": "relay", "channel": 1, ...}; pRedact writes secret parameters as KEY_REDACTED.
void PutSlot(JsonObject pO, const slot::AnySlot& pS, bool pRedact);

} // namespace reefdo::config
