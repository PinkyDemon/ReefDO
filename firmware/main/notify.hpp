#pragma once
// Push notifications through ntfy: one HTTPS POST per event, from its own task, only when enabled.
// The queue is bounded; a push that cannot be delivered is dropped, never retried into the sampler's path.
#include "reefdo/app.hpp"
#include "reefdo/config.hpp"

namespace notify
{

void Start();
void Push(const reefdo::app::Notification& pN, const reefdo::config::Config& pCfg);
bool SendTest(const reefdo::config::Config& pCfg);
uint32_t Sent();
uint32_t Failed();

} // namespace notify
