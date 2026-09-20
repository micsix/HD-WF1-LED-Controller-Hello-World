// Lean build (default): only the effects that matter are subscribed/active -
// static text, solid colours, dim/bright, clocks, shooting stars, mazes 1-3.
// Everything else (games, ray tracer, matrix, flags, ping-based effects,
// maze4/5 breadcrumbs, elevator, auto-test) is compiled but never subscribed,
// and the ping/maze4 tasks and breadcrumb arrays are left out, which frees
// ~25KB of heap on the 128x64 panel (see applog heap logging). Set to 1 to get
// everything back.
#ifndef HUB75_FULL_EFFECTS
#define HUB75_FULL_EFFECTS 0
#endif

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include <cstdarg>
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

// CONFIG_PING_TARGET_TCP_PORT only exists when 'Ping method' (menuconfig) is
// set to TCP connect (it `depends on PING_METHOD_TCP`) - but parse_ping_targets()
// stores a port for every target regardless of which method is active, so
// it needs this to always resolve to something. Matches the Kconfig default.
#ifndef CONFIG_PING_TARGET_TCP_PORT
#define CONFIG_PING_TARGET_TCP_PORT 80
#endif

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include "font8x8_basic.h"
#include "chilanka_1300.h"

static const char *TAG = "hub75_hello";
static const char *WIFI_TAG = "hub75_wifi";

namespace {

// Text + color currently being scrolled, updated by incoming MQTT messages
// and read once per frame by the render loop in app_main(). Protected by
// display_mutex since it's written from the MQTT event task and read from
// the main task.
constexpr size_t kMaxDisplayText = 128;
SemaphoreHandle_t display_mutex = nullptr;
char display_text[kMaxDisplayText] = "Hello, world!";
uint8_t display_r = 0, display_g = 255, display_b = 0;
bool display_dirty = true;  // set when text/color changes, so the render loop can reset scroll position

// Global panel brightness, applied by the render loop regardless of which
// DisplayMode is active - controlled by the hub75/dim (~12.5%, see
// kDimBrightness below) and hub75/bright (100%) topics, independent of
// what's currently on screen. Starts dim (kDimBrightness's value, 32) so
// the panel comes up gentle by default rather than at full brightness.
uint8_t target_brightness = 32;

// Global blink overlay, independent of DisplayMode (same idea as
// target_brightness) - toggled on/off by each hub75/flash message. When on,
// the render loop shows whatever the current mode would normally draw for
// 3s, then blanks the screen for 2s, repeating - see the blink_tick logic in
// app_main()'s render loop.
bool blink_enabled = false;

// hub75/clock: set true once NTP has synced at least once (see ntp_sync_cb).
// The clock render draws two blinking green 2x2 "good connection" dots in
// the lower-right corner while this is true - guarded by display_mutex like
// the rest of this shared display state.
bool ntp_synced = false;

// hub75/ntpset: set true right before forcing a resync, so ntp_sync_cb()
// knows this particular sync was explicitly requested (vs. the silent
// automatic/periodic ones) and should take over the display with the "TIME
// SET" confirmation. Consumed (reset to false) by ntp_sync_cb() itself.
bool ntp_manual_resync_requested = false;

// hub75/clock.<zone>.<color>: which zone the clock renders in - default
// Pacific per the initial requirement (see apply_clock_tz()/ntp_init()).
// India (IST) has no DST, hence no ",Mx.y.z" transition rule in its POSIX TZ
// string below; the four US zones do.
enum class ClockTz { kUtc, kPacific, kIndia, kMountain, kCentral, kEastern };
constexpr const char *kClockTzPosix[] = {
    "UTC0", "PST8PDT,M3.2.0,M11.1.0", "IST-5:30", "MST7MDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0",
    "EST5EDT,M3.2.0,M11.1.0",
};
ClockTz clock_tz = ClockTz::kPacific;
uint8_t clock_color_r = 255, clock_color_g = 255, clock_color_b = 255;

// Solid full-panel color, shown while display_mode == kSolidColor - set by
// the hub75/red, hub75/green, hub75/blue, hub75/yellow, hub75/orange topics.
uint8_t solid_r = 255, solid_g = 0, solid_b = 0;

// hub75/1300L colours, set from its optional JSON payload (see set_fixed1300l())
// - guarded by display_mutex like the rest. hue is a per-channel tint applied
// to both fg and bg (#ffffff = none).
uint8_t f1300l_fg[3] = {255, 255, 255}, f1300l_bg[3] = {0, 0, 0}, f1300l_hue[3] = {255, 255, 255};

// HUB75E panel geometry (single chain). Now 128x64 (1/32 scan, uses the E
// address line) - was 64x32. Defined up here since state below is sized by it.
#define PANEL_WIDTH  128
#define PANEL_HEIGHT 64
#define PANEL_CHAIN  1

// hub75/rainbow: one random color per panel row, generated fresh each time
// the topic fires and held static (same style as kSolidColor) until
// something else is displayed.
uint8_t rainbow_row_r[PANEL_HEIGHT], rainbow_row_g[PANEL_HEIGHT], rainbow_row_b[PANEL_HEIGHT];

// hub75/matrix1/2/3(c): grids of randomly colored squares - one shared color
// buffer sized for the largest grid (matrix3, 16 rows x 32 cols = 512
// cells), reused across all six variants since only one is ever displayed
// at a time. Randomized fresh on each hub75/matrix* trigger; the *c
// (continuous) variants also re-randomize every ~1s while active - see
// randomize_matrix()/set_matrix() and matrix_cont_tick in app_main().
constexpr int kMatrixMaxCells = 16 * 32;
uint8_t matrix_cell_r[kMatrixMaxCells];
uint8_t matrix_cell_g[kMatrixMaxCells];
uint8_t matrix_cell_b[kMatrixMaxCells];

// hub75/ray1 and hub75/ray2: colors for the random ray-traced scene(s)
// (sphere(s) + checkerboard floor) - randomized fresh each time either topic
// fires, same style as kRainbow above. ray_sphere_r/g/b is the first (and
// for hub75/ray1, only) sphere; ray2_sphere2_r/g/b is hub75/ray2's second
// sphere. See set_raytrace()/draw_raytracer().
uint8_t ray_sphere_r = 200, ray_sphere_g = 60, ray_sphere_b = 60;
uint8_t ray2_sphere2_r = 60, ray2_sphere2_g = 60, ray2_sphere2_b = 200;
uint8_t ray_floor1_r = 90, ray_floor1_g = 90, ray_floor1_b = 90;
uint8_t ray_floor2_r = 30, ray_floor2_g = 30, ray_floor2_b = 30;

// Until the first MQTT message arrives, the panel shows live WiFi connection
// status instead of display_text - see set_wifi_status_connecting()/
// set_wifi_status_connected() and handle_message_payload()'s one-way
// transition out of WifiStatus mode. kTest1/kTest2/kTest3 are diagnostic/fun
// patterns, kSolidColor/kRainbow/kFlagUsa/kFlagCanada are simple fills, and
// kAutoTest (hub75/0) cycles through all of the above in sequence forever -
// all triggered by their respective hub75/* topics (see mqtt_event_handler)
// and each running until another message arrives on any of these topics
// (kAutoTest specifically runs until hub75/1).
enum class DisplayMode {
  kWifiStatus,
  kMessage,
  kTest1,
  kTest2,
  kTest3,
  kSolidColor,
  kRainbow,
  kFlagUsa,
  kFlagCanada,
  kAutoTest,
  kElevator,
  kElevatorLarge,
  kElevatorXLarge,
  kClock,
  kClockConfirm,
  kOff,
  kRayTrace,
  kRayTrace2,
  kStandardTest1,
  kSpaceInvaders,
  kMillipede,
  kPong,
  kTicTacToe,
  kMatrix1,
  kMatrix2,
  kMatrix3,
  kMatrix1Cont,
  kMatrix2Cont,
  kMatrix3Cont,
  kBall1,
  kShootingStars,
  kTimeFrame,
  kMaze1,
  kMaze2,
  kMaze3,
  kMaze4,
  kMaze5,
  kPing1,
  kPing2,
  kMatrixRain,
  kFixed1300,
  kFixed1300Alt,
  kFixed1300B,
  kFixed1300C,
  kFixed1300D,
  kFixed13,
  kFixed00,
  kFixed1300E,
  kFixed1300F,
  kFixed1300L,
  kFixed1300LI
};
DisplayMode display_mode = DisplayMode::kWifiStatus;

// hub75/clock: kClockConfirm shows "TIME SET" for kClockConfirmTicks frames,
// then auto-advances to kClock - see ntp_sync_cb() and the render loop. Only
// ever entered for a hub75/ntpset-triggered resync (an explicit user
// request) - a background sync (the initial one after boot, or the periodic
// resync every 5 minutes) stays silent instead, so it never interrupts
// whatever else is currently on screen (e.g. hub75/ping1).
constexpr int kClockConfirmTicks = 100;  // 100 * 30ms = 3.0s

constexpr int kMaxStatusLines = 3;
constexpr size_t kMaxStatusLineLen = 32;
// Row Y for each status line - static, horizontally and vertically
// centered, never scrolled, so no per-line scroll-position state is needed.
// 11px pitch between rows (5px glyph + 6px gap) with the whole 3-row block
// (27px tall) centered in the confirmed 32px-tall panel.
constexpr int16_t kStatusRowY[kMaxStatusLines] = {2, 13, 24};

struct StatusLine {
  char text[kMaxStatusLineLen] = "";
  uint8_t r = 0, g = 0, b = 0;
};
StatusLine status_lines[kMaxStatusLines];
int status_line_count = 0;

bool parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
  if (s == nullptr) return false;
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  char byte_buf[3] = {0, 0, 0};
  auto hex_byte = [&](const char *p) -> int {
    for (int i = 0; i < 2; i++) {
      if (!isxdigit(static_cast<unsigned char>(p[i]))) return -1;
    }
    byte_buf[0] = p[0];
    byte_buf[1] = p[1];
    return static_cast<int>(strtol(byte_buf, nullptr, 16));
  };
  const int rv = hex_byte(s);
  const int gv = hex_byte(s + 2);
  const int bv = hex_byte(s + 4);
  if (rv < 0 || gv < 0 || bv < 0) return false;
  *r = static_cast<uint8_t>(rv);
  *g = static_cast<uint8_t>(gv);
  *b = static_cast<uint8_t>(bv);
  return true;
}

// WiFi is entirely optional here - if CONFIG_WIFI_SSID is left empty
// (the default), wifi_init() is skipped and the matrix runs exactly as
// before. Credentials come from Kconfig (main/Kconfig.projbuild), set via
// 'idf.py menuconfig' - they land only in sdkconfig, which is gitignored,
// never in a file tracked by git.
constexpr int kWifiConnectedBit = BIT0;
constexpr int kWifiRetryDelayMs = 4000;
EventGroupHandle_t wifi_event_group = nullptr;
// True while WiFi has an IP - read by the render loop (boot splash's green
// "connected" pixel). Plain volatile bool: single writer (WiFi event handler).
volatile bool wifi_link_up = false;
int wifi_attempt_count = 0;

// hub75/ping1: a scrolling latency graph (like a network monitor's ping
// strip-chart) - one column per sample, oldest at index 0/left, newest at
// the last index/right. ping1_task() pings on its own cadence and shifts a
// new sample in; the render loop just redraws the whole graph every frame,
// so the display itself is always smooth regardless of how slow or fast
// the network actually is - only ping1_task() below ever waits on the
// network. Protected by display_mutex like the other cross-task display
// state above. kPing1GraphWidth matches PANEL_WIDTH - one sample column per pixel column.
constexpr int kPing1GraphWidth = PANEL_WIDTH;
constexpr uint16_t kPing1Timeout = 0xffff;  // sentinel: this column's ping had no reply

// Up to 8 ping targets (see CONFIG_PING_TARGETS, 'HD-WF1 Ping' in
// menuconfig), shared by hub75/ping1 and hub75/ping2. Populated once by
// parse_ping_targets() (called from app_main() before either ping task
// starts) and read-only after that, so no locking is needed. Each ping
// (see ping_target_once_ms()) picks one of these at random - a "random
// round-robin" across the whole list rather than a fixed sequence.
constexpr int kMaxPingTargets = 8;
struct PingTarget {
  char host[64];  // a literal IPv4 address or a hostname - resolved at ping time, see resolve_target_ipv4()
  uint16_t port;  // only used when CONFIG_PING_METHOD_TCP is set
};
PingTarget ping_targets[kMaxPingTargets];
int ping_target_count = 0;

uint16_t ping1_graph_ms[kPing1GraphWidth] = {0};  // 0 = "no sample yet" (drawn as a bare baseline dot)
double ping1_last_rtt_ms = 0.0;  // most recent ping's round-trip time, shown numerically on screen
uint32_t ping1_success_count = 0;  // successful replies since mode entry - shown left of the readout, green
uint32_t ping1_timeout_count = 0;  // timeouts since mode entry - shown right of the readout, red

// hub75/ping2: a "comet" launch trigger. ping2_task() pings, then drops the
// result here as a one-shot trigger; the render loop's run_ping2_frame()
// (inside app_main()) picks it up once idle and animates the whole
// flight/impact itself, frame by frame - same "background task only ever
// waits on the network, the render loop always animates smoothly" split as
// hub75/ping1, just with a flashier payoff. Protected by display_mutex.
bool ping2_launch_pending = false;
bool ping2_launch_timed_out = false;
uint8_t ping2_launch_r = 0, ping2_launch_g = 255, ping2_launch_b = 0;
int ping2_launch_frames = 6;
double ping2_last_rtt_ms = 0.0;

// hub75/maze4 and hub75/maze5 (the two "breadcrumb" mazes - maze4 at
// maze1's 3px path width, maze5 at maze2's 1px width): maze4_task() pings
// on its own cadence (same ping_target_once_ms() machinery as ping1/ping2)
// and stores the latest response-time color bucket here, for whichever of
// the two is active. run_maze_frame() reads this once per solver step and
// stamps it as a permanent breadcrumb on the cell just entered - so the
// color always reflects a ping actually sent, without the solver's
// movement ever stalling on the network. Same
// green/yellow/orange/red-orange/timeout-red thresholds as hub75/ping1's
// graph. Protected by display_mutex.
uint8_t maze4_latency_r = 0, maze4_latency_g = 255, maze4_latency_b = 0;

// Context shared between ping_once_ms() and its (non-capturing, so
// plain-C-function-pointer-compatible) esp_ping callbacks below.
struct PingCtx {
  SemaphoreHandle_t done;
  double rtt_ms;
  double timeout_ms;
  int64_t start_us;  // esp_timer_get_time() at the moment esp_ping_start() was called
};

// Blocking: sends exactly one ICMP echo to `host_ip` (a literal IPv4/IPv6
// address, not a hostname) and returns its round-trip time in ms, as a real
// (fractional) number - esp_ping's own profile data is only ms-resolution,
// so this times the request-to-reply gap itself via esp_timer_get_time()
// (microsecond resolution) instead. Returns `timeout_ms` if there was no
// reply in time, so a dead/unreachable host still produces a usable (if
// long) delay instead of hanging forever.
double ping_once_ms(const char *host_ip, uint32_t timeout_ms) {
  PingCtx ctx{xSemaphoreCreateBinary(), static_cast<double>(timeout_ms), static_cast<double>(timeout_ms), 0};
  if (ctx.done == nullptr) return ctx.timeout_ms;

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.count = 1;
  config.timeout_ms = timeout_ms;
  if (!ipaddr_aton(host_ip, &config.target_addr)) {
    vSemaphoreDelete(ctx.done);
    return ctx.timeout_ms;
  }

  esp_ping_callbacks_t cbs = {};
  cbs.cb_args = &ctx;
  cbs.on_ping_success = [](esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    auto *c = static_cast<PingCtx *>(args);
    const int64_t elapsed_us = esp_timer_get_time() - c->start_us;
    c->rtt_ms = static_cast<double>(elapsed_us) / 1000.0;
  };
  cbs.on_ping_timeout = [](esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    auto *c = static_cast<PingCtx *>(args);
    c->rtt_ms = c->timeout_ms;  // no reply - dwell for the full timeout instead
  };
  cbs.on_ping_end = [](esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    xSemaphoreGive(static_cast<PingCtx *>(args)->done);
  };

  esp_ping_handle_t hdl;
  if (esp_ping_new_session(&config, &cbs, &hdl) != ESP_OK) {
    vSemaphoreDelete(ctx.done);
    return ctx.timeout_ms;
  }
  ctx.start_us = esp_timer_get_time();
  esp_ping_start(hdl);
  // A little slack beyond the ping's own timeout so this can't hang forever
  // if something inside esp_ping itself misbehaves.
  xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeout_ms + 2000));
  esp_ping_delete_session(hdl);
  vSemaphoreDelete(ctx.done);
  return ctx.rtt_ms;
}

// Blocking: times a TCP three-way handshake to `host_ip`:`port` (host must be
// a literal IPv4 address) and returns the elapsed time in ms as a real
// (fractional) number, same convention as ping_once_ms() above - so either
// can sit behind ping_target_once_ms() below without the callers caring
// which one actually ran. Works through firewalls that drop ICMP but allow
// the given port; a refused (closed-port) or unanswered connection both
// count as `timeout_ms`, same as an ICMP timeout - so aim it at a port
// that's actually open on the target for a meaningful reading.
double tcp_ping_once_ms(const char *host_ip, uint16_t port, uint32_t timeout_ms) {
  const int64_t start_us = esp_timer_get_time();
  const double timeout_val = static_cast<double>(timeout_ms);

  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return timeout_val;

  // Non-blocking connect + select-with-timeout, so a black-holed host (no
  // reply at all, the case ICMP timeouts can't distinguish from "slow") is
  // still bounded by timeout_ms rather than the OS's own much longer TCP
  // connect timeout.
  const int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host_ip, &addr.sin_addr) != 1) {
    close(sock);
    return timeout_val;
  }

  double rtt_ms = timeout_val;
  const int rc = connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
  if (rc == 0) {
    // Connected immediately - rare, but possible on a very short path.
    rtt_ms = static_cast<double>(esp_timer_get_time() - start_us) / 1000.0;
  } else if (errno == EINPROGRESS) {
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    if (select(sock + 1, nullptr, &write_fds, nullptr, &tv) > 0) {
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
      if (so_error == 0) {
        rtt_ms = static_cast<double>(esp_timer_get_time() - start_us) / 1000.0;
      }  // else: refused or otherwise failed - leave rtt_ms at timeout_val
    }
  }
  close(sock);
  return rtt_ms;
}

// Parses CONFIG_PING_TARGETS (see 'HD-WF1 Ping' in menuconfig) - a
// comma-separated list of up to kMaxPingTargets "host" or "host:port"
// entries - into ping_targets/ping_target_count. Called once from
// app_main() before ping1_task()/ping2_task() start; the result is
// read-only after that, so no locking is needed. An entry with no ":port"
// gets CONFIG_PING_TARGET_TCP_PORT as its port. Anything past the 8th entry
// is ignored; an empty/unparseable list falls back to a single hardcoded
// target so the ping tasks always have something to do.
void parse_ping_targets() {
  ping_target_count = 0;

  char buf[8 * (sizeof(PingTarget::host) + 8)];
  strlcpy(buf, CONFIG_PING_TARGETS, sizeof(buf));

  char *saveptr = nullptr;
  char *entry = strtok_r(buf, ",", &saveptr);
  while (entry != nullptr && ping_target_count < kMaxPingTargets) {
    while (*entry == ' ') entry++;  // tolerate "host1, host2" as well as "host1,host2"
    char *end = entry + strlen(entry);
    while (end > entry && *(end - 1) == ' ') *(--end) = '\0';

    if (*entry != '\0') {
      PingTarget &t = ping_targets[ping_target_count];
      t.port = static_cast<uint16_t>(CONFIG_PING_TARGET_TCP_PORT);
      char *colon = strrchr(entry, ':');  // IPv4/hostname only, so the last ':' (if any) is the port separator
      if (colon != nullptr) {
        *colon = '\0';
        const int parsed_port = atoi(colon + 1);
        if (parsed_port > 0 && parsed_port <= 65535) t.port = static_cast<uint16_t>(parsed_port);
      }
      strlcpy(t.host, entry, sizeof(t.host));
      ESP_LOGI(TAG, "ping target %d: %s:%u", ping_target_count, t.host, t.port);
      ping_target_count++;
    }
    entry = strtok_r(nullptr, ",", &saveptr);
  }

  if (ping_target_count == 0) {
    ESP_LOGW(TAG, "CONFIG_PING_TARGETS had no usable entries - falling back to 192.168.92.1");
    strlcpy(ping_targets[0].host, "192.168.92.1", sizeof(ping_targets[0].host));
    ping_targets[0].port = static_cast<uint16_t>(CONFIG_PING_TARGET_TCP_PORT);
    ping_target_count = 1;
  }
}

// Resolves `host` (a literal IPv4 address or a hostname) to a literal IPv4
// address string in `out` - both ping_once_ms() and tcp_ping_once_ms() need
// a literal address, and getaddrinfo() happily accepts either form, so this
// is the one place that has to know the difference. Returns false (leaving
// `out` untouched) if resolution fails; note a hung/unresponsive DNS server
// can make this block well beyond the ping's own timeout_ms, since it's not
// separately bounded here.
bool resolve_target_ipv4(const char *host, char *out, size_t out_len) {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) {
    return false;
  }
  auto *addr = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
  const bool ok = inet_ntop(AF_INET, &addr->sin_addr, out, out_len) != nullptr;
  freeaddrinfo(res);
  return ok;
}

// hub75/ping1 and hub75/ping2 both call only this. Picks one target at
// random from ping_targets (a "random round-robin" across the configured
// list - see CONFIG_PING_TARGETS), resolves it, and pings it; which
// underlying method (ICMP echo vs TCP connect) actually runs is a single
// build-time choice (see 'Ping method' under 'HD-WF1 Ping' in menuconfig),
// not a per-call decision.
double ping_target_once_ms(uint32_t timeout_ms) {
  if (ping_target_count <= 0) return static_cast<double>(timeout_ms);  // parse_ping_targets() hasn't run yet

  const int idx = static_cast<int>(esp_random() % static_cast<uint32_t>(ping_target_count));
  const PingTarget &target = ping_targets[idx];

  char resolved_ip[16];  // "255.255.255.255" + NUL
  if (!resolve_target_ipv4(target.host, resolved_ip, sizeof(resolved_ip))) {
    return static_cast<double>(timeout_ms);  // DNS failure counts as a miss, same as a ping timeout
  }

#if CONFIG_PING_METHOD_TCP
  return tcp_ping_once_ms(resolved_ip, target.port, timeout_ms);
#else
  return ping_once_ms(resolved_ip, timeout_ms);
#endif
}

// Runs continuously in the background (started once from app_main(), like
// network_init_task) so a network-bound ping never blocks the render loop -
// this is the ONLY thing in hub75/ping1 that ever waits on the network; the
// render loop (see DisplayMode::kPing1) just redraws whatever's currently
// in ping1_graph_ms every ~30ms tick, so the graph always animates smoothly
// no matter how slow or flaky the ping itself is. Only actually pings while
// hub75/ping1 is the active mode; otherwise it just naps and re-checks,
// matching the ESP32-friendly "background task updates shared state under
// the mutex, render loop just reads it" pattern used for WiFi status
// elsewhere in this file.
void ping1_task(void *arg) {
  (void)arg;
  constexpr uint32_t kPingTimeoutMs = 500;   // generous enough for a real reply, still bounded
  constexpr uint32_t kPingIntervalMs = 250;  // minimum gap between samples - paces the scroll
  while (true) {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    const bool active = (display_mode == DisplayMode::kPing1);
    xSemaphoreGive(display_mutex);

    if (!active) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    const double rtt_ms = ping_target_once_ms(kPingTimeoutMs);
    const bool timed_out = rtt_ms >= static_cast<double>(kPingTimeoutMs);
    const uint16_t sample = timed_out ? kPing1Timeout : static_cast<uint16_t>(rtt_ms + 0.5);

    xSemaphoreTake(display_mutex, portMAX_DELAY);
    ping1_last_rtt_ms = rtt_ms;  // shown numerically (as a real number), always the true measurement
    for (int i = 0; i < kPing1GraphWidth - 1; i++) ping1_graph_ms[i] = ping1_graph_ms[i + 1];
    ping1_graph_ms[kPing1GraphWidth - 1] = sample;  // newest sample scrolls in on the right
    if (timed_out) {
      ping1_timeout_count++;
    } else {
      ping1_success_count++;
    }
    xSemaphoreGive(display_mutex);

    vTaskDelay(pdMS_TO_TICKS(kPingIntervalMs));
  }
}

// Same "background task only ever waits on the network" split as
// ping1_task() above, but instead of appending to a graph it drops a
// one-shot "launch" trigger for run_ping2_frame() (in app_main()) to pick
// up - color and flight speed are bucketed into the same four latency
// bands as hub75/ping1 (plus a distinct "timed out" band) so the two
// effects read as one consistent language, just very differently staged.
void ping2_task(void *arg) {
  (void)arg;
  constexpr uint32_t kPingTimeoutMs = 500;
  constexpr uint32_t kPingIntervalMs = 150;
  while (true) {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    const bool active = (display_mode == DisplayMode::kPing2);
    xSemaphoreGive(display_mutex);

    if (!active) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    const double rtt_ms = ping_target_once_ms(kPingTimeoutMs);
    const bool timed_out = rtt_ms >= static_cast<double>(kPingTimeoutMs);

    uint8_t r, g, b;
    int frames;  // frames to cross the panel - lower = a faster, more dazzling dart
    if (timed_out) {
      r = 140;
      g = 0;
      b = 0;
      frames = 15;
    } else if (rtt_ms < 15.0) {
      r = 0;
      g = 255;
      b = 0;
      frames = 3;
    } else if (rtt_ms < 40.0) {
      r = 255;
      g = 255;
      b = 0;
      frames = 6;
    } else if (rtt_ms < 80.0) {
      r = 255;
      g = 140;
      b = 0;
      frames = 10;
    } else {
      r = 255;
      g = 40;
      b = 0;
      frames = 15;
    }

    xSemaphoreTake(display_mutex, portMAX_DELAY);
    ping2_last_rtt_ms = rtt_ms;
    ping2_launch_pending = true;
    ping2_launch_timed_out = timed_out;
    ping2_launch_r = r;
    ping2_launch_g = g;
    ping2_launch_b = b;
    ping2_launch_frames = frames;
    xSemaphoreGive(display_mutex);

    vTaskDelay(pdMS_TO_TICKS(kPingIntervalMs));
  }
}

// Same "background task only ever waits on the network" split as
// ping1_task()/ping2_task() above - pings on its own cadence and just
// updates maze4_latency_r/g/b, using the exact same latency thresholds as
// hub75/ping1's graph. Active for either of the two breadcrumb mazes
// (hub75/maze4 and hub75/maze5 - they differ only in path width, not in how
// breadcrumbs are colored, so one task/one color state covers both).
// run_maze_frame() reads whatever's latest once per solver step and stamps
// it as a breadcrumb - the maze's own movement never waits on this.
void maze4_task(void *arg) {
  (void)arg;
  constexpr uint32_t kPingTimeoutMs = 500;
  constexpr uint32_t kPingIntervalMs = 200;
  while (true) {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    const bool active = (display_mode == DisplayMode::kMaze4 || display_mode == DisplayMode::kMaze5);
    xSemaphoreGive(display_mutex);

    if (!active) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    const double rtt_ms = ping_target_once_ms(kPingTimeoutMs);
    const bool timed_out = rtt_ms >= static_cast<double>(kPingTimeoutMs);

    uint8_t r, g, b;
    if (timed_out) {
      r = 255;
      g = 0;
      b = 0;
    } else if (rtt_ms < 15.0) {
      r = 0;
      g = 255;
      b = 0;
    } else if (rtt_ms < 40.0) {
      r = 255;
      g = 255;
      b = 0;
    } else if (rtt_ms < 80.0) {
      r = 255;
      g = 140;
      b = 0;
    } else {
      r = 255;
      g = 40;
      b = 0;
    }

    xSemaphoreTake(display_mutex, portMAX_DELAY);
    maze4_latency_r = r;
    maze4_latency_g = g;
    maze4_latency_b = b;
    xSemaphoreGive(display_mutex);

    vTaskDelay(pdMS_TO_TICKS(kPingIntervalMs));
  }
}

// No-op once display_mode has already moved on to kMessage (first MQTT
// message received) - WiFi status is only shown until then.
void set_wifi_status_connecting(int attempt) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  if (display_mode == DisplayMode::kWifiStatus) {
    strlcpy(status_lines[0].text, "Not Connected", kMaxStatusLineLen);
    status_lines[0].r = 255;
    status_lines[0].g = 0;
    status_lines[0].b = 0;
    strlcpy(status_lines[1].text, "Retrying...", kMaxStatusLineLen);
    status_lines[1].r = 255;
    status_lines[1].g = 0;
    status_lines[1].b = 0;
    snprintf(status_lines[2].text, kMaxStatusLineLen, "Attempt %d", attempt);
    status_lines[2].r = 255;
    status_lines[2].g = 0;
    status_lines[2].b = 0;
    status_line_count = 3;
  }
  xSemaphoreGive(display_mutex);
}

void set_wifi_status_connected(const esp_ip4_addr_t &ip) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  if (display_mode == DisplayMode::kWifiStatus) {
    strlcpy(status_lines[0].text, "Connected", kMaxStatusLineLen);
    status_lines[0].r = 0;
    status_lines[0].g = 255;
    status_lines[0].b = 0;
    snprintf(status_lines[1].text, kMaxStatusLineLen, IPSTR, IP2STR(&ip));
    status_lines[1].r = 255;
    status_lines[1].g = 255;
    status_lines[1].b = 0;
    strlcpy(status_lines[2].text, "Topic: hub75/", kMaxStatusLineLen);
    status_lines[2].r = 255;
    status_lines[2].g = 255;
    status_lines[2].b = 255;
    status_line_count = 3;
  }
  xSemaphoreGive(display_mutex);
}

// esp_wifi_connect() can't be called with a blocking delay from inside the
// event handler (it runs on the shared system event task) - so the 4s retry
// wait happens via this one reusable timer instead of spawning a new task
// per disconnect. A fresh task per disconnect could pile up faster than each
// one finished and self-deleted during a run of repeated failed association
// attempts, adding heap pressure on top of WiFi's own - a likely contributor
// to boot-time crashes seen while the AP was slow to associate.
esp_timer_handle_t wifi_retry_timer = nullptr;

void wifi_retry_timer_cb(void *arg) {
  esp_wifi_connect();
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    wifi_attempt_count = 1;
    set_wifi_status_connecting(wifi_attempt_count);
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    wifi_attempt_count++;
    wifi_link_up = false;
    ESP_LOGW(WIFI_TAG, "disconnected (reason=%d), retrying in %ds (attempt %d)", event->reason,
             kWifiRetryDelayMs / 1000, wifi_attempt_count);
    set_wifi_status_connecting(wifi_attempt_count);
    if (esp_timer_is_active(wifi_retry_timer)) {
      esp_timer_stop(wifi_retry_timer);
    }
    esp_timer_start_once(wifi_retry_timer, static_cast<uint64_t>(kWifiRetryDelayMs) * 1000);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    ESP_LOGI(WIFI_TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    set_wifi_status_connected(event->ip_info.ip);
    wifi_link_up = true;
    xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
  }
}

void wifi_init(void) {
  if (strlen(CONFIG_WIFI_SSID) == 0) {
    ESP_LOGI(WIFI_TAG, "CONFIG_WIFI_SSID empty, skipping WiFi init");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    display_mode = DisplayMode::kMessage;  // nothing will ever connect - show the default message instead
    xSemaphoreGive(display_mutex);
    return;
  }

  ESP_ERROR_CHECK(nvs_flash_init());
  wifi_event_group = xEventGroupCreate();

  esp_timer_create_args_t retry_timer_args = {};
  retry_timer_args.callback = &wifi_retry_timer_cb;
  retry_timer_args.name = "wifi_retry";
  ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &wifi_retry_timer));

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

  wifi_config_t wifi_config = {};
  strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
  strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = strlen(CONFIG_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(WIFI_TAG, "connecting to SSID '%s'...", CONFIG_WIFI_SSID);
  xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);
}

// MQTT is entirely optional, same as WiFi - if CONFIG_MQTT_BROKER_URI is
// left empty (the default), mqtt_init() is skipped. Parameters come from
// Kconfig (main/Kconfig.projbuild), set via 'idf.py menuconfig' - they land
// only in sdkconfig, never in a file tracked by git. Requires WiFi to
// already be connected (mqtt_init() is called after wifi_init() returns).
static const char *MQTT_TAG = "hub75_mqtt";

// Expects JSON on CONFIG_MQTT_MESSAGE_TOPIC: {"text": "...", "color": "#RRGGBB"}
// "color" is optional and defaults to green if omitted or invalid.
void handle_message_payload(const char *data, int data_len) {
  cJSON *root = cJSON_ParseWithLength(data, data_len);
  if (root == nullptr) {
    ESP_LOGW(MQTT_TAG, "payload is not valid JSON, ignoring");
    return;
  }

  const cJSON *text_field = cJSON_GetObjectItemCaseSensitive(root, "text");
  if (!cJSON_IsString(text_field) || text_field->valuestring == nullptr) {
    ESP_LOGW(MQTT_TAG, "payload missing string \"text\" field, ignoring");
    cJSON_Delete(root);
    return;
  }

  uint8_t r = 0, g = 255, b = 0;  // default: green
  const cJSON *color_field = cJSON_GetObjectItemCaseSensitive(root, "color");
  if (cJSON_IsString(color_field) && color_field->valuestring != nullptr) {
    if (!parse_hex_color(color_field->valuestring, &r, &g, &b)) {
      ESP_LOGW(MQTT_TAG, "invalid \"color\" value '%s', defaulting to green", color_field->valuestring);
      r = 0;
      g = 255;
      b = 0;
    }
  }

  xSemaphoreTake(display_mutex, portMAX_DELAY);
  strlcpy(display_text, text_field->valuestring, kMaxDisplayText);
  display_r = r;
  display_g = g;
  display_b = b;
  display_dirty = true;
  display_mode = DisplayMode::kMessage;  // one-way: stop showing WiFi status once real content arrives
  xSemaphoreGive(display_mutex);

  ESP_LOGI(MQTT_TAG, "displaying \"%s\" color #%02X%02X%02X", display_text, r, g, b);
  cJSON_Delete(root);
}

// Diagnostic/fun test patterns - payload is ignored, the topic alone selects
// the mode. Actual animation state/logic lives in the render loop in
// app_main() (search for DisplayMode::kTest1 etc.).
constexpr const char *kTest1Topic = "hub75/test1";
constexpr const char *kTest2Topic = "hub75/test2";
constexpr const char *kTest3Topic = "hub75/test3";

// Global brightness override, independent of DisplayMode - see
// target_brightness. Payload is ignored, same as the test topics.
constexpr const char *kDimTopic = "hub75/dim";
constexpr const char *kBrightTopic = "hub75/bright";
// setBrightnessOE() scales the OE-enable window linearly with this value, but
// perceived brightness is compressive (roughly a cube-root law), so a linear
// 50% (128) barely reads as dimmer to the eye - dropping much lower makes
// the dim/bright difference actually obvious.
constexpr uint8_t kDimBrightness = 32;     // ~12.5%
constexpr uint8_t kFullBrightness = 255;   // 100%

// Solid full-panel colors - payload ignored, topic alone selects the color.
constexpr const char *kRedTopic = "hub75/red";
constexpr const char *kGreenTopic = "hub75/green";
constexpr const char *kBlueTopic = "hub75/blue";
constexpr const char *kYellowTopic = "hub75/yellow";
constexpr const char *kOrangeTopic = "hub75/orange";
constexpr const char *kWhiteTopic = "hub75/white";
constexpr const char *kBlackTopic = "hub75/black";

// hub75/standardtest1: fixed, repeatable white/red/green/blue x dim/bright
// sequence for comparing color/brightness behavior across firmware changes -
// see the kStandardTest1 render-loop case. Always restarts at phase 0 on
// activation. Advances automatically (~2s/phase) and can also be advanced
// manually with the onboard button (WF1_BUTTON_PIN) while active - see the
// button handling in the render loop.
constexpr const char *kStandardTest1Topic = "hub75/standardtest1";

// hub75/si, hub75/mi: self-playing, stylized arcade demos - see
// run_space_invaders_frame()/run_millipede_frame() in app_main().
// Landscape-oriented: the 64px axis is the wide play-field direction
// (player sweep, enemy formation width), the 32px axis is the short
// advance-toward-player direction, matching the panel's physical
// orientation.
constexpr const char *kSpaceInvadersTopic = "hub75/si";
constexpr const char *kMillipedeTopic = "hub75/mi";

// hub75/pong: self-playing Pong with a seven-segment scoreboard - see
// run_pong_frame() and draw_pong_score()/draw_seven_seg_digit() above.
constexpr const char *kPongTopic = "hub75/pong";

// hub75/tictactoe: self-playing tic-tac-toe, playing itself over and over -
// see run_tictactoe_frame(). Mostly-strategic AI (win > block > center >
// corner > edge) with an occasional random move instead, so games stay
// varied rather than replaying the same optimal line every time.
constexpr const char *kTicTacToeTopic = "hub75/tictactoe";

// hub75/matrix1/2/3: a grid of randomly colored squares, sized so each cell
// is an actual square given the panel's 64x32 (2:1) proportions - 4 rows x
// 8 cols, 8 rows x 16 cols, 16 rows x 32 cols respectively. Randomized once
// per trigger and held static. The *c variants are the same grids but
// re-randomize every ~1s while active, until another topic changes the
// mode - see set_matrix()/randomize_matrix()/draw_matrix().
constexpr const char *kMatrix1Topic = "hub75/matrix1";
constexpr const char *kMatrix2Topic = "hub75/matrix2";
constexpr const char *kMatrix3Topic = "hub75/matrix3";
constexpr const char *kMatrix1ContTopic = "hub75/matrix1c";
constexpr const char *kMatrix2ContTopic = "hub75/matrix2c";
constexpr const char *kMatrix3ContTopic = "hub75/matrix3c";

// hub75/matrix: "Matrix digital rain" screensaver - columns of falling,
// randomly-shimmering small-font glyphs with a bright head and a fading
// green tail - see run_matrix_rain_frame().
constexpr const char *kMatrixRainTopic = "hub75/matrix";

// hub75/ball1: a filled 8x8 ball bouncing around the panel, picking a fresh
// random color each time it hits a wall - see run_ball1_frame().
constexpr const char *kBall1Topic = "hub75/ball1";

// hub75/shootingstars: 3 bouncing 2x2 "stars" (red/blue/white), each trailing
// a fading tail up to 6px long pointing back along its direction of travel -
// see run_shootingstars_frame().
constexpr const char *kShootingStarsTopic = "hub75/shootingstars";

// hub75/time: 2px blue border around the whole panel plus a 2px blue
// horizontal line through the vertical center - see draw_time_frame().
constexpr const char *kTimeTopic = "hub75/time";

// hub75/maze1 through hub75/maze3: a random perfect maze (1px walls in a
// random color, never red) solved live by a red pixel doing DFS with
// backtracking, sliding continuously through the corridor (no breadcrumbs,
// no jumps), repeating with a fresh maze - and a freshly re-rolled wall
// color, fading through black between mazes - once solved. See
// run_maze_frame(). maze1 uses 3px wide paths (no fade transition); maze2
// uses 1px wide paths; maze3 is the same as maze2 but the solver "dot" is
// stretched into a 4px-long segment trailing the direction of travel.
//
// hub75/maze4 and hub75/maze5: same solve as maze1/maze2 respectively
// (maze4 at maze1's 3px wide paths, maze5 at maze2's 1px narrow paths), but
// these DO leave breadcrumbs - every cell the solver enters for the first
// time gets permanently stamped with whatever color maze4_task() last
// measured for the configured ping target's response time (see
// CONFIG_PING_TARGETS / 'Ping method' in menuconfig - same
// green/yellow/orange/red thresholds as hub75/ping1's graph, red for a
// timeout). Same "background task only ever waits on the network" split as
// ping1_task/ping2_task, so the solver's movement itself never stalls
// waiting on a ping.
constexpr const char *kMaze1Topic = "hub75/maze1";
constexpr const char *kMaze2Topic = "hub75/maze2";
constexpr const char *kMaze3Topic = "hub75/maze3";
constexpr const char *kMaze4Topic = "hub75/maze4";
constexpr const char *kMaze5Topic = "hub75/maze5";

// hub75/ping1: a scrolling network-latency strip-chart (see ping1_task()
// and the state declared alongside it, and DisplayMode::kPing1 in the
// render loop's switch).
constexpr const char *kPing1Topic = "hub75/ping1";
// hub75/ping2: a "comet" that launches, flies, and explodes (or fizzles
// out, if the ping timed out) once per ping - see ping2_task() and
// DisplayMode::kPing2 in the render loop's switch.
constexpr const char *kPing2Topic = "hub75/ping2";

// Flags - simplified/stylized pixel art given the 64x32 resolution, drawn in
// the render loop (search for DisplayMode::kFlagUsa etc.).
constexpr const char *kFlagUsaTopic = "hub75/flag";
constexpr const char *kFlagCanadaTopic = "hub75/canada";

// Blink overlay toggle - see blink_enabled. hub75/flash toggles on/off;
// hub75/noflash always turns it off regardless of current state.
constexpr const char *kFlashTopic = "hub75/flash";
constexpr const char *kNoFlashTopic = "hub75/noflash";

// One random color per row - see rainbow_row_r/g/b.
constexpr const char *kRainbowTopic = "hub75/rainbow";

// hub75/off: blanks the panel (DisplayMode::kOff draws nothing - the render
// loop already clears the screen every frame before the mode switch).
constexpr const char *kOffTopic = "hub75/off";

// hub75/1300: static "1300", grey on black. hub75/1300a and hub75/1300b:
// the same, but inverted - black text on a grey background, fixed at 25%
// and 50% of full white intensity respectively - see draw_text_scaled()
// and the kFixed1300* constants above it for the shared sizing (largest
// integer font scale of the built-in 8x8 font that still fits all 4
// characters on the 64px-wide panel; kFont8x8Basic glyphs already reserve
// their rightmost column as blank, so placing cells back-to-back still
// reads as separated digits - 4 chars * 8px * scale <= 64 caps scale at 2,
// same 16x16-per-glyph size as hub75/2L's elevator digits).
constexpr const char *kFixed1300Topic = "hub75/1300";
constexpr const char *kFixed1300AltTopic = "hub75/1300a";
constexpr const char *kFixed1300BTopic = "hub75/1300b";

// hub75/1300c and hub75/1300d: "1300" again, but in the compact small font
// (see small_font_glyph()/draw_text_small_scaled()) at its own largest
// integer scale that still fits on the panel - see the kSmallScale
// computation next to DisplayMode::kFixed1300C/D in the render loop. Needs
// to be legible from ~60ft, so bigger wins even though it means a narrower
// base font than hub75/1300's family. hub75/1300c is white on black;
// hub75/1300d is inverted (black on white). Both use the same 50%-of-white
// intensity for their white pixels (kFixed1300BgGray50, shared with
// hub75/1300b).
constexpr const char *kFixed1300CTopic = "hub75/1300c";
constexpr const char *kFixed1300DTopic = "hub75/1300d";

// hub75/13 and hub75/00: just "13" and just "00" respectively, white on
// black at 50% intensity, in the same small font as hub75/1300c/d (see
// small_font_glyph()/draw_text_small_scaled()) at a fixed dot scale of 2 -
// "2 pixel wide" describes the rendered stroke thickness (the scale), not
// the glyph's own column count, and this font is the one that can actually
// hollow out a '0' (an earlier, dedicated 2-column-only font could not, and
// was removed).
constexpr const char *kFixed13Topic = "hub75/13";
constexpr const char *kFixed00Topic = "hub75/00";

// hub75/1300e and hub75/1300f: exactly hub75/1300c/hub75/1300d (same small
// font, same white/black-50%-intensity coloring), but at a fixed dot scale
// of 2 instead of 4 - hub75/1300c/d's original, un-maximized size, kept
// around as its own pair of topics.
constexpr const char *kFixed1300ETopic = "hub75/1300e";
constexpr const char *kFixed1300FTopic = "hub75/1300f";
constexpr const char *kFixed1300LTopic = "hub75/1300L";
constexpr const char *kFixed1300LITopic = "hub75/1300LI";

// hub75/ray1: random ray-traced scene - see draw_raytracer()/set_raytrace().
constexpr const char *kRay1Topic = "hub75/ray1";
// hub75/ray2: same, but with a second sphere - see set_raytrace2().
constexpr const char *kRay2Topic = "hub75/ray2";

// hub75/0 cycles through every display pattern in sequence, forever, until
// hub75/1 stops it (reverting to kMessage) - see the kAutoTest phase table
// in app_main()'s render loop.
constexpr const char *kAutoTestStartTopic = "hub75/0";
constexpr const char *kAutoTestStopTopic = "hub75/1";

// hub75/2: elevator floor display - see app_main()'s kElevator render logic.
constexpr const char *kElevatorTopic = "hub75/2";
// hub75/2L: same, but double-size digits and a longer (2s) pause per floor.
constexpr const char *kElevatorLargeTopic = "hub75/2L";
// hub75/2LL: quadruple-size digits, filling the panel edge to edge. Three
// digits + an arrow can't fit at that size (3*32px alone exceeds the 64px
// panel width), so this shows only the tens+ones digits with no arrow -
// "100" displays as "00", same as any other multiple of 100.
constexpr const char *kElevatorXLargeTopic = "hub75/2LL";

// hub75/clock: show the live 24-hour clock (DisplayMode::kClock).
// hub75/ntpset: force an immediate NTP resync - see ntp_sync_cb() for the
// confirmation that follows every successful sync, automatic or triggered.
constexpr const char *kClockTopic = "hub75/clock";
constexpr const char *kNtpSetTopic = "hub75/ntpset";

// hub75/clock.<zone>.<color>: same live clock, but in a specific zone and
// digit color instead of the current tz/color (default Pacific/white - see
// clock_tz/clock_color_r,g,b above and apply_clock_tz() below). Fixed set,
// same style as the rest of this file's topic constants.
constexpr const char *kClockPacificTopic = "hub75/clock.PST8PDT.blue";
constexpr const char *kClockIndiaTopic = "hub75/clock.IST.blue";
constexpr const char *kClockUtcTopic = "hub75/clock.utc.green";
constexpr const char *kClockMountainTopic = "hub75/clock.mountain.blue";
constexpr const char *kClockCentralTopic = "hub75/clock.central.blue";
constexpr const char *kClockEasternTopic = "hub75/clock.eastern.blue";

bool topic_matches(const char *topic, int topic_len, const char *expected) {
  return topic_len == static_cast<int>(strlen(expected)) && strncmp(topic, expected, topic_len) == 0;
}

void set_test_mode(DisplayMode mode, const char *name) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  display_mode = mode;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "switched to %s", name);
}

// hub75/1300L. Payload is optional JSON {"fg":"#RRGGBB","bg":"#RRGGBB",
// "hue":"#RRGGBB"}: fg = digit colour (default white), bg = background
// (default black), hue = a tint multiplied into both (default #ffffff, i.e.
// none - e.g. "#ffd080" pulls the blue down to compensate a blue-heavy
// panel). Any missing/invalid field keeps its default; an empty or
// non-JSON payload gives plain white on black.
void set_fixed1300l(const char *data, int data_len) {
  uint8_t fg[3] = {255, 255, 255}, bg[3] = {0, 0, 0}, hue[3] = {255, 255, 255};
  if (data != nullptr && data_len > 0) {
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == nullptr) {
      ESP_LOGW(MQTT_TAG, "1300L: payload is not valid JSON, using defaults");
    } else {
      const struct {
        const char *key;
        uint8_t *dst;
      } fields[] = {{"fg", fg}, {"bg", bg}, {"hue", hue}};
      for (const auto &f : fields) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, f.key);
        if (item == nullptr) continue;
        if (!cJSON_IsString(item) || !parse_hex_color(item->valuestring, &f.dst[0], &f.dst[1], &f.dst[2])) {
          ESP_LOGW(MQTT_TAG, "1300L: invalid \"%s\" (want \"#RRGGBB\"), using default", f.key);
        }
      }
      cJSON_Delete(root);
    }
  }
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  memcpy(f1300l_fg, fg, 3);
  memcpy(f1300l_bg, bg, 3);
  memcpy(f1300l_hue, hue, 3);
  display_mode = DisplayMode::kFixed1300L;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "1300L fg=#%02X%02X%02X bg=#%02X%02X%02X hue=#%02X%02X%02X", fg[0], fg[1], fg[2], bg[0], bg[1],
           bg[2], hue[0], hue[1], hue[2]);
}

void set_clock_tz_color(ClockTz tz, uint8_t r, uint8_t g, uint8_t b, const char *name) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  clock_tz = tz;
  clock_color_r = r;
  clock_color_g = g;
  clock_color_b = b;
  display_mode = DisplayMode::kClock;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "switched to %s", name);
}

void set_brightness(uint8_t brightness) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  target_brightness = brightness;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "brightness set to %d/255", brightness);
}

void set_solid_color(uint8_t r, uint8_t g, uint8_t b, const char *name) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  solid_r = r;
  solid_g = g;
  solid_b = b;
  display_mode = DisplayMode::kSolidColor;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "solid color: %s", name);
}

void toggle_blink() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  blink_enabled = !blink_enabled;
  const bool now_enabled = blink_enabled;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "blink %s", now_enabled ? "enabled" : "disabled");
}

void stop_blink() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  blink_enabled = false;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "blink disabled");
}

// Fills *r/*g/*b with independent random bytes in [base, base+range) -
// shared by every "pick a random color" site in this file (default
// base=0/range=256 gives a full 0-255 random byte; callers pass a narrower
// band, e.g. base=80/range=176, for a "vibrant, not too dark" look).
void random_color(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t base = 0, uint16_t range = 256) {
  *r = static_cast<uint8_t>(base + esp_random() % range);
  *g = static_cast<uint8_t>(base + esp_random() % range);
  *b = static_cast<uint8_t>(base + esp_random() % range);
}

void set_rainbow() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  for (int row = 0; row < PANEL_HEIGHT; row++) {
    random_color(&rainbow_row_r[row], &rainbow_row_g[row], &rainbow_row_b[row]);
  }
  display_mode = DisplayMode::kRainbow;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "rainbow");
}

void randomize_matrix(int cell_count) {
  for (int i = 0; i < cell_count; i++) {
    random_color(&matrix_cell_r[i], &matrix_cell_g[i], &matrix_cell_b[i]);
  }
}

void set_matrix(DisplayMode mode, int cell_count, const char *name) {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  randomize_matrix(cell_count);
  display_mode = mode;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "switched to %s", name);
}

void set_raytrace() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  random_color(&ray_sphere_r, &ray_sphere_g, &ray_sphere_b, 80, 176);
  random_color(&ray_floor1_r, &ray_floor1_g, &ray_floor1_b, 40, 120);
  ray_floor2_r = static_cast<uint8_t>(ray_floor1_r / 3);
  ray_floor2_g = static_cast<uint8_t>(ray_floor1_g / 3);
  ray_floor2_b = static_cast<uint8_t>(ray_floor1_b / 3);
  display_mode = DisplayMode::kRayTrace;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "ray tracer (random scene)");
}

void set_raytrace2() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  random_color(&ray_sphere_r, &ray_sphere_g, &ray_sphere_b, 80, 176);
  random_color(&ray2_sphere2_r, &ray2_sphere2_g, &ray2_sphere2_b, 80, 176);
  random_color(&ray_floor1_r, &ray_floor1_g, &ray_floor1_b, 40, 120);
  ray_floor2_r = static_cast<uint8_t>(ray_floor1_r / 3);
  ray_floor2_g = static_cast<uint8_t>(ray_floor1_g / 3);
  ray_floor2_b = static_cast<uint8_t>(ray_floor1_b / 3);
  display_mode = DisplayMode::kRayTrace2;
  xSemaphoreGive(display_mutex);
  ESP_LOGI(MQTT_TAG, "ray tracer 2-sphere (random scene)");
}

// Executes one command (an MQTT topic + payload). Shared by the MQTT event
// handler and the REST API (http_api_init) so both control paths behave
// identically. Returns false if `topic` is not a known/live command. Topics
// gated by HUB75_FULL_EFFECTS are only recognised when that is enabled, same
// as their MQTT subscriptions.
bool dispatch_command(const char *topic, int topic_len, const char *data, int data_len) {
  if (topic_matches(topic, topic_len, CONFIG_MQTT_MESSAGE_TOPIC)) {
    handle_message_payload(data, data_len);
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kTest1Topic)) {
    set_test_mode(DisplayMode::kTest1, "test1 (random pixel)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kTest2Topic)) {
    set_test_mode(DisplayMode::kTest2, "test2 (bar graph)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kTest3Topic)) {
    set_test_mode(DisplayMode::kTest3, "test3 (pacman)");
#endif
  } else if (topic_matches(topic, topic_len, kDimTopic)) {
    set_brightness(kDimBrightness);
  } else if (topic_matches(topic, topic_len, kBrightTopic)) {
    set_brightness(kFullBrightness);
  } else if (topic_matches(topic, topic_len, kRedTopic)) {
    set_solid_color(255, 0, 0, "red");
  } else if (topic_matches(topic, topic_len, kGreenTopic)) {
    set_solid_color(0, 255, 0, "green");
  } else if (topic_matches(topic, topic_len, kBlueTopic)) {
    set_solid_color(0, 0, 255, "blue");
  } else if (topic_matches(topic, topic_len, kYellowTopic)) {
    set_solid_color(255, 255, 0, "yellow");
  } else if (topic_matches(topic, topic_len, kOrangeTopic)) {
    set_solid_color(255, 165, 0, "orange");
  } else if (topic_matches(topic, topic_len, kWhiteTopic)) {
    set_solid_color(255, 255, 255, "white");
  } else if (topic_matches(topic, topic_len, kBlackTopic)) {
    set_solid_color(0, 0, 0, "black");
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kStandardTest1Topic)) {
    set_test_mode(DisplayMode::kStandardTest1, "standard test 1 (hub75/standardtest1)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kSpaceInvadersTopic)) {
    set_test_mode(DisplayMode::kSpaceInvaders, "space invaders (hub75/si)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMillipedeTopic)) {
    set_test_mode(DisplayMode::kMillipede, "millipede (hub75/mi)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kPongTopic)) {
    set_test_mode(DisplayMode::kPong, "pong (hub75/pong)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kTicTacToeTopic)) {
    set_test_mode(DisplayMode::kTicTacToe, "tic-tac-toe (hub75/tictactoe)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix1Topic)) {
    set_matrix(DisplayMode::kMatrix1, 4 * 8, "matrix1 (hub75/matrix1)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix2Topic)) {
    set_matrix(DisplayMode::kMatrix2, 8 * 16, "matrix2 (hub75/matrix2)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix3Topic)) {
    set_matrix(DisplayMode::kMatrix3, 16 * 32, "matrix3 (hub75/matrix3)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix1ContTopic)) {
    set_matrix(DisplayMode::kMatrix1Cont, 4 * 8, "matrix1 continuous (hub75/matrix1c)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix2ContTopic)) {
    set_matrix(DisplayMode::kMatrix2Cont, 8 * 16, "matrix2 continuous (hub75/matrix2c)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrix3ContTopic)) {
    set_matrix(DisplayMode::kMatrix3Cont, 16 * 32, "matrix3 continuous (hub75/matrix3c)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMatrixRainTopic)) {
    set_test_mode(DisplayMode::kMatrixRain, "matrix digital rain (hub75/matrix)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kBall1Topic)) {
    set_test_mode(DisplayMode::kBall1, "ball1 (hub75/ball1)");
#endif
  } else if (topic_matches(topic, topic_len, kShootingStarsTopic)) {
    set_test_mode(DisplayMode::kShootingStars, "shooting stars (hub75/shootingstars)");
  } else if (topic_matches(topic, topic_len, kTimeTopic)) {
    set_test_mode(DisplayMode::kTimeFrame, "time frame (hub75/time)");
  } else if (topic_matches(topic, topic_len, kMaze1Topic)) {
    set_test_mode(DisplayMode::kMaze1, "maze1 (hub75/maze1)");
  } else if (topic_matches(topic, topic_len, kMaze2Topic)) {
    set_test_mode(DisplayMode::kMaze2, "maze2 (hub75/maze2)");
  } else if (topic_matches(topic, topic_len, kMaze3Topic)) {
    set_test_mode(DisplayMode::kMaze3, "maze3 (hub75/maze3)");
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMaze4Topic)) {
    set_test_mode(DisplayMode::kMaze4, "maze4 (hub75/maze4)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kMaze5Topic)) {
    set_test_mode(DisplayMode::kMaze5, "maze5 (hub75/maze5)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kPing1Topic)) {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    for (int i = 0; i < kPing1GraphWidth; i++) ping1_graph_ms[i] = 0;  // always restart with a blank graph
    ping1_last_rtt_ms = 0;
    ping1_success_count = 0;
    ping1_timeout_count = 0;
    xSemaphoreGive(display_mutex);
    set_test_mode(DisplayMode::kPing1, "ping1 (hub75/ping1)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kPing2Topic)) {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    ping2_launch_pending = false;  // always restart idle, no stale launch from a previous run
    ping2_last_rtt_ms = 0;
    xSemaphoreGive(display_mutex);
    set_test_mode(DisplayMode::kPing2, "ping2 (hub75/ping2)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kFlagUsaTopic)) {
    set_test_mode(DisplayMode::kFlagUsa, "USA flag");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kFlagCanadaTopic)) {
    set_test_mode(DisplayMode::kFlagCanada, "Canada flag");
#endif
  } else if (topic_matches(topic, topic_len, kFlashTopic)) {
    toggle_blink();
  } else if (topic_matches(topic, topic_len, kNoFlashTopic)) {
    stop_blink();
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kRainbowTopic)) {
    set_rainbow();
#endif
  } else if (topic_matches(topic, topic_len, kOffTopic)) {
    set_test_mode(DisplayMode::kOff, "off (hub75/off)");
  } else if (topic_matches(topic, topic_len, kFixed1300Topic)) {
    set_test_mode(DisplayMode::kFixed1300, "1300 (hub75/1300)");
  } else if (topic_matches(topic, topic_len, kFixed1300AltTopic)) {
    set_test_mode(DisplayMode::kFixed1300Alt, "1300a (hub75/1300a)");
  } else if (topic_matches(topic, topic_len, kFixed1300BTopic)) {
    set_test_mode(DisplayMode::kFixed1300B, "1300b (hub75/1300b)");
  } else if (topic_matches(topic, topic_len, kFixed1300CTopic)) {
    set_test_mode(DisplayMode::kFixed1300C, "1300c (hub75/1300c)");
  } else if (topic_matches(topic, topic_len, kFixed1300DTopic)) {
    set_test_mode(DisplayMode::kFixed1300D, "1300d (hub75/1300d)");
  } else if (topic_matches(topic, topic_len, kFixed13Topic)) {
    set_test_mode(DisplayMode::kFixed13, "13 (hub75/13)");
  } else if (topic_matches(topic, topic_len, kFixed00Topic)) {
    set_test_mode(DisplayMode::kFixed00, "00 (hub75/00)");
  } else if (topic_matches(topic, topic_len, kFixed1300ETopic)) {
    set_test_mode(DisplayMode::kFixed1300E, "1300e (hub75/1300e)");
  } else if (topic_matches(topic, topic_len, kFixed1300FTopic)) {
    set_test_mode(DisplayMode::kFixed1300F, "1300f (hub75/1300f)");
  } else if (topic_matches(topic, topic_len, kFixed1300LTopic)) {
    set_fixed1300l(data, data_len);
  } else if (topic_matches(topic, topic_len, kFixed1300LITopic)) {
    set_test_mode(DisplayMode::kFixed1300LI, "1300LI (hub75/1300LI)");
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kRay1Topic)) {
    set_raytrace();
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kRay2Topic)) {
    set_raytrace2();
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kAutoTestStartTopic)) {
    set_test_mode(DisplayMode::kAutoTest, "auto-test (hub75/0)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kAutoTestStopTopic)) {
    set_test_mode(DisplayMode::kMessage, "message (auto-test stopped)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kElevatorTopic)) {
    set_test_mode(DisplayMode::kElevator, "elevator (hub75/2)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kElevatorLargeTopic)) {
    set_test_mode(DisplayMode::kElevatorLarge, "elevator large (hub75/2L)");
#endif
#if HUB75_FULL_EFFECTS
  } else if (topic_matches(topic, topic_len, kElevatorXLargeTopic)) {
    set_test_mode(DisplayMode::kElevatorXLarge, "elevator x-large (hub75/2LL)");
#endif
  } else if (topic_matches(topic, topic_len, kClockTopic)) {
    set_test_mode(DisplayMode::kClock, "clock (hub75/clock)");
  } else if (topic_matches(topic, topic_len, kNtpSetTopic)) {
    ESP_LOGI(MQTT_TAG, "hub75/ntpset: forcing NTP resync");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    ntp_manual_resync_requested = true;  // ntp_sync_cb() shows "TIME SET" only because of this
    xSemaphoreGive(display_mutex);
    esp_netif_sntp_start();
  } else if (topic_matches(topic, topic_len, kClockPacificTopic)) {
    set_clock_tz_color(ClockTz::kPacific, 0, 0, 255, "clock (hub75/clock.PST8PDT.blue)");
  } else if (topic_matches(topic, topic_len, kClockIndiaTopic)) {
    set_clock_tz_color(ClockTz::kIndia, 0, 0, 255, "clock (hub75/clock.IST.blue)");
  } else if (topic_matches(topic, topic_len, kClockUtcTopic)) {
    set_clock_tz_color(ClockTz::kUtc, 0, 255, 0, "clock (hub75/clock.utc.green)");
  } else if (topic_matches(topic, topic_len, kClockMountainTopic)) {
    set_clock_tz_color(ClockTz::kMountain, 0, 0, 255, "clock (hub75/clock.mountain.blue)");
  } else if (topic_matches(topic, topic_len, kClockCentralTopic)) {
    set_clock_tz_color(ClockTz::kCentral, 0, 0, 255, "clock (hub75/clock.central.blue)");
  } else if (topic_matches(topic, topic_len, kClockEasternTopic)) {
    set_clock_tz_color(ClockTz::kEastern, 0, 0, 255, "clock (hub75/clock.eastern.blue)");
  } else {
  return false;
  }
  return true;
}

void mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(MQTT_TAG, "connected to broker");
      esp_mqtt_client_publish(event->client, CONFIG_MQTT_STATUS_TOPIC, "online", 0, 1, true);
      esp_mqtt_client_subscribe(event->client, CONFIG_MQTT_MESSAGE_TOPIC, 1);
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kTest1Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kTest2Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kTest3Topic, 1);
#endif
      esp_mqtt_client_subscribe(event->client, kDimTopic, 1);
      esp_mqtt_client_subscribe(event->client, kBrightTopic, 1);
      esp_mqtt_client_subscribe(event->client, kRedTopic, 1);
      esp_mqtt_client_subscribe(event->client, kGreenTopic, 1);
      esp_mqtt_client_subscribe(event->client, kBlueTopic, 1);
      esp_mqtt_client_subscribe(event->client, kYellowTopic, 1);
      esp_mqtt_client_subscribe(event->client, kOrangeTopic, 1);
      esp_mqtt_client_subscribe(event->client, kWhiteTopic, 1);
      esp_mqtt_client_subscribe(event->client, kBlackTopic, 1);
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kStandardTest1Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kSpaceInvadersTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMillipedeTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kPongTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kTicTacToeTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix1Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix2Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix3Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix1ContTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix2ContTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrix3ContTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMatrixRainTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kBall1Topic, 1);
#endif
      esp_mqtt_client_subscribe(event->client, kShootingStarsTopic, 1);
      esp_mqtt_client_subscribe(event->client, kTimeTopic, 1);
      esp_mqtt_client_subscribe(event->client, kMaze1Topic, 1);
      esp_mqtt_client_subscribe(event->client, kMaze2Topic, 1);
      esp_mqtt_client_subscribe(event->client, kMaze3Topic, 1);
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMaze4Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kMaze5Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kPing1Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kPing2Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kFlagUsaTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kFlagCanadaTopic, 1);
#endif
      esp_mqtt_client_subscribe(event->client, kFlashTopic, 1);
      esp_mqtt_client_subscribe(event->client, kNoFlashTopic, 1);
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kRainbowTopic, 1);
#endif
      esp_mqtt_client_subscribe(event->client, kOffTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300Topic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300AltTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300BTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300CTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300DTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed13Topic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed00Topic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300ETopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300FTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300LTopic, 1);
      esp_mqtt_client_subscribe(event->client, kFixed1300LITopic, 1);
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kRay1Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kRay2Topic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kAutoTestStartTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kAutoTestStopTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kElevatorTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kElevatorLargeTopic, 1);
#endif
#if HUB75_FULL_EFFECTS
      esp_mqtt_client_subscribe(event->client, kElevatorXLargeTopic, 1);
#endif
      esp_mqtt_client_subscribe(event->client, kClockTopic, 1);
      esp_mqtt_client_subscribe(event->client, kNtpSetTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockPacificTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockIndiaTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockUtcTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockMountainTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockCentralTopic, 1);
      esp_mqtt_client_subscribe(event->client, kClockEasternTopic, 1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGW(MQTT_TAG, "disconnected from broker");
      break;
    case MQTT_EVENT_DATA:
      dispatch_command(event->topic, event->topic_len, event->data, event->data_len);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGE(MQTT_TAG, "MQTT error, type=%d", event->error_handle->error_type);
      break;
    default:
      break;
  }
}

void mqtt_init(void) {
  if (strlen(CONFIG_MQTT_BROKER_URI) == 0) {
    ESP_LOGI(MQTT_TAG, "CONFIG_MQTT_BROKER_URI empty, skipping MQTT init");
    return;
  }

  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = CONFIG_MQTT_BROKER_URI;
  mqtt_cfg.credentials.client_id = CONFIG_MQTT_CLIENT_ID;
  if (strlen(CONFIG_MQTT_USERNAME) > 0) {
    mqtt_cfg.credentials.username = CONFIG_MQTT_USERNAME;
  }
  if (strlen(CONFIG_MQTT_PASSWORD) > 0) {
    mqtt_cfg.credentials.authentication.password = CONFIG_MQTT_PASSWORD;
  }
  mqtt_cfg.session.last_will.topic = CONFIG_MQTT_STATUS_TOPIC;
  mqtt_cfg.session.last_will.msg = "offline";
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = true;

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &mqtt_event_handler, nullptr);
  ESP_LOGI(MQTT_TAG, "connecting to broker '%s'...", CONFIG_MQTT_BROKER_URI);
  esp_mqtt_client_start(client);
}

// ---- REST API (optional, CONFIG_HTTP_API_ENABLE) ---------------------------
// Same commands as MQTT, over HTTP. Every hub75/<name> topic is reachable as
//   POST /api/command/<name>     body = the MQTT payload (may be empty)
// e.g. POST /api/command/red, POST /api/command/message with
// {"text":"hi","color":"#00ff00"}, POST /api/command/1300L with {"fg":...}.
//   GET  /api/status             current display mode, heap, NTP state
// Both go through dispatch_command(), so behaviour (incl. the lean-build
// topic gating) is identical to MQTT. If CONFIG_HTTP_API_TOKEN is non-empty,
// requests need "Authorization: Bearer <token>".
static const char *HTTP_TAG = "hub75_http";

bool http_authorized(httpd_req_t *req) {
  if (strlen(CONFIG_HTTP_API_TOKEN) == 0) return true;
  char hdr[128];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
  if (strncmp(hdr, "Bearer ", 7) != 0) return false;
  const char *got = hdr + 7;
  const size_t want_len = strlen(CONFIG_HTTP_API_TOKEN);
  if (strlen(got) != want_len) return false;
  uint8_t diff = 0;  // no early exit, so timing doesn't leak the token
  for (size_t i = 0; i < want_len; i++) diff |= static_cast<uint8_t>(got[i] ^ CONFIG_HTTP_API_TOKEN[i]);
  return diff == 0;
}

esp_err_t http_send_json(httpd_req_t *req, const char *status, const char *json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

esp_err_t http_command_handler(httpd_req_t *req) {
  if (!http_authorized(req)) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    return http_send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"unauthorized\"}");
  }

  static const char kPrefix[] = "/api/command/";
  const char *name = req->uri + (sizeof(kPrefix) - 1);
  size_t name_len = strcspn(name, "?#");
  char topic[96];
  if (name_len == 0 || name_len > 60) {
    return http_send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"unknown command\"}");
  }
  if (name_len == 7 && strncmp(name, "message", 7) == 0) {
    strlcpy(topic, CONFIG_MQTT_MESSAGE_TOPIC, sizeof(topic));
  } else {
    snprintf(topic, sizeof(topic), "hub75/%.*s", static_cast<int>(name_len), name);
  }

  char body[512];
  size_t total = req->content_len;
  if (total >= sizeof(body)) {
    return http_send_json(req, "413 Payload Too Large", "{\"ok\":false,\"error\":\"payload too large\"}");
  }
  size_t got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, body + got, total - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = '\0';

  ESP_LOGI(HTTP_TAG, "POST %s (%u bytes)", topic, static_cast<unsigned>(got));
  if (!dispatch_command(topic, static_cast<int>(strlen(topic)), body, static_cast<int>(got))) {
    return http_send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"unknown command\"}");
  }
  return http_send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t http_status_handler(httpd_req_t *req) {
  if (!http_authorized(req)) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    return http_send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"unauthorized\"}");
  }
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  const int mode = static_cast<int>(display_mode);
  const bool synced = ntp_synced;
  xSemaphoreGive(display_mutex);
  char json[192];
  snprintf(json, sizeof(json),
           "{\"mode\":%d,\"ntp_synced\":%s,\"uptime_s\":%lld,\"free_heap\":%u,\"min_free_heap\":%u}", mode,
           synced ? "true" : "false", static_cast<long long>(esp_timer_get_time() / 1000000),
           static_cast<unsigned>(esp_get_free_heap_size()), static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  return http_send_json(req, "200 OK", json);
}

void http_api_init(void) {
#if CONFIG_HTTP_API_ENABLE
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = CONFIG_HTTP_API_PORT;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.max_open_sockets = 3;  // heap is tight - a control API doesn't need more
  cfg.lru_purge_enable = true;
  cfg.stack_size = 4096;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGE(HTTP_TAG, "httpd_start failed");
    return;
  }
  const httpd_uri_t command_post = {"/api/command/*", HTTP_POST, http_command_handler, nullptr};
  const httpd_uri_t command_put = {"/api/command/*", HTTP_PUT, http_command_handler, nullptr};
  const httpd_uri_t status_get = {"/api/status", HTTP_GET, http_status_handler, nullptr};
  httpd_register_uri_handler(server, &command_post);
  httpd_register_uri_handler(server, &command_put);
  httpd_register_uri_handler(server, &status_get);
  ESP_LOGI(HTTP_TAG, "REST API listening on port %d%s", CONFIG_HTTP_API_PORT,
           strlen(CONFIG_HTTP_API_TOKEN) ? " (token required)" : " (NO AUTH)");
#endif
}

// Called by the SNTP client on every successful sync, whether from the
// automatic sync kicked off by ntp_init() once WiFi connects, the periodic
// background resync (every 5 minutes - see CONFIG_LWIP_SNTP_UPDATE_DELAY),
// or a hub75/ntpset-triggered resync. Only takes over the display (the "TIME
// SET" confirmation, then the live clock - see DisplayMode::kClockConfirm in
// the render loop) when this sync was explicitly requested via hub75/ntpset;
// a background sync just updates ntp_synced silently, so it never interrupts
// whatever else is currently on screen (e.g. hub75/ping1).
void ntp_sync_cb(struct timeval *tv) {
  (void)tv;
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  ntp_synced = true;
  if (ntp_manual_resync_requested) {
    display_mode = DisplayMode::kClockConfirm;
    ntp_manual_resync_requested = false;  // consumed
  }
  xSemaphoreGive(display_mutex);
  ESP_LOGI(TAG, "NTP time synced");
}

// Switches the process-wide TZ used by localtime_r() to the given zone -
// only affects wall-clock rendering (run_clock_frame), not the underlying
// UTC epoch that time()/NTP itself work in, so this is safe to call anytime
// without disturbing NTP sync.
void apply_clock_tz(ClockTz tz) {
  setenv("TZ", kClockTzPosix[static_cast<int>(tz)], 1);
  tzset();
}

// Starts the SNTP client (up to 2 servers, from Kconfig - see 'HD-WF1 NTP'
// in menuconfig) once WiFi is connected. System time defaults to the Pacific
// zone (see ClockTz above); the clock itself (see DisplayMode::kClock) shows
// it converted to whichever zone/color hub75/clock.<zone>.<color> selected.
void ntp_init(void) {
  if (strlen(CONFIG_WIFI_SSID) == 0) return;  // no network configured - nothing to sync against

  apply_clock_tz(ClockTz::kPacific);

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NTP_SERVER1);
  config.sync_cb = ntp_sync_cb;
  if (strlen(CONFIG_NTP_SERVER2) > 0) {
    config.num_of_servers = 2;
    config.servers[1] = CONFIG_NTP_SERVER2;
  }
  ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
}

// Runs wifi_init()/mqtt_init() off the main task so app_main() can get the
// HUB75 panel up and running the render loop immediately - otherwise the
// WiFi status screen could never actually be displayed, since wifi_init()
// blocks until connected (or forever, retrying, if it never connects).
void network_init_task(void *arg) {
  // Settle delay before touching WiFi: starting WiFi bring-up right after the
  // HUB75 I2S/DMA driver finishes its own reliably crash-looped the board
  // (see the history in app_main). It used to be a blocking 5s delay in
  // app_main; it lives here now so the render loop can show the boot splash
  // immediately while WiFi still comes up concurrently.
  vTaskDelay(pdMS_TO_TICKS(5000));
  wifi_init();
  ntp_init();
  mqtt_init();
  http_api_init();
  vTaskDelete(nullptr);
}

}  // namespace

// Huidu HD-WF1 (ESP32-S2) onboard HUB75E pinout.
// Source: mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA, src/hd-wf1-esp32s2-config.h
//
// Colour channels are re-pinned vs. that source to match the physical panel,
// so software colour == physical colour (compensating in the GPIO numbers, not
// the drawing code). Confirmed empirically with the current 128x64 panel using
// direct hub75/1300L colours: with the source order (R=2/4, G=3/5, B=6/8) red
// data lit BLUE LEDs, green data lit RED and blue data lit GREEN - i.e. R->B,
// G->R, B->G. So R now uses the pins that light red (3/5), G those that light
// green (6/8), B those that light blue (2/4).
// (The original 64x32 panel needed just G/B swapped - R=2/4, G=6/8, B=3/5's
// old values - so this is a per-panel LED-order quirk; re-check if the panel
// is ever swapped again.)
#define WF1_R1_PIN 3
#define WF1_R2_PIN 5
#define WF1_G1_PIN 6
#define WF1_G2_PIN 8
#define WF1_B1_PIN 2
#define WF1_B2_PIN 4
#define WF1_A_PIN  39
#define WF1_B_PIN  38
#define WF1_C_PIN  37
#define WF1_D_PIN  36
#define WF1_E_PIN  12
#define WF1_OE_PIN  35
#define WF1_CLK_PIN 34
#define WF1_LAT_PIN 33
// Onboard push button, per community documentation for this exact board
// (mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA) - not yet confirmed against
// this physical unit. Active-low (external/internal pull-up, reads LOW when
// pressed) - see the button debounce/edge-detect logic in the render loop.
#define WF1_BUTTON_PIN 11


static MatrixPanel_I2S_DMA *dma_display = nullptr;

// Built-in 8x8 font (NO_GFX build has no Adafruit-GFX print()/text API -
// drawPixelRGB888 is used directly). Full printable ASCII, so arbitrary text
// received over MQTT renders correctly - see font8x8_basic.h.
namespace {

constexpr int16_t kFontAdvance = 9;  // 8px glyph + 1px spacing

void draw_char(int16_t x, int16_t y, char c, uint8_t r, uint8_t g, uint8_t b) {
  const auto index = static_cast<unsigned char>(c);
  if (index >= 128) return;
  const uint8_t *glyph = kFont8x8Basic[index];
  for (int row = 0; row < 8; row++) {
    const int16_t py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      if ((bits & (1 << col)) == 0) continue;
      const int16_t px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

void draw_text(int16_t x, int16_t y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    draw_char(cursor, y, *p, r, g, b);
    cursor += kFontAdvance;
  }
}

// kFont8x8Basic's '0' is slashed (to distinguish it from 'O' in running
// text) - this is a plain, unslashed zero for hub75/1300/hub75/1300a, same
// outline/stroke weight as the original (rows 0/6 are the rounded top/bottom
// cap, rows 1-5 are plain side walls) with the diagonal strike-through
// simply omitted.
constexpr uint8_t kPlainZeroGlyph[8] = {0x3E, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00};

// hub75/1300 and hub75/1300a: draws `text` in kFont8x8Basic at an integer
// pixel scale, cells placed back-to-back (advance = 8*scale, no extra gap) -
// every glyph in this font already reserves its rightmost column as blank
// (see draw_char()/font8x8_basic.h), so characters still read as separated
// without wasting any width on an additional gap. That's what lets "1300"
// (4 characters) reach scale 2 - the largest integer scale that still fits
// within the 64px panel width - instead of stopping at scale 1. `no_slash_zero`
// substitutes kPlainZeroGlyph for every '0' instead of the font's own
// slashed one.
void draw_text_scaled(int16_t x, int16_t y, const char *text, int scale, uint8_t r, uint8_t g, uint8_t b,
                       bool no_slash_zero = false) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    const auto index = static_cast<unsigned char>(*p);
    if (index < 128) {
      const uint8_t *glyph = (no_slash_zero && *p == '0') ? kPlainZeroGlyph : kFont8x8Basic[index];
      for (int row = 0; row < 8; row++) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
          if ((bits & (1 << col)) == 0) continue;
          for (int sy = 0; sy < scale; sy++) {
            const int16_t py = static_cast<int16_t>(y + row * scale + sy);
            if (py < 0 || py >= PANEL_HEIGHT) continue;
            for (int sx = 0; sx < scale; sx++) {
              const int16_t px = static_cast<int16_t>(cursor + col * scale + sx);
              if (px < 0 || px >= PANEL_WIDTH) continue;
              dma_display->drawPixelRGB888(px, py, r, g, b);
            }
          }
        }
      }
    }
    cursor = static_cast<int16_t>(cursor + 8 * scale);
  }
}

// hub75/1300 draws "1300" grey-on-black; hub75/1300a draws it black-on-grey
// (a 25%-of-white background, independent of the panel's global
// hub75/dim/hub75/bright brightness overlay - see kFixed1300BgGray below).
// Both share this geometry: scale 2 is the largest integer scale of
// draw_text_scaled()'s font that still fits all 4 characters on the 64px
// panel (scale 3 would need 96px), so it's centered with zero slack
// horizontally and centered vertically in the 32px-tall panel.
constexpr int kFixed1300Scale = 2;
constexpr int16_t kFixed1300CharW = 8 * kFixed1300Scale;
constexpr int16_t kFixed1300TextW = 4 * kFixed1300CharW;
constexpr int16_t kFixed1300X = (PANEL_WIDTH - kFixed1300TextW) / 2;
constexpr int16_t kFixed1300Y = (PANEL_HEIGHT - kFixed1300CharW) / 2;
constexpr uint8_t kFixed1300Grey = 128;               // hub75/1300's grey text
constexpr uint8_t kFixed1300BgGray = 64;              // hub75/1300a's background - 25% of 255
constexpr uint8_t kFixed1300BgGray50 = 128;           // hub75/1300b's background - 50% of 255
// "1300" is drawn as three separate draw_text_scaled() calls (not one
// contiguous string) so each piece can be nudged independently: "13" (both
// digits) is shifted kFixed1300ShiftPx right of its plain-string position,
// then "1" alone is shifted a further kFixed1300OneExtraShiftPx right of
// that - "3" stays at the "13" shift, and "00" stays at its plain-string
// position (unshifted).
constexpr int16_t kFixed1300ShiftPx = 1;
constexpr int16_t kFixed1300OneExtraShiftPx = 2;

// hub75/1300L: "1300" in the Chilanka font, white on black, stretched to fill
// the whole panel. The glyphs are pre-rendered offline (tools/gen_chilanka_1300.py)
// into an anti-aliased 8-bit bitmap (chilanka_1300.h) that is already gamma-
// corrected for the library's CIE lightness curve - no font renderer or heap
// needed on the device.
constexpr int16_t kFixed1300LX = (PANEL_WIDTH - kChilanka1300W) / 2;
constexpr int16_t kFixed1300LY = (PANEL_HEIGHT - kChilanka1300H) / 2;

// hub75/1300LI: the same bitmap inverted - black digits on a "Tan" (CSS
// #D2B48C, RGB 210/180/140) background. chilanka_1300.h stores values already
// passed through the inverse of the library's CIE 1931 lightness curve, so
// edge pixels can't just be flipped/blended in value space: recover the linear
// coverage (forward CIE), scale the background's linear luminance by
// (1 - coverage), and re-apply the inverse curve, per colour channel.
constexpr uint8_t kFixed1300LIBgR = 210;
constexpr uint8_t kFixed1300LIBgG = 180;
constexpr uint8_t kFixed1300LIBgB = 140;

float cie_value_to_linear(uint8_t v) {  // the library's lumConvTab curve, 0..1
  const float L = v * (100.0f / 255.0f);
  return (L <= 8.0f) ? L / 902.3f : powf((L + 16.0f) / 116.0f, 3.0f);
}

uint8_t cie_linear_to_value(float y) {  // inverse of the above
  if (y <= 0.0f) return 0;
  if (y >= 1.0f) return 255;
  const float L = (y <= 8.0f / 902.3f) ? y * 902.3f : 116.0f * cbrtf(y) - 16.0f;
  const float out = L * (255.0f / 100.0f) + 0.5f;
  return static_cast<uint8_t>(out < 0.0f ? 0.0f : (out > 255.0f ? 255.0f : out));
}

// Draws the Chilanka "1300" bitmap in fg over bg, both first multiplied by hue
// (per channel, /255). Edge pixels blend fg over bg by the glyph coverage in
// linear-light space (via the library's CIE curve, see above), so
// anti-aliasing stays right for any colour pair. White on black with a white
// hue reproduces the raw bitmap values.
void draw_chilanka_1300(const uint8_t fg[3], const uint8_t bg[3], const uint8_t hue[3]) {
  uint8_t f[3], b[3];
  float f_lin[3], b_lin[3];
  for (int i = 0; i < 3; i++) {
    f[i] = static_cast<uint8_t>(fg[i] * hue[i] / 255);
    b[i] = static_cast<uint8_t>(bg[i] * hue[i] / 255);
    f_lin[i] = cie_value_to_linear(f[i]);
    b_lin[i] = cie_value_to_linear(b[i]);
  }
  dma_display->fillScreenRGB888(b[0], b[1], b[2]);
  for (int y = 0; y < kChilanka1300H; y++) {
    for (int x = 0; x < kChilanka1300W; x++) {
      const uint8_t v = kChilanka1300[y * kChilanka1300W + x];
      if (!v) continue;  // background
      uint8_t o[3];
      if (v == 255) {
        memcpy(o, f, 3);
      } else {
        const float cov = cie_value_to_linear(v);
        for (int i = 0; i < 3; i++) o[i] = cie_linear_to_value(b_lin[i] + (f_lin[i] - b_lin[i]) * cov);
      }
      dma_display->drawPixelRGB888(static_cast<int16_t>(kFixed1300LX + x), static_cast<int16_t>(kFixed1300LY + y), o[0],
                                   o[1], o[2]);
    }
  }
}

// Compact 3x5 "single pixel wide" font used only for a few fixed,
// non-scrolling status displays (the reset-reason banner and the WiFi
// not-connected/connected status) where fitting the whole message on the
// 64px panel at once (no scrolling) matters more than covering arbitrary
// text - deliberately narrow: digits, space, '.', '=', and exactly the
// letters those messages use. Bit 0 = leftmost column, matching
// kFont8x8Basic's convention; only the low 3 bits of each row byte are used.
constexpr int16_t kSmallFontAdvance = 4;  // 3px glyph + 1px spacing
constexpr int16_t kSmallFontHeight = 5;

const uint8_t *small_font_glyph(char c) {
  static const uint8_t kSpace[5] = {0, 0, 0, 0, 0};
  static const uint8_t kDot[5] = {0, 0, 0, 0, 2};
  static const uint8_t kEquals[5] = {0, 7, 0, 7, 0};
  static const uint8_t kDigit0[5] = {7, 5, 5, 5, 7};
  static const uint8_t kDigit1[5] = {2, 3, 2, 2, 7};
  static const uint8_t kDigit2[5] = {7, 4, 7, 1, 7};
  static const uint8_t kDigit3[5] = {7, 4, 7, 4, 7};
  static const uint8_t kDigit4[5] = {5, 5, 7, 4, 4};
  static const uint8_t kDigit5[5] = {7, 1, 7, 4, 7};
  static const uint8_t kDigit6[5] = {7, 1, 7, 5, 7};
  static const uint8_t kDigit7[5] = {7, 4, 4, 4, 4};
  static const uint8_t kDigit8[5] = {7, 5, 7, 5, 7};
  static const uint8_t kDigit9[5] = {7, 5, 7, 4, 7};
  static const uint8_t kUpperA[5] = {2, 5, 7, 5, 5};
  static const uint8_t kUpperC[5] = {6, 1, 1, 1, 6};
  static const uint8_t kUpperN[5] = {5, 3, 5, 6, 5};
  static const uint8_t kUpperR[5] = {3, 5, 3, 5, 5};
  static const uint8_t kLowerO[5] = {2, 5, 5, 5, 2};
  static const uint8_t kLowerT[5] = {2, 7, 2, 2, 4};
  static const uint8_t kLowerN[5] = {0, 3, 5, 5, 5};
  static const uint8_t kLowerE[5] = {0, 2, 7, 1, 6};
  static const uint8_t kLowerC[5] = {6, 1, 1, 1, 6};
  static const uint8_t kLowerD[5] = {4, 4, 6, 5, 6};
  static const uint8_t kLowerR[5] = {0, 5, 3, 1, 1};
  static const uint8_t kLowerY[5] = {5, 5, 2, 2, 2};
  static const uint8_t kLowerI[5] = {2, 0, 2, 2, 2};
  static const uint8_t kLowerG[5] = {0, 6, 5, 6, 4};
  static const uint8_t kLowerM[5] = {0, 7, 5, 5, 5};
  static const uint8_t kLowerP[5] = {0, 3, 5, 3, 1};
  static const uint8_t kUpperT[5] = {7, 2, 2, 2, 2};
  static const uint8_t kColon[5] = {0, 2, 0, 2, 0};
  static const uint8_t kLowerH[5] = {1, 1, 3, 5, 5};
  static const uint8_t kLowerU[5] = {0, 5, 5, 5, 6};
  static const uint8_t kLowerB[5] = {1, 1, 3, 5, 3};
  static const uint8_t kSlash[5] = {4, 4, 2, 1, 1};
  static const uint8_t kUpperI[5] = {7, 2, 2, 2, 7};
  static const uint8_t kUpperM[5] = {5, 7, 5, 5, 5};
  static const uint8_t kUpperE[5] = {7, 1, 3, 1, 7};
  static const uint8_t kUpperS[5] = {6, 1, 2, 4, 3};
  // Added for the boot owner-info splash (bit 0 = leftmost column).
  static const uint8_t kUpperP[5] = {3, 5, 3, 1, 1};
  static const uint8_t kUpperF[5] = {7, 1, 3, 1, 1};
  static const uint8_t kUpperJ[5] = {4, 4, 4, 5, 2};
  static const uint8_t kUpperW[5] = {5, 5, 5, 7, 5};
  static const uint8_t kLowerA[5] = {0, 6, 5, 7, 5};
  static const uint8_t kLowerL[5] = {2, 2, 2, 2, 6};
  static const uint8_t kLowerS[5] = {0, 6, 1, 4, 3};
  static const uint8_t kLowerF[5] = {6, 2, 7, 2, 2};
  static const uint8_t kDash[5] = {0, 0, 7, 0, 0};

  switch (c) {
    case ' ': return kSpace;
    case '.': return kDot;
    case '=': return kEquals;
    case ':': return kColon;
    case '/': return kSlash;
    case '0': return kDigit0;
    case '1': return kDigit1;
    case '2': return kDigit2;
    case '3': return kDigit3;
    case '4': return kDigit4;
    case '5': return kDigit5;
    case '6': return kDigit6;
    case '7': return kDigit7;
    case '8': return kDigit8;
    case '9': return kDigit9;
    case 'A': return kUpperA;
    case 'C': return kUpperC;
    case 'N': return kUpperN;
    case 'R': return kUpperR;
    case 'T': return kUpperT;
    case 'I': return kUpperI;
    case 'M': return kUpperM;
    case 'E': return kUpperE;
    case 'S': return kUpperS;
    case 'P': return kUpperP;
    case 'F': return kUpperF;
    case 'J': return kUpperJ;
    case 'W': return kUpperW;
    case 'O': return kLowerO;  // same shape as the small 'o' in this font
    case 'a': return kLowerA;
    case 'l': return kLowerL;
    case 's': return kLowerS;
    case 'f': return kLowerF;
    case '-': return kDash;
    case 'o': return kLowerO;
    case 't': return kLowerT;
    case 'n': return kLowerN;
    case 'e': return kLowerE;
    case 'c': return kLowerC;
    case 'd': return kLowerD;
    case 'r': return kLowerR;
    case 'y': return kLowerY;
    case 'i': return kLowerI;
    case 'g': return kLowerG;
    case 'm': return kLowerM;
    case 'p': return kLowerP;
    case 'h': return kLowerH;
    case 'u': return kLowerU;
    case 'b': return kLowerB;
    default: return kSpace;
  }
}

void draw_char_small(int16_t x, int16_t y, char c, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t *glyph = small_font_glyph(c);
  for (int row = 0; row < kSmallFontHeight; row++) {
    const int16_t py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 3; col++) {
      if ((bits & (1 << col)) == 0) continue;
      const int16_t px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

// Total pixel width of `text` rendered with draw_text_small (glyphs plus
// inter-character spacing, minus the trailing gap after the last glyph).
int16_t small_text_width(const char *text) {
  const size_t len = strlen(text);
  if (len == 0) return 0;
  return static_cast<int16_t>(len * kSmallFontAdvance - 1);
}

void draw_text_small(int16_t x, int16_t y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    draw_char_small(cursor, y, *p, r, g, b);
    cursor += kSmallFontAdvance;
  }
}

// hub75/1300c and hub75/1300d: same small-font glyphs as draw_char_small()
// above, but each dot drawn as a `scale`x`scale` block instead of a single
// pixel - a literal "N px wide font" rather than the 1px-stroke original.
void draw_char_small_scaled(int16_t x, int16_t y, char c, int scale, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t *glyph = small_font_glyph(c);
  for (int row = 0; row < kSmallFontHeight; row++) {
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 3; col++) {
      if ((bits & (1 << col)) == 0) continue;
      for (int sy = 0; sy < scale; sy++) {
        const int16_t py = static_cast<int16_t>(y + row * scale + sy);
        if (py < 0 || py >= PANEL_HEIGHT) continue;
        for (int sx = 0; sx < scale; sx++) {
          const int16_t px = static_cast<int16_t>(x + col * scale + sx);
          if (px < 0 || px >= PANEL_WIDTH) continue;
          dma_display->drawPixelRGB888(px, py, r, g, b);
        }
      }
    }
  }
}

// Total pixel width of `text` rendered with draw_text_small_scaled().
int16_t small_text_width_scaled(const char *text, int scale) {
  const size_t len = strlen(text);
  if (len == 0) return 0;
  return static_cast<int16_t>((static_cast<int>(len) * kSmallFontAdvance - 1) * scale);
}

void draw_text_small_scaled(int16_t x, int16_t y, const char *text, int scale, uint8_t r, uint8_t g, uint8_t b) {
  int16_t cursor = x;
  for (const char *p = text; *p != '\0'; p++) {
    draw_char_small_scaled(cursor, y, *p, scale, r, g, b);
    cursor = static_cast<int16_t>(cursor + kSmallFontAdvance * scale);
  }
}

// hub75/test3: two 8x8 frames (mouth open/closed) toggled while the sprite
// moves, giving a chomping animation - same bit convention as the font
// (bit N of each row byte = column N, bit 0 = leftmost, 1 = lit).
constexpr uint8_t kPacmanOpen[8] = {
    0b00111100, 0b01111100, 0b11111000, 0b11110000, 0b11111000, 0b01111100, 0b00111100, 0b00000000,
};
constexpr uint8_t kPacmanClosed[8] = {
    0b00111100, 0b01111110, 0b11111111, 0b11111111, 0b11111111, 0b11111111, 0b01111110, 0b00111100,
};

void draw_sprite8(int16_t x, int16_t y, const uint8_t *rows, uint8_t r, uint8_t g, uint8_t b) {
  for (int row = 0; row < 8; row++) {
    const int16_t py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    const uint8_t bits = rows[row];
    for (int col = 0; col < 8; col++) {
      if ((bits & (1 << col)) == 0) continue;
      const int16_t px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

// hub75/2: elevator floor display. Both arrows are drawn with draw_sprite8
// above (same 8x8/bit-per-column convention).
constexpr uint8_t kArrowUp[8] = {
    0b00011000, 0b00111100, 0b01111110, 0b11111111, 0b00011000, 0b00011000, 0b00011000, 0b00011000,
};
constexpr uint8_t kArrowDown[8] = {
    0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b11111111, 0b01111110, 0b00111100, 0b00011000,
};

// hub75/pong: seven-segment digits for the scoreboard, evoking the blocky
// digital-clock/scoreboard displays of the original game's era rather than
// a smooth modern font. Standard segment lettering (a=top, b=top-right,
// c=bottom-right, d=bottom, e=bottom-left, f=top-left, g=middle); each
// digit 0-9 is a bitmask of which of the 7 segments are lit, the same
// convention every seven-segment reference table uses.
constexpr uint8_t kSevenSegDigits[10] = {
    0x3F,  // 0: a b c d e f
    0x06,  // 1: b c
    0x5B,  // 2: a b d e g
    0x4F,  // 3: a b c d g
    0x66,  // 4: b c f g
    0x6D,  // 5: a c d f g
    0x7D,  // 6: a c d e f g
    0x07,  // 7: a b c
    0x7F,  // 8: a b c d e f g
    0x6F,  // 9: a b c d f g
};

// Draws one seven-segment digit at (x,y), w px wide x h px tall, with
// segment lines always exactly 1px thick regardless of w/h - only the
// overall glyph size scales, not the stroke weight.
void draw_seven_seg_digit(int16_t x, int16_t y, int digit, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t segs = kSevenSegDigits[digit];
  const int16_t mid = h / 2;
  if (segs & 0x01) dma_display->fillRect(x, y, w, 1, r, g, b);                              // a: top
  if (segs & 0x02) dma_display->fillRect(static_cast<int16_t>(x + w - 1), y, 1,
                                          static_cast<int16_t>(mid + 1), r, g, b);            // b: top-right
  if (segs & 0x04)
    dma_display->fillRect(static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + mid), 1,
                           static_cast<int16_t>(h - mid), r, g, b);                           // c: bottom-right
  if (segs & 0x08) dma_display->fillRect(x, static_cast<int16_t>(y + h - 1), w, 1, r, g, b);  // d: bottom
  if (segs & 0x10)
    dma_display->fillRect(x, static_cast<int16_t>(y + mid), 1, static_cast<int16_t>(h - mid), r, g, b);  // e
  if (segs & 0x20) dma_display->fillRect(x, y, 1, static_cast<int16_t>(mid + 1), r, g, b);   // f: top-left
  if (segs & 0x40) dma_display->fillRect(x, static_cast<int16_t>(y + mid), w, 1, r, g, b);   // g: middle
}

// Draws a 0-99 score as two 3x5 seven-segment digits (always both digits,
// with a leading zero below 10 - authentic to the always-on digital
// scoreboards of the era) with a 1px gap between them.
void draw_pong_score(int16_t x, int16_t y, int value, uint8_t r, uint8_t g, uint8_t b) {
  value = value % 100;
  draw_seven_seg_digit(x, y, value / 10, 3, 5, r, g, b);
  draw_seven_seg_digit(static_cast<int16_t>(x + 4), y, value % 10, 3, 5, r, g, b);
}

// hub75/time: "HH:MM:SS" in the parameterized seven-segment font above -
// see draw_time_frame() for the surrounding border/boxes.
constexpr int16_t kTimeDigitW = 5, kTimeDigitH = 9, kTimeGap = 1, kTimeColonW = 1;
// 6 digits + 2 colons + 7 gaps between the 8 glyphs.
constexpr int16_t kTimeTextW = 6 * kTimeDigitW + 2 * kTimeColonW + 7 * kTimeGap;

void draw_time_hhmmss(int16_t x, int16_t y, int hour, int minute, int second, uint8_t r, uint8_t g, uint8_t b) {
  int16_t cx = x;
  auto digit = [&](int d) {
    draw_seven_seg_digit(cx, y, d, kTimeDigitW, kTimeDigitH, r, g, b);
    cx = static_cast<int16_t>(cx + kTimeDigitW + kTimeGap);
  };
  auto colon = [&]() {
    const int16_t mid = kTimeDigitH / 2;
    dma_display->drawPixelRGB888(cx, static_cast<int16_t>(y + mid - 2), r, g, b);
    dma_display->drawPixelRGB888(cx, static_cast<int16_t>(y + mid + 2), r, g, b);
    cx = static_cast<int16_t>(cx + kTimeColonW + kTimeGap);
  };
  digit(hour / 10);
  digit(hour % 10);
  colon();
  digit(minute / 10);
  digit(minute % 10);
  colon();
  digit(second / 10);
  digit(second % 10);
}

// Renders one digit position scrolling vertically from old_c to new_c, t/dh
// of the way through the transition (t=0: old_c fully shown, t=dh: new_c
// fully shown, where dh = 8*scale). Direction sets which way the digit
// appears to travel: up scrolls old_c out the top/new_c in from the bottom;
// down is the mirror. old_c/new_c may be ' ' (the font's blank glyph) so
// digits can scroll in/out of existence at 9->10 and 99->100 (and back)
// without special-casing. scale nearest-neighbor upscales the source 8x8
// glyph (2 = render each source pixel as a 2x2 block, etc).
void draw_digit_scroll(int16_t x, int16_t y, char old_c, char new_c, int t, bool up, uint8_t r, uint8_t g, uint8_t b,
                        int scale) {
  const uint8_t *old_glyph = kFont8x8Basic[static_cast<unsigned char>(old_c)];
  const uint8_t *new_glyph = kFont8x8Basic[static_cast<unsigned char>(new_c)];
  const int dh = 8 * scale;
  for (int row = 0; row < dh; row++) {
    const int index = up ? (t + row) : ((dh - t) + row);
    const bool first_half = index < dh;
    const uint8_t *glyph = up ? (first_half ? old_glyph : new_glyph) : (first_half ? new_glyph : old_glyph);
    const uint8_t bits = glyph[(first_half ? index : index - dh) / scale];
    const int16_t py = y + row;
    if (py < 0 || py >= PANEL_HEIGHT) continue;
    for (int col = 0; col < dh; col++) {
      if ((bits & (1 << (col / scale))) == 0) continue;
      const int16_t px = x + col;
      if (px < 0 || px >= PANEL_WIDTH) continue;
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

// hub75/flag: simplified/stylized US flag. 64x32 can't fit 50 individually
// legible stars, but it can fit the real 13 stripes (via cumulative integer
// division so the odd remainder pixels spread evenly instead of piling into
// one fat stripe) and a denser star field than a plain grid: 23 dots in the
// canton, laid out as 3 long rows of 5 interleaved with 2 short rows of 4 -
// the same long/short row offset the real 50-star pattern uses, just with
// fewer stars per row.
void draw_flag_usa() {
  constexpr int16_t kStripeCount = 13;
  int16_t y_prev = 0;
  for (int16_t i = 1; i <= kStripeCount; i++) {
    const int16_t y = static_cast<int16_t>((static_cast<int32_t>(i) * PANEL_HEIGHT) / kStripeCount);
    const int16_t h = y - y_prev;
    const bool red = (i % 2) == 1;  // stripe 1 (top) and 13 (bottom) are both red
    dma_display->fillRect(0, y_prev, PANEL_WIDTH, h, red ? 255 : 255, red ? 0 : 255, red ? 0 : 255);
    y_prev = y;
  }

  constexpr int16_t kCantonW = 26;
  constexpr int16_t kCantonH = (7 * PANEL_HEIGHT) / 13;  // covers stripes 1-7, same formula as above
  dma_display->fillRect(0, 0, kCantonW, kCantonH, 0, 0, 128);

  constexpr int16_t kStarRowY[5] = {2, 5, 8, 11, 14};
  constexpr int16_t kLongRowX[5] = {3, 8, 13, 18, 23};
  constexpr int16_t kShortRowX[4] = {5, 10, 15, 20};
  for (int16_t row = 0; row < 5; row++) {
    const bool long_row = (row % 2) == 0;
    const int16_t *xs = long_row ? kLongRowX : kShortRowX;
    const int16_t count = long_row ? 5 : 4;
    for (int16_t col = 0; col < count; col++) {
      dma_display->drawPixelRGB888(xs[col], kStarRowY[row], 255, 255, 255);
    }
  }
}

// hub75/canada: simplified/stylized Canadian flag - red/white/red vertical
// bands with a hand-drawn, stylized (not anatomically exact) maple leaf
// silhouette in the center band.
constexpr uint16_t kMapleLeaf[11] = {
    0b00001000000, 0b00001000000, 0b00011100000, 0b01011101000, 0b11111111110,
    0b00111110000, 0b01111111100, 0b10111110110, 0b00011100000, 0b00001000000,
    0b00001000000,
};

void draw_flag_canada() {
  constexpr int16_t kBandW = PANEL_WIDTH / 4;  // 16px red bands each side
  dma_display->fillRect(0, 0, kBandW, PANEL_HEIGHT, 255, 0, 0);
  dma_display->fillRect(kBandW, 0, PANEL_WIDTH - 2 * kBandW, PANEL_HEIGHT, 255, 255, 255);
  dma_display->fillRect(PANEL_WIDTH - kBandW, 0, kBandW, PANEL_HEIGHT, 255, 0, 0);

  constexpr int16_t kLeafX = (PANEL_WIDTH - 11) / 2;
  constexpr int16_t kLeafY = (PANEL_HEIGHT - 11) / 2;
  for (int16_t row = 0; row < 11; row++) {
    for (int16_t col = 0; col < 11; col++) {
      if ((kMapleLeaf[row] & (1 << (10 - col))) == 0) continue;
      dma_display->drawPixelRGB888(kLeafX + col, kLeafY + row, 255, 0, 0);
    }
  }
}

// hub75/rainbow: one solid-color horizontal line per row.
void draw_rainbow(const uint8_t *r, const uint8_t *g, const uint8_t *b) {
  for (int16_t row = 0; row < PANEL_HEIGHT; row++) {
    dma_display->fillRect(0, row, PANEL_WIDTH, 1, r[row], g[row], b[row]);
  }
}

// hub75/matrix1/2/3(c): rows x cols grid of randomly colored squares, sized
// to exactly divide the panel (see matrix_cell_r/g/b above).
void draw_matrix(int rows, int cols) {
  const int16_t cell_w = static_cast<int16_t>(PANEL_WIDTH / cols);
  const int16_t cell_h = static_cast<int16_t>(PANEL_HEIGHT / rows);
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int idx = r * cols + c;
      dma_display->fillRect(static_cast<int16_t>(c * cell_w), static_cast<int16_t>(r * cell_h), cell_w, cell_h,
                             matrix_cell_r[idx], matrix_cell_g[idx], matrix_cell_b[idx]);
    }
  }
}

// hub75/time: 2px blue border around the whole panel plus a 2px blue
// horizontal line through the vertical center.
void draw_time_frame() {
  constexpr uint8_t r = 0, g = 0, b = 255;
  dma_display->fillRect(0, 0, PANEL_WIDTH, 2, r, g, b);                      // top
  dma_display->fillRect(0, PANEL_HEIGHT - 2, PANEL_WIDTH, 2, r, g, b);       // bottom
  dma_display->fillRect(0, 0, 2, PANEL_HEIGHT, r, g, b);                    // left
  dma_display->fillRect(PANEL_WIDTH - 2, 0, 2, PANEL_HEIGHT, r, g, b);      // right
  dma_display->fillRect(0, (PANEL_HEIGHT - 2) / 2, PANEL_WIDTH, 2, r, g, b);  // center horizontal line
}

// hub75/time: west coast (Pacific) time in the top box, yellow; east coast
// (Eastern) time in the bottom box, green - both 24-hour HH:MM:SS in the
// parameterized seven-segment font (draw_seven_seg_digit()/
// draw_time_hhmmss() above), sized and centered so it can't touch the box
// edges. TZ is switched twice per frame (Pacific then Eastern) via
// apply_clock_tz() - see its comment for why that's safe to do
// unconditionally every frame.
void draw_time_clock() {
  draw_time_frame();

  constexpr int16_t kBoxX = 2;
  constexpr int16_t kBoxW = PANEL_WIDTH - 4;                          // 60
  constexpr int16_t kCenterLineY = (PANEL_HEIGHT - 2) / 2;            // 15
  constexpr int16_t kTopBoxY = 2;
  constexpr int16_t kTopBoxH = kCenterLineY - kTopBoxY;               // 13
  constexpr int16_t kBottomBoxY = kCenterLineY + 2;                   // 17
  constexpr int16_t kBottomBoxH = (PANEL_HEIGHT - 2) - kBottomBoxY;   // 13
  constexpr int16_t kTextX = kBoxX + (kBoxW - kTimeTextW) / 2;
  constexpr int16_t kTopTextY = kTopBoxY + (kTopBoxH - kTimeDigitH) / 2;
  constexpr int16_t kBottomTextY = kBottomBoxY + (kBottomBoxH - kTimeDigitH) / 2;

  apply_clock_tz(ClockTz::kPacific);
  time_t now_w = time(nullptr);
  struct tm tm_w;
  localtime_r(&now_w, &tm_w);
  draw_time_hhmmss(kTextX, kTopTextY, tm_w.tm_hour, tm_w.tm_min, tm_w.tm_sec, 255, 255, 0);

  apply_clock_tz(ClockTz::kEastern);
  time_t now_e = time(nullptr);
  struct tm tm_e;
  localtime_r(&now_e, &tm_e);
  draw_time_hhmmss(kTextX, kBottomTextY, tm_e.tm_hour, tm_e.tm_min, tm_e.tm_sec, 0, 255, 0);
}

// hub75/ray1 (1 sphere) and hub75/ray2 (2 spheres): minimal per-pixel ray
// tracer - sphere(s) over a checkerboard floor, with a point light
// continuously orbiting (driven by esp_timer_get_time(), so this needs no
// frame-counter state threaded in from app_main - it's a pure function of
// wall time), diffuse+specular shading on the sphere(s), and a real shadow
// ray for the floor (occluded by any sphere). Colors are randomized per
// message - see set_raytrace()/set_raytrace2().
struct RtVec3 {
  float x, y, z;
};
inline RtVec3 operator+(RtVec3 a, RtVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline RtVec3 operator-(RtVec3 a, RtVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline RtVec3 operator*(RtVec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float rt_dot(RtVec3 a, RtVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline RtVec3 rt_normalize(RtVec3 a) {
  const float len = sqrtf(rt_dot(a, a));
  return len > 1e-6f ? a * (1.0f / len) : a;
}

struct RtSphere {
  RtVec3 center;
  float radius;
  uint8_t r, g, b;
};

void draw_raytracer(const RtSphere *spheres, int sphere_count, RtVec3 light_center, float floor_y, uint8_t floor1_r,
                     uint8_t floor1_g, uint8_t floor1_b, uint8_t floor2_r, uint8_t floor2_g, uint8_t floor2_b) {
  constexpr RtVec3 kOrigin = {0.0f, 0.0f, 0.0f};
  constexpr float kTanHalfFov = 0.5774f;  // ~60deg field of view
  const float kAspect = static_cast<float>(PANEL_WIDTH) / PANEL_HEIGHT;
  constexpr float kTwoPi = 6.283185307f;

  const float theta = fmodf(static_cast<float>(esp_timer_get_time()) * 1.2e-6f, kTwoPi);
  const RtVec3 light = light_center + RtVec3{3.0f * cosf(theta), 3.0f, 3.0f * sinf(theta)};

  for (int16_t py = 0; py < PANEL_HEIGHT; py++) {
    const float ndc_y = 1.0f - ((py + 0.5f) / PANEL_HEIGHT) * 2.0f;
    for (int16_t px = 0; px < PANEL_WIDTH; px++) {
      const float ndc_x = (((px + 0.5f) / PANEL_WIDTH) * 2.0f - 1.0f) * kAspect;
      const RtVec3 dir = rt_normalize(RtVec3{ndc_x * kTanHalfFov, ndc_y * kTanHalfFov, 1.0f});

      int hit_idx = -1;
      float t_sphere = 0.0f;
      for (int s = 0; s < sphere_count; s++) {
        const RtVec3 oc = kOrigin - spheres[s].center;
        const float sb = 2.0f * rt_dot(oc, dir);
        const float sc = rt_dot(oc, oc) - spheres[s].radius * spheres[s].radius;
        const float sdisc = sb * sb - 4.0f * sc;
        if (sdisc < 0.0f) continue;
        const float t = (-sb - sqrtf(sdisc)) * 0.5f;
        if (t <= 0.0f) continue;
        if (hit_idx == -1 || t < t_sphere) {
          hit_idx = s;
          t_sphere = t;
        }
      }
      const bool sphere_hit = hit_idx >= 0;

      const bool floor_hit = dir.y < -1e-4f;
      const float t_floor = floor_hit ? (floor_y / dir.y) : 0.0f;

      uint8_t r, g, b;
      if (sphere_hit && (!floor_hit || t_sphere < t_floor)) {
        const RtSphere &sph = spheres[hit_idx];
        const RtVec3 hit = dir * t_sphere;
        const RtVec3 normal = (hit - sph.center) * (1.0f / sph.radius);
        const RtVec3 light_dir = rt_normalize(light - hit);
        const RtVec3 view_dir = rt_normalize(kOrigin - hit);
        const RtVec3 half_vec = rt_normalize(light_dir + view_dir);
        const float diffuse = fmaxf(0.0f, rt_dot(normal, light_dir));
        const float spec = powf(fmaxf(0.0f, rt_dot(normal, half_vec)), 32.0f);
        const float intensity = 0.15f + diffuse * 0.85f;
        r = static_cast<uint8_t>(fminf(255.0f, sph.r * intensity + 255.0f * spec * 0.6f));
        g = static_cast<uint8_t>(fminf(255.0f, sph.g * intensity + 255.0f * spec * 0.6f));
        b = static_cast<uint8_t>(fminf(255.0f, sph.b * intensity + 255.0f * spec * 0.6f));
      } else if (floor_hit) {
        const RtVec3 hit = dir * t_floor;
        const int checker = (static_cast<int>(floorf(hit.x)) + static_cast<int>(floorf(hit.z)) + 2000) & 1;
        const uint8_t base_r = checker ? floor1_r : floor2_r;
        const uint8_t base_g = checker ? floor1_g : floor2_g;
        const uint8_t base_b = checker ? floor1_b : floor2_b;
        const RtVec3 shadow_origin = hit + RtVec3{0.0f, 1e-3f, 0.0f};
        const RtVec3 light_dir = rt_normalize(light - shadow_origin);
        bool shadowed = false;
        for (int s = 0; s < sphere_count; s++) {
          const RtVec3 soc = shadow_origin - spheres[s].center;
          const float shb = 2.0f * rt_dot(soc, light_dir);
          const float shc = rt_dot(soc, soc) - spheres[s].radius * spheres[s].radius;
          const float shdisc = shb * shb - 4.0f * shc;
          if (shdisc >= 0.0f && (-shb - sqrtf(shdisc)) > 0.0f) {
            shadowed = true;
            break;
          }
        }
        const float ndotl = fmaxf(0.0f, light_dir.y);
        const float intensity = shadowed ? 0.15f : (0.15f + ndotl * 0.85f);
        r = static_cast<uint8_t>(base_r * intensity);
        g = static_cast<uint8_t>(base_g * intensity);
        b = static_cast<uint8_t>(base_b * intensity);
      } else {
        r = 0;
        g = 0;
        b = static_cast<uint8_t>(30.0f * ((ndc_y + 1.0f) / 2.0f));
      }
      dma_display->drawPixelRGB888(px, py, r, g, b);
    }
  }
}

}  // namespace


// ---- Persistent log ("applog" partition) ----------------------------------
// The console is UART (not USB) on this build, so nothing is readable from a
// running board. Instead, every ESP_LOG line is copied into a small RAM ring
// by a vprintf hook (and still forwarded to the normal console), and a
// low-priority task appends the ring to the 64K "applog" flash partition.
// Read it back with the board in bootloader mode:
//   esptool read-flash 0x120000 0x10000 applog.bin && strings applog.bin
// It appends across reboots (find-the-end scan at boot); when full it erases
// and wraps. The task also logs heap stats every 5s - the main use so far is
// diagnosing out-of-memory at 128x64.
namespace {
constexpr size_t kAppLogRing = 4096;
char applog_ring[kAppLogRing];
volatile size_t applog_head = 0, applog_tail = 0;  // head = next write, tail = next flush
portMUX_TYPE applog_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t applog_orig_vprintf = nullptr;
const esp_partition_t *applog_part = nullptr;
size_t applog_off = 0;

int applog_vprintf(const char *fmt, va_list args) {
  char line[200];
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n > 0) {
    if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;
    portENTER_CRITICAL(&applog_mux);
    size_t h = applog_head;
    for (int i = 0; i < n; i++) {
      const size_t nh = (h + 1) % kAppLogRing;
      if (nh == applog_tail) break;  // ring full: drop the rest of this line
      applog_ring[h] = line[i];
      h = nh;
    }
    applog_head = h;
    portEXIT_CRITICAL(&applog_mux);
  }
  return applog_orig_vprintf ? applog_orig_vprintf(fmt, args) : n;
}

void applog_task(void *) {
  uint8_t buf[256];
  int64_t next_heap_us = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if (esp_timer_get_time() >= next_heap_us) {
      next_heap_us = esp_timer_get_time() + 5000000;
      TaskHandle_t main_h = xTaskGetHandle("main");
      ESP_LOGI("heap", "free=%u min_free=%u largest=%u dma_free=%u main_stack_hwm=%u",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
               main_h ? (unsigned)uxTaskGetStackHighWaterMark(main_h) : 0u);
    }
    while (true) {
      size_t n = 0;
      portENTER_CRITICAL(&applog_mux);
      while (n < sizeof(buf) && applog_tail != applog_head) {
        buf[n++] = static_cast<uint8_t>(applog_ring[applog_tail]);
        applog_tail = (applog_tail + 1) % kAppLogRing;
      }
      portEXIT_CRITICAL(&applog_mux);
      if (n == 0) break;
      if (applog_off + n > applog_part->size) {  // full: wrap
        esp_partition_erase_range(applog_part, 0, applog_part->size);
        applog_off = 0;
      }
      if (esp_partition_write(applog_part, applog_off, buf, n) == ESP_OK) applog_off += n;
    }
  }
}

void applog_init() {
  applog_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
                                         "applog");
  if (!applog_part) return;
  // Find the end of existing data: last non-0xFF byte in the partition.
  uint8_t chunk[256];
  size_t end = 0;
  for (size_t off = 0; off < applog_part->size; off += sizeof(chunk)) {
    if (esp_partition_read(applog_part, off, chunk, sizeof(chunk)) != ESP_OK) return;
    for (size_t i = 0; i < sizeof(chunk); i++)
      if (chunk[i] != 0xFF) end = off + i + 1;
  }
  applog_off = end;
  applog_orig_vprintf = esp_log_set_vprintf(applog_vprintf);
  xTaskCreate(applog_task, "applog", 3072, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}
}  // namespace

extern "C" void app_main(void)
{
    applog_init();
    ESP_LOGI("applog", "===== BOOT (reset reason above/below) =====");
    ESP_LOGI(TAG, "boot, reset reason=%d", esp_reset_reason());

    display_mutex = xSemaphoreCreateMutex();
    parse_ping_targets();  // hub75/ping1 and hub75/ping2's target list - see CONFIG_PING_TARGETS

    HUB75_I2S_CFG::i2s_pins pins = {
        WF1_R1_PIN, WF1_G1_PIN, WF1_B1_PIN, WF1_R2_PIN, WF1_G2_PIN, WF1_B2_PIN,
        WF1_A_PIN, WF1_B_PIN, WF1_C_PIN, WF1_D_PIN, WF1_E_PIN,
        WF1_LAT_PIN, WF1_OE_PIN, WF1_CLK_PIN
    };

    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN, pins);
    mxconfig.driver = HUB75_I2S_CFG::FM6126A;
    // latch_blanking was bumped from the library default (4) to 8 as an
    // untested attempt at fixing a color/brightness-imbalance report (white
    // showing as pure red; hub75/dim and hub75/bright barely affecting red
    // or blue while working correctly on green) - it never got confirmed to
    // help, and BCM (the per-bit-plane PWM that produces intermediate
    // brightness levels) is exactly the kind of timing that extra latch
    // blanking could disturb unevenly across the R/G/B driver chips. Back to
    // the library default while that report gets re-tested.
    mxconfig.latch_blanking = 4;
    // Confirmed fix (photo evidence) for a *different*, already-resolved
    // bug: two identical adjacent pixels on hub75/test1, a clock-edge signal
    // integrity issue. FM6126A doesn't force this to any value (unlike
    // MBI5124/DP3246), so flipping it was safe to try.
    mxconfig.clkphase = false;  // was true (the default)
    // Double-buffered: without this, drawPixelRGB888/fillRect/etc. write
    // directly into the buffer the DMA engine is actively scanning out, so
    // any frame that takes a while to draw (hub75/ray1's per-pixel float
    // math, mainly) is visible mid-draw as tearing/flicker. With this on,
    // drawing happens into a hidden back buffer and flipDMABuffer() (see the
    // render loop) swaps it in atomically once the frame is complete.
    // Off for 128x64: a second 64KB framebuffer at 8-bit depth doesn't fit in
    // the ~139KB free heap (begin() fails with "Not enough memory"). Costs
    // some tearing on slow-to-draw frames (hub75/ray1); flipDMABuffer() is
    // a harmless no-op when this is off. Was true on the 64x32 panel.
    mxconfig.double_buff = false;
    // 90Hz minimum refresh (library default 60): at 60-62Hz the panel visibly
    // flickers. The library's calc gives ~93Hz at lsbMsbTransitionBit 1, i.e.
    // it merges only the lowest bit-plane (100 forced transition bit 2 / 150Hz
    // and cost another bit of depth). See the lsbMsbTransitionBit lines in
    // the applog.
    mxconfig.min_refresh_rate = 90;
    // 6-bit colour depth (library default 8). Heap is very tight at 128x64:
    // 8-bit's 64KB buffer left ~9KB after WiFi init and WiFi association then
    // failed (reason 2/202/205, see applog). 6 bits frees 16KB and, per the
    // library's refresh-rate calc, actually runs all 6 planes at ~62Hz, where
    // 8 bits had to merge low planes (lsbMsbTransitionBit 3) to hit 60Hz.
    mxconfig.setPixelColorDepthBits(6);

    ESP_LOGI(TAG, "constructing MatrixPanel_I2S_DMA: %dx%d chain=%d driver=FM6126A latch_blanking=%d clkphase=%d",
             PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN, mxconfig.latch_blanking, mxconfig.clkphase);

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    bool ok = dma_display->begin();
    ESP_LOGI(TAG, "dma_display->begin() returned %d", ok);
    if (!ok) {
        ESP_LOGE(TAG, "begin() FAILED");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Start dim by default (as if hub75/dim had already been sent) - the
    // very first thing drawn (the owner-info splash)
    // should come up gentle rather than at full brightness.
    dma_display->setBrightness8(kDimBrightness);

    // Onboard button (see WF1_BUTTON_PIN above) - active-low, so an internal
    // pull-up keeps it reading HIGH (unpressed) with nothing else attached.
    gpio_reset_pin(static_cast<gpio_num_t>(WF1_BUTTON_PIN));
    gpio_set_direction(static_cast<gpio_num_t>(WF1_BUTTON_PIN), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(WF1_BUTTON_PIN), GPIO_PULLUP_ONLY);

    // (The old blocking "RR=<reset reason>" banner + 5s settle delay that lived
    // here is gone: the render loop now shows the owner-info splash at once,
    // and the 5s pre-WiFi settle delay moved into network_init_task(), which
    // is load-bearing - see there. The reset reason is still logged at boot,
    // so it's in the applog.)

    // wifi_init() blocks its own task until connected, retrying forever, so
    // this runs off-task for the render loop below to start immediately and
    // show WiFi connection status while it works.
    xTaskCreate(network_init_task, "network_init", 8192, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    // hub75/ping1's ping-and-advance loop - see ping1_task() for why this
    // runs as its own always-on background task rather than inline in the
    // render loop below.
#if HUB75_FULL_EFFECTS
    xTaskCreate(ping1_task, "ping1", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
#endif
#if HUB75_FULL_EFFECTS
    xTaskCreate(ping2_task, "ping2", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
#endif
#if HUB75_FULL_EFFECTS
    xTaskCreate(maze4_task, "maze4", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
#endif

    // Until the first MQTT message arrives, this shows live WiFi status
    // instead (see set_wifi_status_connecting()/set_wifi_status_connected()
    // and display_mode). Scrolled text/color is updated live from
    // CONFIG_MQTT_MESSAGE_TOPIC - display_text/r/g/b start out as
    // "Hello, world!" in green as the eventual fallback.
    char local_text[kMaxDisplayText];
    uint8_t local_r, local_g, local_b;
    int16_t text_px = 0;
    const int16_t message_y = (PANEL_HEIGHT - 8) / 2;
    int16_t x = PANEL_WIDTH;

    StatusLine local_status[kMaxStatusLines];
    int local_status_count = 0;

    // hub75/test1: one random pixel per cycle, smoothly fading in then out
    // (instead of a hard on/off blink) - fast and continuous, no dark gap.
    constexpr int kTest1CycleTicks = 7;  // 7 * 30ms ~= 210ms per pixel
    constexpr int kTest1HalfCycle = kTest1CycleTicks / 2;
    int16_t test1_x = 0, test1_y = 0;
    uint8_t test1_r = 255, test1_g = 255, test1_b = 255;
    int test1_tick = kTest1CycleTicks;  // force picking a pixel on the first frame

    // hub75/test2: vertical bar graph, 0 at the bottom, random heights
    // refreshed periodically, one fixed color per bar.
    constexpr int kBarCount = 10;
    constexpr int16_t kBarWidth = 5;
    constexpr int16_t kBarGap = 1;
    constexpr int16_t kBarsTotalWidth = kBarCount * kBarWidth + (kBarCount - 1) * kBarGap;
    constexpr int16_t kBarsStartX = (PANEL_WIDTH - kBarsTotalWidth) / 2;
    constexpr int kBarRefreshTicks = 8;  // ~240ms
    constexpr uint8_t kBarPalette[kBarCount][3] = {
        {255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {128, 255, 0}, {0, 255, 0},
        {0, 255, 128}, {0, 255, 255}, {0, 128, 255}, {0, 0, 255}, {255, 0, 255},
    };
    uint8_t bar_height[kBarCount] = {};
    int bar_tick = kBarRefreshTicks;  // force an initial randomize on first frame

    // hub75/test3: 8x8 pacman sprite looping around the panel perimeter,
    // chomping as it goes.
    constexpr int kPacmanMoveTicks = 2;   // ~60ms per pixel of movement
    constexpr int kPacmanMouthTicks = 5;  // ~150ms per mouth toggle
    int16_t pac_x = 0, pac_y = 0;
    int pac_dir = 0;  // 0=right along top, 1=down along right, 2=left along bottom, 3=up along left
    int pac_move_tick = 0;
    int pac_mouth_tick = 0;
    bool pac_mouth_open = true;

    // Per-frame update+draw for test1/2/3, factored out so hub75/0 (see
    // kAutoTest below) can play each one back-to-back using this exact same
    // logic and state instead of duplicating it.
    auto run_test1_frame = [&]() {
        if (++test1_tick >= kTest1CycleTicks) {
            test1_tick = 0;
            test1_x = static_cast<int16_t>(esp_random() % PANEL_WIDTH);
            test1_y = static_cast<int16_t>(esp_random() % PANEL_HEIGHT);
            random_color(&test1_r, &test1_g, &test1_b);
        }
        // Triangular fade envelope (0 -> full -> 0 across the cycle) instead
        // of an abrupt on/off blink.
        const int level = test1_tick <= kTest1HalfCycle ? test1_tick : (kTest1CycleTicks - test1_tick);
        const uint16_t scale = static_cast<uint16_t>((level * 255) / kTest1HalfCycle);
        const uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(test1_r) * scale) / 255);
        const uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(test1_g) * scale) / 255);
        const uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(test1_b) * scale) / 255);
        dma_display->drawPixelRGB888(test1_x, test1_y, r, g, b);
    };

    auto run_test2_frame = [&]() {
        if (++bar_tick >= kBarRefreshTicks) {
            bar_tick = 0;
            for (auto &h : bar_height) {
                h = static_cast<uint8_t>(1 + (esp_random() % PANEL_HEIGHT));
            }
        }
        for (int i = 0; i < kBarCount; i++) {
            const int16_t bar_x = kBarsStartX + i * (kBarWidth + kBarGap);
            for (int16_t row = 0; row < bar_height[i]; row++) {
                const int16_t py = PANEL_HEIGHT - 1 - row;
                for (int16_t col = 0; col < kBarWidth; col++) {
                    dma_display->drawPixelRGB888(bar_x + col, py, kBarPalette[i][0], kBarPalette[i][1],
                                                  kBarPalette[i][2]);
                }
            }
        }
    };

    auto run_test3_frame = [&]() {
        if (++pac_mouth_tick >= kPacmanMouthTicks) {
            pac_mouth_tick = 0;
            pac_mouth_open = !pac_mouth_open;
        }
        if (++pac_move_tick >= kPacmanMoveTicks) {
            pac_move_tick = 0;
            switch (pac_dir) {
                case 0:
                    if (++pac_x >= PANEL_WIDTH - 8) pac_dir = 1;
                    break;
                case 1:
                    if (++pac_y >= PANEL_HEIGHT - 8) pac_dir = 2;
                    break;
                case 2:
                    if (--pac_x <= 0) pac_dir = 3;
                    break;
                default:
                    if (--pac_y <= 0) pac_dir = 0;
                    break;
            }
        }
        draw_sprite8(pac_x, pac_y, pac_mouth_open ? kPacmanOpen : kPacmanClosed, 255, 255, 0);
    };

    // hub75/2 (and hub75/2L, the double-size/slower variant): elevator floor
    // display, 1..100..1 forever. Digits sit in three fixed-x slots
    // (hundreds/tens/ones, right-aligned, blank instead of leading zeros) so
    // the arrow after them never moves as digit count changes between 1, 2,
    // and 3. Red counting up, green counting down. Both variants share the
    // same counting state (elev_value/elev_up) - only layout/scale/hold
    // duration differ - so switching from one to the other picks up at the
    // same floor instead of resetting.
    constexpr int kElevAnimTicks = 8;         // 8 * 30ms = 240ms scroll per floor, regardless of scale
    constexpr int kElevHoldTicksNormal = 6;   // 6 * 30ms = 180ms pause once landed (hub75/2)
    constexpr int kElevHoldTicksLarge = 67;   // 67 * 30ms ~= 2s pause once landed (hub75/2L)
    auto elev_slot_char = [](int value, int slot) -> char {
        int digit;
        bool show;
        if (slot == 0) {
            digit = value / 100;
            show = value >= 100;
        } else if (slot == 1) {
            digit = (value / 10) % 10;
            show = value >= 10;
        } else {
            digit = value % 10;
            show = true;
        }
        return show ? static_cast<char>('0' + digit) : ' ';
    };
    int elev_value = 1;
    bool elev_up = true;
    bool elev_animating = false;
    int elev_tick = kElevHoldTicksNormal;  // start by immediately kicking off the first animation

    // scale: 1 for hub75/2, 2 for hub75/2L (nearest-neighbor upscaled
    // digits; the arrow stays 8x8 either way so everything still fits the
    // 64px-wide panel). hold_ticks: how long to pause on each landed floor.
    // digit_count: 3 (hundreds/tens/ones, with arrow) for hub75/2 and
    // hub75/2L, or 2 (tens/ones only, no arrow - doesn't fit otherwise) for
    // hub75/2LL at scale=4. When digit_count==2, "100" shows as "00" same as
    // any other multiple of 100 - there's no room left to disambiguate.
    auto run_elevator_frame = [&](int scale, int hold_ticks, int digit_count, bool show_arrow) {
        const int16_t digit_w = 8 * scale;
        const int16_t digit_gap = digit_count == 3 ? static_cast<int16_t>(scale) : 0;
        const int16_t arrow_gap = scale == 1 ? 3 : 2;
        constexpr int16_t kArrowW = 8;  // arrow sprite is always 8x8
        const int16_t block_w =
            digit_count * digit_w + (digit_count - 1) * digit_gap + (show_arrow ? (arrow_gap + kArrowW) : 0);
        const int16_t start_x = (PANEL_WIDTH - block_w) / 2;
        const int16_t digit_h = 8 * scale;
        const int16_t y = (PANEL_HEIGHT - digit_h) / 2;
        const int16_t arrow_x = start_x + digit_count * digit_w + (digit_count - 1) * digit_gap + arrow_gap;
        const int16_t arrow_y = y + (digit_h - 8) / 2;
        const int slot_offset = 3 - digit_count;  // digit_count==2 skips the hundreds slot

        int target = elev_value + (elev_up ? 1 : -1);
        if (elev_animating) {
            if (++elev_tick >= kElevAnimTicks) {
                elev_tick = 0;
                elev_animating = false;
                elev_value = target;
                if (elev_value >= 100) elev_up = false;
                else if (elev_value <= 1) elev_up = true;
            }
        } else if (++elev_tick >= hold_ticks) {
            elev_tick = 0;
            elev_animating = true;
        }
        target = elev_value + (elev_up ? 1 : -1);  // re-derive in case elev_up just flipped
        const int t = (elev_animating ? elev_tick : kElevAnimTicks) * scale;
        const int from_value = elev_value;
        const int to_value = elev_animating ? target : elev_value;
        const uint8_t r = elev_up ? 255 : 0;
        const uint8_t g = elev_up ? 0 : 255;
        for (int slot = 0; slot < digit_count; slot++) {
            const int16_t slot_x = start_x + slot * (digit_w + digit_gap);
            draw_digit_scroll(slot_x, y, elev_slot_char(from_value, slot_offset + slot),
                               elev_slot_char(to_value, slot_offset + slot), t, elev_up, r, g, 0, scale);
        }
        if (show_arrow) {
            draw_sprite8(arrow_x, arrow_y, elev_up ? kArrowUp : kArrowDown, r, g, 0);
        }
    };

    // hub75/clock: live 24-hour HH:MM:SS clock, each of the 6 digits scrolling
    // odometer-style (draw_digit_scroll, same technique as the elevator)
    // whenever it changes - always "up" since time only moves forward.
    // Six independent per-slot animations (rather than one shared tick like
    // the elevator) since hours/minutes/seconds digits change at different
    // rates and can be mid-scroll independently of each other.
    constexpr int kClockAnimTicks = 8;  // 8 * 30ms = 240ms scroll, well under the 1s min digit period
    constexpr int16_t kClockDigitW = 8;
    constexpr int16_t kClockColonW = 4;
    constexpr int16_t kClockGap = 1;
    constexpr int16_t kClockBlockW =
        6 * kClockDigitW + 2 * kClockColonW + 4 * kClockGap;  // HH:MM:SS = 60px
    constexpr int16_t kClockStartX = (PANEL_WIDTH - kClockBlockW) / 2;
    constexpr int16_t kClockY = (PANEL_HEIGHT - 8) / 2;
    constexpr int16_t kClockSlotX[6] = {
        kClockStartX,
        kClockStartX + kClockDigitW,
        kClockStartX + 2 * kClockDigitW + kClockGap + kClockColonW + kClockGap,
        kClockStartX + 3 * kClockDigitW + kClockGap + kClockColonW + kClockGap,
        kClockStartX + 4 * kClockDigitW + 2 * (kClockGap + kClockColonW + kClockGap),
        kClockStartX + 5 * kClockDigitW + 2 * (kClockGap + kClockColonW + kClockGap),
    };
    constexpr int16_t kClockColonX[2] = {
        kClockStartX + 2 * kClockDigitW + kClockGap,
        kClockStartX + 4 * kClockDigitW + kClockGap + (kClockGap + kClockColonW + kClockGap),
    };
    char clock_digit_old[6];
    char clock_digit_cur[6];
    int clock_anim_tick[6];
    for (int i = 0; i < 6; i++) {
        clock_digit_old[i] = clock_digit_cur[i] = '0';
        clock_anim_tick[i] = kClockAnimTicks;
    }
    bool clock_initialized = false;

    // "Good NTP connection" indicator: two 2x2 green dots in the lower-right
    // corner, blinking ~500ms on/off, shown once ntp_synced is true.
    constexpr int16_t kClockDotSize = 2;
    constexpr int16_t kClockDotGap = 1;
    constexpr int16_t kClockDotY = PANEL_HEIGHT - kClockDotSize;
    constexpr int16_t kClockDotX0 = PANEL_WIDTH - 2 * kClockDotSize - kClockDotGap;
    constexpr int16_t kClockDotX1 = kClockDotX0 + kClockDotSize + kClockDotGap;
    constexpr int kClockDotBlinkTicks = 17;  // 17 * 30ms ~= 500ms per phase
    int clock_dot_tick = 0;

    auto run_clock_frame = [&](bool ntp_ok, ClockTz tz, uint8_t clock_r, uint8_t clock_g, uint8_t clock_b) {
        // Reapplied every frame (cheap - just parses a short string) rather
        // than cached, since hub75/time also drives the process-wide TZ for
        // its own dual-zone display - a cache here could go stale behind
        // its back and silently mislabel the wrong zone.
        apply_clock_tz(tz);
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        const char new_digits[6] = {
            static_cast<char>('0' + tm_now.tm_hour / 10), static_cast<char>('0' + tm_now.tm_hour % 10),
            static_cast<char>('0' + tm_now.tm_min / 10),  static_cast<char>('0' + tm_now.tm_min % 10),
            static_cast<char>('0' + tm_now.tm_sec / 10),  static_cast<char>('0' + tm_now.tm_sec % 10),
        };
        if (!clock_initialized) {
            for (int i = 0; i < 6; i++) {
                clock_digit_old[i] = clock_digit_cur[i] = new_digits[i];
                clock_anim_tick[i] = kClockAnimTicks;
            }
            clock_initialized = true;
        }
        for (int i = 0; i < 6; i++) {
            if (new_digits[i] != clock_digit_cur[i] && clock_anim_tick[i] >= kClockAnimTicks) {
                clock_digit_old[i] = clock_digit_cur[i];
                clock_digit_cur[i] = new_digits[i];
                clock_anim_tick[i] = 0;
            }
            const int t = clock_anim_tick[i] < kClockAnimTicks ? clock_anim_tick[i] : kClockAnimTicks;
            draw_digit_scroll(kClockSlotX[i], kClockY, clock_digit_old[i], clock_digit_cur[i], t, true, clock_r,
                               clock_g, clock_b, 1);
            if (clock_anim_tick[i] < kClockAnimTicks) clock_anim_tick[i]++;
        }
        for (int i = 0; i < 2; i++) {
            dma_display->fillRect(kClockColonX[i], kClockY + 2, 2, 2, clock_r, clock_g, clock_b);
            dma_display->fillRect(kClockColonX[i], kClockY + 5, 2, 2, clock_r, clock_g, clock_b);
        }
        if (ntp_ok) {
            if (++clock_dot_tick >= 2 * kClockDotBlinkTicks) clock_dot_tick = 0;
            if (clock_dot_tick < kClockDotBlinkTicks) {
                dma_display->fillRect(kClockDotX0, kClockDotY, kClockDotSize, kClockDotSize, 0, 255, 0);
                dma_display->fillRect(kClockDotX1, kClockDotY, kClockDotSize, kClockDotSize, 0, 255, 0);
            }
        }
    };

    int clock_confirm_tick = 0;

    // hub75/standardtest1: white/red/green/blue x dim/bright, ~2s/phase,
    // restarts at phase 0 every activation - see the kStandardTest1 case
    // below. Advances automatically, or manually via a short press of the
    // onboard button while this mode is active (see the button poll below,
    // right before the mode switch).
    int std1_phase = 0;
    int std1_phase_tick = 0;
    bool button_prev_raw = true;  // true = unpressed (active-low)

    // hub75/matrix1c/2c/3c: re-randomize every ~0.25s while any of the three
    // continuous variants is active - see the kMatrix*Cont render-loop
    // cases below.
    constexpr int kMatrixContTicks = 8;  // ~0.25s
    int matrix_cont_tick = 0;

    // hub75/matrix: "Matrix digital rain" screensaver - columns of falling,
    // randomly-shimmering small-font glyphs (see draw_text_small()/
    // small_font_glyph() above) with a bright head and a fading green tail.
    // Position/speed/length per column are randomized independently and
    // re-rolled once a column's whole trail has scrolled past the bottom.
    constexpr int kMatrixRainCols = PANEL_WIDTH / kSmallFontAdvance;  // 64/4 = 16
    constexpr int kMatrixRainRowH = kSmallFontHeight + 1;             // 5px glyph + 1px gap
    constexpr int kMatrixRainMaxLen = 9;
    constexpr char kMatrixRainCharset[] = "0123456789ACNRTIMESotnecdryigmphub./=:";
    constexpr int kMatrixRainCharsetLen = sizeof(kMatrixRainCharset) - 1;  // exclude the trailing NUL
    float matrix_rain_row[kMatrixRainCols];
    float matrix_rain_speed[kMatrixRainCols];
    int matrix_rain_len[kMatrixRainCols];
    char matrix_rain_chars[kMatrixRainCols][kMatrixRainMaxLen];

    auto reset_matrix_rain_column = [&](int c, bool randomize_start) {
        matrix_rain_speed[c] = 0.12f + static_cast<float>(esp_random() % 24) / 100.0f;
        matrix_rain_len[c] = 4 + static_cast<int>(esp_random() % (kMatrixRainMaxLen - 3));
        for (int i = 0; i < kMatrixRainMaxLen; i++) {
            matrix_rain_chars[c][i] = kMatrixRainCharset[esp_random() % kMatrixRainCharsetLen];
        }
        // Stagger restarts: start above (or, on first entry, anywhere within)
        // the panel so every column doesn't reset in lockstep.
        constexpr int kRows = PANEL_HEIGHT / kMatrixRainRowH;
        matrix_rain_row[c] = randomize_start
            ? -static_cast<float>(esp_random() % (kRows + kMatrixRainMaxLen))
            : -static_cast<float>(esp_random() % kMatrixRainMaxLen);
    };
    for (int c = 0; c < kMatrixRainCols; c++) reset_matrix_rain_column(c, true);

    auto run_matrix_rain_frame = [&]() {
        for (int c = 0; c < kMatrixRainCols; c++) {
            matrix_rain_row[c] += matrix_rain_speed[c];
            const int head_row = static_cast<int>(floorf(matrix_rain_row[c]));
            const int len = matrix_rain_len[c];
            if ((head_row - len) * kMatrixRainRowH > PANEL_HEIGHT) {
                reset_matrix_rain_column(c, false);
                continue;
            }
            const int16_t x = static_cast<int16_t>(c * kSmallFontAdvance);
            for (int i = 0; i < len; i++) {
                const int row_idx = head_row - i;
                const int16_t y = static_cast<int16_t>(row_idx * kMatrixRainRowH);
                if (y + kSmallFontHeight < 0 || y >= PANEL_HEIGHT) continue;
                if (static_cast<int>(esp_random() % 100) < 6) {
                    matrix_rain_chars[c][i] = kMatrixRainCharset[esp_random() % kMatrixRainCharsetLen];
                }
                const float fade = 1.0f - static_cast<float>(i) / len;
                uint8_t r, g, b;
                if (i == 0) {
                    r = 210; g = 255; b = 210;  // bright near-white head
                } else {
                    r = 0; g = static_cast<uint8_t>(60 + 195 * fade); b = 0;  // fading green trail
                }
                const char glyph[2] = {matrix_rain_chars[c][i], '\0'};
                draw_text_small(x, y, glyph, r, g, b);
            }
        }
    };

    // hub75/ball1: a filled 8x8 ball bouncing around the panel - picks a
    // fresh random (vibrant, not near-black) color whenever it hits any of
    // the four walls. Position/velocity/color reset on every activation
    // (see the kBall1 reset block below).
    float ball1_x = (PANEL_WIDTH - 8) / 2.0f, ball1_y = (PANEL_HEIGHT - 8) / 2.0f;
    float ball1_vx = 0.8f, ball1_vy = 0.6f;
    uint8_t ball1_r = 255, ball1_g = 255, ball1_b = 255;

    auto run_ball1_frame = [&]() {
        constexpr int16_t kBallSize = 8;
        constexpr float kRadius = kBallSize / 2.0f - 0.3f;

        ball1_x += ball1_vx;
        ball1_y += ball1_vy;
        bool hit = false;
        if (ball1_x <= 0.0f) {
            ball1_x = 0.0f;
            ball1_vx = -ball1_vx;
            hit = true;
        } else if (ball1_x >= PANEL_WIDTH - kBallSize) {
            ball1_x = PANEL_WIDTH - kBallSize;
            ball1_vx = -ball1_vx;
            hit = true;
        }
        if (ball1_y <= 0.0f) {
            ball1_y = 0.0f;
            ball1_vy = -ball1_vy;
            hit = true;
        } else if (ball1_y >= PANEL_HEIGHT - kBallSize) {
            ball1_y = PANEL_HEIGHT - kBallSize;
            ball1_vy = -ball1_vy;
            hit = true;
        }
        if (hit) {
            random_color(&ball1_r, &ball1_g, &ball1_b, 80, 176);
        }

        const float cx = ball1_x + (kBallSize - 1) / 2.0f;
        const float cy = ball1_y + (kBallSize - 1) / 2.0f;
        for (int dy = 0; dy < kBallSize; dy++) {
            for (int dx = 0; dx < kBallSize; dx++) {
                const float px = ball1_x + dx;
                const float py = ball1_y + dy;
                const float ddx = px - cx, ddy = py - cy;
                if (ddx * ddx + ddy * ddy <= kRadius * kRadius) {
                    dma_display->drawPixelRGB888(static_cast<int16_t>(px), static_cast<int16_t>(py), ball1_r, ball1_g,
                                                  ball1_b);
                }
            }
        }
    };

    // hub75/shootingstars: 3 bouncing 2x2 stars (red/blue/white), each with
    // a fading tail up to 6px long pointing directly back along its current
    // direction of travel (recomputed every frame, so the tail always
    // points the right way even after a bounce). Position/velocity reset
    // (randomized) on every activation.
    constexpr int kStarCount = 3;
    constexpr uint8_t kStarColors[kStarCount][3] = {
        {255, 0, 0},
        {0, 0, 255},
        {255, 255, 255},
    };
    float star_x[kStarCount] = {8.0f, 28.0f, 48.0f};
    float star_y[kStarCount] = {8.0f, 16.0f, 24.0f};
    float star_vx[kStarCount] = {0.8f, -0.9f, 0.7f};
    float star_vy[kStarCount] = {0.6f, 0.7f, -0.8f};

    // Explosion burst shown when two stars' bounding boxes coincide - each
    // pair (0,1)/(0,2)/(1,2) fires once per contact (star_pair_touching
    // tracks the previous frame's state so a sustained overlap doesn't
    // retrigger every frame), landing in whichever slot is free.
    struct StarExplosion {
        bool active;
        float x, y;
        int tick;
    };
    constexpr int kStarExplosionSlots = 2;
    StarExplosion star_explosions[kStarExplosionSlots] = {{false, 0, 0, 0}, {false, 0, 0, 0}};
    bool star_pair_touching[3] = {false, false, false};

    auto run_shootingstars_frame = [&]() {
        constexpr int16_t kStarSize = 2;
        constexpr int kTailLen = 6;
        constexpr int kExplosionDuration = 12;      // ~0.36s
        constexpr float kExplosionMaxRadius = 4.0f;
        constexpr int kPairA[3] = {0, 0, 1};
        constexpr int kPairB[3] = {1, 2, 2};

        for (int s = 0; s < kStarCount; s++) {
            star_x[s] += star_vx[s];
            star_y[s] += star_vy[s];
            if (star_x[s] <= 0.0f) {
                star_x[s] = 0.0f;
                star_vx[s] = -star_vx[s];
            } else if (star_x[s] >= PANEL_WIDTH - kStarSize) {
                star_x[s] = PANEL_WIDTH - kStarSize;
                star_vx[s] = -star_vx[s];
            }
            if (star_y[s] <= 0.0f) {
                star_y[s] = 0.0f;
                star_vy[s] = -star_vy[s];
            } else if (star_y[s] >= PANEL_HEIGHT - kStarSize) {
                star_y[s] = PANEL_HEIGHT - kStarSize;
                star_vy[s] = -star_vy[s];
            }

            const float speed = sqrtf(star_vx[s] * star_vx[s] + star_vy[s] * star_vy[s]);
            const float dir_x = (speed > 0.001f) ? -star_vx[s] / speed : -1.0f;
            const float dir_y = (speed > 0.001f) ? -star_vy[s] / speed : 0.0f;
            const uint8_t base_r = kStarColors[s][0], base_g = kStarColors[s][1], base_b = kStarColors[s][2];
            const float head_cx = star_x[s] + (kStarSize - 1) / 2.0f;
            const float head_cy = star_y[s] + (kStarSize - 1) / 2.0f;
            for (int i = 1; i <= kTailLen; i++) {
                const float fade = 1.0f - static_cast<float>(i) / (kTailLen + 1);
                const int16_t tx = static_cast<int16_t>(head_cx + dir_x * i);
                const int16_t ty = static_cast<int16_t>(head_cy + dir_y * i);
                if (tx < 0 || tx >= PANEL_WIDTH || ty < 0 || ty >= PANEL_HEIGHT) continue;
                dma_display->drawPixelRGB888(tx, ty, static_cast<uint8_t>(base_r * fade),
                                              static_cast<uint8_t>(base_g * fade), static_cast<uint8_t>(base_b * fade));
            }
            dma_display->fillRect(static_cast<int16_t>(star_x[s]), static_cast<int16_t>(star_y[s]), kStarSize,
                                   kStarSize, base_r, base_g, base_b);
        }

        for (int p = 0; p < 3; p++) {
            const int a = kPairA[p], b = kPairB[p];
            const float acx = star_x[a] + (kStarSize - 1) / 2.0f;
            const float acy = star_y[a] + (kStarSize - 1) / 2.0f;
            const float bcx = star_x[b] + (kStarSize - 1) / 2.0f;
            const float bcy = star_y[b] + (kStarSize - 1) / 2.0f;
            const float ddx = acx - bcx, ddy = acy - bcy;
            const float touch_dist = kStarSize + 1.0f;
            const bool touching = (ddx * ddx + ddy * ddy) <= (touch_dist * touch_dist);
            if (touching && !star_pair_touching[p]) {
                const float ex = (acx + bcx) / 2.0f, ey = (acy + bcy) / 2.0f;
                int slot = 0;
                for (int i = 0; i < kStarExplosionSlots; i++) {
                    if (!star_explosions[i].active) {
                        slot = i;
                        break;
                    }
                }
                star_explosions[slot] = {true, ex, ey, 0};
            }
            star_pair_touching[p] = touching;
        }

        for (int i = 0; i < kStarExplosionSlots; i++) {
            StarExplosion &ex = star_explosions[i];
            if (!ex.active) continue;
            ex.tick++;
            if (ex.tick > kExplosionDuration) {
                ex.active = false;
                continue;
            }
            const float progress = static_cast<float>(ex.tick) / kExplosionDuration;
            const float radius = progress * kExplosionMaxRadius;
            const float fade = 1.0f - progress;
            const uint8_t er = static_cast<uint8_t>(255 * fade);
            const uint8_t eg = static_cast<uint8_t>(180 * fade);
            constexpr int kSparkCount = 8;
            for (int k = 0; k < kSparkCount; k++) {
                const float ang = k * (6.283185307f / kSparkCount);
                const int16_t px = static_cast<int16_t>(ex.x + cosf(ang) * radius);
                const int16_t py = static_cast<int16_t>(ex.y + sinf(ang) * radius);
                if (px < 0 || px >= PANEL_WIDTH || py < 0 || py >= PANEL_HEIGHT) continue;
                dma_display->drawPixelRGB888(px, py, er, eg, 0);
            }
            if (ex.tick < kExplosionDuration / 2) {
                const int16_t cx = static_cast<int16_t>(ex.x), cy = static_cast<int16_t>(ex.y);
                if (cx >= 0 && cx < PANEL_WIDTH && cy >= 0 && cy < PANEL_HEIGHT) {
                    dma_display->drawPixelRGB888(cx, cy, 255, 255, 200);
                }
            }
        }
    };

    // hub75/si: self-playing, stylized Space Invaders. Formation sweeps
    // side to side and steps down on each wall hit; player cannon
    // autonomously tracks the lowest surviving invader's column and
    // auto-fires; the wave respawns when cleared or once it gets too close
    // to the player.
    constexpr int kSiCols = 8;
    constexpr int kSiRows = 3;
    constexpr int16_t kSiColSpacing = 7;
    constexpr int16_t kSiRowSpacing = 4;
    constexpr int16_t kSiStartX = 4;
    constexpr int16_t kSiStartY = 2;
    constexpr int16_t kSiInvaderW = 3;
    constexpr int16_t kSiInvaderH = 2;
    bool si_alive[kSiRows][kSiCols];
    for (int r = 0; r < kSiRows; r++)
        for (int c = 0; c < kSiCols; c++) si_alive[r][c] = true;
    int16_t si_formation_x = 0;
    int16_t si_formation_y = 0;
    int si_formation_dir = 1;
    int si_move_tick = 0;
    int16_t si_player_x = PANEL_WIDTH / 2;
    bool si_bullet_active = false;
    int16_t si_bullet_x = 0, si_bullet_y = 0;
    int si_fire_cooldown = 0;

    auto run_space_invaders_frame = [&]() {
        constexpr int kSiMoveTicks = 8;          // ~240ms per formation step
        constexpr int kSiFireCooldownTicks = 20;  // ~600ms between shots
        constexpr int16_t kSiBulletSpeed = 2;

        if (++si_move_tick >= kSiMoveTicks) {
            si_move_tick = 0;
            const int16_t left = kSiStartX + si_formation_x;
            const int16_t right = kSiStartX + si_formation_x + (kSiCols - 1) * kSiColSpacing + kSiInvaderW;
            if ((si_formation_dir > 0 && right + 1 >= PANEL_WIDTH) || (si_formation_dir < 0 && left - 1 < 0)) {
                si_formation_dir = -si_formation_dir;
                si_formation_y += 2;
            } else {
                si_formation_x += si_formation_dir;
            }
        }

        bool any_alive = false;
        for (int r = 0; r < kSiRows && !any_alive; r++) {
            for (int c = 0; c < kSiCols; c++) {
                if (si_alive[r][c]) {
                    any_alive = true;
                    break;
                }
            }
        }
        if (!any_alive || kSiStartY + si_formation_y + (kSiRows - 1) * kSiRowSpacing >= 24) {
            for (int r = 0; r < kSiRows; r++)
                for (int c = 0; c < kSiCols; c++) si_alive[r][c] = true;
            si_formation_x = 0;
            si_formation_y = 0;
            si_formation_dir = 1;
        }

        int16_t target_x = si_player_x;
        for (int r = kSiRows - 1; r >= 0; r--) {
            bool row_found = false;
            for (int c = 0; c < kSiCols; c++) {
                if (si_alive[r][c]) {
                    target_x = kSiStartX + si_formation_x + c * kSiColSpacing + kSiInvaderW / 2;
                    row_found = true;
                    break;
                }
            }
            if (row_found) break;
        }
        if (si_player_x < target_x) si_player_x++;
        else if (si_player_x > target_x) si_player_x--;
        if (si_player_x < 2) si_player_x = 2;
        if (si_player_x > PANEL_WIDTH - 3) si_player_x = PANEL_WIDTH - 3;

        if (si_fire_cooldown > 0) si_fire_cooldown--;
        if (!si_bullet_active && si_fire_cooldown == 0) {
            si_bullet_active = true;
            si_bullet_x = si_player_x;
            si_bullet_y = PANEL_HEIGHT - 5;
            si_fire_cooldown = kSiFireCooldownTicks;
        }

        if (si_bullet_active) {
            si_bullet_y -= kSiBulletSpeed;
            bool hit = false;
            for (int r = 0; r < kSiRows && !hit; r++) {
                for (int c = 0; c < kSiCols; c++) {
                    if (!si_alive[r][c]) continue;
                    const int16_t ix = kSiStartX + si_formation_x + c * kSiColSpacing;
                    const int16_t iy = kSiStartY + si_formation_y + r * kSiRowSpacing;
                    if (si_bullet_x >= ix && si_bullet_x < ix + kSiInvaderW && si_bullet_y >= iy &&
                        si_bullet_y < iy + kSiInvaderH) {
                        si_alive[r][c] = false;
                        hit = true;
                        break;
                    }
                }
            }
            if (hit || si_bullet_y < 0) si_bullet_active = false;
        }

        for (int r = 0; r < kSiRows; r++) {
            for (int c = 0; c < kSiCols; c++) {
                if (!si_alive[r][c]) continue;
                const int16_t ix = kSiStartX + si_formation_x + c * kSiColSpacing;
                const int16_t iy = kSiStartY + si_formation_y + r * kSiRowSpacing;
                dma_display->fillRect(ix, iy, kSiInvaderW, kSiInvaderH, 0, 255, 0);
            }
        }
        dma_display->fillRect(si_player_x - 1, PANEL_HEIGHT - 3, 3, 1, 0, 255, 255);
        dma_display->drawPixelRGB888(si_player_x, PANEL_HEIGHT - 4, 0, 255, 255);
        if (si_bullet_active) dma_display->drawPixelRGB888(si_bullet_x, si_bullet_y, 255, 255, 0);
    };

    // hub75/mi: self-playing, stylized Millipede. A segmented chain weaves
    // horizontally, reversing and stepping down on hitting a wall or
    // mushroom, wrapping back to the top once it gets too deep. Body
    // segments follow a short history of the head's past positions (the
    // same trailing technique a Snake game would use). Player tracks the
    // head and auto-fires; the chain respawns once fully cleared.
    constexpr int kMiSegments = 10;
    constexpr int kMiHistoryLen = 64;
    constexpr int kMiSegmentGap = 3;  // history samples between body segments
    constexpr int kMiMushroomCount = 10;
    int16_t mi_history_x[kMiHistoryLen];
    int16_t mi_history_y[kMiHistoryLen];
    for (int i = 0; i < kMiHistoryLen; i++) {
        mi_history_x[i] = 4;
        mi_history_y[i] = 2;
    }
    bool mi_alive[kMiSegments];
    for (int i = 0; i < kMiSegments; i++) mi_alive[i] = true;
    int16_t mi_head_x = 4, mi_head_y = 2;
    int mi_dir_x = 1;
    int mi_move_tick = 0;
    int16_t mi_mushroom_x[kMiMushroomCount] = {0};
    int16_t mi_mushroom_y[kMiMushroomCount] = {0};
    int16_t mi_player_x = PANEL_WIDTH / 2;
    bool mi_bullet_active = false;
    int16_t mi_bullet_x = 0, mi_bullet_y = 0;
    int mi_fire_cooldown = 0;

    auto run_millipede_frame = [&]() {
        constexpr int kMiMoveTicks = 3;    // ~90ms per step
        constexpr int16_t kMiMaxY = PANEL_HEIGHT - 6;    // wrap back to top past this depth
        constexpr int kMiFireCooldownTicks = 16;
        constexpr int16_t kMiBulletSpeed = 2;

        if (++mi_move_tick >= kMiMoveTicks) {
            mi_move_tick = 0;
            const int16_t next_x = static_cast<int16_t>(mi_head_x + mi_dir_x);
            bool blocked = next_x < 1 || next_x > PANEL_WIDTH - 2;
            for (int i = 0; i < kMiMushroomCount && !blocked; i++) {
                if (mi_mushroom_x[i] == next_x && mi_mushroom_y[i] == mi_head_y) blocked = true;
            }
            if (blocked) {
                mi_dir_x = -mi_dir_x;
                mi_head_y += 2;
                if (mi_head_y > kMiMaxY) mi_head_y = 2;
            } else {
                mi_head_x = next_x;
            }
            for (int i = kMiHistoryLen - 1; i > 0; i--) {
                mi_history_x[i] = mi_history_x[i - 1];
                mi_history_y[i] = mi_history_y[i - 1];
            }
            mi_history_x[0] = mi_head_x;
            mi_history_y[0] = mi_head_y;
        }

        bool any_alive = false;
        for (int i = 0; i < kMiSegments; i++) {
            if (mi_alive[i]) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) {
            for (int i = 0; i < kMiSegments; i++) mi_alive[i] = true;
        }

        const int16_t target_x = mi_history_x[0];
        if (mi_player_x < target_x) mi_player_x++;
        else if (mi_player_x > target_x) mi_player_x--;
        if (mi_player_x < 2) mi_player_x = 2;
        if (mi_player_x > PANEL_WIDTH - 3) mi_player_x = PANEL_WIDTH - 3;

        if (mi_fire_cooldown > 0) mi_fire_cooldown--;
        if (!mi_bullet_active && mi_fire_cooldown == 0) {
            mi_bullet_active = true;
            mi_bullet_x = mi_player_x;
            mi_bullet_y = PANEL_HEIGHT - 3;
            mi_fire_cooldown = kMiFireCooldownTicks;
        }

        if (mi_bullet_active) {
            mi_bullet_y -= kMiBulletSpeed;
            bool hit = false;
            for (int i = 0; i < kMiSegments && !hit; i++) {
                if (!mi_alive[i]) continue;
                const int16_t sx = mi_history_x[i * kMiSegmentGap];
                const int16_t sy = mi_history_y[i * kMiSegmentGap];
                if (mi_bullet_x == sx && mi_bullet_y >= sy - 1 && mi_bullet_y <= sy) {
                    mi_alive[i] = false;
                    hit = true;
                }
            }
            if (hit || mi_bullet_y < 0) mi_bullet_active = false;
        }

        for (int i = 0; i < kMiMushroomCount; i++) {
            dma_display->drawPixelRGB888(mi_mushroom_x[i], mi_mushroom_y[i], 200, 100, 0);
        }
        for (int i = 0; i < kMiSegments; i++) {
            if (!mi_alive[i]) continue;
            const int16_t sx = mi_history_x[i * kMiSegmentGap];
            const int16_t sy = mi_history_y[i * kMiSegmentGap];
            if (i == 0) dma_display->drawPixelRGB888(sx, sy, 255, 0, 0);
            else dma_display->drawPixelRGB888(sx, sy, 0, 255, 0);
        }
        dma_display->fillRect(mi_player_x - 1, PANEL_HEIGHT - 2, 3, 1, 0, 255, 255);
        dma_display->drawPixelRGB888(mi_player_x, PANEL_HEIGHT - 3, 0, 255, 255);
        if (mi_bullet_active) dma_display->drawPixelRGB888(mi_bullet_x, mi_bullet_y, 255, 255, 0);
    };

    // hub75/pong: self-playing Pong with a seven-segment scoreboard (see
    // draw_pong_score() above). Paddles track the ball's y position but are
    // speed-capped, so fast returns occasionally get past them and score -
    // otherwise a self-playing rally would never end. Scores wrap at 100.
    int16_t pong_left_score = 0, pong_right_score = 0;
    float pong_left_paddle_y = PANEL_HEIGHT * 0.6f, pong_right_paddle_y = PANEL_HEIGHT * 0.6f;
    float pong_ball_x = PANEL_WIDTH / 2.0f, pong_ball_y = PANEL_HEIGHT * 0.6f;
    float pong_ball_vx = 0.6f, pong_ball_vy = 0.4f;

    auto run_pong_frame = [&]() {
        constexpr int16_t kPongCourtTop = 7;
        constexpr int16_t kPongCourtBottom = PANEL_HEIGHT - 1;
        constexpr int16_t kPongPaddleXLeft = 1;
        constexpr int16_t kPongPaddleXRight = PANEL_WIDTH - 2;
        constexpr int16_t kPongPaddleHalfHeight = 3;
        constexpr float kPongPaddleSpeed = 0.6f;
        // Occasional post-bounce speedup so rallies don't settle into an
        // endless, perfectly predictable back-and-forth - resets to base
        // speed at the start of each new point (see the scoring branches
        // below), so it can't run away over a long rally.
        constexpr int kPongSpeedupChancePercent = 30;
        constexpr float kPongSpeedupFactor = 1.3f;
        constexpr float kPongMaxVx = 2.2f;
        constexpr float kPongMaxVy = 1.3f;

        pong_ball_x += pong_ball_vx;
        pong_ball_y += pong_ball_vy;
        if (pong_ball_y <= kPongCourtTop) {
            pong_ball_y = kPongCourtTop;
            pong_ball_vy = -pong_ball_vy;
        } else if (pong_ball_y >= kPongCourtBottom) {
            pong_ball_y = kPongCourtBottom;
            pong_ball_vy = -pong_ball_vy;
        }

        if (pong_left_paddle_y < pong_ball_y) pong_left_paddle_y += kPongPaddleSpeed;
        else if (pong_left_paddle_y > pong_ball_y) pong_left_paddle_y -= kPongPaddleSpeed;
        if (pong_right_paddle_y < pong_ball_y) pong_right_paddle_y += kPongPaddleSpeed;
        else if (pong_right_paddle_y > pong_ball_y) pong_right_paddle_y -= kPongPaddleSpeed;
        const float kPaddleMinY = kPongCourtTop + kPongPaddleHalfHeight;
        const float kPaddleMaxY = kPongCourtBottom - kPongPaddleHalfHeight;
        if (pong_left_paddle_y < kPaddleMinY) pong_left_paddle_y = kPaddleMinY;
        if (pong_left_paddle_y > kPaddleMaxY) pong_left_paddle_y = kPaddleMaxY;
        if (pong_right_paddle_y < kPaddleMinY) pong_right_paddle_y = kPaddleMinY;
        if (pong_right_paddle_y > kPaddleMaxY) pong_right_paddle_y = kPaddleMaxY;

        if (pong_ball_vx < 0 && pong_ball_x <= kPongPaddleXLeft + 1) {
            if (pong_ball_y >= pong_left_paddle_y - kPongPaddleHalfHeight &&
                pong_ball_y <= pong_left_paddle_y + kPongPaddleHalfHeight) {
                pong_ball_vx = -pong_ball_vx;
                pong_ball_vy += (pong_ball_y - pong_left_paddle_y) * 0.08f;
                if (static_cast<int>(esp_random() % 100) < kPongSpeedupChancePercent) {
                    pong_ball_vx *= kPongSpeedupFactor;
                    pong_ball_vy *= kPongSpeedupFactor;
                }
                pong_ball_x = kPongPaddleXLeft + 1;
            } else if (pong_ball_x < 0) {
                pong_right_score = (pong_right_score + 1) % 100;
                pong_ball_x = PANEL_WIDTH / 2;
                pong_ball_y = (kPongCourtTop + kPongCourtBottom) / 2;
                pong_ball_vx = 0.6f;
                pong_ball_vy = 0.3f + static_cast<float>(esp_random() % 40) / 100.0f;
            }
        } else if (pong_ball_vx > 0 && pong_ball_x >= kPongPaddleXRight - 1) {
            if (pong_ball_y >= pong_right_paddle_y - kPongPaddleHalfHeight &&
                pong_ball_y <= pong_right_paddle_y + kPongPaddleHalfHeight) {
                pong_ball_vx = -pong_ball_vx;
                pong_ball_vy += (pong_ball_y - pong_right_paddle_y) * 0.08f;
                if (static_cast<int>(esp_random() % 100) < kPongSpeedupChancePercent) {
                    pong_ball_vx *= kPongSpeedupFactor;
                    pong_ball_vy *= kPongSpeedupFactor;
                }
                pong_ball_x = kPongPaddleXRight - 1;
            } else if (pong_ball_x > PANEL_WIDTH - 1) {
                pong_left_score = (pong_left_score + 1) % 100;
                pong_ball_x = PANEL_WIDTH / 2;
                pong_ball_y = (kPongCourtTop + kPongCourtBottom) / 2;
                pong_ball_vx = -0.6f;
                pong_ball_vy = 0.3f + static_cast<float>(esp_random() % 40) / 100.0f;
            }
        }
        if (pong_ball_vy > kPongMaxVy) pong_ball_vy = kPongMaxVy;
        if (pong_ball_vy < -kPongMaxVy) pong_ball_vy = -kPongMaxVy;
        if (pong_ball_vx > kPongMaxVx) pong_ball_vx = kPongMaxVx;
        if (pong_ball_vx < -kPongMaxVx) pong_ball_vx = -kPongMaxVx;

        draw_pong_score(PANEL_WIDTH / 4 - 4, 1, pong_left_score, 255, 255, 255);
        draw_pong_score(PANEL_WIDTH * 3 / 4, 1, pong_right_score, 255, 255, 255);
        for (int16_t y = kPongCourtTop; y <= kPongCourtBottom; y += 3) {
            dma_display->drawPixelRGB888(PANEL_WIDTH / 2, y, 120, 120, 120);
        }
        dma_display->fillRect(kPongPaddleXLeft, static_cast<int16_t>(pong_left_paddle_y - kPongPaddleHalfHeight), 1,
                               kPongPaddleHalfHeight * 2 + 1, 0, 255, 0);
        dma_display->fillRect(kPongPaddleXRight, static_cast<int16_t>(pong_right_paddle_y - kPongPaddleHalfHeight),
                               1, kPongPaddleHalfHeight * 2 + 1, 0, 255, 0);
        dma_display->fillRect(static_cast<int16_t>(pong_ball_x), static_cast<int16_t>(pong_ball_y), 1, 1, 255, 0, 0);
    };

    // hub75/tictactoe: self-playing tic-tac-toe, looping forever. Board is
    // 9 cells, row-major, ' '/'X'/'O'. AI (see pick_move() in the lambda
    // below) is mostly strategic (win > block > center > corner > edge,
    // random among ties within each tier) but occasionally plays a fully
    // random legal move instead, so repeated games don't all follow the
    // same line. Alternates which mark opens each new game for fairness.
    char ttt_board[9];
    for (int i = 0; i < 9; i++) ttt_board[i] = ' ';
    char ttt_turn = 'X';
    char ttt_first_player = 'X';
    char ttt_winner = ' ';  // ' ' = in progress, 'X'/'O' = winner, 'D' = draw
    int ttt_win_a = -1, ttt_win_b = -1, ttt_win_c = -1;
    int ttt_move_tick = 0;
    int ttt_result_tick = 0;

    auto run_tictactoe_frame = [&]() {
        constexpr int kTttMoveTicks = 33;         // ~1s per move
        constexpr int kTttResultHoldTicks = 90;   // ~2.7s pause on the finished board
        constexpr int kTttRandomMoveChance = 20;  // % chance to ignore strategy entirely
        constexpr int16_t kBoardX = 16, kBoardY = 0, kCell = 10, kGap = 1;
        constexpr int kTttLines[8][3] = {
            {0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6}, {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6},
        };

        auto has_winning_move = [&](char player, int &out_cell) -> bool {
            int candidates[9];
            int n = 0;
            for (int i = 0; i < 9; i++) {
                if (ttt_board[i] != ' ') continue;
                ttt_board[i] = player;
                bool win = false;
                for (const auto &line : kTttLines) {
                    if (ttt_board[line[0]] == player && ttt_board[line[1]] == player &&
                        ttt_board[line[2]] == player) {
                        win = true;
                        break;
                    }
                }
                ttt_board[i] = ' ';
                if (win) candidates[n++] = i;
            }
            if (n == 0) return false;
            out_cell = candidates[esp_random() % n];
            return true;
        };

        auto pick_move = [&](char player) -> int {
            const char opponent = (player == 'X') ? 'O' : 'X';
            if (static_cast<int>(esp_random() % 100) < kTttRandomMoveChance) {
                int empties[9];
                int n = 0;
                for (int i = 0; i < 9; i++)
                    if (ttt_board[i] == ' ') empties[n++] = i;
                return empties[esp_random() % n];
            }
            int cell;
            if (has_winning_move(player, cell)) return cell;
            if (has_winning_move(opponent, cell)) return cell;
            if (ttt_board[4] == ' ') return 4;
            constexpr int kCorners[4] = {0, 2, 6, 8};
            int free_corners[4];
            int nc = 0;
            for (int c : kCorners)
                if (ttt_board[c] == ' ') free_corners[nc++] = c;
            if (nc > 0) return free_corners[esp_random() % nc];
            constexpr int kEdges[4] = {1, 3, 5, 7};
            int free_edges[4];
            int ne = 0;
            for (int e : kEdges)
                if (ttt_board[e] == ' ') free_edges[ne++] = e;
            return free_edges[esp_random() % ne];
        };

        if (ttt_winner == ' ') {
            if (++ttt_move_tick >= kTttMoveTicks) {
                ttt_move_tick = 0;
                const int cell = pick_move(ttt_turn);
                ttt_board[cell] = ttt_turn;
                bool win = false;
                for (const auto &line : kTttLines) {
                    if (ttt_board[line[0]] == ttt_turn && ttt_board[line[1]] == ttt_turn &&
                        ttt_board[line[2]] == ttt_turn) {
                        win = true;
                        ttt_win_a = line[0];
                        ttt_win_b = line[1];
                        ttt_win_c = line[2];
                        break;
                    }
                }
                if (win) {
                    ttt_winner = ttt_turn;
                    ttt_result_tick = 0;
                } else {
                    bool full = true;
                    for (int i = 0; i < 9; i++)
                        if (ttt_board[i] == ' ') full = false;
                    if (full) {
                        ttt_winner = 'D';
                        ttt_result_tick = 0;
                    } else {
                        ttt_turn = (ttt_turn == 'X') ? 'O' : 'X';
                    }
                }
            }
        } else {
            if (++ttt_result_tick >= kTttResultHoldTicks) {
                for (int i = 0; i < 9; i++) ttt_board[i] = ' ';
                ttt_first_player = (ttt_first_player == 'X') ? 'O' : 'X';
                ttt_turn = ttt_first_player;
                ttt_winner = ' ';
                ttt_win_a = ttt_win_b = ttt_win_c = -1;
                ttt_move_tick = 0;
                ttt_result_tick = 0;
            }
        }

        dma_display->fillRect(kBoardX + kCell, kBoardY, kGap, 3 * kCell + 2 * kGap, 90, 90, 90);
        dma_display->fillRect(kBoardX + 2 * kCell + kGap, kBoardY, kGap, 3 * kCell + 2 * kGap, 90, 90, 90);
        dma_display->fillRect(kBoardX, kBoardY + kCell, 3 * kCell + 2 * kGap, kGap, 90, 90, 90);
        dma_display->fillRect(kBoardX, kBoardY + 2 * kCell + kGap, 3 * kCell + 2 * kGap, kGap, 90, 90, 90);

        for (int i = 0; i < 9; i++) {
            if (ttt_board[i] == ' ') continue;
            const int row = i / 3, col = i % 3;
            const int16_t cx = static_cast<int16_t>(kBoardX + col * (kCell + kGap));
            const int16_t cy = static_cast<int16_t>(kBoardY + row * (kCell + kGap));
            const bool on_win_line = i == ttt_win_a || i == ttt_win_b || i == ttt_win_c;
            uint8_t r, g, b;
            if (on_win_line) {
                r = 255;
                g = 255;
                b = 0;
            } else if (ttt_board[i] == 'X') {
                r = 255;
                g = 0;
                b = 0;
            } else {
                r = 0;
                g = 200;
                b = 255;
            }
            if (ttt_board[i] == 'X') {
                for (int d = 0; d < kCell; d++) {
                    dma_display->drawPixelRGB888(static_cast<int16_t>(cx + d), static_cast<int16_t>(cy + d), r, g, b);
                    dma_display->drawPixelRGB888(static_cast<int16_t>(cx + kCell - 1 - d), static_cast<int16_t>(cy + d),
                                                  r, g, b);
                }
            } else {
                const float ocx = cx + (kCell - 1) / 2.0f;
                const float ocy = cy + (kCell - 1) / 2.0f;
                const float orad = kCell / 2.0f - 1.0f;
                for (int dy = 0; dy < kCell; dy++) {
                    for (int dx = 0; dx < kCell; dx++) {
                        const float ddx = (cx + dx) - ocx;
                        const float ddy = (cy + dy) - ocy;
                        const float dist = sqrtf(ddx * ddx + ddy * ddy);
                        if (fabsf(dist - orad) < 0.9f) {
                            dma_display->drawPixelRGB888(static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy + dy),
                                                          r, g, b);
                        }
                    }
                }
            }
        }
    };

    // hub75/maze1 (3px paths) and hub75/maze2 (1px paths): a random perfect
    // maze (walls 1px thick, a random color re-rolled each maze, never red)
    // solved live by a single red pixel doing
    // depth-first search with backtracking - no breadcrumbs, just the red
    // pixel, which moves continuously through every pixel of the corridor
    // (never jumping straight from one cell center to the next) so its path
    // is always visible as it moves. Repeats with a fresh maze once solved.
    // Both topics share this same generation/solving code - only the path
    // width (and therefore the grid size) differs, configured at runtime by
    // configure_maze() below rather than duplicating the logic.
    constexpr int kMazeMaxCols = (PANEL_WIDTH - 1) / 2;  // grid size at the narrowest (1px path) config
    constexpr int kMazeMaxRows = (PANEL_HEIGHT - 1) / 2;
    int maze_cols = 0, maze_rows = 0;
    int16_t maze_cell_size = 0, maze_wall_size = 1, maze_cell_pitch = 0;
    int16_t maze_off_x = 0, maze_off_y = 0;
    bool maze_variant_narrow = false;  // false = maze1 (3px paths), true = maze2/maze3 (1px paths)
    bool maze_long_dot = false;        // hub75/maze3 only: solver "dot" stretched to 4px long
    // hub75/maze5 only: walls are randomized within a blue/cyan/teal band
    // instead of the fully random palette the other mazes use.
    bool maze_blueish = false;
    uint8_t maze_wall_r = 0, maze_wall_g = 255, maze_wall_b = 0;  // re-rolled each new maze; never red
    static bool maze_wall_right[kMazeMaxCols][kMazeMaxRows];
    static bool maze_wall_down[kMazeMaxCols][kMazeMaxRows];
    static bool maze_solve_visited[kMazeMaxCols][kMazeMaxRows];
    // uint8_t is enough (cols <= 63, rows <= 31) and halves these vs int16_t -
    // they're `static` (.bss), which comes straight out of the heap's DRAM.
    static uint8_t maze_stack_c[kMazeMaxCols * kMazeMaxRows];
    static uint8_t maze_stack_r[kMazeMaxCols * kMazeMaxRows];
    int maze_sp = 0;
    bool maze_solved = false;
    int maze_hold_tick = 0;
    bool maze_primed = false;  // true once a maze/solver is ready to go
    // Visual (pixel) position of the red pixel, separate from the logical
    // DFS stack above - moves toward maze_target_* a few px/frame so it's
    // always seen sliding through the corridor, never teleporting.
    int16_t maze_pix_x = 0, maze_pix_y = 0;
    // Pixels the solver dot covered last frame (1, or 4 for maze3's long dot).
    // Maze modes don't clearScreen() every frame (that made them flicker with
    // single buffering) - walls are simply redrawn over identical values and
    // only these dot pixels get erased, see erase_maze_dot().
    int16_t maze_dot_prev_x[4] = {0, 0, 0, 0}, maze_dot_prev_y[4] = {0, 0, 0, 0};
    int maze_dot_prev_n = 0;
    int16_t maze_target_x = 0, maze_target_y = 0;
    // hub75/maze3 only: current direction of travel (-1/0/+1 per axis,
    // always axis-aligned since corridors only run horizontally or
    // vertically), used to stretch the solver dot into a trailing 4px
    // segment - see maze_long_dot and run_maze_frame().
    int16_t maze_dir_x = 1, maze_dir_y = 0;
    // hub75/maze2 only: once solved, the completed maze fades smoothly to
    // black before the next maze is generated - see
    // run_maze_transition_frame().
    bool maze_transitioning = false;
    int maze_transition_tick = 0;
    // hub75/maze4 and hub75/maze5 only: breadcrumbs - a permanent per-cell
    // trail color, stamped once (see maze_solver_step()) the moment the
    // solver first enters that cell, using whatever maze4_task() last
    // measured for the configured ping target's response time. The
    // conn_right/conn_down arrays additionally mark the 1px-wide corridor
    // pixel(s) connecting two breadcrumbed cells - without these, that
    // shared wall pixel (removed to open the passage) stays unpainted
    // background, showing up as a dark gap breaking up the trail,
    // especially visible on hub75/maze5's 1px-wide paths. Their color is
    // just whichever cell they lead into (maze_trail_r/g/b[c+1][r] for
    // conn_right[c][r], [c][r+1] for conn_down[c][r]) - no separate color
    // storage needed since both ends of a given connector are always
    // stamped together, from the same maze_current_latency_* snapshot.
    // `static` (not a plain stack local) since these are sizeable fixed
    // arrays and app_main()'s single giant stack frame has already needed a
    // larger stack size once before (see CONFIG_ESP_MAIN_TASK_STACK_SIZE in
    // sdkconfig.defaults) after similar maze state was added.
    bool maze_breadcrumbs = false;  // true only while hub75/maze4 or hub75/maze5 is the active mode
    uint8_t maze_current_latency_r = 0, maze_current_latency_g = 255, maze_current_latency_b = 0;
#if HUB75_FULL_EFFECTS
    static bool maze_trail_set[kMazeMaxCols][kMazeMaxRows];
    static uint8_t maze_trail_r[kMazeMaxCols][kMazeMaxRows];
    static uint8_t maze_trail_g[kMazeMaxCols][kMazeMaxRows];
    static uint8_t maze_trail_b[kMazeMaxCols][kMazeMaxRows];
    static bool maze_trail_conn_right_set[kMazeMaxCols][kMazeMaxRows];
    static bool maze_trail_conn_down_set[kMazeMaxCols][kMazeMaxRows];
#endif  // HUB75_FULL_EFFECTS

    // (Re)computes the grid geometry for the given path width and clamps
    // the resulting cols/rows to the fixed-size arrays above.
    auto configure_maze = [&](bool narrow) {
        maze_cell_size = narrow ? 1 : 3;
        maze_wall_size = 1;
        maze_cell_pitch = static_cast<int16_t>(maze_cell_size + maze_wall_size);
        const int cols = (PANEL_WIDTH - 1) / maze_cell_pitch;
        const int rows = (PANEL_HEIGHT - 1) / maze_cell_pitch;
        maze_cols = cols > kMazeMaxCols ? kMazeMaxCols : cols;
        maze_rows = rows > kMazeMaxRows ? kMazeMaxRows : rows;
        maze_off_x = static_cast<int16_t>((PANEL_WIDTH - (maze_cols * maze_cell_pitch + 1)) / 2);
        maze_off_y = static_cast<int16_t>((PANEL_HEIGHT - (maze_rows * maze_cell_pitch + 1)) / 2);
    };
    configure_maze(false);  // start configured for maze1 (3px paths)

    auto maze_cell_cx = [&](int c) {
        return static_cast<int16_t>(maze_off_x + c * maze_cell_pitch + maze_wall_size + maze_cell_size / 2);
    };
    auto maze_cell_cy = [&](int r) {
        return static_cast<int16_t>(maze_off_y + r * maze_cell_pitch + maze_wall_size + maze_cell_size / 2);
    };

    // Picks a fresh random wall color, re-rolling if it would read as red
    // (which is reserved for the solver pixel). hub75/maze5 instead stays
    // within a blue -> cyan -> blue-green/teal band (see maze_blueish) -
    // never warm - so it never needs the red-reroll check.
    auto pick_maze_wall_color = [&]() {
        if (maze_blueish) {
            maze_wall_r = static_cast<uint8_t>(esp_random() % 50);         // 0..49: never warm/red
            maze_wall_g = static_cast<uint8_t>(90 + esp_random() % 150);   // 90..239: teal/green-cyan..cyan
            maze_wall_b = static_cast<uint8_t>(170 + esp_random() % 86);   // 170..255: always strongly blue
            return;
        }
        do {
            maze_wall_r = static_cast<uint8_t>(esp_random() % 256);
            maze_wall_g = static_cast<uint8_t>(esp_random() % 256);
            maze_wall_b = static_cast<uint8_t>(esp_random() % 256);
        } while (maze_wall_r > 160 && maze_wall_g < 110 && maze_wall_b < 110);
    };

    // Randomized DFS (recursive backtracker) over the cell-adjacency graph -
    // carves a spanning tree, i.e. a "perfect maze" (exactly one path
    // between any two cells, no loops).
    auto generate_maze = [&]() {
        // New maze: the old walls are different, so this is the one time a
        // maze mode needs a full clear (see maze_dot_prev_* above).
        dma_display->clearScreen();
        maze_dot_prev_n = 0;
        pick_maze_wall_color();
        for (int c = 0; c < maze_cols; c++) {
            for (int r = 0; r < maze_rows; r++) {
                maze_wall_right[c][r] = true;
                maze_wall_down[c][r] = true;
            }
        }
        // Reuse the solver's arrays as generation scratch: every
        // generate_maze() call is immediately followed by
        // reset_maze_solver(), which re-initializes them, so nothing is lost
        // and we save ~10KB of .bss (heap) on the 128x64 panel.
        auto &visited = maze_solve_visited;
        for (int c = 0; c < maze_cols; c++)
            for (int r = 0; r < maze_rows; r++) visited[c][r] = false;
        auto &stack_c = maze_stack_c;
        auto &stack_r = maze_stack_r;
        int sp = 0;
        visited[0][0] = true;
        stack_c[sp] = 0;
        stack_r[sp] = 0;
        sp++;
        while (sp > 0) {
            const int c = stack_c[sp - 1], r = stack_r[sp - 1];
            int nb_c[4], nb_r[4];
            int n = 0;
            if (r > 0 && !visited[c][r - 1]) {
                nb_c[n] = c;
                nb_r[n] = r - 1;
                n++;
            }
            if (r < maze_rows - 1 && !visited[c][r + 1]) {
                nb_c[n] = c;
                nb_r[n] = r + 1;
                n++;
            }
            if (c < maze_cols - 1 && !visited[c + 1][r]) {
                nb_c[n] = c + 1;
                nb_r[n] = r;
                n++;
            }
            if (c > 0 && !visited[c - 1][r]) {
                nb_c[n] = c - 1;
                nb_r[n] = r;
                n++;
            }
            if (n == 0) {
                sp--;
                continue;
            }
            const int pick = static_cast<int>(esp_random() % n);
            const int nc = nb_c[pick], nr = nb_r[pick];
            if (nc == c + 1) maze_wall_right[c][r] = false;
            else if (nc == c - 1) maze_wall_right[nc][r] = false;
            else if (nr == r + 1) maze_wall_down[c][r] = false;
            else if (nr == r - 1) maze_wall_down[c][nr] = false;
            visited[nc][nr] = true;
            stack_c[sp] = nc;
            stack_r[sp] = nr;
            sp++;
        }
    };

    auto reset_maze_solver = [&]() {
        for (int c = 0; c < maze_cols; c++)
            for (int r = 0; r < maze_rows; r++) maze_solve_visited[c][r] = false;
        maze_solve_visited[0][0] = true;
        maze_stack_c[0] = 0;
        maze_stack_r[0] = 0;
        maze_sp = 1;
        maze_solved = false;
        maze_hold_tick = 0;
        maze_transitioning = false;  // any full reset cancels an in-flight transition
        maze_pix_x = maze_target_x = maze_cell_cx(0);
        maze_pix_y = maze_target_y = maze_cell_cy(0);
        maze_dir_x = 1;
        maze_dir_y = 0;
        // hub75/maze4/maze5: a fresh maze means a fresh (empty) breadcrumb
        // trail - harmless to clear even when this isn't the active mode.
#if HUB75_FULL_EFFECTS
        for (int c = 0; c < maze_cols; c++)
            for (int r = 0; r < maze_rows; r++) {
                maze_trail_set[c][r] = false;
                maze_trail_conn_right_set[c][r] = false;
                maze_trail_conn_down_set[c][r] = false;
            }
        if (maze_breadcrumbs) {
            maze_trail_set[0][0] = true;
            maze_trail_r[0][0] = maze_current_latency_r;
            maze_trail_g[0][0] = maze_current_latency_g;
            maze_trail_b[0][0] = maze_current_latency_b;
        }
#endif  // HUB75_FULL_EFFECTS
    };

    // One DFS step: advance to a random unvisited neighbor reachable through
    // an open passage, or backtrack (pop the stack) if the current cell has
    // none left - "trying every possible path" until the exit is reached.
    auto maze_solver_step = [&]() {
        if (maze_sp == 0) return;
        const int c = maze_stack_c[maze_sp - 1], r = maze_stack_r[maze_sp - 1];
        if (c == maze_cols - 1 && r == maze_rows - 1) {
            maze_solved = true;
            return;
        }
        int nb_c[4], nb_r[4];
        int n = 0;
        if (r > 0 && !maze_wall_down[c][r - 1] && !maze_solve_visited[c][r - 1]) {
            nb_c[n] = c;
            nb_r[n] = r - 1;
            n++;
        }
        if (r < maze_rows - 1 && !maze_wall_down[c][r] && !maze_solve_visited[c][r + 1]) {
            nb_c[n] = c;
            nb_r[n] = r + 1;
            n++;
        }
        if (c < maze_cols - 1 && !maze_wall_right[c][r] && !maze_solve_visited[c + 1][r]) {
            nb_c[n] = c + 1;
            nb_r[n] = r;
            n++;
        }
        if (c > 0 && !maze_wall_right[c - 1][r] && !maze_solve_visited[c - 1][r]) {
            nb_c[n] = c - 1;
            nb_r[n] = r;
            n++;
        }
        if (n == 0) {
            maze_sp--;
            return;
        }
        const int pick = static_cast<int>(esp_random() % n);
        const int nc = nb_c[pick], nr = nb_r[pick];
        maze_solve_visited[nc][nr] = true;
        maze_stack_c[maze_sp] = nc;
        maze_stack_r[maze_sp] = nr;
        maze_sp++;
#if HUB75_FULL_EFFECTS
        if (maze_breadcrumbs) {
            // Stamped once, at the moment this cell is first entered - a
            // permanent record of whatever the most recent ping (see
            // maze4_task()) measured right then, not updated again even if
            // the solver later backtracks through here.
            maze_trail_set[nc][nr] = true;
            maze_trail_r[nc][nr] = maze_current_latency_r;
            maze_trail_g[nc][nr] = maze_current_latency_g;
            maze_trail_b[nc][nr] = maze_current_latency_b;
            // Also mark the single-pixel corridor connecting (c,r) to
            // (nc,nr) - otherwise that pixel (the wall that was removed to
            // open this passage) stays unpainted background, showing as a
            // dark gap between two breadcrumbed cells.
            if (nc == c && nr == r - 1) {
                maze_trail_conn_down_set[c][nr] = true;  // moved up: connector belongs to the cell above
            } else if (nc == c && nr == r + 1) {
                maze_trail_conn_down_set[c][r] = true;  // moved down
            } else if (nc == c + 1 && nr == r) {
                maze_trail_conn_right_set[c][r] = true;  // moved right
            } else if (nc == c - 1 && nr == r) {
                maze_trail_conn_right_set[nc][r] = true;  // moved left: connector belongs to the cell on the left
            }
        }
#endif  // HUB75_FULL_EFFECTS
    };

    // Wall pillars (every grid crossing) are always drawn - a maze corner is
    // structurally always a wall regardless of which straight segments are
    // open - plus the conditional straight segments themselves. brightness
    // scales the wall color toward black (used by the hub75/maze2
    // fade-to-black transition; 1.0 = full brightness, the normal case).
    auto draw_maze_walls = [&](float brightness = 1.0f) {
        const uint8_t wr = static_cast<uint8_t>(maze_wall_r * brightness);
        const uint8_t wg = static_cast<uint8_t>(maze_wall_g * brightness);
        const uint8_t wb = static_cast<uint8_t>(maze_wall_b * brightness);
        for (int cb = 0; cb <= maze_cols; cb++) {
            for (int r = 0; r < maze_rows; r++) {
                const bool present = (cb == 0 || cb == maze_cols) ? true : maze_wall_right[cb - 1][r];
                if (!present) continue;
                const int16_t x = static_cast<int16_t>(maze_off_x + cb * maze_cell_pitch);
                const int16_t y = static_cast<int16_t>(maze_off_y + r * maze_cell_pitch + 1);
                dma_display->fillRect(x, y, 1, maze_cell_size, wr, wg, wb);
            }
        }
        for (int rb = 0; rb <= maze_rows; rb++) {
            for (int c = 0; c < maze_cols; c++) {
                const bool present = (rb == 0 || rb == maze_rows) ? true : maze_wall_down[c][rb - 1];
                if (!present) continue;
                const int16_t x = static_cast<int16_t>(maze_off_x + c * maze_cell_pitch + 1);
                const int16_t y = static_cast<int16_t>(maze_off_y + rb * maze_cell_pitch);
                dma_display->fillRect(x, y, maze_cell_size, 1, wr, wg, wb);
            }
        }
        for (int cb = 0; cb <= maze_cols; cb++) {
            for (int rb = 0; rb <= maze_rows; rb++) {
                dma_display->drawPixelRGB888(static_cast<int16_t>(maze_off_x + cb * maze_cell_pitch),
                                              static_cast<int16_t>(maze_off_y + rb * maze_cell_pitch), wr, wg, wb);
            }
        }
    };

    // hub75/maze2 only: once the maze is solved, it fades gently to black
    // over kMazeFadeTicks frames, then the next maze is generated and fades
    // just as gently back in from black over another kMazeFadeTicks frames
    // before its solve begins. Eased with smoothstep (zero velocity at both
    // ends of each fade) rather than a linear ramp, and the solver pixel
    // fades in/out right along with the walls, so nothing ever snaps
    // suddenly to full brightness.
    auto erase_maze_dot = [&]() {
        for (int i = 0; i < maze_dot_prev_n; i++) dma_display->drawPixelRGB888(maze_dot_prev_x[i], maze_dot_prev_y[i], 0, 0, 0);
        maze_dot_prev_n = 0;
    };

    auto run_maze_transition_frame = [&]() {
        erase_maze_dot();
        constexpr int kMazeFadeTicks = 40;  // ~1.2s each way - slow and gentle

        const auto smoothstep = [](float t) { return t * t * (3.0f - 2.0f * t); };

        if (maze_transition_tick < kMazeFadeTicks) {
            // Fading the just-solved maze (walls + the resting solver
            // pixel) out to black.
            const float t = static_cast<float>(maze_transition_tick + 1) / kMazeFadeTicks;
            const float brightness = 1.0f - smoothstep(t);
            draw_maze_walls(brightness);
            dma_display->drawPixelRGB888(maze_pix_x, maze_pix_y, static_cast<uint8_t>(255 * brightness), 0, 0);
            maze_dot_prev_x[0] = maze_pix_x;
            maze_dot_prev_y[0] = maze_pix_y;
            maze_dot_prev_n = 1;
            if (++maze_transition_tick == kMazeFadeTicks) {
                generate_maze();
                reset_maze_solver();
            }
            return;
        }

        // Fading the freshly generated maze (walls + the solver pixel at
        // its starting cell) in from black.
        const int fade_in_tick = maze_transition_tick - kMazeFadeTicks;
        const float t = static_cast<float>(fade_in_tick + 1) / kMazeFadeTicks;
        const float brightness = smoothstep(t);
        draw_maze_walls(brightness);
        dma_display->drawPixelRGB888(maze_pix_x, maze_pix_y, static_cast<uint8_t>(255 * brightness), 0, 0);
        maze_dot_prev_x[0] = maze_pix_x;
        maze_dot_prev_y[0] = maze_pix_y;
        maze_dot_prev_n = 1;

        if (++maze_transition_tick >= kMazeFadeTicks * 2) {
            maze_transitioning = false;
        }
    };

    auto run_maze_frame = [&]() {
        constexpr int16_t kMazeMoveSpeed = 2;  // px/frame - 2x the old per-step pace, and continuous
        constexpr int kMazeHoldTicks = 60;     // ~1.8s pause once solved

        if (maze_transitioning) {
            run_maze_transition_frame();
            return;
        }

        if (!maze_solved) {
            if (maze_pix_x == maze_target_x && maze_pix_y == maze_target_y) {
                maze_solver_step();
                if (!maze_solved && maze_sp > 0) {
                    maze_target_x = maze_cell_cx(maze_stack_c[maze_sp - 1]);
                    maze_target_y = maze_cell_cy(maze_stack_r[maze_sp - 1]);
                }
            } else {
                if (maze_pix_x < maze_target_x) {
                    maze_dir_x = 1;
                    maze_dir_y = 0;
                    maze_pix_x = static_cast<int16_t>(maze_pix_x + kMazeMoveSpeed);
                    if (maze_pix_x > maze_target_x) maze_pix_x = maze_target_x;
                } else if (maze_pix_x > maze_target_x) {
                    maze_dir_x = -1;
                    maze_dir_y = 0;
                    maze_pix_x = static_cast<int16_t>(maze_pix_x - kMazeMoveSpeed);
                    if (maze_pix_x < maze_target_x) maze_pix_x = maze_target_x;
                }
                if (maze_pix_y < maze_target_y) {
                    maze_dir_x = 0;
                    maze_dir_y = 1;
                    maze_pix_y = static_cast<int16_t>(maze_pix_y + kMazeMoveSpeed);
                    if (maze_pix_y > maze_target_y) maze_pix_y = maze_target_y;
                } else if (maze_pix_y > maze_target_y) {
                    maze_dir_x = 0;
                    maze_dir_y = -1;
                    maze_pix_y = static_cast<int16_t>(maze_pix_y - kMazeMoveSpeed);
                    if (maze_pix_y < maze_target_y) maze_pix_y = maze_target_y;
                }
            }
        } else {
            if (++maze_hold_tick >= kMazeHoldTicks) {
                if (maze_variant_narrow) {
                    // hub75/maze2 only, per the user's request: fade the
                    // completed maze to black before the next one is
                    // generated (see run_maze_transition_frame()).
                    maze_transition_tick = 0;
                    maze_transitioning = true;
                    return;
                }
                generate_maze();
                reset_maze_solver();
            }
        }

        erase_maze_dot();
        draw_maze_walls();
#if HUB75_FULL_EFFECTS
        if (maze_breadcrumbs) {
            // hub75/maze4/maze5: every previously-visited cell stays
            // permanently filled with the latency color it was stamped
            // with (see maze_solver_step()) - a literal trail of
            // breadcrumbs through the maze, colored by recent ping
            // history. The conn_right/conn_down passes fill in the
            // connecting corridor pixel(s) between two breadcrumbed cells
            // (the wall pixel removed to open that passage), so the trail
            // reads as continuous with no dark gaps - most visible on
            // hub75/maze5's 1px-wide paths, where a single missed pixel
            // would otherwise break the trail entirely.
            for (int c = 0; c < maze_cols; c++) {
                for (int r = 0; r < maze_rows; r++) {
                    if (!maze_trail_set[c][r]) continue;
                    const int16_t x = static_cast<int16_t>(maze_off_x + c * maze_cell_pitch + maze_wall_size);
                    const int16_t y = static_cast<int16_t>(maze_off_y + r * maze_cell_pitch + maze_wall_size);
                    dma_display->fillRect(x, y, maze_cell_size, maze_cell_size, maze_trail_r[c][r], maze_trail_g[c][r],
                                           maze_trail_b[c][r]);
                }
            }
            for (int c = 0; c < maze_cols; c++) {
                for (int r = 0; r < maze_rows; r++) {
                    if (maze_trail_conn_right_set[c][r]) {
                        const int16_t x = static_cast<int16_t>(maze_off_x + (c + 1) * maze_cell_pitch);
                        const int16_t y = static_cast<int16_t>(maze_off_y + r * maze_cell_pitch + maze_wall_size);
                        dma_display->fillRect(x, y, maze_wall_size, maze_cell_size, maze_trail_r[c + 1][r],
                                               maze_trail_g[c + 1][r], maze_trail_b[c + 1][r]);
                    }
                    if (maze_trail_conn_down_set[c][r]) {
                        const int16_t x = static_cast<int16_t>(maze_off_x + c * maze_cell_pitch + maze_wall_size);
                        const int16_t y = static_cast<int16_t>(maze_off_y + (r + 1) * maze_cell_pitch);
                        dma_display->fillRect(x, y, maze_cell_size, maze_wall_size, maze_trail_r[c][r + 1],
                                               maze_trail_g[c][r + 1], maze_trail_b[c][r + 1]);
                    }
                }
            }
        }
#endif  // HUB75_FULL_EFFECTS
        if (maze_long_dot) {
            // hub75/maze3 only: stretch the dot into a 4px segment
            // trailing behind the head along the current direction of
            // travel, so it still fits cleanly inside the 1px-wide
            // corridor.
            for (int16_t i = 0; i < 4; i++) {
                const int16_t dx = static_cast<int16_t>(maze_pix_x - maze_dir_x * i);
                const int16_t dy = static_cast<int16_t>(maze_pix_y - maze_dir_y * i);
                dma_display->drawPixelRGB888(dx, dy, 255, 0, 0);
                maze_dot_prev_x[i] = dx;
                maze_dot_prev_y[i] = dy;
            }
            maze_dot_prev_n = 4;
        } else {
            dma_display->drawPixelRGB888(maze_pix_x, maze_pix_y, 255, 0, 0);
            maze_dot_prev_x[0] = maze_pix_x;
            maze_dot_prev_y[0] = maze_pix_y;
            maze_dot_prev_n = 1;
        }
    };

    // hub75/ping2: comet flight/impact animation, entirely advanced here
    // one frame at a time - ping2_task() (a separate background task) only
    // ever sets the ping2_launch_* trigger above; everything about how it
    // actually looks lives in this local state and run_ping2_frame() below.
    // phase: 0=idle, 1=flying, 2=exploding (or fizzling, if the launch that
    // started this flight was a timeout).
    constexpr float kPing2StartX = 2.0f;
    constexpr float kPing2TargetX = 58.0f;                                    // "impact" point
    constexpr float kPing2FizzleX = kPing2StartX + (kPing2TargetX - kPing2StartX) * 0.6f;  // timeouts die en route
    constexpr int16_t kPing2Y = 12;  // vertically centered in the area above the numeric readout
    constexpr int kPing2TailLen = 6;
    constexpr int kPing2FxTicks = 10;  // explosion/fizzle duration, in frames
    int ping2_phase = 0;
    float ping2_x = kPing2StartX;
    int ping2_anim_frame = 0;
    int ping2_anim_total_frames = 6;
    bool ping2_anim_timed_out = false;
    uint8_t ping2_anim_r = 0, ping2_anim_g = 255, ping2_anim_b = 0;
    int ping2_fx_tick = 0;
    float ping2_tail_x[kPing2TailLen];
    for (int i = 0; i < kPing2TailLen; i++) ping2_tail_x[i] = kPing2StartX;

    // launch_pending/timed_out/r/g/b/frames are this frame's freshly
    // snapshotted trigger (already consumed/cleared from the shared
    // ping2_launch_* globals by the time this runs) - see the mutex block
    // below where local_ping2_* is filled in.
    auto run_ping2_frame = [&](bool launch_pending, bool timed_out, uint8_t r, uint8_t g, uint8_t b, int frames,
                                double rtt_ms) {
        if (launch_pending) {
            // A fresh launch always takes over immediately, interrupting
            // whatever the previous one was doing - constant motion is the
            // whole point here.
            ping2_phase = 1;
            ping2_anim_frame = 0;
            ping2_anim_total_frames = frames > 0 ? frames : 1;
            ping2_anim_timed_out = timed_out;
            ping2_anim_r = r;
            ping2_anim_g = g;
            ping2_anim_b = b;
            ping2_x = kPing2StartX;
            for (int i = 0; i < kPing2TailLen; i++) ping2_tail_x[i] = kPing2StartX;
            ping2_fx_tick = 0;
        }

        if (ping2_phase == 1) {
            ping2_anim_frame++;
            const float t = static_cast<float>(ping2_anim_frame) / static_cast<float>(ping2_anim_total_frames);
            const float dest = ping2_anim_timed_out ? kPing2FizzleX : kPing2TargetX;
            ping2_x = kPing2StartX + (dest - kPing2StartX) * (t < 1.0f ? t : 1.0f);
            for (int i = kPing2TailLen - 1; i > 0; i--) ping2_tail_x[i] = ping2_tail_x[i - 1];
            ping2_tail_x[0] = ping2_x;
            if (ping2_anim_frame >= ping2_anim_total_frames) {
                ping2_phase = 2;
                ping2_fx_tick = 0;
            }
        }

        // Fading tail, dimmest-oldest to brightest-newest.
        for (int i = kPing2TailLen - 1; i >= 1; i--) {
            const uint8_t fade = static_cast<uint8_t>(255 * (kPing2TailLen - i) / kPing2TailLen);
            dma_display->drawPixelRGB888(static_cast<int16_t>(ping2_tail_x[i]), kPing2Y,
                                          static_cast<uint8_t>(ping2_anim_r * fade / 255),
                                          static_cast<uint8_t>(ping2_anim_g * fade / 255),
                                          static_cast<uint8_t>(ping2_anim_b * fade / 255));
        }

        if (ping2_phase == 1) {
            // Bright white-hot head with a colored halo above/below it.
            dma_display->drawPixelRGB888(static_cast<int16_t>(ping2_x), kPing2Y, 255, 255, 255);
            dma_display->drawPixelRGB888(static_cast<int16_t>(ping2_x), static_cast<int16_t>(kPing2Y - 1),
                                          ping2_anim_r, ping2_anim_g, ping2_anim_b);
            dma_display->drawPixelRGB888(static_cast<int16_t>(ping2_x), static_cast<int16_t>(kPing2Y + 1),
                                          ping2_anim_r, ping2_anim_g, ping2_anim_b);
        } else if (ping2_phase == 2) {
            ping2_fx_tick++;
            if (ping2_anim_timed_out) {
                // Fizzle: a few dim, slowly spreading sparks - "the packet
                // never arrived" reads as visibly weaker than a real
                // impact, not just a smaller version of it.
                const float radius = 1.0f + static_cast<float>(ping2_fx_tick) * 0.3f;
                for (int i = 0; i < 5; i++) {
                    const float angle = static_cast<float>(i) * (2.0f * 3.14159265f / 5.0f) +
                                         static_cast<float>(ping2_fx_tick) * 0.2f;
                    dma_display->drawPixelRGB888(
                        static_cast<int16_t>(ping2_x + cosf(angle) * radius),
                        static_cast<int16_t>(static_cast<float>(kPing2Y) + sinf(angle) * radius * 0.6f), 90, 15, 15);
                }
            } else {
                // Impact: a bright flash for the first half, then an
                // expanding ring of colored sparks - same technique as
                // hub75/shootingstars' collision explosions.
                const float radius = 1.0f + static_cast<float>(ping2_fx_tick) * 0.8f;
                if (ping2_fx_tick < kPing2FxTicks / 2) {
                    dma_display->fillRect(static_cast<int16_t>(ping2_x - 1), static_cast<int16_t>(kPing2Y - 1), 3, 3,
                                           255, 255, 255);
                }
                for (int i = 0; i < 8; i++) {
                    const float angle = static_cast<float>(i) * (2.0f * 3.14159265f / 8.0f);
                    dma_display->drawPixelRGB888(static_cast<int16_t>(ping2_x + cosf(angle) * radius),
                                                  static_cast<int16_t>(static_cast<float>(kPing2Y) + sinf(angle) * radius),
                                                  ping2_anim_r, ping2_anim_g, ping2_anim_b);
                }
            }
            if (ping2_fx_tick >= kPing2FxTicks) {
                ping2_phase = 0;
            }
        }

        // Numeric ms readout, same spot/style as hub75/ping1 - always the
        // true measured delay.
        char ping2_ms_text[16];
        snprintf(ping2_ms_text, sizeof(ping2_ms_text), "%.1f", rtt_ms);
        const int16_t ping2_ms_x = static_cast<int16_t>((PANEL_WIDTH - small_text_width(ping2_ms_text)) / 2);
        draw_text_small(ping2_ms_x, static_cast<int16_t>(PANEL_HEIGHT - kSmallFontHeight), ping2_ms_text, 255, 255,
                         255);
    };

    uint8_t applied_brightness = kDimBrightness;  // matches the initial dma_display->setBrightness8() above

    // hub75/flash: blink whatever the current mode draws, 3s on / 2s off.
    constexpr int kBlinkOnTicks = 100;   // 100 * 30ms = 3.0s
    constexpr int kBlinkOffTicks = 67;   // 67 * 30ms ~= 2.0s
    constexpr int kBlinkPeriodTicks = kBlinkOnTicks + kBlinkOffTicks;
    int blink_tick = 0;
    // Static screens (solid colours, hub75/1300*, off) don't change frame to
    // frame; with single buffering, clearing + redrawing them every tick makes
    // the panel flicker, so they're only redrawn when this key changes.
    uint32_t last_static_key = 0xFFFFFFFFu;
    DisplayMode prev_frame_mode = DisplayMode::kWifiStatus;

    // Boot splash: up to 4 lines of text from CONFIG_SPLASH_LINE1..4 (menuconfig
    // "HD-WF1 Boot Splash"; the real values live in the gitignored sdkconfig),
    // yellow on black in the 1px-stroke small font, shown for kSplashMs from
    // the first frame as an OVERLAY - display_mode, the WiFi status lines and
    // MQTT handling all keep running underneath, so WiFi comes up
    // concurrently and the "next screen" is simply whatever is current when
    // the splash ends. A single green pixel in the lower right corner lights
    // while WiFi has an IP, and vanishes with the splash. Empty lines are
    // skipped; if all are empty there is no splash.
    constexpr int64_t kSplashMs = 10000;
    const char *splash_lines[4];
    int kSplashLineCount = 0;
    for (const char *line : {CONFIG_SPLASH_LINE1, CONFIG_SPLASH_LINE2, CONFIG_SPLASH_LINE3, CONFIG_SPLASH_LINE4}) {
        if (line[0] != '\0') splash_lines[kSplashLineCount++] = line;
    }
    constexpr int16_t kSplashLinePitch = 9;  // 5px glyph + 4px gap
    const int64_t splash_end_us = esp_timer_get_time() + kSplashMs * 1000;
    bool splash_over = (kSplashLineCount == 0);
    int splash_drawn_state = -1;  // -1 nothing drawn yet, 0 text only, 1 text + green pixel

    // hub75/0: cycles through every pattern above (plus solid colors,
    // rainbow, flags, and a message) in sequence, forever, until hub75/1
    // stops it. Reuses the exact same per-frame lambdas/functions as the
    // standalone modes - only the phase-advance and one-time-per-phase setup
    // (e.g. regenerating rainbow colors) lives here.
    enum class AutoPhase { kRed, kGreen, kBlue, kYellow, kOrange, kRainbow, kBars, kPacman, kPixel, kFlagUsa,
                            kFlagCanada, kMessage, kCount };
    constexpr int kAutoPhaseDurationTicks[static_cast<int>(AutoPhase::kCount)] = {
        67, 67, 67, 67, 67,  // solids, ~2s each
        100,                 // rainbow, ~3s
        167,                 // bars, ~5s
        267,                 // pacman, ~8s
        167,                 // pixel, ~5s
        100, 100,            // flags, ~3s each
        167,                 // message, ~5s
    };
    constexpr const char *kAutoTestMessage = "HD-WF1 AUTO TEST";
    int auto_phase = 0;
    int auto_phase_tick = 0;
    bool auto_phase_started = true;  // triggers each phase's one-time setup below
    uint8_t auto_rainbow_r[PANEL_HEIGHT], auto_rainbow_g[PANEL_HEIGHT], auto_rainbow_b[PANEL_HEIGHT];
    int16_t auto_msg_x = PANEL_WIDTH;
    int16_t auto_msg_text_px = 0;

    while (true) {
        xSemaphoreTake(display_mutex, portMAX_DELAY);
        const DisplayMode mode = display_mode;
        const uint8_t wanted_brightness = target_brightness;
        const bool wanted_blink = blink_enabled;
        const bool local_ntp_synced = ntp_synced;
        const ClockTz local_clock_tz = clock_tz;
        const uint8_t local_clock_r = clock_color_r, local_clock_g = clock_color_g, local_clock_b = clock_color_b;
        if (mode != DisplayMode::kClockConfirm) clock_confirm_tick = 0;
        if (mode != DisplayMode::kStandardTest1) {
            std1_phase = 0;
            std1_phase_tick = 0;
        }
        if (mode != DisplayMode::kSpaceInvaders) {
            for (int r = 0; r < kSiRows; r++)
                for (int c = 0; c < kSiCols; c++) si_alive[r][c] = true;
            si_formation_x = 0;
            si_formation_y = 0;
            si_formation_dir = 1;
            si_move_tick = 0;
            si_player_x = PANEL_WIDTH / 2;
            si_bullet_active = false;
            si_fire_cooldown = 0;
        }
        if (mode != DisplayMode::kMillipede) {
            mi_head_x = 4;
            mi_head_y = 2;
            mi_dir_x = 1;
            mi_move_tick = 0;
            for (int i = 0; i < kMiHistoryLen; i++) {
                mi_history_x[i] = mi_head_x;
                mi_history_y[i] = mi_head_y;
            }
            for (int i = 0; i < kMiSegments; i++) mi_alive[i] = true;
            for (int i = 0; i < kMiMushroomCount; i++) {
                mi_mushroom_x[i] = static_cast<int16_t>(2 + esp_random() % (PANEL_WIDTH - 4));
                mi_mushroom_y[i] = static_cast<int16_t>(6 + esp_random() % 18);
            }
            mi_player_x = PANEL_WIDTH / 2;
            mi_bullet_active = false;
            mi_fire_cooldown = 0;
        }
        if (mode != DisplayMode::kPong) {
            pong_left_score = 0;
            pong_right_score = 0;
            pong_left_paddle_y = PANEL_HEIGHT * 0.6f;
            pong_right_paddle_y = PANEL_HEIGHT * 0.6f;
            pong_ball_x = PANEL_WIDTH / 2.0f;
            pong_ball_y = PANEL_HEIGHT * 0.6f;
            pong_ball_vx = 0.6f;
            pong_ball_vy = 0.4f;
        }
        if (mode != DisplayMode::kTicTacToe) {
            for (int i = 0; i < 9; i++) ttt_board[i] = ' ';
            ttt_turn = 'X';
            ttt_first_player = 'X';
            ttt_winner = ' ';
            ttt_win_a = ttt_win_b = ttt_win_c = -1;
            ttt_move_tick = 0;
            ttt_result_tick = 0;
        }
        if (mode != DisplayMode::kMatrix1Cont && mode != DisplayMode::kMatrix2Cont &&
            mode != DisplayMode::kMatrix3Cont) {
            matrix_cont_tick = 0;
        }
        if (mode != DisplayMode::kMatrixRain) {
            for (int c = 0; c < kMatrixRainCols; c++) reset_matrix_rain_column(c, true);
        }
        if (mode != DisplayMode::kBall1) {
            ball1_x = (PANEL_WIDTH - 8) / 2.0f;
            ball1_y = (PANEL_HEIGHT - 8) / 2.0f;
            ball1_vx = (esp_random() % 2) ? 0.8f : -0.8f;
            ball1_vy = (esp_random() % 2) ? 0.6f : -0.6f;
            random_color(&ball1_r, &ball1_g, &ball1_b, 80, 176);
        }
        if (mode != DisplayMode::kShootingStars) {
            for (int s = 0; s < kStarCount; s++) {
                star_x[s] = static_cast<float>(2 + esp_random() % (PANEL_WIDTH - 4));
                star_y[s] = static_cast<float>(2 + esp_random() % (PANEL_HEIGHT - 4));
                star_vx[s] = ((esp_random() % 2) ? 1.0f : -1.0f) * (0.6f + static_cast<float>(esp_random() % 50) / 100.0f);
                star_vy[s] = ((esp_random() % 2) ? 1.0f : -1.0f) * (0.5f + static_cast<float>(esp_random() % 50) / 100.0f);
            }
            for (int i = 0; i < kStarExplosionSlots; i++) star_explosions[i].active = false;
            for (int p = 0; p < 3; p++) star_pair_touching[p] = false;
        }
        if (mode == DisplayMode::kMaze1 || mode == DisplayMode::kMaze2 || mode == DisplayMode::kMaze3 ||
            mode == DisplayMode::kMaze4 || mode == DisplayMode::kMaze5) {
            const bool want_narrow = (mode == DisplayMode::kMaze2 || mode == DisplayMode::kMaze3 ||
                                       mode == DisplayMode::kMaze5);
            maze_long_dot = (mode == DisplayMode::kMaze3);
            maze_breadcrumbs = (mode == DisplayMode::kMaze4 || mode == DisplayMode::kMaze5);
            maze_blueish = (mode == DisplayMode::kMaze5);
            if (want_narrow != maze_variant_narrow) {
                // switching directly between maze topics (no idle frame in
                // between) - reconfigure geometry for the new path width
                // and carve a matching maze before it's drawn.
                configure_maze(want_narrow);
                generate_maze();
                reset_maze_solver();
                maze_variant_narrow = want_narrow;
            }
            maze_primed = false;  // will need a fresh maze prepped once this mode is left
        } else if (!maze_primed) {
            generate_maze();
            reset_maze_solver();
            maze_primed = true;
        }
        uint16_t local_ping1_graph_ms[kPing1GraphWidth];
        memcpy(local_ping1_graph_ms, ping1_graph_ms, sizeof(local_ping1_graph_ms));
        const double local_ping1_rtt_ms = ping1_last_rtt_ms;
        const uint32_t local_ping1_success_count = ping1_success_count;
        const uint32_t local_ping1_timeout_count = ping1_timeout_count;
        // hub75/maze4: snapshotted straight into the persistent (pre-loop)
        // maze_current_latency_* locals - maze_solver_step() reads those
        // directly (it was defined before this per-tick block exists, so it
        // can't capture a freshly-redeclared local by reference).
        maze_current_latency_r = maze4_latency_r;
        maze_current_latency_g = maze4_latency_g;
        maze_current_latency_b = maze4_latency_b;
        bool local_ping2_launch_pending = false;
        bool local_ping2_timed_out = false;
        uint8_t local_ping2_r = 0, local_ping2_g = 255, local_ping2_b = 0;
        int local_ping2_frames = 6;
        if (ping2_launch_pending) {
            local_ping2_launch_pending = true;
            local_ping2_timed_out = ping2_launch_timed_out;
            local_ping2_r = ping2_launch_r;
            local_ping2_g = ping2_launch_g;
            local_ping2_b = ping2_launch_b;
            local_ping2_frames = ping2_launch_frames;
            ping2_launch_pending = false;  // consumed - one-shot trigger
        }
        const double local_ping2_rtt_ms = ping2_last_rtt_ms;
        uint8_t local_solid_r = solid_r, local_solid_g = solid_g, local_solid_b = solid_b;
        uint8_t local_f1300l_fg[3], local_f1300l_bg[3], local_f1300l_hue[3];
        memcpy(local_f1300l_fg, f1300l_fg, 3);
        memcpy(local_f1300l_bg, f1300l_bg, 3);
        memcpy(local_f1300l_hue, f1300l_hue, 3);
        const uint8_t local_ray_sphere_r = ray_sphere_r, local_ray_sphere_g = ray_sphere_g,
                      local_ray_sphere_b = ray_sphere_b;
        const uint8_t local_ray2_sphere2_r = ray2_sphere2_r, local_ray2_sphere2_g = ray2_sphere2_g,
                      local_ray2_sphere2_b = ray2_sphere2_b;
        const uint8_t local_ray_floor1_r = ray_floor1_r, local_ray_floor1_g = ray_floor1_g,
                      local_ray_floor1_b = ray_floor1_b;
        const uint8_t local_ray_floor2_r = ray_floor2_r, local_ray_floor2_g = ray_floor2_g,
                      local_ray_floor2_b = ray_floor2_b;
        uint8_t local_rainbow_r[PANEL_HEIGHT], local_rainbow_g[PANEL_HEIGHT], local_rainbow_b[PANEL_HEIGHT];
        memcpy(local_rainbow_r, rainbow_row_r, sizeof(local_rainbow_r));
        memcpy(local_rainbow_g, rainbow_row_g, sizeof(local_rainbow_g));
        memcpy(local_rainbow_b, rainbow_row_b, sizeof(local_rainbow_b));
        if (mode == DisplayMode::kMessage) {
            strlcpy(local_text, display_text, kMaxDisplayText);
            local_r = display_r;
            local_g = display_g;
            local_b = display_b;
        } else if (mode == DisplayMode::kWifiStatus) {
            local_status_count = status_line_count;
            for (int i = 0; i < local_status_count; i++) {
                local_status[i] = status_lines[i];
            }
        }
        const bool reset_scroll = display_dirty;
        display_dirty = false;
        xSemaphoreGive(display_mutex);

        if (wanted_brightness != applied_brightness) {
            applied_brightness = wanted_brightness;
            dma_display->setBrightness8(applied_brightness);
        }

        // Onboard button: short press advances hub75/standardtest1 to its
        // next phase immediately (resetting that phase's dwell timer), so it
        // can be driven by hand at the panel instead of waiting out the
        // automatic ~2s/phase timer. No effect in any other mode.
        {
            const bool button_raw = gpio_get_level(static_cast<gpio_num_t>(WF1_BUTTON_PIN)) != 0;  // true = unpressed
            if (button_prev_raw && !button_raw && mode == DisplayMode::kStandardTest1) {
                std1_phase = (std1_phase + 1) % 8;
                std1_phase_tick = 0;
            }
            button_prev_raw = button_raw;
        }

        if (!splash_over) {
            if (esp_timer_get_time() < splash_end_us) {
                const int want = wifi_link_up ? 1 : 0;
                if (want != splash_drawn_state) {
                    splash_drawn_state = want;
                    dma_display->clearScreen();
                    const int16_t block_h = static_cast<int16_t>(kSplashLineCount * kSplashLinePitch - 4);
                    const int16_t y0 = static_cast<int16_t>((PANEL_HEIGHT - block_h) / 2);
                    for (int i = 0; i < kSplashLineCount; i++) {
                        draw_text_small(static_cast<int16_t>((PANEL_WIDTH - small_text_width(splash_lines[i])) / 2),
                                        static_cast<int16_t>(y0 + i * kSplashLinePitch), splash_lines[i], 255, 255, 0);
                    }
                    if (want) dma_display->drawPixelRGB888(PANEL_WIDTH - 1, PANEL_HEIGHT - 1, 0, 255, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
            // Splash over: force a full redraw of whatever comes next (also
            // clears the green pixel), even for static/maze modes.
            splash_over = true;
            last_static_key = 0xFFFFFFFFu;
            prev_frame_mode = static_cast<DisplayMode>(-1);
        }

        {
            const bool static_mode =
                    !wanted_blink && (mode == DisplayMode::kSolidColor || mode == DisplayMode::kOff ||
                                      mode == DisplayMode::kFixed1300 || mode == DisplayMode::kFixed1300Alt ||
                                      mode == DisplayMode::kFixed1300B || mode == DisplayMode::kFixed1300C ||
                                      mode == DisplayMode::kFixed1300D || mode == DisplayMode::kFixed1300E ||
                                      mode == DisplayMode::kFixed1300F || mode == DisplayMode::kFixed1300L ||
                                      mode == DisplayMode::kFixed1300LI ||
                                      mode == DisplayMode::kFixed13 || mode == DisplayMode::kFixed00);
            if (static_mode) {
                uint32_t key = (static_cast<uint32_t>(mode) + 1u) * 2654435761u;
                key ^= (static_cast<uint32_t>(local_solid_r) << 16) | (static_cast<uint32_t>(local_solid_g) << 8) |
                       static_cast<uint32_t>(local_solid_b);
                key ^= static_cast<uint32_t>(applied_brightness) << 24;
                if (mode == DisplayMode::kFixed1300L) {  // colours from the payload
                    for (int i = 0; i < 3; i++)
                        key = key * 16777619u ^ (static_cast<uint32_t>(local_f1300l_fg[i]) << 16 |
                                                 static_cast<uint32_t>(local_f1300l_bg[i]) << 8 |
                                                 static_cast<uint32_t>(local_f1300l_hue[i]));
                }
                if (key == last_static_key) {
                    vTaskDelay(pdMS_TO_TICKS(30));
                    continue;  // already on screen, unchanged
                }
                last_static_key = key;
            } else {
                last_static_key = 0xFFFFFFFFu;
            }
        }

        // Maze modes redraw over themselves instead of clearing (see
        // maze_dot_prev_*); a mode change or blink still gets a full clear.
        const bool mode_changed = (mode != prev_frame_mode);
        prev_frame_mode = mode;
        const bool skip_clear = !wanted_blink && !mode_changed &&
                                (mode == DisplayMode::kMaze1 || mode == DisplayMode::kMaze2 ||
                                 mode == DisplayMode::kMaze3);
        if (!skip_clear) dma_display->clearScreen();

        bool blink_visible = true;
        if (wanted_blink) {
            blink_visible = (blink_tick % kBlinkPeriodTicks) < kBlinkOnTicks;
            blink_tick++;
        } else {
            blink_tick = 0;
        }

        if (blink_visible) switch (mode) {
            case DisplayMode::kMessage:
                if (reset_scroll) {
                    text_px = static_cast<int16_t>(strlen(local_text) * kFontAdvance);
                    x = PANEL_WIDTH;
                }
                draw_text(x, message_y, local_text, local_r, local_g, local_b);
                if (--x < -text_px) {
                    x = PANEL_WIDTH;
                }
                break;

            case DisplayMode::kWifiStatus:
                // Small font, centered, static - the longest line here (an
                // IPv4 address) fits inside the panel width, so there's
                // nothing to scroll.
                for (int i = 0; i < local_status_count; i++) {
                    const int16_t line_x =
                        static_cast<int16_t>((PANEL_WIDTH - small_text_width(local_status[i].text)) / 2);
                    draw_text_small(line_x, kStatusRowY[i], local_status[i].text, local_status[i].r,
                                     local_status[i].g, local_status[i].b);
                }
                break;

            case DisplayMode::kTest1:
                run_test1_frame();
                break;

            case DisplayMode::kTest2:
                run_test2_frame();
                break;

            case DisplayMode::kTest3:
                run_test3_frame();
                break;

            case DisplayMode::kSolidColor:
                dma_display->fillScreenRGB888(local_solid_r, local_solid_g, local_solid_b);
                break;

            case DisplayMode::kRainbow:
                draw_rainbow(local_rainbow_r, local_rainbow_g, local_rainbow_b);
                break;

            case DisplayMode::kOff:
                break;

            case DisplayMode::kFixed1300:
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300OneExtraShiftPx),
                                  kFixed1300Y, "1", kFixed1300Scale, kFixed1300Grey, kFixed1300Grey, kFixed1300Grey,
                                  true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300CharW),
                                  kFixed1300Y, "3", kFixed1300Scale, kFixed1300Grey, kFixed1300Grey, kFixed1300Grey,
                                  true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + 2 * kFixed1300CharW), kFixed1300Y, "00",
                                  kFixed1300Scale, kFixed1300Grey, kFixed1300Grey, kFixed1300Grey, true);
                break;

            case DisplayMode::kFixed1300L:
                draw_chilanka_1300(local_f1300l_fg, local_f1300l_bg, local_f1300l_hue);
                break;

            case DisplayMode::kFixed1300LI: {
                static const uint8_t kBlack[3] = {0, 0, 0};
                static const uint8_t kTan[3] = {kFixed1300LIBgR, kFixed1300LIBgG, kFixed1300LIBgB};
                static const uint8_t kNoHue[3] = {255, 255, 255};
                draw_chilanka_1300(kBlack, kTan, kNoHue);
                break;
            }

            case DisplayMode::kFixed1300Alt:
                dma_display->fillScreenRGB888(kFixed1300BgGray, kFixed1300BgGray, kFixed1300BgGray);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300OneExtraShiftPx),
                                  kFixed1300Y, "1", kFixed1300Scale, 0, 0, 0, true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300CharW),
                                  kFixed1300Y, "3", kFixed1300Scale, 0, 0, 0, true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + 2 * kFixed1300CharW), kFixed1300Y, "00",
                                  kFixed1300Scale, 0, 0, 0, true);
                break;

            case DisplayMode::kFixed1300B:
                dma_display->fillScreenRGB888(kFixed1300BgGray50, kFixed1300BgGray50, kFixed1300BgGray50);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300OneExtraShiftPx),
                                  kFixed1300Y, "1", kFixed1300Scale, 0, 0, 0, true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + kFixed1300ShiftPx + kFixed1300CharW),
                                  kFixed1300Y, "3", kFixed1300Scale, 0, 0, 0, true);
                draw_text_scaled(static_cast<int16_t>(kFixed1300X + 2 * kFixed1300CharW), kFixed1300Y, "00",
                                  kFixed1300Scale, 0, 0, 0, true);
                break;

            case DisplayMode::kFixed1300C: {
                // Largest integer scale of the small font (3px glyph + 1px
                // gap, 4 chars) that still fits "1300" on the 64px-wide
                // panel: width = (4*4-1)*scale = 15*scale <= 64 caps scale
                // at 4 (60px wide, 20px tall) - visibly bigger than
                // hub75/1300's family despite the narrower base font.
                constexpr int16_t kSmallScale = 4;
                constexpr const char *kText = "1300";
                const int16_t w = small_text_width_scaled(kText, kSmallScale);
                const int16_t x = static_cast<int16_t>((PANEL_WIDTH - w) / 2);
                const int16_t y = static_cast<int16_t>((PANEL_HEIGHT - kSmallFontHeight * kSmallScale) / 2);
                draw_text_small_scaled(x, y, kText, kSmallScale, kFixed1300BgGray50, kFixed1300BgGray50,
                                        kFixed1300BgGray50);
                break;
            }

            case DisplayMode::kFixed1300D: {
                constexpr int16_t kSmallScale = 4;  // see kFixed1300C above
                constexpr const char *kText = "1300";
                dma_display->fillScreenRGB888(kFixed1300BgGray50, kFixed1300BgGray50, kFixed1300BgGray50);
                const int16_t w = small_text_width_scaled(kText, kSmallScale);
                const int16_t x = static_cast<int16_t>((PANEL_WIDTH - w) / 2);
                const int16_t y = static_cast<int16_t>((PANEL_HEIGHT - kSmallFontHeight * kSmallScale) / 2);
                draw_text_small_scaled(x, y, kText, kSmallScale, 0, 0, 0);
                break;
            }

            case DisplayMode::kFixed1300E: {
                // Exactly hub75/1300c's font (the standard small font,
                // 3px-wide columns - the only one of the fonts here that
                // can actually hollow out a '0') at a literal, fixed dot
                // scale of 2 - "2 pixel wide" describes the rendered stroke
                // thickness, not the glyph's own column count.
                constexpr int16_t kScale = 2;
                constexpr const char *kText = "1300";
                const int16_t w = small_text_width_scaled(kText, kScale);
                const int16_t x = static_cast<int16_t>((PANEL_WIDTH - w) / 2);
                const int16_t y = static_cast<int16_t>((PANEL_HEIGHT - kSmallFontHeight * kScale) / 2);
                draw_text_small_scaled(x, y, kText, kScale, kFixed1300BgGray50, kFixed1300BgGray50,
                                        kFixed1300BgGray50);
                break;
            }

            case DisplayMode::kFixed1300F: {
                constexpr int16_t kScale = 2;  // see kFixed1300E above
                constexpr const char *kText = "1300";
                dma_display->fillScreenRGB888(kFixed1300BgGray50, kFixed1300BgGray50, kFixed1300BgGray50);
                const int16_t w = small_text_width_scaled(kText, kScale);
                const int16_t x = static_cast<int16_t>((PANEL_WIDTH - w) / 2);
                const int16_t y = static_cast<int16_t>((PANEL_HEIGHT - kSmallFontHeight * kScale) / 2);
                draw_text_small_scaled(x, y, kText, kScale, 0, 0, 0);
                break;
            }

            case DisplayMode::kFixed13:
            case DisplayMode::kFixed00: {
                // Same standard small font as hub75/1300c/e (not the old
                // 2-column-only font, which could never hollow out a '0')
                // at a fixed dot scale of 2 - see kFixed1300E above.
                constexpr int16_t kScale = 2;
                const char *text = (mode == DisplayMode::kFixed13) ? "13" : "00";
                const int16_t w = small_text_width_scaled(text, kScale);
                const int16_t x = static_cast<int16_t>((PANEL_WIDTH - w) / 2);
                const int16_t y = static_cast<int16_t>((PANEL_HEIGHT - kSmallFontHeight * kScale) / 2);
                draw_text_small_scaled(x, y, text, kScale, kFixed1300BgGray50, kFixed1300BgGray50,
                                        kFixed1300BgGray50);
                break;
            }

            case DisplayMode::kRayTrace: {
                const RtSphere spheres[1] = {
                    {{0.0f, 0.0f, 4.0f}, 1.2f, local_ray_sphere_r, local_ray_sphere_g, local_ray_sphere_b},
                };
                draw_raytracer(spheres, 1, RtVec3{0.0f, 0.0f, 4.0f}, -1.2f, local_ray_floor1_r, local_ray_floor1_g,
                                local_ray_floor1_b, local_ray_floor2_r, local_ray_floor2_g, local_ray_floor2_b);
                break;
            }

            case DisplayMode::kRayTrace2: {
                const RtSphere spheres[2] = {
                    {{-1.3f, 0.0f, 4.3f}, 1.0f, local_ray_sphere_r, local_ray_sphere_g, local_ray_sphere_b},
                    {{1.3f, 0.0f, 4.7f}, 1.0f, local_ray2_sphere2_r, local_ray2_sphere2_g, local_ray2_sphere2_b},
                };
                draw_raytracer(spheres, 2, RtVec3{0.0f, 0.0f, 4.5f}, -1.1f, local_ray_floor1_r, local_ray_floor1_g,
                                local_ray_floor1_b, local_ray_floor2_r, local_ray_floor2_g, local_ray_floor2_b);
                break;
            }

            case DisplayMode::kStandardTest1: {
                struct Std1Phase {
                    uint8_t r, g, b, brightness;
                    const char *name;
                };
                static constexpr Std1Phase kStd1Phases[8] = {
                    {255, 255, 255, kDimBrightness, "white dim"},
                    {255, 255, 255, kFullBrightness, "white bright"},
                    {255, 0, 0, kDimBrightness, "red dim"},
                    {255, 0, 0, kFullBrightness, "red bright"},
                    {0, 255, 0, kDimBrightness, "green dim"},
                    {0, 255, 0, kFullBrightness, "green bright"},
                    {0, 0, 255, kDimBrightness, "blue dim"},
                    {0, 0, 255, kFullBrightness, "blue bright"},
                };
                constexpr int kStd1PhaseTicks = 67;  // ~2s/phase
                const Std1Phase &phase = kStd1Phases[std1_phase];
                if (std1_phase_tick == 0) {
                    set_brightness(phase.brightness);
                    ESP_LOGI(TAG, "hub75/standardtest1: phase %d/8 - %s", std1_phase + 1, phase.name);
                }
                dma_display->fillScreenRGB888(phase.r, phase.g, phase.b);
                // Phase number in black (0,0,0 - LEDs off) so it reads
                // against any of the fill colors above, including white.
                char phase_label[2] = {static_cast<char>('1' + std1_phase), '\0'};
                draw_text(1, 1, phase_label, 0, 0, 0);
                if (++std1_phase_tick >= kStd1PhaseTicks) {
                    std1_phase_tick = 0;
                    std1_phase = (std1_phase + 1) % 8;
                }
                break;
            }

            case DisplayMode::kSpaceInvaders:
                run_space_invaders_frame();
                break;

            case DisplayMode::kMillipede:
                run_millipede_frame();
                break;

            case DisplayMode::kPong:
                run_pong_frame();
                break;

            case DisplayMode::kTicTacToe:
                run_tictactoe_frame();
                break;

            case DisplayMode::kMatrix1:
                draw_matrix(4, 8);
                break;

            case DisplayMode::kMatrix2:
                draw_matrix(8, 16);
                break;

            case DisplayMode::kMatrix3:
                draw_matrix(16, 32);
                break;

            case DisplayMode::kMatrix1Cont:
                if (++matrix_cont_tick >= kMatrixContTicks) {
                    matrix_cont_tick = 0;
                    xSemaphoreTake(display_mutex, portMAX_DELAY);
                    randomize_matrix(4 * 8);
                    xSemaphoreGive(display_mutex);
                }
                draw_matrix(4, 8);
                break;

            case DisplayMode::kMatrix2Cont:
                if (++matrix_cont_tick >= kMatrixContTicks) {
                    matrix_cont_tick = 0;
                    xSemaphoreTake(display_mutex, portMAX_DELAY);
                    randomize_matrix(8 * 16);
                    xSemaphoreGive(display_mutex);
                }
                draw_matrix(8, 16);
                break;

            case DisplayMode::kMatrix3Cont:
                if (++matrix_cont_tick >= kMatrixContTicks) {
                    matrix_cont_tick = 0;
                    xSemaphoreTake(display_mutex, portMAX_DELAY);
                    randomize_matrix(16 * 32);
                    xSemaphoreGive(display_mutex);
                }
                draw_matrix(16, 32);
                break;

            case DisplayMode::kMatrixRain:
                run_matrix_rain_frame();
                break;

            case DisplayMode::kBall1:
                run_ball1_frame();
                break;

            case DisplayMode::kShootingStars:
                run_shootingstars_frame();
                break;

            case DisplayMode::kTimeFrame:
                draw_time_clock();
                break;

            case DisplayMode::kMaze1:
            case DisplayMode::kMaze2:
            case DisplayMode::kMaze3:
            case DisplayMode::kMaze4:
            case DisplayMode::kMaze5:
                run_maze_frame();
                break;

            case DisplayMode::kPing1: {
                // Scrolling latency strip-chart: one column per sample
                // (oldest at x=0, newest at the right edge), bar height and
                // color both encode the delay - green (fast) through
                // yellow/orange up to red (slow), with a full-height red
                // column marking a timed-out ping. The bottom kSmallFontHeight
                // rows are reserved for the numeric readout below, so the
                // graph itself only ever grows into the rows above that.
                constexpr int16_t kGraphBaselineY = PANEL_HEIGHT - kSmallFontHeight - 2;  // 1px gap above the text
                constexpr int16_t kGraphMaxHeight = kGraphBaselineY + 1;
                constexpr double kGraphMaxMs = 100.0;  // latency at/above this fills the graph to the top
                for (int x = 0; x < kPing1GraphWidth; x++) {
                    const uint16_t sample = local_ping1_graph_ms[x];
                    if (sample == 0) continue;  // no sample scrolled into this column yet
                    uint8_t r, g, b;
                    int16_t height;
                    if (sample == kPing1Timeout) {
                        r = 255;
                        g = 0;
                        b = 0;
                        height = kGraphMaxHeight;
                    } else {
                        if (sample < 15) {
                            r = 0;
                            g = 255;
                            b = 0;
                        } else if (sample < 40) {
                            r = 255;
                            g = 255;
                            b = 0;
                        } else if (sample < 80) {
                            r = 255;
                            g = 140;
                            b = 0;
                        } else {
                            r = 255;
                            g = 40;
                            b = 0;
                        }
                        height = static_cast<int16_t>(sample / kGraphMaxMs * kGraphMaxHeight + 0.5);
                        if (height < 1) height = 1;
                        if (height > kGraphMaxHeight) height = kGraphMaxHeight;
                    }
                    dma_display->fillRect(static_cast<int16_t>(x), static_cast<int16_t>(kGraphBaselineY - height + 1),
                                           1, height, r, g, b);
                }

                // Numeric ms readout, bottom strip below the graph, small
                // fixed-width font (see draw_text_small()) - always the
                // true measured delay, never scaled or capped.
                char ping1_ms_text[16];
                snprintf(ping1_ms_text, sizeof(ping1_ms_text), "%.1f", local_ping1_rtt_ms);
                const int16_t ping1_ms_x = static_cast<int16_t>((PANEL_WIDTH - small_text_width(ping1_ms_text)) / 2);
                draw_text_small(ping1_ms_x, static_cast<int16_t>(PANEL_HEIGHT - kSmallFontHeight), ping1_ms_text, 255,
                                 255, 255);

                // Success/timeout counts flank the ms readout on the same
                // strip - success count left-justified in green, timeout
                // count right-justified in red.
                char ping1_success_text[16];
                snprintf(ping1_success_text, sizeof(ping1_success_text), "%lu",
                          static_cast<unsigned long>(local_ping1_success_count));
                draw_text_small(0, static_cast<int16_t>(PANEL_HEIGHT - kSmallFontHeight), ping1_success_text, 0, 255,
                                 0);

                char ping1_timeout_text[16];
                snprintf(ping1_timeout_text, sizeof(ping1_timeout_text), "%lu",
                          static_cast<unsigned long>(local_ping1_timeout_count));
                const int16_t ping1_timeout_x =
                    static_cast<int16_t>(PANEL_WIDTH - small_text_width(ping1_timeout_text));
                draw_text_small(ping1_timeout_x, static_cast<int16_t>(PANEL_HEIGHT - kSmallFontHeight),
                                 ping1_timeout_text, 255, 0, 0);
                break;
            }

            case DisplayMode::kPing2:
                run_ping2_frame(local_ping2_launch_pending, local_ping2_timed_out, local_ping2_r, local_ping2_g,
                                 local_ping2_b, local_ping2_frames, local_ping2_rtt_ms);
                break;

            case DisplayMode::kFlagUsa:
                draw_flag_usa();
                break;

            case DisplayMode::kFlagCanada:
                draw_flag_canada();
                break;

            case DisplayMode::kElevator:
                run_elevator_frame(1, kElevHoldTicksNormal, 3, true);
                break;

            case DisplayMode::kElevatorLarge:
                run_elevator_frame(2, kElevHoldTicksLarge, 3, true);
                break;

            case DisplayMode::kElevatorXLarge:
                run_elevator_frame(4, kElevHoldTicksLarge, 2, false);
                break;

            case DisplayMode::kClock:
                run_clock_frame(local_ntp_synced, local_clock_tz, local_clock_r, local_clock_g, local_clock_b);
                break;

            case DisplayMode::kClockConfirm: {
                constexpr const char *kTimeSetMsg = "TIME SET";
                const int16_t msg_x = static_cast<int16_t>((PANEL_WIDTH - small_text_width(kTimeSetMsg)) / 2);
                draw_text_small(msg_x, (PANEL_HEIGHT - kSmallFontHeight) / 2, kTimeSetMsg, 0, 255, 0);
                if (++clock_confirm_tick >= kClockConfirmTicks) {
                    set_test_mode(DisplayMode::kClock, "clock (auto, after NTP sync confirmation)");
                }
                break;
            }

            case DisplayMode::kAutoTest: {
                if (++auto_phase_tick >= kAutoPhaseDurationTicks[auto_phase]) {
                    auto_phase_tick = 0;
                    auto_phase = (auto_phase + 1) % static_cast<int>(AutoPhase::kCount);
                    auto_phase_started = true;
                }
                switch (static_cast<AutoPhase>(auto_phase)) {
                    case AutoPhase::kRed:
                        dma_display->fillScreenRGB888(255, 0, 0);
                        break;
                    case AutoPhase::kGreen:
                        dma_display->fillScreenRGB888(0, 255, 0);
                        break;
                    case AutoPhase::kBlue:
                        dma_display->fillScreenRGB888(0, 0, 255);
                        break;
                    case AutoPhase::kYellow:
                        dma_display->fillScreenRGB888(255, 255, 0);
                        break;
                    case AutoPhase::kOrange:
                        dma_display->fillScreenRGB888(255, 165, 0);
                        break;
                    case AutoPhase::kRainbow:
                        if (auto_phase_started) {
                            for (int row = 0; row < 32; row++) {
                                random_color(&auto_rainbow_r[row], &auto_rainbow_g[row], &auto_rainbow_b[row]);
                            }
                        }
                        draw_rainbow(auto_rainbow_r, auto_rainbow_g, auto_rainbow_b);
                        break;
                    case AutoPhase::kBars:
                        run_test2_frame();
                        break;
                    case AutoPhase::kPacman:
                        run_test3_frame();
                        break;
                    case AutoPhase::kPixel:
                        run_test1_frame();
                        break;
                    case AutoPhase::kFlagUsa:
                        draw_flag_usa();
                        break;
                    case AutoPhase::kFlagCanada:
                        draw_flag_canada();
                        break;
                    case AutoPhase::kMessage:
                    default:
                        if (auto_phase_started) {
                            auto_msg_text_px = static_cast<int16_t>(strlen(kAutoTestMessage) * kFontAdvance);
                            auto_msg_x = PANEL_WIDTH;
                        }
                        draw_text(auto_msg_x, message_y, kAutoTestMessage, 255, 255, 255);
                        if (--auto_msg_x < -auto_msg_text_px) {
                            auto_msg_x = PANEL_WIDTH;
                        }
                        break;
                }
                auto_phase_started = false;
                break;
            }
        }

        dma_display->flipDMABuffer();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
