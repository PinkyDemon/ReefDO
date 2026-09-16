#pragma once
// HTTP server: the embedded web UI plus /api/* over the core's api module. Writes need Basic auth
// (user "reef", password from NVS, default "reefdo"); OTA additionally needs maintenance mode.
#include <string_view>

namespace web
{

void Start();
bool SetPassword(std::string_view pPassword); // persisted; empty restores the default

} // namespace web
