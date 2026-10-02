# AGENTS.md

Facts an agent needs to work in this repository. The README covers usage; this covers the rules.

## Shape

- `core/` is a hardware-independent C++23 library: no ESP-IDF headers, no exceptions, no RTTI, no heap in the
  sampling path, fixed-size containers (`FixedVector`, `FixedString`). Hardware enters through interfaces:
  `probe::IUart`, `probe::IProbe`, `log::IBlockStore`. Time is always passed in (`app::Clock`). The core is the
  logic only: it knows no requests, transports or users.
- `gateway/` is how every transport reaches the App: `gateway::` views (status / test JSON, chart, event and
  export CSV through `gateway::ISink`, config apply, commands), the `router` (every `/api/*` route but the OTA
  upload: `Request` in, `IResponse` out; who may call what; `IPlatform` for what only a board or the simulator
  has), and `cloud` (the phone app's parameters, 5-minute medians, pushes, and its two writes). Same rules as the
  core, same 100 % gate; the firmware and the host each keep only a thin adapter per transport.
- Callers (`gateway::Who`): Anonymous reads; User (the password) does everything; Cloud (the phone app) reads and
  may send exactly one command, `ack` or `suspend_alerts` — anything else is 403. A new transport is an adapter
  plus, if it needs one, a `Who`.
- `sim/` is the host simulator (`reefdo_sim`): the App on `sim::Tank`, file-backed NOR-like log stores, a small
  HTTP server over the gateway router, and text controls (`sim/controls.cpp`). Not under the coverage gate: like
  `firmware/`, it only wires tested parts together.
- ArduinoJson is used by `core/src/config.cpp` and `gateway/src/views.cpp` only
  (`core/include/reefdo/slot_json.hpp` shares the slot writer between them).
- Device outputs are slots (`core/include/reefdo/slot.hpp`): `slot::Slot` with `Relay` and `Tuya`, held by value in
  `slot::AnySlot`. Each type lists its parameters as `slot::Field`s and walks them with `Fields(IFields&)`; the
  config reads, writes and checks them from that list, `config::SlotSchema` turns it into the JSON Schema the page
  builds its editors from (`GET /api/slots`). A new output type is a new `Slot` subclass and its field list —
  no config, validation or page code of its own. `Slot ==` is `IsSame` (the same physical output: uniqueness);
  `AnySlot ==` compares every parameter (configuration equality).
- The device registry's size is `DEVICES` in `core/include/reefdo/devices.hpp` (8), next to `RELAYS` (6, the
  board's). Everything else — arrays, the config document, `config::DOC_MAX` / `gateway::JSON_MAX` and the
  firmware's buffers — follows from those two.
- `firmware/main/` is thin glue over ESP-IDF v6.1: `sampler` owns the `app::App` and ticks it on its own task
  under a recursive mutex (`sampler::Guard`); `web`, `console_cmds`, `indicator` and `cloud_link` reach the App only
  through that guard. Relays are written only from `sampler`. `platform.cpp` is the board's `gateway::IPlatform`,
  shared by the web server and the cloud link.
- `cloud_link.cpp` is the phone app (ESP RainMaker Neo, downloaded by `firmware/cmake/neo_sdk.cmake`): its own task,
  the gateway's cloud agent fed by `cloud_link::OnSample` from the sampler (under the lock, no network work there),
  SDK writes queued and answered through the router as `Who::Cloud`. Without a device certificate in `fctry` it
  stays off; linking is an on-demand BLE window (`StartLink`, security 2). Neo's own Wi-Fi handling, local control,
  SNTP, console and OTA are not used: ReefDO keeps its own.
- Build switches (`firmware/main/Kconfig.projbuild`): `REEFDO_CLOUD` (default on) and `REEFDO_QEMU` (the
  `tools\qemu.cmd` variant: open_eth instead of Wi-Fi, the virtual tank, no LED, buzzer, button or Bluetooth).
- The host build never fetches sources: Catch2 and ArduinoJson are in `third_party/`. The firmware build downloads
  the component manager's registry components and the ESP RainMaker Neo firmware SDK, which is not on the registry:
  `firmware/cmake/neo_sdk.cmake` fetches it into `firmware/external/` (pinned commit and SHA-256) and applies
  `firmware/patches/esp-rainmaker-neo-firmware/*.patch` (each change marked "ReefDO patch" in the source); the SDK
  then clones its esp-aws-iot libraries itself. Updating it: a new commit and hash there, patches that still apply.
- `firmware/components/reefdo_core` and `reefdo_gateway` compile `core/` and `gateway/` as IDF components from
  their `sources.cmake`.
- The web page is a single file, `firmware/main/www/index.html`, embedded at build time; vanilla JS, no CDN.
- Version: `core/include/reefdo/version.hpp` (the firmware CMake reads `PROJECT_VER` from it).

## Rules

- 100 % line and branch coverage on `core/` and `gateway/` is enforced by `cmake --build --preset coverage` (one
  test binary: `core/tests` and `gateway/tests`); a change to either is not done until that passes. A branch that cannot be tested is removed, not excluded.
- Both host compilers must be warning-free: MSVC `/W4 /WX`, clang-cl and the Xtensa GCC `-Wall -Wextra -Werror`.
- GitHub Actions (`.github/workflows/`) run the same gate on Linux clang, the MSVC build and the ESP-IDF build on
  every push; a `v*` tag publishes a release and must equal `VERSION` in `core/include/reefdo/version.hpp`.
- Relays must be de-energised before anything else runs at boot (`board::InitRelaysDeenergised` is the first
  call in `app_main`). NC wiring means de-energised = device on.
- The ladder (`core/src/ladder.cpp`) decides device states and whether the alarm may sound (`Output::sound`);
  config decides the buzzer patterns; the LED codes are fixed in the firmware (`indicator.cpp`).
- The self test's settings live in `config.test` (`selftest::Config`, with `test.devices[i]`), like the ladder's
  (`config.ladder`) and the boost's (`config.boost`); the App steps them straight from `mCfg`.
- Blue is silent unless configured; Normal is always silent. The level is only what the readings support — no
  hidden escalation: a failed test verdict is shown and pushed but changes nothing that runs or sounds
  (`App::ClearFailure` clears it by hand). FAULT shows as FAULT and runs `fault.level`'s devices (Red by default);
  the level stays frozen at what the last readings said.
- The ladder's own caution is visible too: a sudden drop (`Input::fastSlopeMglPer10min`, the App's 2 min
  `SlopeEstimator`, steeper than `sudden_drop.slope_mgl_per_10min`) sets `Output::suspect`, logged as
  SuddenDrop / SuddenClear and flagged in Measurement records (`FLAG_SUSPECT`); while suspect every dwell is
  `sudden_drop.extend_s` longer. `night.day_alarm` only changes the sound by day: `AutoAck` acknowledges after
  `ladder::DAY_ACK_AFTER_S` (logged as AutoAck), `Suppress` sets `Output::suppressed`; the level, its devices
  and the pushes never depend on it. The night and an unknown clock (with `unknown_time_is_night`) always sound.
- An alert suspend (`App::SuspendAlerts`, `Input::suspendAlertsS`) puts the devices and the alarm of the current
  alert back to Normal (`DevicesLevel` = Normal; a pulse device just gets no pulses) for at most
  `ladder::ALERT_SUSPEND_MAX_S` per request (asking again restarts it); the level stays real and is shown. It starts
  only during an alert, never in a FAULT, never in the sample the level went deeper, and never when the level is
  deeper than the one the asker saw (`suspend_alerts.level`: the page and the phone send what they show); it ends by itself at a deeper level, a FAULT, back at Normal or when its time runs out (`ResumeReason`, logged
  as AlertSuspend / AlertResume, `FLAG_ALERTS_SUSPENDED`); no repeat pushes meanwhile. Windows, the test, the
  boost and manual switches are not alert-driven and do not depend on it.
- Device demand is the OR of the ladder, the self test (`selftest.cpp`, called "service" up to 1.0), the
  boost (`boost.cpp`), the always-on windows and the manual switch (`app::Demand`, reported as
  `Status::deviceWhy`); the boost yields to a test run and maintenance and runs at most once per local day; an
  exclusive test (`test.exclusive`) holds the windows off and ends manual switches, and only an exclusive run
  measures or judges. A manual on/off (`App::SetManual`) overrides the demands until the device's demands or
  the level or FAULT change, or an exclusive test starts; then it is automatic again.
- ReefDO maintenance (maintenance mode: controller work, hushes the buzzer, ends by itself after
  `app::MAINTENANCE_S` = 30 min) and device maintenance (a device out of order, RAM only) are independent. An
  out-of-order device is off regardless of every demand, alerts included; a test that would exercise it skips or
  aborts.
- Writes over HTTP need Basic auth; reads are open. Never log or store the password anywhere but NVS.
- Tuya plugs: `core/src/tuya.cpp` is the protocol (3.3 / 3.4 / 3.5) behind `tuya::ICrypto` and
  `tuya::IConnection`; `firmware/main/tuya_link.cpp` implements them with PSA Crypto (ESP-IDF 6 ships
  mbedTLS 4: the legacy `mbedtls_aes_*` / `mbedtls_gcm_*` calls are gone) and lwIP, and is the only code that
  switches a plug. Local keys never leave the board: anything a browser or the console sees uses
  `config::Write(..., true)`; only NVS gets the full document.
- Each device drives at most one output (`devices.N.slot`), and no two devices share one (`Validate`, by
  `Slot ==`); NC/NO is a relay's parameter. `Status::relayEnergised` is per channel; a channel no device owns
  stays de-energised.
- A device's self-test response is the largest *rise* of saturation above the pre-run trend (a fall never counts),
  and a device is judged only with ≥ 9 min of trend that ends now (`TrendReady`): a scheduled run waits for it after
  a boot or a probe gap, Run now meanwhile comes back inconclusive.
- A self-test miss that started at or above `test.no_fail_above_mgl` (DO at the device's start) is Unchecked,
  not Fail; the DeviceEnd record carries flag 8, the DO in f1 and the limit in f2.
- The log rings assume raw NOR flash: a torn record seals its segment, a partial tail is dropped on open,
  segments are erased right before reuse. Tests model this in `core/tests/fake_store.hpp`.

## Style

Allman braces, 4 spaces, `Type* pVar`, `if(` without a space, ≤ 120 columns (`.clang-format`,
`tools\format.cmd`). Names: `PascalCase` functions and types, `mMember`, `pParam`, `sGlobal`, `camelCase`
public fields and locals, `ALL_CAPS` constants, `enum class`. One variable per declaration, every variable
initialised. Comments short (≤ 2 lines); longer explanations go to the README.

## Commands

```bat
tools\vsenv.cmd cmake --preset msvc                       host configure (VS 2026 environment)
tools\vsenv.cmd cmake --build --preset msvc-debug         host build
tools\vsenv.cmd ctest --preset msvc-debug                 host tests
tools\vsenv.cmd cmake --preset clang-coverage             coverage configure
tools\vsenv.cmd cmake --build --preset coverage           coverage gate (100/100)
tools\vsenv.cmd tools\format.cmd                          clang-format everything
tools\idfenv.cmd idf.py -C firmware build                 firmware build (ESP-IDF via EIM)
tools\idfenv.cmd idf.py -C firmware -p COM3 flash monitor flash + serial console
out\msvc\sim\Debug\reefdo_sim.exe --speed 60             simulator: the page on http://localhost:8080/, controls on /sim
tools\qemu.cmd                                            the real firmware in QEMU: the page on http://localhost:8096/
```

EIM's `idf.py` launcher drops quotes around arguments; where they matter (`--qemu-extra-args`), call
`idf.py` through the IDF's Python as `tools\qemu.cmd` does.

Coverage output: `out/coverage/coverage/report.txt` and `html/index.html`. The compilation database for
clang tooling can be generated with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` on a Ninja/clang-cl configure.

## Board

Waveshare ESP32-S3-Relay-6CH: relays GPIO 1, 2, 41, 42, 45, 46 (HIGH = coil energised), buzzer GPIO 21
(LEDC), WS2812 GPIO 38 (RGB byte order), BOOT GPIO 0, RS485 UART1 TX 17 / RX 18 (auto direction). Console on
the native USB-Serial-JTAG; a terminal must not toggle DTR/RTS. Partitions in `firmware/partitions.csv`: two
3 MB OTA slots, four raw log partitions (`loga` 5 MB, `logb` 4 MB, `loge`, `logd` 256 KB each) and `fctry`
(24 KB NVS, the phone app's device certificate; after the logs so that adding it kept them in place — a board
with the older table needs one USB flash, OTA cannot change the table). `factory yes` erases ReefDO's NVS
namespace only, never `fctry`: each linking costs one of the account's 20 lifetime node IDs.

## Serial diagnostics

`idf.py monitor` holds the port; nothing else can open COM3 while it runs. Flashing over USB needs no password;
OTA over HTTP (`POST /api/ota`) needs the password and maintenance mode, and a pushed image must run for
2 minutes with a healthy probe before it confirms itself — a second push before that is refused.
