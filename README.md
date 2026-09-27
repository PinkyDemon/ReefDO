<img src="assets/logo_ReefDO.svg" width="96" align="right" alt="ReefDO">

# ReefDO

[![build](https://github.com/PinkyDemon/ReefDO/actions/workflows/build.yml/badge.svg)](https://github.com/PinkyDemon/ReefDO/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/PinkyDemon/ReefDO?include_prereleases)](https://github.com/PinkyDemon/ReefDO/releases)

Dissolved-oxygen failsafe for a small reef aquarium. A Rika RK500-04 optical DO probe (RS485 Modbus RTU) on a
Waveshare ESP32-S3-Relay-6CH watches the water; when oxygen falls, an escalation ladder (Blue → Yellow → Red)
switches devices (air pumps, powerheads, sirens) on its six relays or on Tuya Wi-Fi plugs, sounds the
buzzer and can send a push notification. Everything runs on the board: logging, web UI, WiFi, time, updates.
No server, no cloud.

![24 hours of a 90-litre reef: the night dive into Blue and Yellow, the morning recovery under the lights](assets/screenshot_chart.jpg)

## What it does

- Reads DO, saturation and temperature every 10 s and logs every sample (15 days full resolution, 1.7 years
  of 5-minute aggregates, events, daily summaries) on the board's flash.
- Three cumulative alert levels with mg/L thresholds, hysteresis and dwell times; each device is assigned to
  a level (or to over-temperature). NC-wired devices keep running if the controller dies.
- FAULT when the probe stops answering, freezes or reports nonsense: it shows as FAULT and runs the devices of
  `fault.level` (Red by default). Otherwise only the level the readings support decides what runs and sounds —
  there is no hidden escalation.
- A daily self test in the evening exercises the failsafe devices one by one and checks that each one
  actually moves the oxygen level; a device that does not is flagged and pushed (the flag changes nothing that
  runs; clear it on the Test page or the device card once the device is fixed). A miss that started at or above
  `test.no_fail_above_mgl` (default 6.1 mg/L, ~90 % saturation) is logged as unchecked with the reason, not as
  FAIL: near saturation a working aerator can barely move DO.
- An optional boost runs chosen devices inside a daily window until DO reaches a target,
  so the tank starts the night with a full buffer.
- Up to four daily always-on windows per device (a skimmer that runs all day but at night only in an alert),
  and a manual auto / on / off control on every device card. A device is on while anything wants it: its
  level, the test, the boost, a window; switched on or off by hand it stays so until the next scheduled or
  triggered change.
- A device registry: eight devices by default (`DEVICES` in `core/include/reefdo/devices.hpp`), each with its
  own output slot — one of the six relays or a Tuya Wi-Fi plug over the local network (protocol 3.3, 3.4, 3.5;
  no Tuya cloud at runtime). No two devices share an output. A plug that stops answering is logged and pushed.
- Device maintenance takes one device out of order for a while (cleaning the skimmer): it stays off even in an
  alert. It is separate from ReefDO maintenance, which is for work on the controller itself.
- Web UI (status, chart, events, test history, configuration, maintenance, export, setup), USB console,
  OTA updates with automatic rollback, optional push through ntfy.sh.

![Status page: live values, the ladder with its thresholds, tonight's plan and the devices](assets/screenshot_main.jpg)

## Hardware

- Waveshare ESP32-S3-Relay-6CH (ESP32-S3-WROOM-1U-N16), 12 V supply on `DC +/−`.
- RK500-04 probe: red/black (power) on the same `DC +/−` terminals, yellow (A) → `A+`, green (B) → `B−`,
  white (analog out) unused.
- Relays are dry contacts; loads have their own power. Wire aeration to the **NC** contact so it runs when
  the controller is off.
- Optional: Tuya Wi-Fi plugs (Smart Life and compatible) for more loads — see [Tuya plugs](#tuya-plugs).

## Ready-made firmware

Every tagged version is built on GitHub and published under [Releases](https://github.com/PinkyDemon/ReefDO/releases):

- `reefdo-vX.Y.Z-flash.bin` — a complete image for a new board. Connect the USB-C port and write it at 0x0 with
  [esptool](https://docs.espressif.com/projects/esptool/): `esptool --chip esp32s3 write-flash 0x0 reefdo-vX.Y.Z-flash.bin`
- `reefdo-vX.Y.Z-ota.bin` — the update image. Maintenance → OTA upload on the web page, or `POST /api/ota`.
  The board reboots into the new image and rolls back by itself if it does not come up.

Every push runs the tests and the coverage gate on Linux, the MSVC build on Windows and the firmware build
(`.github/workflows/build.yml`); a `v*` tag matching `core/include/reefdo/version.hpp` publishes a release
(`release.yml`).

## Build

Host side (the core library and its tests): Visual Studio 2026 with the C++ workload (cl, clang-cl,
llvm-cov, ninja, lld-link), CMake ≥ 3.24.

```bat
tools\vsenv.cmd cmake --preset msvc
tools\vsenv.cmd cmake --build --preset msvc-debug
tools\vsenv.cmd ctest --preset msvc-debug
```

Coverage gate (clang-cl + llvm-cov; the build fails below 100 % lines and branches on `core/`; on Linux use the
`linux-coverage` presets with clang, llvm and ninja installed):

```bat
tools\vsenv.cmd cmake --preset clang-coverage
tools\vsenv.cmd cmake --build --preset coverage
```

Firmware: ESP-IDF v6.1 installed with Espressif's EIM. `tools\idfenv.cmd` runs a command inside that
environment.

```bat
tools\idfenv.cmd idf.py -C firmware set-target esp32s3      (once)
tools\idfenv.cmd idf.py -C firmware -p COM3 build flash monitor
```

The USB-C port is the S3's native USB-Serial-JTAG; no driver on Windows. Any 115200 terminal works if it does
not toggle DTR/RTS; `idf.py monitor` handles that (Ctrl+] exits).

## First start

1. Flash, connect the probe and the 12 V supply. The LED is white while booting, then green.
2. On the console: `wifi <ssid> <password>`. Or join the open access point **reefdo-setup** and use
   http://192.168.4.1/ → Setup. Once on your network the page is at http://reefdo.local/.
3. Open the page, sign in (🔒 in the header, user `reef`, password `reefdo`) and change the password on Setup.
4. Config: name the devices, pick each one's output (a relay with its channel and NC/NO wiring, or a Tuya
   plug), assign each to a level, set `test (s)` for the ones the evening check should exercise. Save.
5. Probe: set `salinity (psu)` to your tank's value, put the seawater hint into `correction scale` if the probe
   reports freshwater-referenced mg/L (a bucket of aerated tank water should read ~100 % saturation; if it
   reads well above, do an air calibration from the Maintenance page).

Every setting on the Config page has a tooltip.

## Using it

- **Status**: live values, level, devices, test state. **Acknowledge** silences the buzzer and any
  ack-silenced device for a while; it never lowers the level.
- **Chart**: 1 h / 6 h / 12 h / 24 h / 7 d / 90 d with thresholds, night hours, and event markers (hover them).
- **Maintenance** has two separate parts:
  - **ReefDO maintenance** (work on the controller): maintenance mode mutes the buzzer and enables air
    calibration and OTA upload for 30 min, then ends by itself (logged; the page counts down; entering it again
    restarts the 30 min), so a forgotten maintenance cannot keep the alarm quiet. The device test there switches a device on by hand for 5 s — the manual
    switch, on and then auto.
  - **Device maintenance** (work on a device): takes one device out of order for 5–120 min, no mode needed. It
    stays off whatever wants it on, alerts included, until the time runs out or you put it back; the alarm
    itself is untouched. It lives in RAM: a reboot or power loss puts every device back in order (an NC device
    runs again), so unplug anything you put your hands into.
- **Manual control**: *auto / on / off* on a device card. *On* or *off* holds until the device's next scheduled
  or triggered change takes over — a window opening or closing, the level changing, the test reaching it, an
  exclusive test starting — and then it is *auto* again. *Off* by hand also stops a device an alert wants on,
  until the level changes; the card says "off by hand · wanted: ladder" meanwhile. Refused while an exclusive
  test runs.
- **Export**: CSV of everything, incrementally since the last export.
- **LED**: green steady = OK online, green blinking = OK offline, blue 1 Hz / yellow 2 Hz / red 4 Hz =
  alert, blue/red alternating = FAULT, cyan = maintenance, white pulse = test run, purple flash = a
  device failed its last check.
- **BOOT button**: short press = acknowledge, hold 3 s = maintenance on/off.

| ![Events](assets/screenshot_log.jpg) | ![Config](assets/screenshot_config.jpg) |
|:--:|:--:|
| Events: every level change, test step and boot with its value | Config: every setting with a tooltip; saved and applied in one step |

### Console

| Command | |
|---|---|
| `status [json]` | values, level, devices, test, log counts |
| `ack` · `maint on\|off` · `test run` · `test clear <device>` · `cal air` | as on the page |
| `suspend <device> <min>\|off` | device maintenance: out of order (≤ 120 min) |
| `manual <device> on\|off\|auto` | manual control |
| `tuya` · `tuya test <device> on\|off` | the plugs and their links · one switch now, with the error spelled out |
| `config get` · `config set <json>` · `config reset` | partial documents are fine; `'single quotes'` become `"` |
| `time [unix [tz]]` · `wifi …` · `passwd <new>` · `passwd reset` · `ntfy test` | |
| `buzzer test <pattern> <vol> [hz] [s]` · `buzzer mute\|unmute` | try patterns · mute for testing |
| `probe sim\|rk500` | run on a simulated tank instead of the probe (after reboot) |
| `debug sim …` · `debug uart mute\|corrupt\|ok` · `debug hang` | fault injection |
| `export [since]` · `events [since]` · `reboot` · `factory yes` | |

### HTTP API

Reads are open; writes need Basic auth (`reef` / your password), e.g. `curl -u reef:… `.

| | |
|---|---|
| `GET /api/status` · `/api/test` · `/api/config` · `/api/sys` | JSON |
| `GET /api/slots` | every output type's parameters as JSON Schema (titles, help, limits, defaults) |
| `PUT /api/config` | full or partial document, validated |
| `POST /api/cmd` | `{"ack":true}` · `{"maintenance":b}` · `{"test":"run"}` · `{"cal":"air"}` · `{"suspend":{"device":n,"s":secs}}` (`0` puts it back) · `{"manual":{"device":n,"on":b|null}}` (`null` = auto) · `{"clear":{"device":n}}` (a failed test verdict)\|null}}` (`null` = auto) · `{"time":{"unix":s,"tz":s}}` |
| `GET /api/series?tier=A\|B&from&to&every` · `/api/events?since` · `/api/export.csv?since` | CSV |
| `POST /api/wifi` · `/api/passwd` · `/api/ota` · `/api/ntfy-test` · `/api/buzzer-test` · `/api/mute` | |

## Configuration keys

`sample_period_s`, `levels.{blue,yellow,red}.{mgl,hysteresis,dwell_s}`, `levels.blue.slope_mgl_per_10min`,
`recover_sustain_s`, `night.{start,end,lock,unknown_time_is_night}`, `pulse.{s,min_interval_s}`,
`heat.{on_c,off_c}`, `fault.{consecutive_failures,stuck_minutes,level,alert}`, `ack_silence_s`,
`devices."1".."8".{name,slot,trigger,mode,ack_silences,test_s,min_response_pct,boost,windows}`
(slot: the device's output, `{"type":"none"}`, `{"type":"relay","channel":1..6,"wiring":"NO"|"NC"}` or
`{"type":"tuya","id","ip","key","version":"3.3"|"3.4"|"3.5","dp":1..255}` — `GET /api/slots` lists every type's
parameters; no two devices share a relay or a plug outlet, and a device with a trigger, test, boost or window
needs one; another type starts from that type's defaults; by default device N is on relay N, and a 1.0 document
with `wired` at the device loads that way; windows: up to 4 `["HH:MM","HH:MM"]` pairs, wrapping midnight when the
end is earlier; not for pulse or alarm devices),
`test.{window,settle_s,tail_s,min_headroom_pct,inconclusive_days,chirp,induce_deficit_s,induce_deficit_device,exclusive,no_fail_above_mgl}`
(`exclusive`, default on: during a run only the tested device runs; off: devices are still exercised but nothing
is measured or judged; `no_fail_above_mgl`, default 6.1, 0 = off: a device that misses its response after
starting at or above this DO is unchecked, not FAIL; a stored document with the old `service` / `service_s`
names still loads),
`boost.{enabled,window,target_mgl}`,
`correction.{scale,offset}`, `salinity_psu`, `allow_zero_cal`, `ntfy.{enabled,topic,min_level}`,
`signals.{buzzer_hz,led_brightness,normal,blue,yellow,red,fault}` with `{buzzer,volume}` per state.

## Tuya plugs

A device whose output slot is a Tuya plug switches that plug over your LAN. Each
plug outlet belongs to one device; the two outlets of a power strip are two devices with the same id and
different dps. ReefDO talks the plugs' local protocol (3.3, 3.4 or 3.5) directly on TCP 6668 — the Tuya cloud
is needed once, to learn the plug's local key, never while it runs.

1. Pair the plug with the Tuya / Smart Life app as usual and give it a fixed address (a DHCP reservation in
   your router). ReefDO does not search for it.
2. Get its **device id** and **local key**: the free [tinytuya](https://github.com/jasonacox/tinytuya) wizard
   (`python -m tinytuya wizard`, with a Tuya IoT developer account linked to your app) lists both;
   `python -m tinytuya scan` shows each plug's address and protocol **version**. Re-pairing a plug changes its
   local key.
3. Config → **Devices**: set the device's output to *Tuya plug* and fill in the fields that appear beside it —
   device id, address, local key, version and dp (the switching data point: 1 on single plugs, 1..4 on power
   strips). Save. The key is shown as `********` from then on and never leaves
   the board (the API's config document masks it too).
4. Check it: the device card shows *plug* when the last transaction worked; the console's `tuya test <n> on`
   tells you what went wrong otherwise (no connection, no answer = wrong key or version, …).

ReefDO sends every plug the state it wants when that changes and re-asserts it every minute (a plug switched
in the app, or restarted, is put right within a minute). Three failed transactions in a row mark the plug
**lost**: logged, pushed, red on its card; the first success brings it back.

**A plug is not a failsafe relay.** It keeps its last state when ReefDO, the Wi-Fi or the router goes down, and
it cannot be wired NC. Put the aeration you rely on on an NC relay; plugs are for the extras — a skimmer, a
second air pump, a fan.

## Layout

```
core/       hardware-independent C++23 library (modbus, probe, filters, ladder, selftest, boost, log, config, slot, api, app, sim, tuya)
  tests/    Catch2 tests, property tests, scenario runner on the virtual tank
firmware/   ESP-IDF project: main/ (board, hal/, sampler, indicator, console, net, web + www/, notify, tuya_link)
assets/     logo (SVG, embedded in the firmware and served at /logo.svg) and README screenshots
third_party/  Catch2, ArduinoJson (vendored)
tools/      vsenv.cmd, idfenv.cmd, idf-shell.ps1, format.cmd
```

## Licence

MIT, see [LICENSE](LICENSE). No warranty of any kind: test it on your own tank and keep a failsafe that does not
depend on it. Third-party code: Catch2 (Boost Software License 1.0, tests only), ArduinoJson (MIT), ESP-IDF
and its components (Apache-2.0; FreeRTOS MIT), espressif/led_strip and espressif/mdns (Apache-2.0). The Tuya
local protocol follows the layout documented by tinytuya (MIT); no tinytuya code is included. Tuya is a
trademark of its owner; ReefDO is not affiliated with it.
