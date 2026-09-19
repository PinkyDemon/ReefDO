# AGENTS.md

Facts an agent needs to work in this repository. The README covers usage; this covers the rules.

## Shape

- `core/` is a hardware-independent C++23 library: no ESP-IDF headers, no exceptions, no RTTI, no heap in the
  sampling path, fixed-size containers (`FixedVector`, `FixedString`). Hardware enters through interfaces:
  `probe::IUart`, `probe::IProbe`, `log::IBlockStore`, `api::ISink`. Time is always passed in (`app::Clock`).
- `core/src/config.cpp` is the only file that includes ArduinoJson.
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
- The ladder (`core/src/ladder.cpp`) decides device states, whether the buzzer may sound, and the LED colour;
  the firmware only renders. Config decides buzzer patterns; LED codes are fixed in `indicator.cpp`.
- Blue is silent unless configured; Normal is always silent; FAULT acts as `fault.level` (Red by default).
- Device demand is the OR of the ladder, the service run (`service.cpp`) and the boost (`boost.cpp`);
  the boost yields to the other two and runs at most once per local day.
- Writes over HTTP need Basic auth; reads are open. Never log or store the password anywhere but NVS.
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
