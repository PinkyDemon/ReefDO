#pragma once
// The App's home on the board: owns config, probe and stores, ticks the App on its own task, drives the
// relays. Everything else (console, web, indicator) reaches the App through the Guard.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "reefdo/app.hpp"
#include "reefdo/config.hpp"
#include "reefdo/sim.hpp"

#include "hal/rs485_uart.hpp"

namespace sampler
{

enum class ProbeSource : uint8_t
{
    Sim = 0,
    Rk500 = 1
};

// Snapshot for the indicator task.
struct Indication
{
    reefdo::ladder::Level effective = reefdo::ladder::Level::Normal;
    reefdo::ladder::Buzzer buzzer = reefdo::ladder::Buzzer::Off;
    reefdo::ladder::Led led = reefdo::ladder::Led::Green;
    bool fault = false;
    bool maintenance = false;
    bool serviceRunning = false;
    bool anyFailed = false;
    uint32_t chirps = 0; // total so far; the indicator plays one per increment
    reefdo::config::SignalsConfig signals;
};

// Loads NVS + flash, builds the App, starts the task. Returns false if a log partition is unusable
// (the App still runs; the affected ring just fails its appends).
bool Start();

// Serialises App access; hold one for the whole of any App() use.
class Guard
{
public:
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    // Let the sampler in while a long response drains to a slow client (the CSV sinks): Unlock, send, Lock.
    void Unlock();
    void Lock();
};
reefdo::app::App& App(); // only under a Guard

reefdo::app::Clock ClockNow();
Indication GetIndication();

ProbeSource GetProbeSource();
void SetProbeSource(ProbeSource pSrc); // persisted; effective after reboot
reefdo::sim::Tank* Tank();             // the virtual tank when the source is Sim, else nullptr
hal::Rs485Uart& Uart();

// Config: validate, apply, persist. `pOut` receives the load result (ok or error path/message).
bool ApplyConfigJson(std::string_view pJson, reefdo::config::LoadResult& pOut);
bool ResetConfig();
std::size_t ConfigJson(std::span<char> pOut);

// Wall clock and zone; persisted zone, TimeSync logged by the App on the next tick.
void SetTime(uint32_t pUnixS, std::optional<int32_t> pTzOffsetS);
int32_t TzOffsetS();
const char* ConfigState(); // "stored", "defaults", or "rejected at <path>: <message>" (the tank is then unprotected!)
void ApplyRelays(); // after a command that changes device states (override, ack): do not wait for the next sample

void Hang(); // stop feeding the task watchdog: proves the panic → reboot → relays-off path

} // namespace sampler
