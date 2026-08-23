# HD-WF1 HUB75 "Hello, world!"

### Hardware used for this project:

https://www.aliexpress.us/item/2255801119828944.html

https://www.aliexpress.us/item/2255799816372142.html


ESP-IDF firmware that scrolls "Hello, world!" across a HUB75 RGB LED matrix
panel connected to a **Huidu HD-WF1** control card (ESP32-S2, 4MB flash, no
PSRAM, single HUB75E port).

Builds and runs on **ESP-IDF v6.0.1** (also works, unmodified, on v5.4.1).

## Quick start

```sh
source ~/esp/esp-idf-v6.0.1/export.sh
idf.py set-target esp32s2
idf.py build
```

To flash, the board must first be put into the ROM bootloader (see
[Flashing](#flashing) below), then:

```sh
idf.py -p /dev/ttyACM1 flash
```

## Hardware

- **Board**: Huidu HD-WF1 — ESP32-S2, HUB75E connector, RTC (BM8563), no
  auto-reset circuit (see [Flashing](#flashing)).
- **Panel**: 64×32 HUB75E, FM6126A-driven (confirmed by testing, see
  [Root cause history](#root-cause-history)).

### Pinout

The onboard HUB75E pin-to-GPIO mapping was reverse-engineered by the
community (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`), **except G1/B1
and G2/B2, which are swapped here** — empirically confirmed wrong on this
physical board via solid-color tests (pure green data lit up blue LEDs and
vice versa; red was correct). See `main/main.cpp` for the exact `#define`s.

| Signal | GPIO |
|---|---|
| R1 / R2 | 2 / 4 |
| G1 / G2 | 3 / 5 |
| B1 / B2 | 6 / 8 |
| A / B / C / D / E | 39 / 38 / 37 / 36 / 12 |
| LAT / OE / CLK | 33 / 35 / 34 |

If you swap in a different panel, the G1/B1 pair may need to be swapped
back — this looks like a per-panel wiring quirk, not a WF1 board defect.

## Flashing

The HD-WF1 has no auto-reset circuit — you must manually put it into the
ROM bootloader before every flash:

1. **Bridge the two small pads next to the micro-USB connector** (short
   them — a jumper wire or tweezers works).
2. Connect the board via its **USB-A port** (not the micro-USB — that's
   power-only) with a USB-A-to-USB-A cable.
3. `idf.py -p /dev/ttyACM* flash`
4. **Unbridge the pads and power-cycle** (unplug/replug) to boot the
   flashed app normally.

To check whether the board is actually running the app vs. still stuck in
the bootloader, probe it without triggering a reset:

```sh
python -m esptool --chip esp32s2 -p /dev/ttyACM1 --before no-reset --after no-reset chip-id
```

If this connects instantly, the board is still in the bootloader (strap
still held). If it times out with "No serial data received", the app is
running.

### Serial console

ESP32-S2 has no USB_SERIAL_JTAG peripheral, so the console runs over the
ROM's USB-CDC driver (`CONFIG_ESP_CONSOLE_USB_CDC=y` in
`sdkconfig.defaults`) — same USB-A port used for flashing:

```sh
idf.py -p /dev/ttyACM1 monitor
```

## Project layout

```
main/
  main.cpp            — active firmware: pin config, FM6126A init, built-in
                         5x7 font renderer, scroll loop
  idf_component.yml    — pulls esp32-hub75-matrixpanel-dma (git) +
                         esphome/esp-hub75 (unused by main.cpp, kept as a
                         documented dead end — see below)
  main_native.cpp,
  font5x7.{h,cpp}       — dead code: an earlier attempt using
                         esphome/esp-hub75, a pure-ESP-IDF HUB75 driver.
                         It initializes successfully but produces zero
                         physical output on this board (real bug in that
                         library, not a config issue). Left in the tree
                         since the font renderer code is reusable if that
                         library ever gets fixed upstream.
managed_components/
  esp32-hub75-matrixpanel-dma/  — mrcodetastic/ESP32-HUB75-MatrixPanel-DMA,
                         patched locally for ESP-IDF v6 compatibility (see
                         below). ⚠️ Not tracked by git — see caveat below.
```

The active/inactive backend is selected by `CONFIG_HUB75_BACKEND_ARDUINO_GFX`
in `sdkconfig.defaults` (misleadingly named — it's neither Arduino nor GFX;
built with `ESP32_HUB75_USE_GFX=n`, so no Adafruit-GFX/Arduino dependency).

## ⚠️ Vendored library patches are not durable

`managed_components/esp32-hub75-matrixpanel-dma/` is fetched by the ESP-IDF
component manager from `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` and
**patched in place** to compile under IDF v6 (that upstream repo only
supports up to v5.x as shipped). These patches will be **silently lost** if
`managed_components/` or `dependencies.lock` is deleted, or if
`idf.py update-dependencies` is run.

**TODO**: fork the upstream repo, commit these patches, and point
`main/idf_component.yml`'s `git:` URL at the fork instead of upstream — this
was in progress but blocked on GitHub access from this environment (no `gh`
CLI auth, no PAT; SSH access confirmed working as GitHub user `micsix`).

The IDF v6 patches themselves, for reference:

- `esp32_i2s_parallel_dma.hpp` / `.cpp`: version-gate `ESP_IDF_VERSION_MAJOR
  == 5` → `>= 5` (already avoids removed legacy `driver/i2s.h`); route
  `soc/i2s_periph.h` → `hal/i2s_periph.h` for v6 (moved to the new
  `esp_hal_i2s` component); `esp_private/periph_ctrl.h` version-gate fix;
  `gpio_matrix_out`/`gpio_pad_select_gpio` aliased to their new
  `rom_`-prefixed names; `PERIPH_I2S0_MODULE` (removed from ESP32-S2's
  peripheral list in v6) replaced with
  `PERIPH_RCC_ATOMIC() { i2s_ll_enable_bus_clock(0, true);
  i2s_ll_reset_register(0); }`, matching IDF's own `i2s_platform.c`.
- `CMakeLists.txt`: added `esp_driver_i2s` and `esp_hal_i2s` to the
  component's dependencies for IDF ≥ 6 (v6 split these out of the old
  monolithic `driver` component).

The underlying I2S register layout (`i2s_struct.h`) is byte-identical
between v5.4 and v6.0.1, which is why this was a header/API-surface port
rather than a hardware-behavior risk.

## Root cause history

Getting from "nothing displays" to working took several real, distinct
bugs — worth knowing if you're touching this board/library combo again:

1. **Board kept re-entering the ROM bootloader** instead of booting the
   app (the pad-bridge strap was still held on some power-cycles) — until
   ruled out, this masked every other symptom as "nothing works."
2. **`esphome/esp-hub75`** (a modern, pure ESP-IDF HUB75 library)
   initializes cleanly and reports success on this board, but produces
   **zero physical output** — a real bug/incompatibility on ESP32-S2, not
   a config issue. This is why `main_native.cpp` is dead code.
3. **`mrcodetastic/ESP32-HUB75-MatrixPanel-DMA`** (the library actually
   proven on this board) uses the legacy `driver/i2s.h` API, fully removed
   in ESP-IDF v6.0 — required porting (see patches above).
4. Even with a working driver, only **red** displayed initially. Solid-color
   tests (white→red-only, green→blue, blue→green) proved it wasn't dead
   pins — G1/B1 and G2/B2 were swapped relative to the community pin
   reference for this exact panel. Swapping those two GPIO pairs in
   software fixed it completely.
