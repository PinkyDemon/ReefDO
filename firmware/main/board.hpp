#pragma once
// Waveshare ESP32-S3-Relay-6CH pin map and the thin drivers around it.
// Relay coils are energised by a HIGH level; with NC wiring, "de-energised" = fail-safe.
#include <cstdint>

#include "driver/gpio.h"

namespace board
{

constexpr int RELAY_COUNT = 6;
constexpr gpio_num_t RELAY_PINS[RELAY_COUNT] = {GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_41,
                                                GPIO_NUM_42, GPIO_NUM_45, GPIO_NUM_46};
constexpr gpio_num_t BUZZER_PIN = GPIO_NUM_21; // passive buzzer, LEDC PWM
constexpr gpio_num_t RGB_PIN = GPIO_NUM_38;    // one WS2812
constexpr gpio_num_t BOOT_BUTTON_PIN = GPIO_NUM_0;
constexpr gpio_num_t RS485_TX_PIN = GPIO_NUM_17;
constexpr gpio_num_t RS485_RX_PIN = GPIO_NUM_18;

// Must be the first thing app_main does: every coil de-energised before anything else can run.
void InitRelaysDeenergised();
void SetRelay(int pIndex, bool energised); // pIndex 0..5
bool RelayEnergised(int pIndex);

void InitBuzzer();
void BuzzerSet(bool pOn, uint32_t pHz, uint32_t pVolume = 3); // non-blocking; pVolume 0..3
void BuzzerTone(uint32_t pHz, uint32_t pMs);                  // blocking; console tests only

void InitLed();
void LedRgb(uint8_t pR, uint8_t pG, uint8_t pB);

void InitButton();
bool BootButtonPressed();

} // namespace board
