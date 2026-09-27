# AGENTS.md

Facts an agent needs to work in this repository. The README covers usage; this covers the rules.

## Shape

- `core/` is a hardware-independent C++23 library: no ESP-IDF headers, no exceptions, no RTTI, no heap in the
  sampling path, fixed-size containers (`FixedVector`, `FixedString`). Hardware enters through interfaces:
  `probe::IUart`, `probe::IProbe`, `log::IBlockStore`, `api::ISink`. Time is always passed in (`app::Clock`).
- ArduinoJson is used by `core/src/config.cpp` and `core/src/api.cpp` only (the core-internal
  `core/src/slot_json.hpp` shares the slot writer between them).
- Device outputs are slots (`core/include/reefdo/slot.hpp`): `slot::Slot` with `Relay` and `Tuya`, held by value in
  `slot::AnySlot`. Each type lists its parameters as `slot::Field`s and walks them with `Fields(IFields&)`; the
  config reads, writes and checks them from that list, `config::SlotSchema` turns it into the JSON Schema the page
  builds its editors from (`GET /api/slots`). A new output type is a new `Slot` subclass and its field list —
  no config, validation or page code of its own. `Slot ==` is `IsSame` (the same physical output: uniqueness);
  `AnySlot ==` compares every parameter (configuration equality).
- The device registry's size is `DEVICES` in `core/include/reefdo/devices.hpp` (8), next to `RELAYS` (6, the
  board's). Everything else — arrays, the config document, `config::DOC_MAX` / `api::JSON_MAX` and the
  firmware's buffers — follows from those two.
- `firmware/main/` is thin glue over ESP-IDF v6.1: `sampler` owns the `app::App` and ticks it on its own task
  under a recursive mutex (`sampler::Guard`); `web`, `console_cmds` and `indicator` reach the App only through
  that guard. Relays are written only from `sampler`.
- `firmware/components/reefdo_core` compiles `core/` as an IDF component from `core/sources.cmake`.
- The web page is a single file, `firmware/main/www/index.html`, embedded at build time; vanilla JS, no CDN.
- Version: `core/include/reefdo/version.hpp` (the firmware CMake reads `PROJECT_VER` from it).

## Rules

- 100 % line and branch coverage on `core/` is enforced by `cmake --build --preset coverage`; a change to the
  core is not done until that passes. A branch that cannot be tested is removed, not excluded.
- Both host compilers must be warning-free: MSVC `/W4 /WX`, clang-cl and the Xtensa GCC `-Wall -Wextra -Werror`.
- GitHub Actions (`.github/workflows/`) run the same gate on Linux clang, the MSVC build and the ESP-IDF build on
  every push; a `v*` tag publishes a release and must equal `VERSION` in `core/include/reefdo/version.hpp`.
- Relays must be de-energised before anything else runs at boot (`board::InitRelaysDeenergised` is the first
  call in `app_main`). NC wiring means de-energised = device on.
- The ladder (`core/src/ladder.cpp`) decides device states and whether the alarm may sound (`Output::sound`);
  config decides the buzzer patterns; the LED codes are fixed in the firmware (`indicator.cpp`).
- The self test's settings live in `config.test` (`selftest::Config`, with `test.devices[i]`), like the ladder's
  (`config.ladder`) and the boost's (`config.boost`); the App steps them straight from `mCfg`.
- Blue is silent unless configured; Normal is always silent; FAULT acts as `fault.level` (Red by default).
- Device demand is the OR of the ladder, the self test (`selftest.cpp`, called "service" up to 1.0), the
  boost (`boost.cpp`), the always-on windows and the manual switch (`app::Demand`, reported as
  `Status::deviceWhy`); the boost yields to a test run and maintenance and runs at most once per local day; an
  exclusive test (`test.exclusive`) holds the windows off and ends manual switches, and only an exclusive run
  measures or judges. A manual on/off (`App::SetManual`) overrides the demands until the device's demands or
  the effective level change, or an exclusive test starts; then it is automatic again.
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
```

Coverage output: `out/coverage/coverage/report.txt` and `html/index.html`. The compilation database for
clang tooling can be generated with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` on a Ninja/clang-cl configure.

## Board

Waveshare ESP32-S3-Relay-6CH: relays GPIO 1, 2, 41, 42, 45, 46 (HIGH = coil energised), buzzer GPIO 21
(LEDC), WS2812 GPIO 38 (RGB byte order), BOOT GPIO 0, RS485 UART1 TX 17 / RX 18 (auto direction). Console on
the native USB-Serial-JTAG; a terminal must not toggle DTR/RTS. Partitions in `firmware/partitions.csv`: two
3 MB OTA slots and four raw log partitions (`loga` 5 MB, `logb` 4 MB, `loge`, `logd` 256 KB each).

## Serial diagnostics

`idf.py monitor` holds the port; nothing else can open COM3 while it runs. Flashing over USB needs no password;
OTA over HTTP (`POST /api/ota`) needs the password and maintenance mode, and a pushed image must run for
2 minutes with a healthy probe before it confirms itself — a second push before that is refused.
