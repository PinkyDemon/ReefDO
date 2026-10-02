#pragma once
// What a fresh simulator starts with: a small reef's failsafe devices and a tank whose nights dip into Blue
// and Yellow, so that there is something to watch. Saving a configuration on the page replaces the devices.
#include "reefdo/config.hpp"
#include "reefdo/sim.hpp"

namespace sim
{

inline reefdo::config::Config DemoConfig()
{
    using reefdo::ladder::Mode;
    using reefdo::ladder::Trigger;
    reefdo::config::Config c = reefdo::config::Defaults();
    struct Device
    {
        const char* name;
        bool nc;
        Trigger trigger;
        Mode mode;
        bool ackSilences;
        uint32_t testS;
        float minResponsePct;
    };
    const Device devices[] = {
        {"small bubbler", true, Trigger::Blue, Mode::On, false, 300, 1.0f},
        {"extra powerhead", true, Trigger::Blue, Mode::On, false, 300, 0.0f},
        {"strong air pump", true, Trigger::Yellow, Mode::On, false, 300, 1.0f},
        {"siren", false, Trigger::Yellow, Mode::On, true, 0, 0.0f},
        {"return pump", true, Trigger::Blue, Mode::PulseOff, false, 0, 0.0f},
    };
    for(std::size_t i = 0; i < std::size(devices); ++i)
    {
        const Device& d = devices[i];
        c.devices[i].name.assign(d.name);
        c.devices[i].slot = reefdo::slot::Relay(static_cast<uint32_t>(i + 1),
                                                d.nc ? reefdo::slot::Wiring::Nc : reefdo::slot::Wiring::No);
        c.ladder.devices[i] = {d.trigger, d.mode, d.ackSilences};
        c.test.devices[i] = {d.testS, d.minResponsePct};
    }
    return c;
}

inline reefdo::sim::TankConfig DemoTank()
{
    reefdo::sim::TankConfig t;
    t.kDevicePerH = {3.0f, 0.3f, 6.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // bubbler, powerhead, air pump
    t.returnPumpDevice = 4;
    t.respirationNightPctPerH = 14.0f; // with the flow alone, nights settle near 77 %: Blue, now and then Yellow
    t.sat0Pct = 98.0f;
    return t;
}

} // namespace sim
