# HD-WF1 HUB75 "Hello, world!"

ESP-IDF firmware that scrolls "Hello, world!" across a HUB75 RGB LED matrix
panel connected to a **Huidu HD-WF1** control card (ESP32-S2, 4MB flash, no
PSRAM, single HUB75E port) or a **Huidu HD-WF2** (ESP32-S3, 8MB flash, no
PSRAM confirmed yet, much more internal SRAM, two HUB75E ports) - see
[HD-WF2 (ESP32-S3) bring-up notes](#hd-wf2-esp32-s3-bring-up-notes).

Builds and runs on **ESP-IDF v6.0.1** (also works, unmodified, on v5.4.1,
for the HD-WF1/esp32s2 target - HD-WF2/esp32s3 has only been built against
v6.0.1).

## Quick start

HD-WF1 (ESP32-S2):

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

HD-WF2 (ESP32-S3) - uses a **separate sdkconfig**, since the two targets
aren't configuration-compatible (see [HD-WF2 bring-up
notes](#hd-wf2-esp32-s3-bring-up-notes)):

```sh
source ~/esp/esp-idf-v6.0.1/export.sh
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 set-target esp32s3
# then copy WIFI_SSID/PASSWORD, NTP, HTTP API, MQTT, etc. from your sdkconfig
# into sdkconfig.esp32s3 (menuconfig, or hand-edit - these aren't in
# sdkconfig.defaults.esp32s3, which only covers target-level settings)
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 -p /dev/ttyACM0 flash
```

## Hardware

- **Board**: Huidu HD-WF1 — ESP32-S2, HUB75E connector, RTC (BM8563), no
  auto-reset circuit (see [Flashing](#flashing)). Also builds for the
  **Huidu HD-WF2** (ESP32-S3) - see [HD-WF2 (ESP32-S3) bring-up
  notes](#hd-wf2-esp32-s3-bring-up-notes).
- **Panel**: two physical panels are supported, selected by a menuconfig
  choice (*HD-WF1 Panel → Panel resolution*, default 128×64) - fixed at
  build time, reflash after changing it to drive the other panel. The
  current 128×64 (1/32 scan, uses the E address line on GPIO12) and the
  original 64×32 this project started on. Both are FM6126A-driven (confirmed
  by testing, see [Root cause history](#root-cause-history)). The bigger
  panel is memory-hungry on a no-PSRAM ESP32-S2 - see [128×64 bring-up
  notes](#12864-bring-up-notes).

### Pinout

The onboard HUB75E pin-to-GPIO mapping was reverse-engineered by the
community (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`), **but the colour
channels are re-pinned per panel to match the physical panel** - empirically
confirmed with solid colours (send `hub75/red`, `green`, `blue`, and
re-check whenever a *new* panel - one neither of these two profiles covers -
is wired up). A/B/C/D/E (row select) and
LAT/OE/CLK are identical between both known panels; only R/G/B differ. Both
sets are baked in as the two `PanelProfile`s in `main/main.cpp`, selected by
the *Panel resolution* menuconfig choice above, rather than requiring a
firmware edit - a firmware edit is only needed for a third, not-yet-seen
panel.

| Signal | 128×64 GPIO | 64×32 GPIO |
|---|---|---|
| R1 / R2 | 3 / 5 | 2 / 4 |
| G1 / G2 | 6 / 8 | 3 / 5 |
| B1 / B2 | 2 / 4 | 6 / 8 |
| A / B / C / D / E | 39 / 38 / 37 / 36 / 12 | (same) |
| LAT / OE / CLK | 33 / 35 / 34 | (same) |

The 64×32 column is recovered from this project's first commit (a working
64×32 build), not guessed. One setting is genuinely **unverified** for that
panel: `clkphase` (a signal-integrity workaround, see [128×64 bring-up
notes](#12864-bring-up-notes)) defaults to `false` for both profiles, but
that value was only ever confirmed by testing on the 128×64 panel - if
`hub75/test1` shows doubled/ghosted pixels on the 64×32 panel, try flipping
`kProfile64x32.clkphase` to `true` in `main/main.cpp`.

### Onboard button

GPIO 11, active-low (internal pull-up, reads LOW when pressed) — per
community documentation (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`),
confirmed working on this physical unit.

- **Short press**: reboots the board immediately on release.
- **Long press** (~1s): pings the current default gateway 5 times,
  blinking the lower-right pixel green for each packet (on while waiting
  for a reply, off between pings), then leaves it off - a quick "is this
  thing actually on the network" check that works regardless of the active
  display mode, without needing MQTT/REST/a console. A ping that times out
  still blinks (all misses); if WiFi was never configured/connected, every
  ping times out.

Panel resolution is no longer switched by the button - see
[Pinout](#pinout) above for the menuconfig choice.

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

## WiFi, MQTT & REST API

All are optional and off by default. Configure via `idf.py menuconfig`:

- **HD-WF1 WiFi** — SSID/password. Leave the SSID empty to skip WiFi
  entirely - the panel shows "No WiFi configured" in red instead of ever
  reaching the WiFi-status/MQTT-driven screens below (nothing will ever
  connect without an SSID, so the REST API and MQTT are skipped too rather
  than crash-looping trying to reach a network stack that never came up).
- **HD-WF1 MQTT** — broker URI/username/password/client ID, the status and
  message topic names (defaults below), and whether topics are prefixed with
  this device's WiFi MAC (default on - see [MQTT topics](#mqtt-topics)).
  Leave the broker URI empty to skip MQTT (requires WiFi to be configured
  too). The status/message topic fields always hold the canonical,
  un-prefixed form (e.g. `hub75/status`) - there's no `<mac>` placeholder to
  fill in here, since the MAC isn't known until boot; when the MAC-prefix
  setting above is on, it's spliced into the wire topic automatically at
  runtime. The client ID field is just a short prefix (default
  `hd-wf1-hub75`) - this device's WiFi MAC is always appended to it as
  `-<mac>` at runtime, unconditionally (not tied to the MAC-prefix setting
  above), so the exact same build/`sdkconfig` can be flashed onto multiple
  boards sharing one broker without their client IDs colliding - MQTT
  requires unique client IDs per broker, and a collision silently
  disconnects the older board rather than erroring loudly. Don't add your
  own unique suffix here.
- **HD-WF1 Default Text** — what the panel shows (`DisplayMode::kMessage`)
  before any `hub75/message` command has ever arrived, and how it's
  rendered:
  - **Default text to display** — plain text, default empty (blank
    screen/scroll). Overwritten for good by the first real `hub75/message`
    received that boot - there's no way back to this default without a
    reboot.
  - **Default text font** — 8x8 (default; full ASCII, scrolls) or a compact
    3x5 font (fits without scrolling, but only covers digits/space/`.`/`=`
    and the specific letters the WiFi-status screen needs - anything else
    renders as blank space). This is a build-time choice that applies to
    every `hub75/message`, not just the boot default, since both share the
    same renderer.
  - **Default text foreground/background color** — `#RRGGBB` hex, default
    green-on-black (`#00FF00`/`#000000`), same format as other `hub75/*`
    JSON color fields. An invalid or empty value falls back to those
    defaults at boot.
- **HD-WF1 REST API** — enable (default on), port (default 80), an optional
  bearer token, and whether paths are prefixed with this device's WiFi MAC
  (default on, independent of the MQTT setting above); see
  [REST API](#rest-api) below. Requires WiFi.
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
brightness; send `hub75/bright` at any time to go to 100%. For the first 2s
it shows "RR=n" in white (`esp_reset_reason()`'s value - notably `4` means
the *previous* boot ended in a panic, e.g. from a crash loop) so a reboot
reason is visible on the panel itself without pulling the applog; this is a
render-loop overlay only, so it doesn't delay WiFi/MQTT coming up
concurrently underneath it. Then, if any *HD-WF1 Boot Splash* lines are
configured, up to 10s of that (also non-blocking) before showing live WiFi
status - "Not Connected" /
"Retrying..." / attempt count in red (3 lines), then once connected
"Connected" (green) / the IP address (yellow) / "Topic: hub75/" / this
device's WiFi MAC (white, 4 lines - the MAC gets its own line rather than
being appended to the topic line, since it's also the REST API's path
prefix and useful for network discovery regardless of *HD-WF1 MQTT → Prefix
hub75/ topics with this device's WiFi MAC*) - until the first MQTT message
arrives, after which it switches to showing MQTT-driven content permanently.
These displays use a compact 3px-wide font and are both horizontally and
vertically centered as a block (no scrolling), computed against the live
panel height so they stay centered on both the 128×64 and 64×32 profiles. If
nothing arrives within 15s of connecting, it falls back on its own to the
equivalent of `hub75/1300L` with `{"fg":"#df6f3f","bg":"#000000","hue":"#555555"}`
(a dim burnt-orange "1300" on black) rather than sitting on the WiFi status
screen indefinitely - a real message before then cancels this, same as any
other topic moving `DisplayMode` past `kWifiStatus`.

### REST API

The same commands as MQTT are available over HTTP, so the panel can be driven
with `curl`, scripts, Home Assistant `rest_command`, etc. The server starts
once WiFi is connected and needs no broker. It costs a few KB of heap; turn it
off with *HD-WF1 REST API → Enable REST API* if memory gets tight.

By default (*HD-WF1 REST API → Prefix REST paths with this device's WiFi
MAC*), routes are namespaced with this device's WiFi station MAC as 12
lowercase hex digits, e.g. `aabbccddeeff`:

| Request | Body | Effect |
|---|---|---|
| `POST /api/<mac>/command/<name>` (or `PUT`) | the MQTT payload (empty for "any" topics) | Runs the command for topic `hub75/<name>`. |
| `GET /api/<mac>/status` | – | Current state as JSON. |
| `POST /api/<mac>/save` | – | Persists what the panel is showing (last content command + brightness) to flash; it is replayed on every boot, before WiFi is up. `409` if nothing has been shown yet. |
| `POST /api/<mac>/unsave` | – | Forgets the saved display; boot behaves as before. |
| `GET /api/whoami` | – | Fixed, unauthenticated path for LAN discovery: `{"device":"hd-wf1-hub75","device_id":"<mac>","auth":false}` - plus `"https_port"`/`"tls_fingerprint"` on an HTTPS-enabled build (hd-wf2 branch only, see [HTTPS](#https-hd-wf2-branch-only) below). |
| `GET /api/<mac>/info` | – | Static facts as JSON: `{"device_id":"...","width":128,"height":64,"full_effects":false,"version":"v2026.10.10.025"}`. |

Disable that setting for the legacy flat `/api/command/<name>` and
`/api/status` paths (fine for a single board, or behind a reverse proxy that
already routes per device). It's independent of *HD-WF1 MQTT*'s own MAC
prefix setting for topics - either can be on or off regardless of the other,
though both default to on and use the same MAC/hex format so scripts can
derive both IDs the same way. Find a board's MAC in its boot log (`device
ID: <mac>`) or from your router's client list.

`<name>` is the MQTT topic without the `hub75/` prefix - `red`, `dim`,
`3392`, `maze1`, `clock.utc.green`, ... The one exception is `message`, which
maps to the configured message topic. Bodies are limited to 511 bytes. Every
command in the [MQTT topics](#mqtt-topics) table works, subject to the same
lean-build limits: a topic that isn't live in the lean build returns 404.

```sh
curl -X POST http://<device-ip>/api/aabbccddeeff/command/red
curl -X POST http://<device-ip>/api/aabbccddeeff/command/message -d '{"text":"hello","color":"#00ff00"}'
curl -X POST http://<device-ip>/api/aabbccddeeff/command/3392 -d '{"fg":"#ffffff","bg":"#000000"}'
curl http://<device-ip>/api/aabbccddeeff/status
# {"mode":12,"ntp_synced":true,"uptime_s":3456,"free_heap":41234,"min_free_heap":30120}
```

Responses are JSON: `200 {"ok":true}` on success, otherwise
`{"ok":false,"error":"..."}` with `401` (missing/wrong token), `404`
(unknown or disabled command) or `413` (body too large). `mode` in
`/api/status` is the firmware's internal display-mode number, useful only for
checking that a command took effect.

**Security:** with the token left empty (the default) anyone on the network
can control the panel. Set *REST API bearer token* in menuconfig and send
`-H "Authorization: Bearer <token>"`. Traffic is plain HTTP, so treat the token
as LAN-grade protection only - or, on the hd-wf2 branch, use HTTPS instead so
it isn't sent in the clear at all; see below.

#### HTTPS (hd-wf2 branch only)

An optional second server, identical to the plain one above but TLS-wrapped,
on *HD-WF1 HTTPS API → HTTPS REST API port* (default 443) - see
[HD-WF2 (ESP32-S3) bring-up notes](#hd-wf2-esp32-s3-bring-up-notes) for why
this isn't offered on the HD-WF1/esp32s2 branch (each TLS connection costs
~40KB of heap, which that board doesn't have to spare). The plain HTTP
server keeps running unchanged on its own port - nothing is forced to
switch over.

1. **Generate a certificate first** (not committed - each board gets its
   own, like its WiFi password): `tools/gen_https_cert.sh`. Writes
   `certs/server_cert.pem`/`server_key.pem` (ECDSA prime256v1, self-signed,
   10-year validity).
2. **Enable it**: `idf.py menuconfig` → *HD-WF1 HTTPS API* → *Enable HTTPS
   on the REST API*. The build fails with a clear message if the cert/key
   from step 1 don't exist yet.
3. **Pin the certificate, don't rely on CA/hostname validation** - this is
   a LAN device with a DHCP-assigned IP and no real DNS name, so standard
   validation doesn't apply (and will visibly fail: `curl --cacert
   server_cert.pem` against the board's IP correctly rejects it with
   "certificate subject name ... does not match target host name", since
   the cert's CN is cosmetic, not a hostname promise). Instead, fetch
   the certificate's SHA-256 fingerprint from the unauthenticated,
   plain-HTTP `/api/whoami` endpoint's `"tls_fingerprint"` field *before*
   ever connecting over HTTPS, remember it, and compare it against the
   live certificate on every subsequent HTTPS connection (trust-on-first-
   use, the same model SSH host keys and apps like Home Assistant use for
   self-signed local devices). Both Android apps in this project do this.
   `openssl x509 -in certs/server_cert.pem -noout -fingerprint -sha256`
   (colons and case stripped) computes the same value locally, to confirm
   you're pinning the right one.

```sh
# first, over plain HTTP, learn the fingerprint to pin:
curl http://<device-ip>/api/whoami
# {"device":"hd-wf1-hub75","device_id":"aabbccddeeff","auth":false,"https_port":443,
#  "tls_fingerprint":"24dc1a480e0c02fd4699df29ddff36d99f578f63582e01c30c7c7fdec99881f9"}

# then, once that fingerprint is pinned client-side, use HTTPS (here -k skips
# curl's own hostname/CA check, since this project's clients pin the
# fingerprint themselves instead - see point 3 above):
curl -k -X POST https://<device-ip>:443/api/aabbccddeeff/command/red
```

### MQTT topics

All topics default to the `hub75/` prefix shown below; the status and
message topic names are configurable in menuconfig, the rest are fixed.
Payload content doesn't matter for topics marked "any" - the topic alone
triggers the action.

By default (*HD-WF1 MQTT → Prefix hub75/ topics with this device's WiFi
MAC*), every topic below is actually subscribed/matched one level deeper, as
`hub75/<mac>/<topic>` - e.g. `hub75/aabbccddeeff/red` - where `<mac>` is this
device's WiFi station MAC as 12 lowercase hex digits (same ID/format as the
REST API's own MAC prefix, see [REST API](#rest-api) above, but an
independent setting), so multiple boards can share one broker without
colliding on topic names. This applies to the status topic and a message
topic left at its `hub75/message` default too; a message topic customized to
something outside `hub75/` is never touched. Disable that menuconfig setting
for the legacy flat `hub75/<topic>` scheme used below (fine for a single
board on a broker) - the table below always shows the un-prefixed, canonical
form. Find a board's MAC in its boot log (`device ID: <mac>`) or from your
router's client list.

> **Lean build (default):** to save heap on the 128×64 panel only these are
> subscribed: `message`, `1300`/`1300a`-`f`/`1300L`/`1300LI`/`nn`-`nnnnnn`[`L`] (numbers)/`13`/`00`, the solid colours,
> `off`, `dim`/`bright`/`brightness`, `flash`/`noflash`, `shootingstars`, `time`, `maze1`-`3`, `maze99`,
> and the clock/`ntpset` topics. Everything else below (games, matrix,
> elevator, auto-test, ping effects, `maze4`/`maze5`) is still in
> the source but not subscribed, and the ping/`maze4` tasks aren't started. Set
> `HUB75_FULL_EFFECTS` to `1` at the top of `main/main.cpp` to get them all back
> (and expect to fight for heap again). Descriptions below were written for the
> 64×32 panel - fixed-size sprites/text are the same pixel size on 128×64,
> while fills, mazes, grids and centered layouts use the whole panel.

| Topic | Payload | Effect |
|---|---|---|
| `hub75/status` | *(published by device)* | Retained `online` on connect / `offline` (LWT) on ungraceful disconnect. |
| `hub75/message` | `{"text": "...", "color": "#RRGGBB"}` | Scrolls `text` in `color` (optional, defaults to green). |
| `hub75/test1` | any | One random pixel per ~210ms, smoothly fading in and out (no hard blink/dark gap), continuously. |
| `hub75/test2` | any | Animated 10-bar vertical bar graph, one fixed color per bar, random heights refreshed ~4x/sec. |
| `hub75/test3` | any | 8x8 pacman sprite chomping its way around the panel perimeter. |
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
| `hub75/nn` … `hub75/nnnnnn` (optional trailing `L`) | optional JSON `{"fg":"#RRGGBB","bg":"#RRGGBB","hue":"#RRGGBB"}` | **Any 2-6 digit number** (e.g. `hub75/42`, `hub75/1300`-style `hub75/3392L`, `hub75/123456`) shown in the Chilanka font (SIL OFL), always large: the whole number is stretched non-uniformly so its ink touches all four edges of the 128x64 panel, anti-aliased and blended in linear light through the display library's CIE curve. The digits are pre-rendered offline (`tools/gen_chilanka_digits.py` → `main/chilanka_digits.h`, ~32 KB flash) and laid out/scaled on the device. `fg` = digit colour (default `#ffffff`), `bg` = background (default `#000000`), `hue` = a per-channel tint multiplied into both (default `#ffffff` = none; e.g. `#ffd080` reduces blue to compensate a blue-heavy panel). Every field is optional; an empty/non-JSON payload gives white on black. **Exceptions:** the existing topics `hub75/1300`, `hub75/13` and `hub75/00` keep their original small static look - use `hub75/1300L` for the large 1300. Numbers of 1 or 7+ digits are not recognised. Examples: `mosquitto_pub -t hub75/3392 -m '{"fg":"#000000","bg":"#d2b48c"}'`, `curl -X POST http://<device-ip>/api/command/3392 -d '{"fg":"#00ff00"}'`. |
| `hub75/1300LI` | any | `hub75/1300L` inverted: the same Chilanka "1300", but black digits on a "Tan" (CSS `#D2B48C`, RGB 210/180/140) background instead of black on white; edge pixels are blended per channel and re-corrected for the CIE curve. A lit light-coloured 128x64 panel is still a heavy supply-current load - see the "Root cause history" power-supply entry. |
| `hub75/pong` | any | Self-playing Pong, red ball. Both paddles track the ball but are speed-capped, so fast returns occasionally get past - that's what keeps score moving. Each paddle bounce also has a 30% chance of speeding the ball up by 30% (capped, and resets to normal pace at the start of each new point), so rallies don't settle into a predictable back-and-forth. Score for each side is shown as a two-digit seven-segment readout (the blocky segmented-digit style of the era's digital scoreboards, not a smooth modern font), wrapping at 100. |
| `hub75/tictactoe` | any | Self-playing tic-tac-toe, looping forever (X red, O cyan). AI is mostly strategic (win > block > center > corner > edge, picking randomly among ties) but has a 20% chance per move of playing a fully random legal move instead, so games stay varied rather than replaying the same line every time. The winning line highlights in yellow; the board holds for ~2.7s before a new game starts, alternating which mark opens. |
| `hub75/matrix` | any | "Matrix digital rain" screensaver - 16 independent columns of small-font glyphs fall down the panel, each with its own speed and trail length; the leading glyph is bright near-white, the trail fades through green, and glyphs shimmer (re-randomize) occasionally as they fall. A column resets to a fresh random speed/length/character set once its whole trail has scrolled off the bottom. |
| `hub75/shootingstars` | any | 3 bouncing 2x2 "stars" - star1 red, star2 blue, star3 white - each trailing a fading tail up to 6px long that always points directly back along its current direction of travel. Whenever two stars' bounding boxes coincide, a brief expanding/fading spark-burst explosion fires at the contact point. |
| `hub75/time` | any | 2px blue border around the panel with a 2px blue horizontal center line dividing it into two boxes. Top box: west coast (Pacific) time in yellow. Bottom box: east coast (Eastern) time in green. Both 24-hour `HH:MM:SS` in a compact seven-segment font (1px-thick strokes, fixed-width digits), sized and centered so it never touches a box edge. |
| `hub75/maze1` | any | A random perfect maze (1px walls in a random color - re-rolled each new maze, never red - 3px paths) solved live by a single red pixel doing depth-first search - no breadcrumb trail, just the pixel sliding continuously through every pixel of each corridor (never jumping straight from one cell center to the next). Repeats with a fresh maze (and a new wall color) once solved. |
| `hub75/maze2` | any | Same as `hub75/maze1`, but with 1px wide paths instead of 3px - a much denser maze fills the panel. Once solved, the completed maze fades gently to black (eased, no sudden brightness jumps), then the next maze fades just as gently back in before its solve begins. |
| `hub75/maze3` | any | Same as `hub75/maze2`, but the solver "dot" is stretched into a 4px-long segment trailing behind the head along its current direction of travel, so it fits cleanly inside the 1px-wide corridor. |
| `hub75/maze4` | any | Same solve as `hub75/maze1` (3px wide paths), but this one leaves breadcrumbs: every cell the solver enters for the first time is permanently filled with whatever color a background ping to the configured target (see 'HD-WF1 Ping' in menuconfig - same target list/method as `hub75/ping1`/`hub75/ping2`) most recently measured - green (fast) through yellow/orange to red (slow), full red for a timeout. The connecting corridor pixel between two breadcrumbed cells is filled too, so the trail has no dark gaps. The trail only grows forward, never repainted on backtrack, so it reads as a literal history of recent network latency traced through the maze. Pinging happens on its own background cadence and never slows the solver's movement. |
| `hub75/maze5` | any | Same breadcrumb behavior as `hub75/maze4`, but at `hub75/maze2`'s 1px narrow path width instead of `hub75/maze1`'s 3px - a denser maze with a finer-grained latency trail. |
| `hub75/maze99` | any | Ported from the TI-99/4A assembly original (`../maze_a99/micmz2.a99`): a random perfect maze built with **Wilson's algorithm** (every possible maze equally likely, unlike `maze1`-`5`'s randomized-DFS carve) raced by two balls at `maze1`'s 3px path width - red keeping its right hand on the wall, blue its left - each dragging a fading 3-dot tail. The first to reach the bottom-right cell wins the round at once (both arriving on the same tick is a tie); a running red/tie/blue tally is kept for as long as this mode stays active and shown briefly (~1.8s) between rounds before the next maze is carved. Unlike the original, there is no "preparing next maze" progress screen - carving this size of maze is instant on the ESP32. |
| `hub75/ping1` | any | A scrolling network-latency strip-chart: roughly every ~250ms+RTT, pings one target picked at random from the configured list (see 'HD-WF1 Ping' in menuconfig - up to 8 targets, default just `192.168.92.1`) and scrolls a new bar in from the right (like a ping/oscilloscope graph). Bar height and color both encode the delay - green (fast) through yellow/orange to red (slow), with a full-height red bar marking a timed-out ping (or DNS failure, or - in TCP mode - a refused/closed port; 500ms timeout). The display itself always animates smoothly regardless of network speed, since only the background sampling waits on the network. The true measured delay is also shown numerically as a real number in ms, one decimal place (small fixed-width font, white, microsecond-resolution timing), centered on a reserved strip below the graph - flanked by the running successful-reply count (left-justified, green) and timed-out-reply count (right-justified, red), both reset to 0 each time the mode is (re)entered. |
| `hub75/ping2` | any | A "comet" launch/impact animation: each ping (to the same randomly-picked, configurable target list as `hub75/ping1`) launches a bright white-hot comet (with a 6px fading tail) that streaks left-to-right across the panel. Flight *speed* encodes the latency - a fast reply darts across in a few frames, a slow one crawls - so the effect gets visibly more dramatic as the network struggles. A successful ping ends in a flash + expanding 8-point spark-burst explosion at the target; a timed-out ping instead peters out 60% of the way there in a dim, slow 5-point fizzle, visually distinguishing "it arrived" from "it never showed up." Same numeric ms readout convention as `hub75/ping1`, in a reserved strip below the animation. |
| `hub75/dim` | any | Sets panel brightness to a fixed ~12.5% (32/255). |
| `hub75/bright` | any | Sets panel brightness to the *HD-WF1 Power → Maximum panel brightness* cap (default 128/255; 255 = 100%), ramped up over ~1s. The cap exists because full-brightness solid colors can brown out an undersized 5V supply (`RR=9`). |
| `hub75/brightness` | optional JSON `{"percent":0-100}` | Sets panel brightness to any level in between, as a percentage of the same capped maximum `hub75/bright` uses (so 100 here equals `hub75/bright`, not necessarily 255/255) - e.g. `{"percent":25}` for a quarter of that cap. Missing/invalid payload or an out-of-range percent falls back to 100. Also ramped up over ~1s, same as `hub75/bright`. |
| `hub75/red` `hub75/green` `hub75/blue` `hub75/yellow` `hub75/orange` `hub75/white` `hub75/black` | any | Fills the whole panel with that solid color. |
| `hub75/flash` | any | Toggles blinking whatever's currently on screen: 3s on / 2s off, repeating. Each publish flips it on/off. |
| `hub75/noflash` | any | Forces blinking off, regardless of current state. |
| `hub75/0` | any | Auto-test: cycles through every pattern above (solids, rainbow, bars, pacman, pixel, a message) in sequence, a few seconds each, forever. |
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
| `hub75/ntpset` | any | Forces an immediate NTP resync and shows a "TIME SET" confirmation for ~3s before switching to (or returning to) `hub75/clock` in whatever zone/color was last selected. The automatic sync on boot and the periodic background resync every 24 hours (see 'HD-WF1 NTP' in menuconfig) also keep the clock accurate, but do so silently - they never take over the display, so they don't interrupt whatever else is currently showing (e.g. `hub75/ping1`). |

`hub75/test1`/`test2`/`test3`/`off`/`1300`/`1300a`/`1300b`/`1300c`/`1300d`/`1300e`/`1300f`/`1300L`/`1300LI`/`nn`-`nnnnnn`[`L`]/`13`/`00`/`pong`/`tictactoe`/`matrix`/`shootingstars`/`time`/`maze1`/`maze2`/`maze3`/`maze4`/`maze5`/`ping1`/`ping2`/solid-colors/`hub75/0`/`hub75/2`/`hub75/2L`/`hub75/2LL`/`hub75/clock`(`.*`)
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
  chilanka_digits.h    — pre-rendered digits 0-9 + metrics (Chilanka font,
                         SIL OFL) for hub75/nn..nnnnnn[L], from
                         tools/gen_chilanka_digits.py
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
                         below). Committed to git so the patches survive a
                         `managed_components/` wipe — see caveat below.
```

The active/inactive backend is selected by `CONFIG_HUB75_BACKEND_ARDUINO_GFX`
in `sdkconfig.defaults` (misleadingly named — it's neither Arduino nor GFX;
built with `ESP32_HUB75_USE_GFX=n`, so no Adafruit-GFX/Arduino dependency).

## ⚠️ Vendored library patches are not durable

`managed_components/esp32-hub75-matrixpanel-dma/` is fetched by the ESP-IDF
component manager from `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` and
**patched in place** to compile under IDF v6 (that upstream repo only
supports up to v5.x as shipped). The patched directory is committed to git
(as of commit `90cab52`), so a plain `managed_components/` deletion —
e.g. from `idf.py fullclean` refusing to reuse a stale build dir and running
`remove_managed_components` — no longer loses the patches; `git status`
comes back clean afterward. They can still be silently overwritten by
`idf.py update-dependencies`, which re-fetches from upstream and clobbers
the patched files in place; that *would* show up as an unstaged diff (or an
unexplained build failure if committed without noticing), so check
`git status` on this directory after running it.

**TODO**: fork the upstream repo, commit these patches there too, and point
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
- **`hub75/nn`..`hub75/nnnnnn[L]`** (numbers) draw pre-rendered anti-aliased
  Chilanka digit glyphs (`main/chilanka_digits.h`, regenerate with
  `tools/gen_chilanka_digits.py`) instead of the built-in 8×8 font. The digits
  are laid out with the font's advances, box-filtered to fill the 128×64 panel
  in linear coverage, and blended through the display library's CIE 1931
  lightness curve so edge pixels get the right weight.

## HD-WF2 (ESP32-S3) bring-up notes

A second physical board - a Huidu HD-WF2, with much more RAM than the
HD-WF1 (hundreds of KB free heap vs. the HD-WF1's tens of KB headroom; see
[128×64 bring-up notes](#12864-bring-up-notes) above) - was brought up
against this same codebase. `main.cpp`'s pins and `PanelProfile`s are
`#if CONFIG_IDF_TARGET_ESP32S3` branches alongside the HD-WF1's (`esp32s2`)
ones; everything else (REST API, MQTT, effects, save/restore, ...) is
unchanged and works identically on both.

- **Separate sdkconfig required.** `esp32s2` and `esp32s3` sdkconfigs are
  not interchangeable - `idf.py set-target esp32s3` regenerates one from
  scratch. Use a distinct `-D SDKCONFIG=`/`-B` pair so this never shares a
  config (or build dir) with an HD-WF1 build; see [Quick start](#quick-start).
  `sdkconfig.defaults.esp32s3` (ESP-IDF's standard per-target overlay,
  applied on top of the base `sdkconfig.defaults`) carries the esp32s3-only
  settings: target, 8MB flash size, no PSRAM (unconfirmed either way on
  this board - left off rather than guessed), and routing the console
  through the native USB-Serial/JTAG peripheral instead of the HD-WF1's
  UART workaround (see that bring-up note above - this board's native USB
  console hasn't shown the same crash, though it also hasn't been stress
  tested as hard).
- **Two HUB75 ports, 75EX1 and 75EX2, can't be used at once** (shared
  A/B/C/D/OE/CLK/LAT; only R/G/B/E differ per port) - only 75EX1 is
  supported here, since 75EX2's E line is undocumented upstream and
  therefore unimplemented. Pin source, same as the HD-WF1's:
  `mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`,
  `src/hd-wf2-esp32s3-config.h`.

  | Signal | 75EX1 GPIO |
  |---|---|
  | R1 / R2 | 2 / 3 |
  | G1 / G2 | 10 / 11 |
  | B1 / B2 | 6 / 7 |
  | A / B / C / D / E | 39 / 38 / 37 / 36 / 21 |
  | LAT / OE / CLK | 33 / 35 / 34 |
  | Onboard button | 17 (per community docs, not yet confirmed) |

  R/G/B above are already corrected (see next point) - this table is the
  as-flashed values, not the community reference's naive ones.
- **R/G/B empirically confirmed** on the physically-attached 64×32 panel,
  the same way the HD-WF1 panels were: the community reference's naive
  order (R=2/3, G=6/7, B=10/11) got red right but swapped green and blue -
  green data lit blue LEDs and vice versa. Fixed by swapping G and B's pins
  (G=10/11, B=6/7); R unchanged. `clkphase` is **not yet verified** either
  way (still `false`, copied from the HD-WF1 128×64 profile as a starting
  guess) - no ghosting observed, but it hasn't been deliberately tested
  with `hub75/test1` the way the HD-WF1's was.
- **No manual pad-bridging needed to flash** - unlike the HD-WF1 (see
  [Flashing](#flashing)), this board's native USB-Serial/JTAG peripheral
  handles auto-reset into the ROM bootloader on its own, the normal way
  `esptool`/`idf.py flash` expects.
- **That auto-reset circuit is touchy, though.** Opening the serial port
  (even just to read console output) can leave the board latched in the
  ROM bootloader ("waiting for download") instead of booting normally -
  `esptool --before no-reset --after no-reset read-mac` connecting
  successfully with no prior bootloader-entry step is the tell. A software
  reset (`esptool ... --after hard-reset ...`) doesn't reliably clear this;
  a full power cycle (unplug/replug) does. `esp_reset_reason()` reports
  `11` (`ESP_RST_USB`) after a reset caused by this peripheral - expected
  and benign after a flash, not a fault code like `4` (panic) or `9`
  (brownout).
- **Vendored library patch** (see [Vendored library patches are not
  durable](#️-vendored-library-patches-are-not-durable) above):
  `managed_components/esp32-hub75-matrixpanel-dma/src/platforms/esp32s3/gdma_lcd_parallel16.cpp`
  needed an IDF v6 patch of its own, separate from the generic one already
  documented there - the ESP32-S3-specific GDMA backend predates IDF v6's
  GDMA API changes (`gdma_new_ahb_channel()` gained a third, RX-channel
  out-param; `gdma_channel_alloc_config_t` dropped `.sibling_chan`/
  `.direction`/`.flags.reserve_sibling` in favor of implying direction from
  which out-param is non-NULL; `gdma_strategy_config_t` gained a required
  `eof_till_data_popped` field). Patched with a version-gated
  `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)` branch alongside the
  existing ones, so older-IDF builds of this same vendored file are
  unaffected.
- **HTTPS REST API** (see [REST API → HTTPS](#https-hd-wf2-branch-only)
  above) is this branch's one *intentional* divergence from the HD-WF1's
  feature set, not a porting workaround - the extra ~40KB-per-connection
  heap cost of TLS is exactly the kind of thing the HD-WF1's tens-of-KB
  budget couldn't absorb (see the WiFi-buffer-trimming and lean-build notes
  above), while this board's hundreds of KB make it a non-issue. Verified
  on hardware: heap stayed flat (~190KB free, ~181KB min) across repeated
  HTTPS requests - no per-connection leak.
- **ESP-IDF v6's mbedTLS is 4.x**, which moved single-algorithm APIs like
  `mbedtls_sha256()` out of the public headers (`mbedtls/sha256.h` is now
  under a `private/` include path) in favor of the PSA Crypto API -
  `psa_hash_compute()` instead, used here to fingerprint the HTTPS
  certificate (see above). No separate init call needed:
  `psa_crypto_init()` already runs automatically at boot, during
  `esp_system`'s SECONDARY init stage.

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
   color rendered correctly. `hub75/red`/`green`/`blue`/`white` plus
   `hub75/dim`/`bright` (see [MQTT topics](#mqtt-topics)) make this class of
   problem fast to re-check. The one real firmware contributor: `latch_blanking` had
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
