# HUB75 Digital Address Plaque — User Manual

**Firmware version:** v2026.10.10.026
**Boards:** Huidu HD-WF1 (ESP32-S2) and Huidu HD-WF2 (ESP32-S3)
**Panels:** 128×64 or 64×32 HUB75E RGB LED matrix
**Author:** Michael J. Wolters

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Quick Start: Flashing](#2-quick-start-flashing)
   - 2.1 [What you need](#21-what-you-need)
   - 2.2 [Flashing the HD-WF2 (ESP32-S3)](#22-flashing-the-hd-wf2-esp32-s3)
   - 2.3 [Flashing the HD-WF1 (ESP32-S2)](#23-flashing-the-hd-wf1-esp32-s2)
   - 2.4 [Checking that it worked](#24-checking-that-it-worked)
3. [Hardware](#3-hardware)
   - 3.1 [System overview](#31-system-overview)
   - 3.2 [Board comparison](#32-board-comparison)
   - 3.3 [Panels and colour-channel wiring](#33-panels-and-colour-channel-wiring)
   - 3.4 [Pin tables](#34-pin-tables)
   - 3.5 [Power](#35-power)
4. [Configuration (menuconfig)](#4-configuration-menuconfig)
   - 4.1 [Where settings live](#41-where-settings-live)
   - 4.2 [Settings reference](#42-settings-reference)
   - 4.3 [Building with credentials safely](#43-building-with-credentials-safely)
5. [Boot Sequence and Status Screens](#5-boot-sequence-and-status-screens)
6. [Controlling the Plaque](#6-controlling-the-plaque)
   - 6.1 [How commands flow](#61-how-commands-flow)
   - 6.2 [MQTT](#62-mqtt)
   - 6.3 [REST API (HTTP)](#63-rest-api-http)
   - 6.4 [HTTPS (HD-WF2 only)](#64-https-hd-wf2-only)
   - 6.5 [Finding your device ID](#65-finding-your-device-id)
7. [Command Reference](#7-command-reference)
   - 7.1 [Text](#71-text)
   - 7.2 [Large numbers](#72-large-numbers)
   - 7.3 [Solid colours, brightness and flashing](#73-solid-colours-brightness-and-flashing)
   - 7.4 [Clocks and time](#74-clocks-and-time)
   - 7.5 [Animations and screensavers](#75-animations-and-screensavers)
   - 7.6 [Mazes](#76-mazes)
   - 7.7 [Network-latency displays](#77-network-latency-displays)
   - 7.8 [Elevator and auto-test](#78-elevator-and-auto-test)
   - 7.9 [Which commands are live in the lean build](#79-which-commands-are-live-in-the-lean-build)
8. [Onboard Button](#8-onboard-button)
9. [Saving the Display Across Reboots](#9-saving-the-display-across-reboots)
10. [Firmware Updates](#10-firmware-updates)
    - 10.1 [USB flashing](#101-usb-flashing)
    - 10.2 [Over-the-air (OTA) updates](#102-over-the-air-ota-updates)
11. [Memory, Build Variants and Limits](#11-memory-build-variants-and-limits)
12. [Logs and Diagnostics](#12-logs-and-diagnostics)
13. [Troubleshooting](#13-troubleshooting)
14. [Safety and Power Notes](#14-safety-and-power-notes)
15. [Credits](#15-credits)
- [Appendix A: Glossary](#appendix-a-glossary)
- [Appendix B: Index](#appendix-b-index)

---

## 1. Introduction

This firmware turns a Huidu **HD-WF1** or **HD-WF2** LED-controller card plus a
HUB75E RGB LED matrix panel into a network-controlled display — a "digital
address plaque". It shows big house numbers, scrolling messages, clocks,
games, mazes, network-latency visualisations and more. You control it by
sending small commands over **MQTT** or a **REST (HTTP/HTTPS) API**; no
cloud service is involved.

Key characteristics:

- **Static by design.** A plain number or colour is drawn once and left
  alone — the display does not repaint itself periodically. (Animated
  effects naturally redraw every frame.)
- **Two ways in.** Every command works identically over MQTT and REST,
  because both go through the same internal dispatcher.
- **Two panels.** 128×64 or 64×32, chosen when you build the firmware.
- **Self-contained.** Settings are baked in at build time; the device keeps
  working with no server (it just needs WiFi to be commanded).
- **Updatable.** USB flashing on both boards; over-the-air updates on the
  HD-WF2.

```
   +-----------+     MQTT      +----------+
   | Phone /   |--------------->|  Broker  |-----+
   | script /  |                +----------+     |  hub75/<id>/red
   | Home Asst |                                 v
   |           |   HTTP(S) REST            +-----------+      HUB75E       +-----------+
   |           |-------------------------->| HD-WF1/2  |------------------>| LED panel |
   +-----------+  POST /api/<id>/command/  |  card     |   ribbon cable    | 128x64 or |
                                           +-----------+                   |  64x32    |
                                                                           +-----------+
```

---

## 2. Quick Start: Flashing

This is the fastest path from a bare board to a running plaque.

### 2.1 What you need

| Item | Notes |
|---|---|
| Huidu HD-WF2 **or** HD-WF1 card, wired to a HUB75E panel | Panel must have its own 5 V supply (see [Power](#35-power)). |
| A computer running Linux/macOS/Windows | Commands below are for a Linux shell. |
| ESP-IDF **v6.0.1** | Installed at `~/esp/esp-idf-v6.0.1` (v5.4.1 also works for the HD-WF1 only). |
| USB cable | HD-WF2: the board's USB port. HD-WF1: USB-A-to-USB-A cable into the **USB-A** port (the micro-USB is power-only). |
| This repository | `git clone` it, then work from its root directory. |
| Your WiFi name/password (and optionally MQTT broker details and an API token) | Entered in menuconfig. |

### 2.2 Flashing the HD-WF2 (ESP32-S3)

The HD-WF2 resets itself into the bootloader automatically — no jumper needed.
It uses its **own** config file (`sdkconfig.esp32s3`) and build folder
(`build_esp32s3`) so it never mixes with an HD-WF1 build.

```
 Step 1            Step 2               Step 3               Step 4              Step 5
 Load ESP-IDF  ->  Create S3 config ->  Enter your       ->  Build + flash   ->  Power-cycle
                   (once)               settings (once)                          & verify
```

```sh
# 1. Load the ESP-IDF environment (every new terminal)
source ~/esp/esp-idf-v6.0.1/export.sh

# 2. Create the ESP32-S3 configuration (first time only)
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 set-target esp32s3

# 3. Enter WiFi, MQTT, API token, panel size ... (first time, and whenever they change)
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 menuconfig

# 4. Build and flash (port may be /dev/ttyACM0, /dev/ttyACM1 ...: check `ls /dev/ttyACM*`)
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 -p /dev/ttyACM0 flash
```

Step 5: unplug and replug power. If the board seems stuck "waiting for
download", a full power cycle is the fix (see [Troubleshooting](#13-troubleshooting)).

> **Tip — keep passwords out of your terminal.** Use the credential-safe
> form from [section 4.3](#43-building-with-credentials-safely).

> **Panel size:** in menuconfig choose *HD-WF1 Panel → Panel resolution*
> (**128x64** is the default). The panel is fixed at build time; reflash to
> change it.

### 2.3 Flashing the HD-WF1 (ESP32-S2)

The HD-WF1 has **no auto-reset**, so you must force it into the ROM
bootloader by hand every time.

```
   micro-USB end of board
   +-------------------------------+
   |  [ micro-USB ]   (o)--(o)     |   <- 1. Bridge these two small pads
   |   power only      pads        |
   |                               |
   |  [ USB-A ]  <---- data cable  |   <- 2. Connect here
   +-------------------------------+
```

1. **Bridge** the two small pads next to the micro-USB connector (jumper wire or tweezers).
2. Connect the **USB-A** port to your computer.
3. Build and flash:

   ```sh
   source ~/esp/esp-idf-v6.0.1/export.sh
   idf.py set-target esp32s2          # first time only
   idf.py menuconfig                  # first time / when settings change
   idf.py build
   idf.py -p /dev/ttyACM0 flash
   ```
4. **Unbridge** the pads and **power-cycle** the board to boot the new app.

### 2.4 Checking that it worked

- The panel shows `RR=n` for about 2 seconds (the reset reason — see
  [section 5](#5-boot-sequence-and-status-screens)), then optional splash
  text, then the WiFi status screen.
- Once connected, the status screen shows `Connected`, the IP address, the
  topic prefix and the device ID (MAC).
- From any computer on the same network:

  ```sh
  curl http://<device-ip>/api/whoami
  curl -X POST -H "Authorization: Bearer <token>" \
       http://<device-ip>/api/<device-id>/command/red
  ```
  (Omit the `Authorization` header if you left the token empty.) The panel
  should fill with red.
- Boot-state check on the HD-WF1: `lsusb` showing `303a:0002` means the
  board is **in the ROM bootloader**, not running the app.

---

## 3. Hardware

### 3.1 System overview

```
                          +---------------------------------------------+
   5 V power supply  ---> |  LED PANEL power terminals  (separate supply  |
   (several amps)         |  from the controller; see section 14)         |
                          +---------------------------------------------+
                                              ^
                                              | HUB75E ribbon (data only)
   +---------------------------------+        |
   |   Huidu HD-WF1 / HD-WF2 card    |--------+
   |                                 |
   |  ESP32 (S2 or S3)               |
   |   - WiFi                        |
   |   - DMA display driver          |
   |   - button  (short / long)      |
   |   - USB for flashing            |
   +---------------------------------+
        ^                  ^
   5 V USB / supply     WiFi to your network
```

### 3.2 Board comparison

| Feature | HD-WF1 | HD-WF2 |
|---|---|---|
| Chip | ESP32-S2 | ESP32-S3 |
| Flash | 4 MB | 8 MB |
| Internal heap at boot | ~139–163 KB | several hundred KB |
| HUB75E ports | 1 | 2 (only **75EX1** supported) |
| Flashing | Bridge pads, USB-A port | Automatic |
| Console | UART (logs read from flash) | USB-Serial/JTAG |
| HTTPS REST API | No (not enough memory) | Yes (optional) |
| OTA updates | No (single app slot) | **Yes** |
| Build config | `sdkconfig`, `build/` | `sdkconfig.esp32s3`, `build_esp32s3/` |

### 3.3 Panels and colour-channel wiring

Two panels are supported. Both use FM6126A driver chips. Which one is used is
selected in menuconfig; there is no runtime switch.

| Panel | Size | Scan | Notes |
|---|---|---|---|
| 128×64 | 8192 px | 1/32, uses the **E** address line | Memory-hungry; 6-bit colour depth. |
| 64×32 | 2048 px | 1/16 | The original panel. |

Panels differ in which data pins drive which colour LEDs, so the red, green
and blue pin groups are **re-assigned per panel and per board**. The symptom
of a wrong mapping is simple: send `red`, `green`, `blue` and see which
colours actually appear. For example "red shows blue, green shows red, blue
shows green" means the three groups are rotated.

```
   Data sent        What a wrong mapping shows       Fix
   ---------        ---------------------------      ------------------------
   hub75/red   -->  BLUE                              reassign R/G/B pin groups
   hub75/green -->  RED                               in the board's PanelProfile
   hub75/blue  -->  GREEN                             (main/main.cpp)
```

A genuinely new panel type needs its own profile in `main/main.cpp`; the
drawing code never changes.

### 3.4 Pin tables

**HD-WF2 (ESP32-S3), 75EX1 port — as flashed:**

| Signal | 128×64 panel | 64×32 panel |
|---|---|---|
| R1 / R2 | 10 / 11 | 2 / 3 |
| G1 / G2 | 6 / 7 | 10 / 11 |
| B1 / B2 | 2 / 3 | 6 / 7 |
| A / B / C / D / E | 39 / 38 / 37 / 36 / 21 | same |
| LAT / OE / CLK | 33 / 35 / 34 | same |
| Button | 17 | 17 |

**HD-WF1 (ESP32-S2):**

| Signal | 128×64 panel | 64×32 panel |
|---|---|---|
| R1 / R2 | 3 / 5 | 2 / 4 |
| G1 / G2 | 6 / 8 | 3 / 5 |
| B1 / B2 | 2 / 4 | 6 / 8 |
| A / B / C / D / E | 39 / 38 / 37 / 36 / 12 | same |
| LAT / OE / CLK | 33 / 35 / 34 | same |
| Button | 11 | 11 |

### 3.5 Power

A HUB75 panel has its **own 5 V power terminals**, separate from the HUB75E
ribbon and separate from the controller card's supply. A 128×64 panel can draw
**10 A or more at full white**. See [section 14](#14-safety-and-power-notes).

---

## 4. Configuration (menuconfig)

### 4.1 Where settings live

```
   sdkconfig.defaults            checked in, no secrets   --+
   sdkconfig.defaults.esp32s3    S3-only overlay, no secrets +--> merged when
                                                                  a config is created
   sdkconfig            (HD-WF1)  gitignored, YOUR secrets     <--+
   sdkconfig.esp32s3    (HD-WF2)  holds YOUR secrets  <-- not gitignored by default!
```

> **Important:** `sdkconfig.esp32s3` contains your WiFi and MQTT credentials
> but is **not** covered by `.gitignore` by default. Add it to `.gitignore`
> before committing anything.

Open the menu with `idf.py menuconfig` (HD-WF1) or
`idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 menuconfig` (HD-WF2).

After editing `main/Kconfig.projbuild` (developers only), run
`idf.py reconfigure`.

### 4.2 Settings reference

| Menu | Setting | Default | Meaning |
|---|---|---|---|
| HD-WF1 Panel | Panel resolution | 128x64 | 128x64 or 64x32. Requires reflash to change. |
| HD-WF1 WiFi | WiFi SSID / password | empty | Empty SSID = no WiFi; the panel shows "No WiFi configured" and MQTT/REST are skipped. |
| HD-WF1 REST API | Enable REST API | on | HTTP server once WiFi is up. |
| | Port | 80 | |
| | Bearer token | empty | If set, requests need `Authorization: Bearer <token>`. **Required for OTA.** |
| | Prefix paths with MAC | on | Paths become `/api/<id>/...`. |
| HD-WF1 HTTPS API | Enable HTTPS | off | HD-WF2 only; needs a certificate first (see [6.4](#64-https-hd-wf2-only)). |
| | HTTPS port | 443 | |
| HD-WF1 MQTT | Broker URI | empty | Empty = MQTT off. E.g. `mqtt://192.168.1.10`. |
| | Client ID prefix | `hd-wf1-hub75` | The device MAC is always appended, so identical builds on several boards don't collide. |
| | Username / password | empty | |
| | Status / message topic | `hub75/status` / `hub75/message` | Canonical un-prefixed names. |
| | Prefix topics with MAC | on | Topics become `hub75/<id>/...`. |
| HD-WF1 Default Text | Text, font, fg, bg | empty, 8x8, `#00FF00`, `#000000` | What `message` mode shows before any message arrives. |
| HD-WF1 Ping | Targets, method, TCP port | `192.168.92.1`, ICMP, 80 | Used by the latency displays (full build only). |
| HD-WF1 Power | Maximum brightness | 128 | Cap (1–255) used by `bright`/`brightness`. |
| HD-WF1 NTP | Primary/secondary server | `pool.ntp.org`, none | Needed for clocks. |
| HD-WF1 Boot Splash | Lines 1–4 | empty | Up to four lines of text shown for 10 s at power-up (e.g. owner contact info). Leave all empty to disable. |

### 4.3 Building with credentials safely

`idf.py` can echo your WiFi password and broker URI in its output. Filter it:

```sh
export KCONFIG_REPORT_VERBOSITY=quiet
idf.py -D SDKCONFIG=sdkconfig.esp32s3 -B build_esp32s3 build 2>&1 \
  | grep -v -i "wifi_ssid\|wifi_password\|mqtt_broker_uri\|default value"
```

---

## 5. Boot Sequence and Status Screens

```
 power on
    |
    v
 [ RR=n ]  ~2 s     white text: the reset reason (4 = previous boot panicked,
    |               9 = brownout/power problem, 11 = USB reset after a flash: normal)
    v
 [ Boot splash ]    up to 10 s, your 4 lines, yellow on black (only if configured)
    |
    |   (WiFi starts ~5 s after power-on, underneath, in parallel)
    v
 [ WiFi status ]    "Not Connected / Retrying / attempt count"   (red)
    |                      |
    |  connected           v   no WiFi configured -> "No WiFi configured" (red)
    v
 [ Connected ]      "Connected" (green), IP (yellow), "Topic: hub75/", device ID (white)
    |
    |-- a command arrives ------------------------> that command's display
    |-- saved display exists -------------------> replayed at boot, before WiFi
    |-- nothing for 15 s after connecting ------> default "1300" in dim burnt orange
    v
 [ normal operation ]
```

Notes:

- The panel starts **dim** (as if `dim` had been sent) so the first thing
  drawn is gentle. Send `bright` to raise it.
- A single **green pixel in the lower-right corner** during the splash means
  WiFi got an IP.
- The first successful NTP sync after boot blinks the lower-right pixel green
  for about 4 seconds. Later background syncs are silent.
- If a **saved display** exists ([section 9](#9-saving-the-display-across-reboots)),
  it is shown immediately at boot.

---

## 6. Controlling the Plaque

### 6.1 How commands flow

Every command, from MQTT or REST, enters the same function and takes the same
path:

```
   MQTT message                       REST request
   hub75/<id>/red                     POST /api/<id>/command/red
        |                                   |
        |  strip device-id prefix           |  prepend "hub75/"
        v                                   v
        +-------------> dispatch_command("hub75/red", payload) <---+
                                   |
                  unknown / disabled? ---> 404 (REST) / ignored (MQTT)
                                   |
                                   v
                      set display mode + parameters
                       (guarded by the display mutex)
                                   |
                                   v
              render loop (~30 ms tick) snapshots state, draws it
```

Because REST and MQTT share one dispatcher, any topic that works on one works
on the other, now and in future versions.

Anything slow (network pings, DNS) runs in background tasks and never blocks
drawing, so animations stay smooth whatever the network does.

### 6.2 MQTT

1. Set the broker URI (and credentials) in menuconfig.
2. By default every topic is namespaced by the device ID:

   | You publish to | Meaning |
   |---|---|
   | `hub75/aabbccddeeff/red` | Fill red on the board with ID `aabbccddeeff` |
   | `hub75/aabbccddeeff/message` | Scroll text |
   | `hub75/aabbccddeeff/status` | *Published by the device*: `online` (retained) / `offline` (last-will) |

   Disable *Prefix topics with MAC* for the flat `hub75/<topic>` scheme
   (reasonable for a single board).
3. Payload content is ignored for commands marked "any"; others take JSON.

Examples:

```sh
mosquitto_pub -t hub75/aabbccddeeff/red -m ''
mosquitto_pub -t hub75/aabbccddeeff/message -m '{"text":"Welcome!","color":"#00ff00"}'
mosquitto_pub -t hub75/aabbccddeeff/3392 -m '{"fg":"#ffffff","bg":"#000000"}'
```

The firmware subscribes with **one wildcard** subscription, which keeps
memory use low.

### 6.3 REST API (HTTP)

Routes (with the default MAC prefix; replace `<id>` with the device ID):

| Request | Body | Effect |
|---|---|---|
| `POST /api/<id>/command/<name>` (or `PUT`) | the same payload MQTT uses (may be empty) | Run command `hub75/<name>`. `message` maps to the message topic. |
| `GET /api/<id>/status` | — | `{"mode":12,"ntp_synced":true,"uptime_s":3456,"free_heap":41234,"min_free_heap":30120}` |
| `GET /api/<id>/info` | — | `{"device_id":"...","width":128,"height":64,"full_effects":false,"version":"v2026.10.10.026"}` |
| `POST /api/<id>/save` | — | Remember the current display for every boot ([section 9](#9-saving-the-display-across-reboots)). |
| `POST /api/<id>/unsave` | — | Forget the saved display. |
| `POST /api/<id>/ota` | raw firmware `.bin` | Over-the-air update ([10.2](#102-over-the-air-ota-updates)). HD-WF2 only. |
| `GET /api/whoami` | — | **Unauthenticated, fixed path** for LAN discovery: device name, ID, whether auth is on, and (HTTPS builds) the port and certificate fingerprint. |

Responses are JSON. Success is `200 {"ok":true}`. Errors:

| Code | Meaning |
|---|---|
| 401 | Missing or wrong bearer token |
| 403 | OTA attempted while no token is configured |
| 404 | Unknown command, or one not compiled into the lean build |
| 409 | `save` with nothing shown yet, or an OTA already running |
| 413 | Body larger than 511 bytes |
| 501 | OTA on a board without OTA partitions (HD-WF1) |

Examples:

```sh
curl -X POST http://<ip>/api/<id>/command/red
curl -X POST -H "Authorization: Bearer <token>" \
     http://<ip>/api/<id>/command/message -d '{"text":"hello","color":"#00ff00"}'
curl -H "Authorization: Bearer <token>" http://<ip>/api/<id>/status
```

**Security.** With an empty token, anyone on your network can control the
plaque. Set a token. Plain HTTP sends that token in clear text on your LAN —
use HTTPS on the HD-WF2 where you can.

### 6.4 HTTPS (HD-WF2 only)

A second server, identical to the plain one but encrypted, on port 443 by
default. The plain HTTP server keeps running.

```
   1. tools/gen_https_cert.sh      --> certs/server_cert.pem, server_key.pem
   2. menuconfig: enable HTTPS     --> build embeds the certificate
   3. flash                        --> board serves https://<ip>:443/...
   4. client reads /api/whoami     --> learns "tls_fingerprint"
   5. client pins that fingerprint --> compares it on every later connection
```

The certificate is **self-signed** (ECDSA P-256, 10-year validity) and tied
to an IP address, not a hostname, so normal CA/hostname validation does not
apply. The model used is **trust on first use**: learn the SHA-256
fingerprint once over `/api/whoami`, then insist every HTTPS connection
presents the same certificate. You can compute the expected value yourself:

```sh
openssl x509 -in certs/server_cert.pem -noout -fingerprint -sha256
```

For quick tests with curl, `-k` skips curl's own checks:

```sh
curl -k -X POST https://<ip>:443/api/<id>/command/red
```

Each TLS connection costs about 40 KB of heap, which is why this is limited
to the HD-WF2.

### 6.5 Finding your device ID

The **device ID** is the board's WiFi station MAC address as 12 lowercase hex
digits (e.g. `aabbccddeeff`). Find it:

- On the panel's "Connected" status screen (the last line);
- From `GET /api/whoami`;
- In your router's client list.

---

## 7. Command Reference

Commands are named after the MQTT topic without the `hub75/` prefix. "Any"
means the payload is ignored. Colours are `#RRGGBB`.

### 7.1 Text

| Command | Payload | Effect |
|---|---|---|
| `message` | `{"text":"...","color":"#RRGGBB"}` | Scroll text (colour optional, default green). Uses the font chosen in menuconfig (8x8 scrolling, or compact 3×5). |
| `off` | any | Blank the panel. |

### 7.2 Large numbers

| Command | Payload | Effect |
|---|---|---|
| `nn` … `nnnnnn` (2–6 digits), optional trailing `L` | optional `{"fg":"#…","bg":"#…","hue":"#…"}` | Any number shown in the Chilanka font, stretched to touch all four panel edges, anti-aliased. `fg` = digit colour (default white), `bg` = background (default black), `hue` = per-channel tint (default `#ffffff` = none). |
| `1300L` | optional JSON as above | The large "1300". |
| `1300LI` | any | Inverted: black digits on tan. |
| `1300`, `13`, `00` | any | Small static fonts (original look). |
| `1300a` / `b` | any | Inverted "1300" on 25 % / 50 % grey. |
| `1300c` / `d` / `e` / `f` | any | "1300" in the compact font at various sizes/inversions. |

Examples:

```sh
curl -X POST http://<ip>/api/<id>/command/3392L -d '{"fg":"#000000","bg":"#d2b48c"}'
```

> Numbers of 1 digit or 7+ digits are not recognised. A bright, light-coloured
> 128×64 panel is a heavy power load — see [section 14](#14-safety-and-power-notes).

### 7.3 Solid colours, brightness and flashing

| Command | Payload | Effect |
|---|---|---|
| `red` `green` `blue` `yellow` `orange` `white` `black` | any | Fill the whole panel. Also the quickest way to test colour wiring. |
| `dim` | any | Brightness ≈ 12.5 % (32/255). |
| `bright` | any | Brightness to the configured cap (default 128/255), ramped over ~1 s. |
| `brightness` | `{"percent":0-100}` | Any level, as a percentage of the same cap. Invalid input = 100. |
| `flash` | any | Toggle blinking of whatever is showing: 3 s on, 2 s off. |
| `noflash` | any | Force blinking off. |

`dim`, `bright`, `brightness`, `flash` and `noflash` are **overlays**: they
modify what is showing rather than replacing it.

### 7.4 Clocks and time

| Command | Effect |
|---|---|
| `clock` | 24-hour HH:MM:SS, odometer-style scrolling digits (default Pacific time, white). Two green dots blink in the corner once NTP has synced. |
| `clock.PST8PDT.blue` | US Pacific (auto-DST), blue |
| `clock.mountain.blue` | US Mountain (auto-DST), blue |
| `clock.central.blue` | US Central (auto-DST), blue |
| `clock.eastern.blue` | US Eastern (auto-DST), blue |
| `clock.IST.blue` | India (UTC+5:30), blue |
| `clock.utc.green` | UTC, green |
| `time` | Bordered display: Pacific time in yellow (top box), Eastern in green (bottom). |
| `ntpset` | Force an immediate resync, show "TIME SET" for ~3 s, then the clock. |

Time comes from **NTP** (SNTP) over WiFi; system time is kept in UTC and only
converted for display. The board's onboard RTC chip is not used.

**NTP behaviour:** a sync happens automatically after boot, and again every
**24 hours**, silently. Only the first sync and `ntpset` blink the corner
pixel. Earlier firmware resynced every 5 minutes with a blink, which caused a
visible full repaint — that was removed so the display stays static.

### 7.5 Animations and screensavers

| Command | Effect |
|---|---|
| `test1` | One random pixel at a time, fading in and out. |
| `test2` | Animated 10-bar graph. |
| `test3` | Pac-Man chomping around the perimeter. |
| `shootingstars` | Three bouncing 2×2 stars with fading tails; sparks on collision. |
| `matrix` | "Digital rain" screensaver (full build). |
| `pong` | Self-playing Pong with seven-segment scores (full build). |
| `tictactoe` | Self-playing tic-tac-toe, X red / O cyan (full build). |

### 7.6 Mazes

| Command | Effect |
|---|---|
| `maze1` | Random maze (random wall colour), 3 px paths, solved live by a red pixel (depth-first search). |
| `maze2` | 1 px paths, denser; fades out and in between mazes. |
| `maze3` | As `maze2`, solver drawn as a 4 px segment. |
| `maze99` | Wilson's-algorithm maze (all mazes equally likely); two balls race — red keeps its right hand on the wall, blue its left — with a running red/tie/blue tally. |
| `maze4`, `maze5` | Maze that leaves **latency breadcrumbs** (full build). |

`maze1`–`maze3` avoid clearing the whole screen every frame; they only erase
the previous dot, which minimises flicker.

### 7.7 Network-latency displays

(Full build only.)

| Command | Effect |
|---|---|
| `ping1` | Scrolling strip-chart of ping times (green fast → red slow, full red = timeout), with the measured milliseconds, a success count and a timeout count. |
| `ping2` | A "comet" launched per ping; speed encodes latency; explosion on success, fizzle on timeout. |
| `maze4` / `maze5` | Maze trail coloured by latency. |

Targets come from the *HD-WF1 Ping* settings (up to 8 `host[:port]` entries,
ICMP or TCP-connect). Each sample picks a target at random.

### 7.8 Elevator and auto-test

| Command | Effect |
|---|---|
| `2` | Elevator floor display counting 1→100→1, odometer digits, red ▲ up / green ▼ down. |
| `2L` | Double-size digits, ~2 s pause per floor (shares the counter with `2`). |
| `2LL` | Quadruple size (tens/ones only). |
| `0` | Auto-test: cycles through the patterns forever. |
| `1` | Stop auto-test and return to `message`. |

### 7.9 Which commands are live in the lean build

By default the firmware is built **lean** to save memory. Live in the lean
build:

`message`, `off`, numbers (`nn`…`nnnnnn`, `1300*`, `13`, `00`), solid colours,
`dim`/`bright`/`brightness`, `flash`/`noflash`, `shootingstars`, `time`,
`maze1`–`maze3`, `maze99`, and all `clock*`/`ntpset`.

Games, matrix rain, elevator, auto-test, ping displays and `maze4`/`maze5`
are compiled but not reachable (a REST call returns 404). See
[section 11](#11-memory-build-variants-and-limits) to enable them.

Commands that **replace** the current display: `test1`–`3`, `off`, all number
topics, `pong`, `tictactoe`, `matrix`, `shootingstars`, `time`, mazes, pings,
solid colours, `0`, `2`/`2L`/`2LL`, `clock*`. They stay up until another such
command arrives.

---

## 8. Onboard Button

| Action | Effect |
|---|---|
| **Short press** | Reboots the board on release. Works with no network or console. |
| **Long press** (~1 s) | Pings the default gateway 5 times and blinks the **lower-right pixel green** for each packet (on while waiting, off between). A timeout still blinks; with no WiFi every ping times out. |

Both work in any display mode. The button is GPIO 11 on the HD-WF1 and GPIO 17
on the HD-WF2.

```
   hold ~1 s             five pings to the router
   ----------+           ping#1  ping#2  ping#3  ping#4  ping#5
   release   |           [***]   [***]   [***]   [***]   [***]   pixel ends OFF
   quick tap --> reboot
```

---

## 9. Saving the Display Across Reboots

By default the plaque forgets what it was showing when power is lost. To make
it permanent:

```
   send commands until it looks right
                 |
                 v
        POST /api/<id>/save
                 |
                 v  stored in flash (NVS): last content command + brightness
                 |
   every boot --> replayed immediately (before WiFi is up)
                 |
        POST /api/<id>/unsave   --> forget it again
```

`save` returns 409 if nothing has been shown yet since boot. Modifiers such as
`flash`, `noflash` and `ntpset` are not remembered.

---

## 10. Firmware Updates

### 10.1 USB flashing

See [section 2](#2-quick-start-flashing). Required for the HD-WF1 every time,
and for the HD-WF2 whenever the flash layout changes (such as the first OTA
build).

### 10.2 Over-the-air (OTA) updates

**HD-WF2 only.** The HD-WF2's flash holds two app slots; an update is written
to the slot that is *not* running, then the board switches to it.

```
   Flash layout (HD-WF2, 8 MB)

   0x009000  nvs        24 KB   saved display, settings
   0x00F000  phy_init    4 KB
   0x010000  otadata     8 KB   which slot boots / rollback state
   0x020000  ota_0     1.5 MB   app slot A  <-- running
   0x1A0000  ota_1     1.5 MB   app slot B  <-- update is written here
   0x320000  coredump   64 KB   crash dumps
   0x330000  applog     64 KB   persistent log
```

```
   OTA flow

   you ----- POST .../ota (binary) ----> board writes to inactive slot
                                                  |
                                       validate image (checksum, chip type)
                                                  |  fail -> 500, old firmware keeps running
                                                  v
                                   set boot slot = new, reboot
                                                  |
                                       new firmware runs "pending verify"
                                                  |
                          stays up ~30 s with WiFi/MQTT/HTTP up?
                              yes -> marked valid, update is permanent
                              no (crash / reset) -> bootloader rolls back to old slot
```

**How to run an update:**

1. Set a **bearer token** in menuconfig (OTA is refused with 403 without one).
2. Build the new version; the file to upload is
   `build_esp32s3/hub75_hello.bin`.
3. Upload it over HTTPS (preferred) or HTTP:

   ```sh
   curl -k -X POST -H "Authorization: Bearer <token>" \
        --data-binary @build_esp32s3/hub75_hello.bin \
        https://<ip>:443/api/<id>/ota
   ```
   Expected reply: `{"ok":true,"rebooting":true}`; the board reboots about a
   second later. The display keeps drawing during the upload.
4. After reboot, confirm with `GET /api/<id>/info` that `version` changed.

**Notes:**

- The **first** OTA-capable build must be flashed over USB, because the
  partition layout changed. After that, USB is not needed.
- The firmware image must be built for the **same chip** (ESP32-S3); other
  images fail validation.
- Always bump `PROJECT_VER` in `CMakeLists.txt` so you can tell the slots
  apart.
- Only one OTA can run at a time (409 otherwise).
- The HD-WF1 has a single 1 MB app slot already about 90 % full, so it has no
  OTA; it answers 501.

---

## 11. Memory, Build Variants and Limits

The HD-WF1 has **no PSRAM** and tens of KB of spare heap; the display
framebuffer alone uses 49 KB. Hence:

- The display is **single-buffered, 6-bit colour**. Double buffering or 8-bit
  colour fails to start or starves WiFi.
- A **lean build** is the default. `HUB75_FULL_EFFECTS` at the top of
  `main/main.cpp` (0 = lean, 1 = full) controls whether games, matrix rain,
  elevator, ping tasks and `maze4`/`maze5` are reachable. Full costs about
  25 KB of heap — fine on the HD-WF2, tight on the HD-WF1.
- Because the display is single-buffered, anything drawn is scanned out
  immediately. Static screens are therefore drawn **once** and not repainted;
  that is why a number stays rock steady.

```
   HD-WF1 (S2)                          HD-WF2 (S3)
   heap  [###.....]  tight              heap  [########]  roomy
   app   [#######.]  ~90% of 1 MB       app   [###.....]  ~63% of 1.5 MB slot
   OTA   no                             OTA   yes
   HTTPS no                             HTTPS optional
```

---

## 12. Logs and Diagnostics

**HD-WF2:** the console is on the USB-Serial/JTAG port. Opening the port can
leave the board in the bootloader; power-cycle if so.

**HD-WF1:** the console is UART, so `idf.py monitor` does not work over USB.
Every log line is also appended to the flash partition `applog`, along with a
heap line every 5 seconds:

```
heap: free=… min_free=… largest=… dma_free=… main_stack_hwm=…
```

Read it with the board in bootloader mode:

```sh
# HD-WF1 (applog at 0x120000); HD-WF2 builds with OTA use 0x330000 instead
python -m esptool --chip esp32s2 -p /dev/ttyACM0 --after no-reset \
    read-flash 0x120000 0x10000 applog.bin
strings applog.bin | less
python -m esptool --chip esp32s2 -p /dev/ttyACM0 --after no-reset \
    erase-region 0x120000 0x10000      # clear before a fresh test
```

Crash dumps are in the `coredump` partition (HD-WF1: 0x110000; HD-WF2 with
OTA: 0x320000) and decode with:

```sh
esp-coredump info_corefile --core core.bin --core-format raw build/hub75_hello.elf
```

Useful runtime checks:

- `GET /api/<id>/status` — mode, NTP state, uptime, free heap.
- The `RR=n` boot value — see [section 5](#5-boot-sequence-and-status-screens).

---

## 13. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Red shows blue, green shows red, blue shows green | Colour pin groups don't match this panel | Use `red`/`green`/`blue` to confirm; adjust the R/G/B pins in the panel's profile in `main/main.cpp` (see [3.3](#33-panels-and-colour-channel-wiring)). |
| Only red appears / colours very dim or skewed | Panel's own 5 V supply missing or too weak | Connect a proper supply to the panel's power terminals ([14](#14-safety-and-power-notes)). |
| Two adjacent pixels identical (ghosting) | Clock-edge integrity | Flip `clkphase` in the panel profile. |
| Panel shows only a quarter / wrong size | Wrong *Panel resolution* | Reconfigure, rebuild, reflash. |
| Display repaints every 5 minutes | Old firmware (periodic NTP blink) | Use v2026.10.10.025 or newer. |
| "Not Connected, Retrying" forever | Wrong WiFi credentials, **or** out of memory (HD-WF1) | Check `heap:` lines in applog; credentials come from `sdkconfig`. |
| HD-WF1 never boots after flash | Pads still bridged | Unbridge and power-cycle. |
| `lsusb` shows `303a:0002` (HD-WF1) | Board is in ROM bootloader | Normal while flashing; unbridge + power-cycle to run the app. |
| HD-WF2 stuck "waiting for download" | Serial port opened during reset | Unplug and replug power. |
| `idf.py flash` says "This chip is ESP32-S3, not ESP32-S2" | Wrong build config for this board | Use the `sdkconfig.esp32s3`/`build_esp32s3` commands. |
| Build error: undeclared `CONFIG_*` | Kconfig edited but not regenerated | `idf.py reconfigure`. |
| Build error: "was configured for project X" | Old build directory | Use `-B <dir>` or `fullclean`. |
| REST returns 404 for a command | Command not in the lean build | Enable `HUB75_FULL_EFFECTS` ([11](#11-memory-build-variants-and-limits)). |
| REST returns 401 | Token missing/wrong | Send `Authorization: Bearer <token>`. |
| OTA returns 403 | No token configured | Set one and reflash. |
| OTA returns 501 | HD-WF1 build | Flash over USB. |
| After OTA the board returns to the old version | New image didn't run 30 s (rollback) | Check logs; fix the crash; try again. |
| Brownout (`RR=9`), flicker at full white | Supply can't deliver the current | Lower `bright` cap, use a stronger supply. |
| Reboot loop right after display starts | Console on ROM USB-CDC (HD-WF1) | Keep the console on UART. |

---

## 14. Safety and Power Notes

- A 128×64 panel can pull **10 A or more** at full white. Use a supply rated
  for it with proper wire gauge and fusing. Light, bright content (white,
  tan, `1300LI`) loads it far more than dark content.
- The **default brightness cap is 128/255** (`HD-WF1 Power`) precisely to
  protect undersized supplies. Raise it only with an adequate supply.
- The panel's 5 V terminals are **separate** from the controller's USB
  power. Both must be powered.
- Power the panel and controller from supplies sharing a common ground.
- Never hot-plug the HUB75E ribbon.
- Treat the API token and your `sdkconfig*` files as secrets.
- OTA and the REST API are only as safe as your network; use a token and,
  where available, HTTPS.

---

## 15. Credits

**Author and maintainer:** Michael J. Wolters

**Acknowledgements and third-party components:**

- **ESP32-HUB75-MatrixPanel-DMA** (`mrcodetastic`) — the display driver
  library, patched in this project to build under ESP-IDF v6.
- **HD-WF1-WF2-LED-MatrixPanel-DMA** (`mrcodetastic`) — community pin
  documentation for the Huidu boards.
- **ESP-IDF** (Espressif Systems) — framework, WiFi, MQTT, HTTP/HTTPS server,
  OTA, mbedTLS.
- **cJSON** — JSON parsing.
- **Chilanka** font (SIL Open Font License) — the large-number glyphs.
- **Adafruit-GFX** drawing primitives, via the display library.
- The TI-99/4A assembly maze program that `maze99` was ported from.

Version of this manual: matches firmware **v2026.10.10.026**.

---

## Appendix A: Glossary

| Term | Meaning |
|---|---|
| **75EX1 / 75EX2** | The HD-WF2's two HUB75E connectors; only 75EX1 is supported (they share control lines). |
| **applog** | A 64 KB flash partition where every log line is saved, readable without a console. |
| **Bearer token** | A secret string sent as `Authorization: Bearer <token>` to authorise REST requests. |
| **Bootloader (ROM)** | Built-in chip code that accepts new firmware over USB. |
| **Brownout** | The supply voltage sags and the chip resets (reset reason 9). |
| **CIE curve** | A perceptual brightness curve used so dim levels look right. |
| **clkphase** | A panel setting that selects the clock edge; used to fix doubled pixels. |
| **Coredump** | A saved crash snapshot used for post-mortem debugging. |
| **Device ID** | The WiFi MAC as 12 lowercase hex digits; namespaces topics and paths. |
| **DMA** | Direct Memory Access; hardware that streams pixel data to the panel with no CPU effort. |
| **ESP-IDF** | Espressif's development framework for the ESP32 family. |
| **ESP32-S2 / S3** | The microcontrollers on the HD-WF1 / HD-WF2. |
| **FM6126A** | The LED driver chip family used by the supported panels. |
| **Fingerprint** | SHA-256 hash of the HTTPS certificate, used for pinning. |
| **GPIO** | A general-purpose input/output pin. |
| **HD-WF1 / HD-WF2** | Huidu LED controller cards (ESP32-S2 / ESP32-S3). |
| **Heap** | Memory available for dynamic allocation; scarce on the HD-WF1. |
| **HUB75E** | The standard 16-pin ribbon connector/protocol for RGB LED matrix panels (E = fifth row-address line for tall panels). |
| **Lean build** | Default build with memory-heavy effects disabled (`HUB75_FULL_EFFECTS=0`). |
| **LWT** | MQTT "last will": the broker publishes `offline` if the device drops uncleanly. |
| **menuconfig** | ESP-IDF's text menu for build-time settings. |
| **MQTT** | Lightweight publish/subscribe messaging protocol; needs a broker. |
| **mutex** | A lock protecting state shared between the render loop and other tasks. |
| **NTP / SNTP** | Network Time Protocol; sets the clock over the network. |
| **NVS** | Non-volatile storage partition in flash; holds the saved display. |
| **OE / LAT / CLK** | Output-enable, latch and clock lines of the HUB75 interface. |
| **OTA** | Over-the-air firmware update, with no USB cable. |
| **otadata** | Partition recording which OTA slot to boot and rollback state. |
| **Overlay** | A command (`dim`, `flash`…) that modifies the current display instead of replacing it. |
| **PanelProfile** | The per-panel/per-board structure holding size and R/G/B pin assignments. |
| **PSRAM** | External RAM; neither board has any populated/enabled. |
| **REST API** | The HTTP interface for commands (`/api/<id>/command/<name>`). |
| **Render loop** | The ~30 ms loop in `app_main()` that draws the current mode. |
| **Rollback** | Automatic return to the previous firmware slot if a new one fails to confirm itself. |
| **sdkconfig** | The file holding your build settings and secrets. |
| **Single-buffered** | Drawing goes directly to the memory being displayed; no back buffer. |
| **TOFU** | Trust on first use; remember a certificate fingerprint the first time and require it afterwards. |
| **Topic** | An MQTT channel name such as `hub75/<id>/red`. |
| **USB-Serial/JTAG** | The ESP32-S3's built-in USB interface for flashing and console. |
| **Wilson's algorithm** | A maze-generation method giving every possible maze equal probability (`maze99`). |

---

## Appendix B: Index

Section numbers refer to the headings in this manual.

**A**
API token — 4.2, 6.3, 10.2, 14 ·
applog — 12, A ·
Author — 15 ·
Auto-test (`0`, `1`) — 7.8

**B**
Bearer token — 4.2, 6.3 ·
`bright`, `brightness` — 7.3 ·
Brightness cap — 4.2, 7.3, 14 ·
Boot sequence — 5 ·
Boot splash — 4.2, 5 ·
Bootloader (ROM) — 2.3, 2.4, 13 ·
Button — 8 ·
Brownout — 13, 14

**C**
Clocks (`clock*`, `time`) — 7.4 ·
`clkphase` — 13 ·
Colour-channel wiring — 3.3, 13 ·
Configuration — 4 ·
Core dump — 12 ·
Credentials — 4.1, 4.3, 14 ·
Credits — 15

**D**
Default text — 4.2 ·
Device ID — 6.5 ·
`dim` — 7.3 ·
`dispatch_command` — 6.1

**E**
Elevator (`2`, `2L`, `2LL`) — 7.8 ·
ESP-IDF — 2.1, 15

**F**
`flash`, `noflash` — 7.3 ·
Flashing — 2, 10.1 ·
Flash layout — 10.2 ·
Full build (`HUB75_FULL_EFFECTS`) — 7.9, 11

**G**
Games (`pong`, `tictactoe`) — 7.5 ·
Gateway ping (button) — 8 ·
Glossary — Appendix A

**H**
HD-WF1 — 2.3, 3.2, 3.4 ·
HD-WF2 — 2.2, 3.2, 3.4 ·
Heap — 11 ·
HTTPS — 6.4 ·
HUB75E — 3, A

**I**
Info endpoint — 6.3 ·
Introduction — 1

**K**
Kconfig — 4.1

**L**
Large numbers — 7.2 ·
Lean build — 7.9, 11 ·
Logs — 12

**M**
`matrix` — 7.5 ·
Mazes — 7.6 ·
`message` — 7.1 ·
menuconfig — 4 ·
Memory — 11 ·
MQTT — 6.2

**N**
NTP — 4.2, 7.4 ·
`ntpset` — 7.4 ·
Numbers — 7.2

**O**
`off` — 7.1 ·
OTA — 10.2 ·
Overlay commands — 7.3

**P**
Panel resolution — 3.3, 4.2 ·
Pin tables — 3.4 ·
Ping displays — 7.7 ·
Power — 3.5, 14

**Q**
Quick start — 2

**R**
Reset reason (`RR=n`) — 5 ·
REST API — 6.3 ·
Rollback — 10.2 ·
Render loop — 6.1, 11

**S**
Save / unsave — 9 ·
Security — 6.3, 14 ·
`shootingstars` — 7.5 ·
Solid colours — 7.3 ·
Splash — 5

**T**
Troubleshooting — 13 ·
Table of contents — top

**U**
USB flashing — 2, 10.1

**V**
Version — header, 6.3

**W**
WiFi — 4.2, 5 ·
`whoami` — 6.3, 6.5

**Z**
(no entries)
