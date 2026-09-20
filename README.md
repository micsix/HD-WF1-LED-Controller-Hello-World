# HD-WF1 HUB75 "Hello, world!"

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
- **Panel**: 128×64 HUB75E (1/32 scan, uses the E address line on GPIO12;
  originally a 64×32 panel), FM6126A-driven (confirmed by testing, see
  [Root cause history](#root-cause-history)). The bigger panel is memory-hungry
  on a no-PSRAM ESP32-S2 - see [128×64 bring-up notes](#12864-bring-up-notes).

### Pinout

The onboard HUB75E pin-to-GPIO mapping was reverse-engineered by the
community (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`), **but the colour
channels are re-pinned here to match the physical panel** - empirically
confirmed with solid colours. The original 64×32 panel only needed G/B
swapped. With the current 128×64 panel, the source order lit the wrong LEDs
(red data → blue, green data → red, blue data → green), so R/G/B now use the
GPIOs shown below. It's a per-panel LED-order quirk, not a board defect;
re-check it (send `hub75/red`, `green`, `blue`) whenever the panel changes.
See `main/main.cpp` for the exact `#define`s.

| Signal | GPIO |
|---|---|
| R1 / R2 | 3 / 5 |
| G1 / G2 | 6 / 8 |
| B1 / B2 | 2 / 4 |
| A / B / C / D / E | 39 / 38 / 37 / 36 / 12 |
| LAT / OE / CLK | 33 / 35 / 34 |

### Onboard button

GPIO 11, active-low (internal pull-up, reads LOW when pressed) — per
community documentation (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`),
confirmed working on this physical unit. Currently only used to advance
`hub75/standardtest1` to its next phase on a short press; no effect in any
other mode.

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
the bootloader: the app no longer enumerates on USB (its console is UART, see
below), so **`lsusb` showing `303a:0002` means the board is in the ROM
bootloader** and nothing showing means the app is running (or the pads are
unbridged and it hasn't been power-cycled). (Older builds used the ROM USB-CDC
console, where both states showed up as `303a:0002` - don't rely on that ID
alone with those.) You can also probe it without triggering a reset:

```sh
python -m esptool --chip esp32s2 -p /dev/ttyACM1 --before no-reset --after no-reset chip-id
```

If this connects instantly, the board is still in the bootloader (strap
still held). If it times out with "No serial data received", the app is
running.

### Serial console / logs

The console is on **UART** (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`), not the
ROM USB-CDC driver. At 128×64 the ROM USB console interrupt crashed
(`esp_usb_console_interrupt`, LoadProhibited) as soon as the display DMA
started, reboot-looping the board - see the
[bring-up notes](#12864-bring-up-notes). So `idf.py monitor` over the USB-A
port **no longer works**.

Instead every `ESP_LOG` line is also appended to a 64K flash partition,
`applog` (0x120000), by a small hook + low-priority task in `main.cpp`. It
also logs heap stats every 5 s (`free`, `min_free`, `largest`, `dma_free`,
`main_stack_hwm`), which is how the memory problems below were found. Read it
with the board in bootloader mode (pads bridged, power-cycled):

```sh
python -m esptool --chip esp32s2 -p /dev/ttyACM0 --after no-reset \
    read-flash 0x120000 0x10000 applog.bin
strings applog.bin | less                    # appends across boots; wraps when full
python -m esptool --chip esp32s2 -p /dev/ttyACM0 --after no-reset \
    erase-region 0x120000 0x10000            # clear it before a fresh test run
```

(Use `--after no-reset` so the board stays in the bootloader between reads;
after a normal flash it re-enters the bootloader and may re-enumerate as a
different `/dev/ttyACM*` number.) Crash dumps go to the `coredump` partition
(0x110000) and decode with
`esp-coredump info_corefile --core core.bin --core-format raw build/hub75_hello.elf`.

### Boot splash

For the first 10 s after power-on the panel shows up to four lines of text
(intended for owner contact info) in yellow on black, using the 3×5 small font
(1px strokes). The text comes from `CONFIG_SPLASH_LINE1`..`4` (menuconfig →
"HD-WF1 Boot Splash"); **keep the real values in `sdkconfig`, which is
gitignored - not in `sdkconfig.defaults`** - and leave all four empty to
disable the splash. It is an overlay: WiFi, NTP and MQTT run concurrently
underneath (WiFi still starts ~5 s after boot, see `network_init_task()`), and
when it ends the panel shows whatever is current, normally the WiFi status
screen. A single green pixel in the lower right corner lights if WiFi gets an
IP during the splash and goes away with it. (Supported glyphs are limited to
what `small_font_glyph()` defines - add any missing letters there.)

## WiFi & MQTT

Both are optional and off by default. Configure via `idf.py menuconfig`:

- **HD-WF1 WiFi** — SSID/password. Leave the SSID empty to skip WiFi
  entirely (the panel just shows the default scrolling message).
- **HD-WF1 MQTT** — broker URI/username/password/client ID, plus the status
  and message topic names (defaults below). Leave the broker URI empty to
  skip MQTT (requires WiFi to be configured too).
- **HD-WF1 NTP** — primary/secondary NTP server hostnames for `hub75/clock` +
  `hub75/ntpset` (default `pool.ntp.org` + none). Requires WiFi to be
  configured too.
- **HD-WF1 Ping** — up to 8 targets pinged by `hub75/ping1` and
  `hub75/ping2`, plus how they're pinged:
  - **Ping targets** — comma-separated list (default just `192.168.92.1`).
    Each entry is `HOST` or `HOST:PORT` - `HOST` is a literal IPv4 address
    or a hostname (resolved via DNS at ping time), `PORT` is optional and
    only used in TCP mode (falls back to *Default TCP ping port* below).
    Example: `192.168.92.1,example.com:8080,10.0.0.5:22`. Every sample
    picks one target at random from the list - a random round-robin across
    all of them, not a fixed sequence.
  - **Ping method** — ICMP echo (the default) or TCP connect ("tcpping" -
    times a TCP handshake instead, for targets/firewalls that drop ICMP but
    allow a specific port; a closed port counts the same as a timeout).
  - **Default TCP ping port** — used for any target above that doesn't
    specify its own port (default 80).

Credentials are stored only in `sdkconfig`, which is gitignored — never
committed.

The panel starts dim by default (as if `hub75/dim` had already been sent) -
the very first thing it draws comes up gentle rather than at full
brightness; send `hub75/bright` at any time to go to 100%. Before WiFi
starts, the panel briefly shows the previous boot's reset reason ("RR=n").
While connecting, it then shows live WiFi status ("Not Connected" /
"Retrying..." / attempt count in red, then "Connected" + IP in green/yellow +
"Topic: hub75/" in white) until the first MQTT message arrives, after which
it switches to showing MQTT-driven content permanently. These displays use a
compact 3px-wide font and are centered statically (no scrolling) since
everything fits on the panel at once.

### MQTT topics

All topics default to the `hub75/` prefix shown below; the status and
message topic names are configurable in menuconfig, the rest are fixed.
Payload content doesn't matter for topics marked "any" - the topic alone
triggers the action.

> **Lean build (default):** to save heap on the 128×64 panel only these are
> subscribed: `message`, `1300`/`1300a`-`f`/`1300L`/`1300LI`/`13`/`00`, the solid colours,
> `off`, `dim`/`bright`, `flash`/`noflash`, `shootingstars`, `time`, `maze1`-`3`,
> and the clock/`ntpset` topics. Everything else below (games, ray tracer,
> matrix, flags, elevator, auto-test, ping effects, `maze4`/`maze5`) is still in
> the source but not subscribed, and the ping/`maze4` tasks aren't started. Set
> `HUB75_FULL_EFFECTS` to `1` at the top of `main/main.cpp` to get them all back
> (and expect to fight for heap again). Descriptions below were written for the
> 64×32 panel - fixed-size sprites/text are the same pixel size on 128×64,
> while fills, mazes, grids and centered layouts use the whole panel.

### REST API

Everything in the topic table below is also available over HTTP (menuconfig →
"HD-WF1 REST API": enable, port (default 80), optional bearer token; starts
after WiFi connects, costs a few KB of heap):

```sh
curl -X POST http://<device-ip>/api/command/red
curl -X POST http://<device-ip>/api/command/message -d '{"text":"hello","color":"#00ff00"}'
curl -X POST http://<device-ip>/api/command/1300L -d '{"fg":"#ffffff"}'
curl http://<device-ip>/api/status        # mode, uptime, heap, NTP state
```

`<name>` is the topic without the `hub75/` prefix (`message` maps to the
configured message topic); the body is the MQTT payload. Unknown/disabled
topics return 404. With a token set, add `-H "Authorization: Bearer <token>"`.
Both paths share `dispatch_command()` in `main.cpp`, so new topics added there
work over REST automatically.

| Topic | Payload | Effect |
|---|---|---|
| `hub75/status` | *(published by device)* | Retained `online` on connect / `offline` (LWT) on ungraceful disconnect. |
| `hub75/message` | `{"text": "...", "color": "#RRGGBB"}` | Scrolls `text` in `color` (optional, defaults to green). |
| `hub75/test1` | any | One random pixel per ~210ms, smoothly fading in and out (no hard blink/dark gap), continuously. |
| `hub75/test2` | any | Animated 10-bar vertical bar graph, one fixed color per bar, random heights refreshed ~4x/sec. |
| `hub75/test3` | any | 8x8 pacman sprite chomping its way around the panel perimeter. |
| `hub75/rainbow` | any | One random solid color per row (32 horizontal lines), regenerated fresh each time this fires. |
| `hub75/ray1` | any | Real-time ray-traced scene: one sphere over a checkerboard floor, with a point light continuously orbiting and casting a real shadow. Diffuse + specular (highlight) shading on the sphere. Sphere and floor colors are randomized fresh each time this fires. |
| `hub75/ray2` | any | Same ray tracer, with a second sphere side by side with the first (both cast/receive shadows from each other and the shared light). Colors randomized fresh each time this fires. |
| `hub75/off` | any | Blanks the panel. |
| `hub75/1300` | any | Static "1300", grey on black, at the largest integer scale of the built-in 8x8 font that still fits all 4 characters on the 64px-wide panel (scale 2 - same 16x16-per-glyph size as `hub75/2L`'s elevator digits). Both zeroes render unslashed (the font's own '0' is slashed to distinguish it from 'O' in running text; not needed here). The "1" and "3" are nudged a few pixels right of their plain-string position (the "1" more than the "3"); "00" is unshifted. |
| `hub75/1300a` | any | Same "1300" (same size, same digit positioning) as `hub75/1300`, but inverted: black text on a grey background - the background is fixed at 25% of full white intensity, independent of the panel's global `hub75/dim`/`hub75/bright` brightness setting. |
| `hub75/1300b` | any | Same as `hub75/1300a`, but the background is 50% of full white intensity instead of 25%. |
| `hub75/1300c` | any | "1300" again, but in the compact 3px-wide status font (the one used for the boot/WiFi-status screens), white on black, at that font's own largest integer scale that still fits on the 64px-wide panel (scale 4 - 60px wide, 20px tall, actually taller than `hub75/1300`'s family despite the narrower base font). The white is 50% of full intensity. |
| `hub75/1300d` | any | Same as `hub75/1300c`, but inverted: black text on a white (50%-intensity) background. |
| `hub75/13` | any | Just "13", white (50%-intensity) on black, in the same small font as `hub75/1300c`/`hub75/1300d` at a fixed dot scale of 2 - "2 pixel wide" describes the rendered stroke thickness, not the font's column count. |
| `hub75/00` | any | Same font/size/color as `hub75/13`, but "00" - the small font's `0` glyph has a real hollow center. |
| `hub75/1300e` | any | Exactly `hub75/1300c`, but at a fixed dot scale of 2 instead of 4 - `hub75/1300c`'s original, un-maximized size, kept around as its own topic. |
| `hub75/1300f` | any | Exactly `hub75/1300d`, but at a fixed dot scale of 2 instead of 4 - the inverted (black-on-grey) counterpart to `hub75/1300e`. |
| `hub75/1300L` | optional JSON `{"fg":"#RRGGBB","bg":"#RRGGBB","hue":"#RRGGBB"}` | Static "1300" in the Chilanka font (SIL OFL), stretched to fill the entire 128x64 panel edge to edge (a ~1.3x vertical stretch of the font's natural shape), anti-aliased and blended in linear light through the display library's CIE curve. `fg` = digit colour (default `#ffffff`), `bg` = background (default `#000000`), `hue` = a per-channel tint multiplied into both (default `#ffffff` = none; e.g. `#ffd080` reduces blue to compensate a blue-heavy panel). Every field is optional, and an empty/non-JSON payload gives white on black. Pre-rendered offline by `tools/gen_chilanka_1300.py` into `main/chilanka_1300.h`. Example: `mosquitto_pub -t hub75/1300L -m '{"fg":"#000000","bg":"#d2b48c","hue":"#ffe0a0"}'`. |
| `hub75/1300LI` | any | `hub75/1300L` inverted: the same Chilanka "1300", but black digits on a "Tan" (CSS `#D2B48C`, RGB 210/180/140) background instead of black on white; edge pixels are blended per channel and re-corrected for the CIE curve. A lit light-coloured 128x64 panel is still a heavy supply-current load - see the "Root cause history" power-supply entry. |
| `hub75/standardtest1` | any | Fixed, repeatable diagnostic sequence for comparing color/brightness behavior across firmware changes: white/red/green/blue, each at `hub75/dim` then `hub75/bright`, ~2s/phase, looping. Always restarts at phase 1 (white dim) when triggered. The current phase number (1-8) is shown in the top-left corner in black (LEDs off), readable against any of the fill colors. A short press of the onboard button (see [Onboard button](#onboard-button)) advances to the next phase immediately instead of waiting out the timer. |
| `hub75/si` | any | Self-playing, stylized Space Invaders: a formation sweeps side to side and steps down on each wall hit; the player cannon autonomously tracks the lowest surviving invader and auto-fires. The wave respawns when cleared or once it gets too close to the player. |
| `hub75/mi` | any | Self-playing, stylized Millipede: a segmented chain weaves horizontally, reversing and stepping down on hitting a wall or one of several scattered mushrooms, wrapping back to the top once it gets too deep. Player tracks the head and auto-fires; the chain respawns once fully cleared. |
| `hub75/pong` | any | Self-playing Pong, red ball. Both paddles track the ball but are speed-capped, so fast returns occasionally get past - that's what keeps score moving. Each paddle bounce also has a 30% chance of speeding the ball up by 30% (capped, and resets to normal pace at the start of each new point), so rallies don't settle into a predictable back-and-forth. Score for each side is shown as a two-digit seven-segment readout (the blocky segmented-digit style of the era's digital scoreboards, not a smooth modern font), wrapping at 100. |
| `hub75/tictactoe` | any | Self-playing tic-tac-toe, looping forever (X red, O cyan). AI is mostly strategic (win > block > center > corner > edge, picking randomly among ties) but has a 20% chance per move of playing a fully random legal move instead, so games stay varied rather than replaying the same line every time. The winning line highlights in yellow; the board holds for ~2.7s before a new game starts, alternating which mark opens. |
| `hub75/matrix1` `hub75/matrix2` `hub75/matrix3` | any | A grid of randomly colored squares - 4x8, 8x16, and 16x32 cells respectively (each cell an exact square given the panel's 64x32 proportions). Randomized once per trigger and held static. |
| `hub75/matrix1c` `hub75/matrix2c` `hub75/matrix3c` | any | Same grids, but re-randomize every ~0.25s while active, looping until another topic is received. |
| `hub75/matrix` | any | "Matrix digital rain" screensaver - 16 independent columns of small-font glyphs fall down the panel, each with its own speed and trail length; the leading glyph is bright near-white, the trail fades through green, and glyphs shimmer (re-randomize) occasionally as they fall. A column resets to a fresh random speed/length/character set once its whole trail has scrolled off the bottom. |
| `hub75/ball1` | any | A filled 8x8 ball bounces around the panel, picking a fresh random (vibrant) color each time it hits any of the four walls. |
| `hub75/shootingstars` | any | 3 bouncing 2x2 "stars" - star1 red, star2 blue, star3 white - each trailing a fading tail up to 6px long that always points directly back along its current direction of travel. Whenever two stars' bounding boxes coincide, a brief expanding/fading spark-burst explosion fires at the contact point. |
| `hub75/time` | any | 2px blue border around the panel with a 2px blue horizontal center line dividing it into two boxes. Top box: west coast (Pacific) time in yellow. Bottom box: east coast (Eastern) time in green. Both 24-hour `HH:MM:SS` in a compact seven-segment font (1px-thick strokes, fixed-width digits), sized and centered so it never touches a box edge. |
| `hub75/maze1` | any | A random perfect maze (1px walls in a random color - re-rolled each new maze, never red - 3px paths) solved live by a single red pixel doing depth-first search - no breadcrumb trail, just the pixel sliding continuously through every pixel of each corridor (never jumping straight from one cell center to the next). Repeats with a fresh maze (and a new wall color) once solved. |
| `hub75/maze2` | any | Same as `hub75/maze1`, but with 1px wide paths instead of 3px - a much denser maze fills the panel. Once solved, the completed maze fades gently to black (eased, no sudden brightness jumps), then the next maze fades just as gently back in before its solve begins. |
| `hub75/maze3` | any | Same as `hub75/maze2`, but the solver "dot" is stretched into a 4px-long segment trailing behind the head along its current direction of travel, so it fits cleanly inside the 1px-wide corridor. |
| `hub75/maze4` | any | Same solve as `hub75/maze1` (3px wide paths), but this one leaves breadcrumbs: every cell the solver enters for the first time is permanently filled with whatever color a background ping to the configured target (see 'HD-WF1 Ping' in menuconfig - same target list/method as `hub75/ping1`/`hub75/ping2`) most recently measured - green (fast) through yellow/orange to red (slow), full red for a timeout. The connecting corridor pixel between two breadcrumbed cells is filled too, so the trail has no dark gaps. The trail only grows forward, never repainted on backtrack, so it reads as a literal history of recent network latency traced through the maze. Pinging happens on its own background cadence and never slows the solver's movement. |
| `hub75/maze5` | any | Same breadcrumb behavior as `hub75/maze4`, but at `hub75/maze2`'s 1px narrow path width instead of `hub75/maze1`'s 3px - a denser maze with a finer-grained latency trail. |
| `hub75/ping1` | any | A scrolling network-latency strip-chart: roughly every ~250ms+RTT, pings one target picked at random from the configured list (see 'HD-WF1 Ping' in menuconfig - up to 8 targets, default just `192.168.92.1`) and scrolls a new bar in from the right (like a ping/oscilloscope graph). Bar height and color both encode the delay - green (fast) through yellow/orange to red (slow), with a full-height red bar marking a timed-out ping (or DNS failure, or - in TCP mode - a refused/closed port; 500ms timeout). The display itself always animates smoothly regardless of network speed, since only the background sampling waits on the network. The true measured delay is also shown numerically as a real number in ms, one decimal place (small fixed-width font, white, microsecond-resolution timing), centered on a reserved strip below the graph - flanked by the running successful-reply count (left-justified, green) and timed-out-reply count (right-justified, red), both reset to 0 each time the mode is (re)entered. |
| `hub75/ping2` | any | A "comet" launch/impact animation: each ping (to the same randomly-picked, configurable target list as `hub75/ping1`) launches a bright white-hot comet (with a 6px fading tail) that streaks left-to-right across the panel. Flight *speed* encodes the latency - a fast reply darts across in a few frames, a slow one crawls - so the effect gets visibly more dramatic as the network struggles. A successful ping ends in a flash + expanding 8-point spark-burst explosion at the target; a timed-out ping instead peters out 60% of the way there in a dim, slow 5-point fizzle, visually distinguishing "it arrived" from "it never showed up." Same numeric ms readout convention as `hub75/ping1`, in a reserved strip below the animation. |
| `hub75/dim` | any | Sets panel brightness to ~50%. |
| `hub75/bright` | any | Sets panel brightness to 100%. |
| `hub75/red` `hub75/green` `hub75/blue` `hub75/yellow` `hub75/orange` `hub75/white` `hub75/black` | any | Fills the whole panel with that solid color. |
| `hub75/flag` | any | Simplified/stylized US flag - the real 13 stripes, and a 23-star canton laid out in the same long/short interleaved-row pattern as the real 50-star field (64x32 is too small to fit 50 individually legible stars). |
| `hub75/canada` | any | Simplified/stylized Canadian flag (red/white/red bands, hand-drawn maple leaf silhouette). |
| `hub75/flash` | any | Toggles blinking whatever's currently on screen: 3s on / 2s off, repeating. Each publish flips it on/off. |
| `hub75/noflash` | any | Forces blinking off, regardless of current state. |
| `hub75/0` | any | Auto-test: cycles through every pattern above (solids, rainbow, bars, pacman, pixel, both flags, a message) in sequence, a few seconds each, forever. |
| `hub75/1` | any | Stops the `hub75/0` auto-test and returns to `hub75/message`'s scrolling text. |
| `hub75/2` | any | Elevator floor display: counts 1→100→1→... forever, each digit scrolling vertically odometer-style into the next (~240ms) with a brief pause (~180ms) at each floor. Red + up-arrow while counting up, green + down-arrow while counting down. Digits sit in three fixed-width slots (blank instead of leading zeros) so the arrow never shifts position as the floor number goes from 1 to 2 to 3 digits. |
| `hub75/2L` | any | Same elevator display as `hub75/2`, but with double-size (16x16) digits and a longer ~2s pause at each floor. Shares the same running floor count as `hub75/2`, so switching between the two continues from the same number instead of resetting. |
| `hub75/2LL` | any | Same elevator display again, quadruple-size (32x32) digits. Only room for the tens/ones digits (no hundreds, no arrow) at this scale - "100" displays the same as "00". Shares the same running count as `hub75/2`/`hub75/2L`. |
| `hub75/clock` | any | Live 24-hour clock, HH:MM:SS, in the current zone/color (default Pacific/white - see the zone-specific topics below) with a static colon. Each digit scrolls odometer-style (same vertical-scroll technique as the elevator) when it changes, independently per digit. Once NTP has synced at least once, two small green dots blink in the lower-right corner as a "good connection" indicator. |
| `hub75/clock.PST8PDT.blue` | any | Clock in US Pacific time (PST/PDT, auto-DST), blue digits. |
| `hub75/clock.mountain.blue` | any | Clock in US Mountain time (MST/MDT, auto-DST), blue digits. |
| `hub75/clock.central.blue` | any | Clock in US Central time (CST/CDT, auto-DST) - e.g. Texas, blue digits. |
| `hub75/clock.eastern.blue` | any | Clock in US Eastern time (EST/EDT, auto-DST) - e.g. New York, blue digits. |
| `hub75/clock.IST.blue` | any | Clock in India Standard Time (fixed UTC+5:30, no DST), blue digits. |
| `hub75/clock.utc.green` | any | Clock in UTC, green digits. |
| `hub75/ntpset` | any | Forces an immediate NTP resync and shows a "TIME SET" confirmation for ~3s before switching to (or returning to) `hub75/clock` in whatever zone/color was last selected. The automatic sync on boot and the periodic background resync every 5 minutes (see 'HD-WF1 NTP' in menuconfig) also keep the clock accurate, but do so silently - they never take over the display, so they don't interrupt whatever else is currently showing (e.g. `hub75/ping1`). |

`hub75/test1`/`test2`/`test3`/`rainbow`/`ray1`/`ray2`/`off`/`1300`/`1300a`/`1300b`/`1300c`/`1300d`/`1300e`/`1300f`/`1300L`/`1300LI`/`13`/`00`/`si`/`mi`/`pong`/`tictactoe`/`matrix1`/`matrix2`/`matrix3`(`c`)/`matrix`/`ball1`/`shootingstars`/`time`/`maze1`/`maze2`/`maze3`/`maze4`/`maze5`/`ping1`/`ping2`/solid-colors/flags/`hub75/0`/`hub75/2`/`hub75/2L`/`hub75/2LL`/`hub75/clock`(`.*`)
all replace whatever was showing (WiFi status or a previous message/pattern)
and stay up until another message arrives on any of these topics (`hub75/0`
specifically runs until `hub75/1`). `hub75/dim`/`bright` and
`hub75/flash`/`noflash` are independent overlays - they apply on top of
whatever's currently showing rather than switching to something new.

### RTC / clock

The HD-WF1 has an onboard BM8563 RTC chip, but its I2C pin mapping was never
reverse-engineered for this board (unlike the HUB75 pins - see
[Pinout](#pinout) - that took solid-color testing on real hardware to nail
down; the RTC would need similar hands-on investigation, e.g. schematic or
continuity tracing). `hub75/clock` is built on ESP-IDF's SNTP-synced system
time instead (`esp_netif_sntp`), which sidesteps that gap entirely and
directly serves the NTP requirements above. System time is kept in UTC.

## Project layout

```
main/
  main.cpp            — active firmware: pin config, FM6126A init, built-in
                         5x7 font renderer, scroll loop, applog hook
  chilanka_1300.h      — pre-rendered "1300" bitmap (Chilanka font, SIL OFL)
                         for hub75/1300L, from tools/gen_chilanka_1300.py
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

## 128×64 bring-up notes

The project started on a 64×32 panel; moving to 128×64 (`PANEL_WIDTH`/
`PANEL_HEIGHT` in `main/main.cpp`) worked, but only after fixing a chain of
memory problems. Kept here so they aren't rediscovered:

- **Heap is the constraint.** No PSRAM, ~139-163 KB of internal heap free at
  boot. `static` arrays and task stacks come out of the same pool. Budget
  (6-bit, lean build): ~106 KB after the display, ~59 KB after WiFi init,
  ~53 KB steady, ~44 KB low-water while connecting.
- **Single buffer, 6-bit colour.** Double-buffered 8-bit needs >139 KB (`begin()`
  fails with "Not enough memory"). Now `double_buff=false`,
  `setPixelColorDepthBits(6)` (49 KB). 8-bit (64 KB) left ~9 KB after WiFi init
  and WiFi association then failed (`reason=2/202/205`, "Not Connected,
  Retrying"). `flipDMABuffer()` is a no-op with single buffering.
- **`min_refresh_rate = 90`** (library default 60): ~62 Hz visibly flickered.
  The library merges low bit-planes to reach it - 90 gives ~93 Hz at
  `lsbMsbTransitionBit` 1 (only the lowest plane merged). 100 forced transition
  bit 2 (~150 Hz) and cost another bit of colour depth, visible as banding in
  anti-aliased edges. The panel's applog lists each candidate's refresh rate.
- **WiFi buffers trimmed** in `sdkconfig.defaults` (static RX 10→6, dynamic RX/TX
  32→16, AMPDU off) - `esp_wifi_init` returned `ESP_ERR_NO_MEM` (257) otherwise.
- **Lean build** (`HUB75_FULL_EFFECTS=0`): dropped the ping tasks, `maze4`
  task/trail arrays (~12 KB stack + ~12 KB `.bss`) and shrank
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 16384→10240 (watch `main_stack_hwm` in the
  applog when adding effects). Maze scratch arrays are shared/`uint8_t`.
- **ROM USB console crash.** With `CONFIG_ESP_CONSOLE_USB_CDC=y` the board
  reboot-looped at 128×64 (crash in `esp_usb_console_interrupt`, garbage
  pointer `0x13801380`/`0x19801980` - repeated 16-bit display-DMA-looking
  words - and USB enumeration failing with `-71`/`-110`). Bisected: the same
  code at 64×32 was stable; 128×64 was stable at 2-bit but crashed at 4/6/8-bit;
  total buffer size alone wasn't the discriminator. Root cause is **not
  identified**; the console was moved to UART, which removed the crash. A
  `303a:0002` USB ID does *not* mean bootloader on builds that still had the
  USB console - that misled several debugging steps.
- **Flicker.** With single buffering the panel scans the buffer being drawn.
  Static screens (solid colours, `1300*`, `off`) are now only drawn when their
  content changes, and `maze1`-`3` overdraw in place and erase only the moving
  dot instead of `clearScreen()`-ing every frame. Other animated effects
  (`shootingstars`, clocks) still clear each frame and may flicker slightly.
- **`hub75/1300L`** uses a pre-rendered anti-aliased Chilanka bitmap
  (`main/chilanka_1300.h`, 128×64 px, regenerate with
  `tools/gen_chilanka_1300.py`) instead of the built-in 8×8 font. The library
  runs every 8-bit value through a CIE 1931 lightness curve, so the bitmap's
  coverage values go through the inverse curve to keep edge pixels the right
  weight (`--no-gamma` / `--crisp` flags exist for comparison).

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
5. **White rendered as red; green/blue barely responded to `hub75/dim` /
   `hub75/bright`; yellow/orange also skewed red** - looked like a firmware
   color/timing bug (and one real firmware issue was mixed in - see below),
   but the bulk of it was hardware: the panel's own 5V LED power terminals
   (separate from the HUB75E ribbon connector, and separate from whatever
   powers the HD-WF1 control card) weren't connected to a real supply -
   confirmed by measuring only ~260mA draw on the control card's supply at
   peak (a 128×64 panel can want ~10A or more at full white - much more than the old 64×32; check your panel's rating). Once the panel's
   power terminals got a proper 5V feed (measured ~2A at peak white), every
   color rendered correctly. `hub75/standardtest1` (see [MQTT
   topics](#mqtt-topics)) exists specifically to make this class of problem
   fast to re-check. The one real firmware contributor: `latch_blanking` had
   been speculatively bumped from the library default (4) to 8 while chasing
   an unrelated bug (see #6 below) and never confirmed against this color
   report - reverting it to 4 fixed `hub75/dim`/`hub75/bright` acting
   inconsistently per-channel (correct on green, barely-there on red/blue)
   independently of the power issue.
6. **Two identical adjacent pixels on `hub75/test1`** - a clock-edge signal
   integrity issue (a noisy CLK edge double-shifting the same pixel data
   into two columns), confirmed via photo evidence showing both pixels at
   identical color/brightness (ruling out brightness-dependent ghosting).
   Fixed by setting `clkphase = false` - FM6126A doesn't force this to any
   particular value (unlike MBI5124/DP3246), so it was safe to try.
