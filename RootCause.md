# Root Cause History

How this went from "nothing displays" to a working "Hello, world!" scroll,
in the order the bugs were found and fixed.

1. **Board kept re-entering the ROM bootloader** instead of booting the
   flashed app (the download-mode pad-bridge strap was sometimes still
   held across a power-cycle). Until this was ruled out — by probing with
   `esptool --before no-reset --after no-reset chip-id` and checking
   whether it connected instantly (bootloader) or timed out (app
   running) — it masked every other symptom as "nothing works."

2. **`esphome/esp-hub75`** (a modern, pure ESP-IDF HUB75 library)
   initializes cleanly and reports success on this board, but produces
   **zero physical output** — a real bug/incompatibility on ESP32-S2, not
   a config issue. Ruled out by exhausting its config surface (shift
   driver, latch blanking, brightness, scan wiring) with no change.
   `main_native.cpp`/`font5x7.{h,cpp}` are the leftover code from this
   attempt, kept as dead code in case that library ever gets fixed
   upstream.

3. **`mrcodetastic/ESP32-HUB75-MatrixPanel-DMA`** (the library actually
   proven on this board, per the community's own WF1 reference firmware)
   was the fix for #2 — but it uses the legacy `driver/i2s.h` API. That
   header was fully removed in ESP-IDF v6.0, so it only built at all under
   **v5.4.1** at first.

4. Even under v5.4.1 with a working driver, only **red** displayed
   initially — full-white showed as red-only, not white. Solid-color
   tests (pure green → all blue, pure blue → all green) proved it wasn't
   dead pins, it was a real color-channel mismatch: **G1/B1 and G2/B2
   swapped** relative to the community-sourced pin reference for this
   specific panel. Swapping those two GPIO pairs in software (not the
   library code) fixed it completely.

5. **Ported the library forward to ESP-IDF v6.0.1** (this project's final,
   current state — see `README.md` for the working build/flash
   instructions). This was a header/API-surface port, not a
   hardware-behavior risk: the underlying I2S register layout
   (`soc/i2s_struct.h`) is byte-identical between v5.4 and v6.0.1, and
   everything the library needed still existed in v6.0.1, just relocated:
   - `soc/i2s_periph.h` → `hal/i2s_periph.h` (moved to the new
     `esp_hal_i2s` component)
   - `driver/i2s_types.h` now lives in its own `esp_driver_i2s` component
     (the old monolithic `driver` component no longer pulls it in)
   - `esp_private/periph_ctrl.h` version-gate (`ESP_IDF_VERSION_MAJOR ==
     5` → `>= 5`) needed to also cover v6
   - `gpio_matrix_out` / `gpio_pad_select_gpio` renamed to
     `rom_gpio_matrix_out` / `rom_gpio_pad_select_gpio` (aliased back via
     `#define` for v6)
   - `PERIPH_I2S0_MODULE` was removed from ESP32-S2's peripheral list in
     v6; replaced with the modern
     `PERIPH_RCC_ATOMIC() { i2s_ll_enable_bus_clock(0, true);
     i2s_ll_reset_register(0); }` pattern, matching how IDF's own
     `esp_driver_i2s/i2s_platform.c` does it now.
   - `CMakeLists.txt` needed `esp_driver_i2s` and `esp_hal_i2s` added to
     its component dependencies for IDF ≥ 6.

   Confirmed working on real hardware after the port — same scrolling
   green "Hello, world!" as under v5.4.1.

## Current state

Building and flashing now happens entirely under **ESP-IDF v6.0.1**
(matching the other projects on this machine). See `README.md` for
instructions, pinout, and the flashing procedure (the board still needs
its download-mode pads bridged manually — no auto-reset circuit).

The v6.0.1 patches to `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` currently
live only in `managed_components/`, which the ESP-IDF component manager
can silently overwrite (see the caveat in `README.md`). Forking the
library with these patches committed was requested but is blocked on
GitHub access from this environment.
