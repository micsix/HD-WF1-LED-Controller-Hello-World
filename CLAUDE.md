# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF firmware for a **Huidu HD-WF1** control card (ESP32-S2, 4MB flash, no
PSRAM) driving a 128×64 HUB75E RGB LED matrix panel. It renders a large set of
MQTT-triggered patterns/animations/effects (scrolling text, games, clocks,
network-latency visualizations, mazes, etc.) — see the topic table in
`README.md` for the full, current list; don't try to re-derive it from memory,
it changes often.

## Build / flash / monitor

```sh
source ~/esp/esp-idf-v6.0.1/export.sh   # or v5.4.1 - both work unmodified
idf.py set-target esp32s2
idf.py build
```

The board has **no auto-reset circuit** — before every flash, bridge the two
small pads next to the micro-USB connector to force it into the ROM
bootloader, connect via the **USB-A port** (not micro-USB, which is
power-only), then unbridge and power-cycle afterward to boot the flashed app.
Full procedure, plus how to tell bootloader-vs-app state without triggering a
reset, is in `README.md` under "Flashing".

```sh
idf.py -p /dev/ttyACM0 flash      # port varies; check `ls /dev/ttyACM*`
```

**`idf.py monitor` does not work**: the console is UART, not USB (the ROM USB
console crashed at 128×64). Read logs from the `applog` flash partition
instead (see "Memory budget and debugging without a console" below). Also: the app
doesn't enumerate on USB, so `lsusb` showing `303a:0002` means the board is in
the ROM bootloader (pads bridged); after a flash it may re-enumerate as a new
`/dev/ttyACM*` number.

If `idf.py build` says the build directory "was configured for project X not
Y", build elsewhere with `idf.py -B <dir> ...` (or `fullclean` if you don't
mind rebuilding) - `build/` may point at an old checkout path.

There is no test suite and no linter configured for this project.

### Credential-safe build/flash output

`idf.py build`/`flash` print Kconfig defaults verbosely, which leaks
WiFi/MQTT credentials (`CONFIG_WIFI_PASSWORD`, `CONFIG_MQTT_BROKER_URI`, etc.)
into terminal output/logs. Always wrap these commands:

```sh
export KCONFIG_REPORT_VERBOSITY=quiet && idf.py build 2>&1 | grep -v -i "wifi_ssid\|wifi_password\|mqtt_broker_uri\|default value"
export KCONFIG_REPORT_VERBOSITY=quiet && idf.py -p /dev/ttyACM0 flash 2>&1 | grep -v -i "wifi_ssid\|wifi_password\|mqtt_broker_uri\|default value"
```

### Kconfig changes require a reconfigure

Editing `main/Kconfig.projbuild` (adding/renaming/removing a `config`) is
**not** picked up by a plain `idf.py build` if `sdkconfig` already exists —
CMake only regenerates `sdkconfig`/`sdkconfig.h` on its own config-check
heuristics, which can miss a Kconfig-only edit. Run `idf.py reconfigure`
first (harmless no-op if nothing changed) whenever a build fails with an
undeclared `CONFIG_*` symbol right after a Kconfig edit. Also note: renaming
a `config` symbol resets its stored value in `sdkconfig` to the new default —
if you're preserving existing behavior across a rename, manually carry the
old value over into the new key.

All secrets/config live only in `sdkconfig` (gitignored, machine-local) —
`sdkconfig.defaults` is the checked-in, secret-free baseline that seeds new
checkouts.

## Architecture

### One giant `main/main.cpp`

There is a single active source file, ~4500 lines, selected by
`CONFIG_HUB75_BACKEND_ARDUINO_GFX` in `sdkconfig.defaults` (misleadingly
named — it's neither Arduino nor GFX; built with `ESP32_HUB75_USE_GFX=n`).
`main_native.cpp`/`font5x7.{h,cpp}` are dead code from an earlier
`esphome/esp-hub75` attempt that initializes but produces zero physical
output on this board — kept only in case that library gets fixed upstream.
Don't "clean up" that dead code without checking `README.md`'s dead-end note.

### `app_main()` is one giant render loop

Almost everything lives in `app_main()`: hardware/driver init, all per-effect
state (as plain local variables — dozens of maze/ray-tracer/game/animation
state blocks all coexist in one stack frame), all the per-effect drawing
lambdas, and a `while (true)` loop ticking every ~30ms that reads
`display_mode` and dispatches through one big `switch` to whichever effect's
draw lambda. `CONFIG_ESP_MAIN_TASK_STACK_SIZE` (`sdkconfig.defaults`) is
now 10240 (it was 16384; 8192 overflowed once before) - check
`main_stack_hwm` in the applog after adding local state. Prefer `static`
locals for large fixed-size arrays so they don't bloat this frame - **but
`static`/`.bss` and task stacks come out of the same heap the display and
WiFi need** (see "Memory budget" below), so size them for what's actually
used, not the maximum.

**Render loop draw conventions** (single-buffered, so anything drawn is
immediately scanned out):
- Static screens (solid colours, `hub75/1300*`, `off`) are drawn once and
  skipped until their content changes (`last_static_key` in the loop). Add new
  static modes to that `static_mode` list.
- `maze1`-`3` skip the per-frame `clearScreen()`, overdraw identical wall
  pixels, and erase only the previous dot pixels (`maze_dot_prev_*`); a new
  maze (`generate_maze()`) or mode change does one full clear. Other animated
  effects still clear + redraw every frame and can flicker slightly.
- `flipDMABuffer()` is a no-op (double buffering is off).

### Cross-task state: mutex + snapshot, never block the render loop

`display_mutex` guards every piece of state shared between `app_main()`'s
render loop and any other task (MQTT event task, WiFi/NTP callbacks,
background ping tasks). The established pattern, used everywhere:

1. A writer (MQTT dispatch, a background task) takes the mutex, mutates
   shared globals, releases it.
2. Each render-loop tick takes the mutex once, copies out everything it
   needs into local snapshot variables (`local_ping1_graph_ms`,
   `local_clock_tz`, etc.), releases it, then draws using only the local
   copies for the rest of that tick.

Anything that can be slow or block (network I/O — ICMP/TCP pings, DNS
resolution) **must** run in its own background FreeRTOS task
(`ping1_task`, `ping2_task`, `maze4_task`, `network_init_task` — all spawned
early in `app_main()` via `xTaskCreate`), never inline in the render loop.
Each such task naps (`vTaskDelay`) and re-checks `display_mode` when its mode
isn't active, and drops its result into mutex-protected shared state (either
continuously-updated, like `ping1_graph_ms`, or as a one-shot "trigger" flag
like `ping2_launch_pending` that the render loop consumes and clears) for the
render loop to pick up on its own smooth cadence. This split is why effects
stay visually smooth regardless of how slow/flaky the network is — don't
break it by adding a network call directly inside a render-loop draw lambda.

### Lean build: `HUB75_FULL_EFFECTS`

`HUB75_FULL_EFFECTS` (top of `main.cpp`, default `0`) gates the heap-costly
extras: subscriptions for the games/ray tracer/matrix/flags/elevator/auto-test/
ping/`maze4`/`maze5` topics, the `ping1`/`ping2`/`maze4` task creation, and the
maze breadcrumb arrays. Their code is still compiled, just unreachable. Only
message/static text/solid colours/dim-bright/clocks/`shootingstars`/`time`/
`maze1`-`3`/`1300*` are live. Setting it to `1` restores everything but eats
~25 KB of heap. A new topic that should be live in the lean build must not be
inside a `HUB75_FULL_EFFECTS` block in the subscribe list.

### Adding a new `hub75/<topic>` MQTT command

Follow the existing convention exactly (grep any existing topic, e.g.
`kMaze1Topic` or `kFixed1300LTopic`, for the full set of touch points): a `DisplayMode` enum value,
a `constexpr const char *kXTopic` constant, a subscribe call, an
`else if (topic_matches(...))` dispatch branch calling `set_test_mode()` (or
a custom setup block if the mode needs one-time state reset), and a `case` in
the render loop's big `switch`. Update `README.md`'s topic table and the
catch-all "replaces whatever was showing" topic list in the same change —
they're the actual user-facing documentation and go stale easily.

### Ping infrastructure (`hub75/ping1`, `hub75/ping2`, `hub75/maze4`/`maze5`)

(Disabled in the default lean build - the tasks aren't started; needs
`HUB75_FULL_EFFECTS=1`.)

`ping_target_once_ms()` is the single entry point every effect uses; it
picks a random target from `CONFIG_PING_TARGETS` (menuconfig, up to 8
`host[:port]` entries), resolves it (`resolve_target_ipv4()`, works for both
literal IPs and hostnames via `getaddrinfo()`), then times either an ICMP
echo or a TCP connect handshake depending on `CONFIG_PING_METHOD_*`
(build-time choice). Reuse this rather than adding a new ping path.

## Memory budget and debugging without a console (128×64)

The project is heap-limited: no PSRAM, ~139-163 KB internal heap at boot. The
display framebuffer is single-buffered 6-bit (`double_buff=false`,
`setPixelColorDepthBits(6)`, 49 KB) with `min_refresh_rate=90`; WiFi buffers
are trimmed in `sdkconfig.defaults`. Known failure modes, all seen for real:
- Double buffering or 8-bit depth → `begin()` "Not enough memory", or WiFi init
  `ESP_ERR_NO_MEM` (257), or WiFi stuck at "Not Connected, Retrying" with
  `reason=2/202/205` (heap ~5 KB). Check the `heap:` lines in the applog
  (`free`/`min_free`/`largest`) before blaming WiFi credentials - they're baked
  in from `sdkconfig` and were fine.
- Console on the ROM USB CDC (`CONFIG_ESP_CONSOLE_USB_CDC`) → reboot loop, crash
  in `esp_usb_console_interrupt`, USB enumeration errors `-71`/`-110`. Root
  cause unidentified (stable at 64×32 and at 128×64 2-bit; crashes at 4/6/8-bit);
  console is UART now. Don't switch it back without re-testing.

**Reading logs from a board you can't attach to:** every `ESP_LOG` line is also
appended to the 64K `applog` partition (0x120000) by a hook in `main.cpp`,
along with `heap:`/`main_stack_hwm` stats every 5 s. With the pads bridged:
`python -m esptool --chip esp32s2 -p /dev/ttyACM0 --after no-reset read-flash
0x120000 0x10000 applog.bin` (use `--after no-reset` so it stays in the
bootloader; erase the region before a fresh run). Crash dumps: read the
`coredump` partition at 0x110000 and decode with `esp-coredump info_corefile
--core core.bin --core-format raw build/hub75_hello.elf`. Full details are in
`README.md` ("Serial console / logs" and "128×64 bring-up notes").

## Hardware notes

- **Panel size**: the panel is 128×64 (`PANEL_WIDTH`/`PANEL_HEIGHT` in
  `main/main.cpp`; it was 64×32 originally, so older comments/README text may
  still mention 64×32). Most layout code derives from those macros; fixed-pixel
  sprites/fonts are the same size as before. `partitions.csv` also has the
  `applog` (0x120000, 64K) partition.
- `partitions.csv` is a custom layout (1M factory app, 64K coredump); the
  `RootCause.md` file at the repo root holds the detailed bring-up debugging
  notes referenced by `README.md`.

- **Pinout**: reverse-engineered by the community, but the **R/G/B channels
  are re-pinned per panel** (confirmed via solid-colour tests): the current
  128×64 panel needed R=3/5, G=6/8, B=2/4 (source order lit red→blue,
  green→red, blue→green); the old 64×32 panel only needed G/B swapped. If
  colours look wrong after a panel change, fix the pin `#define`s in
  `main/main.cpp`, not the drawing code — see the Pinout section of
  `README.md`.
- Full bring-up war story (bootloader strap confusion, a dead HUB75 library,
  the color-channel swap, a power-supply issue that looked like a firmware
  bug, a clock-integrity glitch) is in `README.md`'s "Root cause history" —
  read it before assuming a display symptom is a new bug rather than a
  known class of issue.

## Vendored library patches are not durable

`managed_components/esp32-hub75-matrixpanel-dma/` (fetched by the ESP-IDF
component manager from `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA`) is
**patched in place** to compile under ESP-IDF v6 — that upstream repo only
supports up to v5.x as shipped. These patches are silently lost if
`managed_components/` or `dependencies.lock` is deleted, or if
`idf.py update-dependencies` is run. See `README.md` for the exact patch
list and the (currently blocked) plan to fork upstream instead.
