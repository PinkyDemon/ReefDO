#pragma once
// LED, buzzer and BOOT button on a 50 ms task: renders sampler::GetIndication(), turns presses into ack /
// maintenance. Nothing here can affect the relays.
#include <cstdint>

#include "reefdo/config.hpp"

namespace indicator
{

void Start();
void SetMuted(bool pMuted); // persisted test switch: the ladder still decides, the buzzer just stays quiet
bool Muted();
void TestBuzzer(reefdo::config::BuzzerPattern pPattern, uint32_t pVolume, uint32_t pHz,
                uint32_t pSeconds); // plays through the mute
bool ParseBuzzerPattern(const char* pName, reefdo::config::BuzzerPattern& pOut);

} // namespace indicator
