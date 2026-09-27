#pragma once
// Tuya plugs: a task that sends every plug the state the App wants, re-asserts it every minute, and reports each
// transaction back to the App (which logs and pushes a lost plug). The only code that switches a plug.
#include "reefdo/tuya.hpp"

namespace tuya_link
{

void Start();
// Bench test from the console: the plug task runs one transaction for us; blocks for up to 12 s.
reefdo::tuya::Result Test(const reefdo::tuya::Settings& pS, bool pOn);
const char* ErrorName(reefdo::tuya::Error pE);

} // namespace tuya_link
