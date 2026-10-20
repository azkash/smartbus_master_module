#include <Arduino.h>
#include <soc/gpio_reg.h>
#include <WiFi.h>
#include <WiFiUdp.h>      // one UDP socket receives all router-connected seats
#include <WebServer.h>     // local phone dashboard on the master AP
#include <WebSocketsServer.h>
#include <HTTPClient.h>
#include <FastLED.h>
#include <Preferences.h>   // NVS persistence for LED settings
#include <ArduinoJson.h>   // JSON serialization — Library oleh Benoit Blanchon
#include <ESPmDNS.h>       // mDNS — reachable as bus-led.local (bundled with core)
#include <esp_log.h>       // esp_log_level_set — mute noisy WiFi driver logs
#include <esp_system.h>    // esp_reset_reason — explain the previous restart
#include <esp_wifi_ap_get_sta_list.h>
#ifdef ENABLE_NAPT
#include <esp_netif.h>     // NAT + DHCP DNS option (core 3.x `napt` env only)
#endif
#include "serial_mirror.h"
// #include <esp_now.h>  // replaced by MQTT
// #include "lwip/lwip_napt.h"  // NAT removed — ip_napt_enable not in current framework build

Preferences prefs;

// ─── Audio Configuration ────────────────────────────────────────────────
// DY-HV8F DIP 000: integrated I/O inputs, active LOW.
static constexpr uint8_t AUDIO_IO_PINS[8] = {
  13,  // IO0, value 1
  14,  // IO1, value 2
  19,  // IO2, value 4
  21,  // IO3, value 8
  22,  // IO4, value 16
  23,  // IO5, value 32
  25,  // IO6, value 64
  26   // IO7, value 128
};
#define BUSY_PIN 15

// WebServer server(80);            // -- replaced by WebSocket server --
WebSocketsServer webSocket(81);     // WebSocket server — Android connects to ws://<AP-IP>:81

// ─── AP Configuration (seats connect here) ───────────────────────────
static constexpr uint8_t MASTER_AP_MAX_CLIENTS = 15;

// ─── Upstream internet WiFi (STA — gives the MASTER internet) ─────────
// Fill in your router / 4G-hotspot credentials. The master joins this for its
// own internet (NTP, cloud upload, etc.) while still running the AP above.
// NOTE: AP clients (seats) do NOT get internet through the master — that needs
// NAT, which isn't in this framework build. This is master-only internet.
// Leave STA_SSID empty ("") to skip upstream entirely (pure AP mode).
const char* STA_SSID    = "iot_bus";
const char* STA_PASS    = "iot_BUS@5758";

// Router-centric seat transport. Seats discover the master by UDP broadcast,
// then send all heartbeat/event packets to this single socket.
static constexpr uint16_t SMARTBUS_UDP_PORT = 5758;

// Safe hybrid policy: determine the hotspot channel before the seat AP starts.
// Runtime credential updates are applied through a controlled reboot.
constexpr unsigned long STA_BOOT_TIMEOUT_MS = 12000;
static bool          wifiRestartPending = false;
static unsigned long wifiRestartAt      = 0;
static bool          mdnsStarted        = false;

constexpr unsigned long STA_RECOVERY_INITIAL_DELAY_MS = 5000;
constexpr unsigned long STA_RECOVERY_SCAN_INTERVAL_MS = 15000;
constexpr unsigned long STA_RECOVERY_CONNECT_TIMEOUT_MS = 10000;

enum class StaRecoveryPhase : uint8_t {
  Idle,
  WaitingToScan,
  Scanning,
  ConnectingSameChannel,
  WaitingSafeRestart
};

static StaRecoveryPhase staRecoveryPhase = StaRecoveryPhase::Idle;
static String        configuredStaSsid;
static String        configuredStaPass;
static unsigned long staRecoveryAt       = 0;
static unsigned long staRecoveryLogAt    = 0;
static uint8_t       activeApChannel     = 11;
static uint8_t       recoveryFoundChannel = 0;
static bool          staRecoveryEnabled  = false;
static volatile bool audioAnnouncementActive = false;
constexpr uint32_t STA_RECOVERY_RESTART_MAGIC = 0x53545239;  // "STR9"
RTC_NOINIT_ATTR uint32_t staRecoveryRestartMarker;
static bool staCrossChannelRestartBlocked = false;

// ─── Inverter module (separate ESP32, HTTP :80, /data JSON) ──────────
// -- Inverter relay disabled --
// const char* INVERTER_HOSTNAME    = "bus-inverter";
// const char* INVERTER_FALLBACK_IP = "";   // e.g. "192.168.4.3" — leave "" to rely on mDNS only

// ─── MQTT Topics ─── reverted: HTTP server instead of MQTT broker ─────
// #define MQTT_TOPIC_CMD        "bus/led/command"   // clients publish → LED control
// #define MQTT_TOPIC_SEAT_EVENT "bus/seat/event"   // sensor nodes publish → triggers audio/LED
// #define MQTT_TOPIC_SENSOR     "bus/sensor/data"  // broker forwards seat events to all subscribers
// #define MQTT_TOPIC_STATUS     "bus/status"       // broker publishes ack / status

// ─── Headunit IP ─────────────────────────────────────────────────────
static String headunitIP = "192.168.4.2";

// ─── WS2811 Configuration ────────────────────────────────────────────
#define LED_PIN     5
#define DIM_PIN     18
#define NUM_LEDS    66
#define NUM_DIMS    160
#define LED_TYPE    WS2811
// Verified from the installed strip: the old RBG order produced the cycle
// red -> green, green -> blue, blue -> red. GRB restores logical RGB colors.
#define COLOR_ORDER GRB
// #define COOLING  55
// #define SPARKING 120
// #define FRAMES_PER_SECOND 60
// #define GROUP_SIZE 5
// #define CAR_LENGTH 10
// #define FADE_RATE 1

// // Car Effect Parameters
// CRGB carColor;
// uint8_t carPosition = 0;

// // Snake Effect Parameters
// const int snakeLength = 10;
// const int gapBetweenSnakes = 1;
// const int moveDelay = 50;
// const int colorChangeInterval = 10000;
// const int fadeSize = 5;

// // Color List
// const CRGB colorPalette[] = {
//   CRGB::Red, CRGB::Green, CRGB::Blue,
//   CRGB::Purple, CRGB::Yellow, CRGB::Cyan
// };
// const int paletteCount = sizeof(colorPalette)/sizeof(colorPalette[0]);

// // Brightness Configuration
// const int stateMin = -1;
// const int stateMax = 4;
// int currentState = stateMin;
// int targetBrightness = 255;
// int currentBrightness = 255;
// unsigned long lastBrightnessUpdate = 0;
// const int brightnessUpdateInterval = 20;

CRGB leds[NUM_LEDS];
CRGB dimLeds[NUM_DIMS];

// ─── LED Effect ──────────────────────────────────────────────────────
enum LedEffect : uint8_t {
    EFF_RAINBOW_GLITTER,
    EFF_CONFETTI_PARTY,
    EFF_SINELON_WAVE,
    EFF_BPM_RHYTHM,
    EFF_JUGGLE_BOUNCE,
    EFF_FIRE_FLAME,
    EFF_AURORA_GLOW,
    EFF_BINARY_COUNTER,
    EFF_PALETTE_WAVE,
    EFF_DUAL_SCANNER,
    EFF_COLOR_BOUNCE,
    EFF_SNAKE,
    EFF_SOLID,
    EFF_BLINK,
    EFF_CHASE,
    EFF_FADE
};

DEFINE_GRADIENT_PALETTE( ocean_gp ) {
  0,   0,   0, 143,
  48,  0,   0, 255,
  96,  0, 255, 255,
  144, 42, 255,  45,
  192,255, 255,   0,
  240,255,   0,   0,
  255,143,   0,   0
};

// ─── Headunit tracking ───────────────────────────────────────────────
// Slot number of the registered headunit client (-1 = none connected).
// Seat node data is only processed once the headunit has identified itself.
static int headunitNum = -1;

struct WsPeerInfo {
  bool used;
  IPAddress ip;
  char role[12];
  char name[32];
};
static WsPeerInfo wsPeers[WEBSOCKETS_SERVER_CLIENT_MAX];
static IPAddress webMonitorClientIp;

// ─── WS2811 State ────────────────────────────────────────────────────
uint8_t   ledR = 0, ledG = 0, ledB = 0;
uint8_t   ledBrightness = 128;
LedEffect ledEffect = EFF_SOLID;
volatile bool ledOn = true;
