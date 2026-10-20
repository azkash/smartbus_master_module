#include "variables.h"
#include <esp_wifi.h>
// #include "mqtt_broker.h"  // -- reverted: MQTT broker replaced by HTTP server --
#include "wifi_connect_guard.h"

// Implemented in udp_transport.h (included by main.cpp after this header).
// The declaration is needed here so Head Unit pairing commands can be routed
// back to a seat connected through UDP instead of WebSocket.
static bool udpSendJson(const IPAddress& ip, uint16_t port, const char* json);

// Fungsi mapping XY dengan pola ular
// int XY(int x, int y) {
//   int rowStart = y * NUM_LEDS;
//   if(y % 2 == 0) return rowStart + x;
//   else            return rowStart + (NUM_LEDS - 1 - x);
// }

// Forward-declare so the broker class can call it before it's defined below
bool processCommand(const String& cmd);

// Forward-declare so wifiInit() can auto-connect using creds saved by staConnect()
static bool loadStaCreds(String& ssid, String& pass);
#ifdef ENABLE_NAPT
// Forward-declare so wifiInit() can advertise DNS right after the AP starts
static void apAdvertiseDns();
#endif

// ─── MQTT seat-event flags ─── reverted: not used with HTTP server ────
// static volatile bool mqttSeatReceived = false;
// static volatile int  mqttSeatState    = 0;
// static volatile bool mqttSeatOccupied = false;
// static char          mqttSeatId[32]   = {0};

// ─── Seat-call announcement queue ─────────────────────────────────────
// Filled in onWsEvent/udpHandleSeat, drained in audioTask. A dedicated audio
// task keeps the blocking playTrack()/waitForAudioDone() sequence away from
// both the WebSocket loop and the LED animation task. Carries the seat id and
// the kind of announcement so the audio task knows which phrase to voice.
enum CallKind : uint8_t {
  CALL_SEAT     = 0,   // "Panggilan kursi nomor <N>"
  CALL_SEATBELT = 1    // "kursi nomor <N> tidak memakai seatbelt"
};
struct CallEvent { char id[32]; uint8_t kind; };
static QueueHandle_t callQueue;   // created in setup()

// ─── Dynamic seat-ID provisioning ───────────────────────────────────
// Unassigned seats all display "RandomID", but each includes a stable UID
// derived from its MAC. The UID lets one command safely traverse/broadcast
// through gateway chains while only the intended node accepts it.
// Production buses can carry 50 seat nodes. Keep provisioning capacity aligned
// with the 64-slot roster so a full unassigned fleet can be commissioned in
// any order without exhausting the pending table.
#define MAX_PENDING_PAIRINGS 64
struct PendingPairing {
  char          uid[13];
  uint8_t       sourceClient;
  bool          viaUdp;
  IPAddress     sourceIp;
  uint16_t      sourcePort;
  unsigned long lastSeen;
  bool          used;
};
static PendingPairing pendingPairings[MAX_PENDING_PAIRINGS];

static int pairingFind(const char* uid) {
  for (int i = 0; i < MAX_PENDING_PAIRINGS; ++i) {
    if (pendingPairings[i].used && strcmp(pendingPairings[i].uid, uid) == 0) return i;
  }
  return -1;
}

static int pairingRemember(const char* uid, uint8_t sourceClient) {
  int slot = pairingFind(uid);
  if (slot < 0) {
    for (int i = 0; i < MAX_PENDING_PAIRINGS; ++i) {
      if (!pendingPairings[i].used) { slot = i; break; }
    }
  }
  if (slot < 0) return -1;
  strlcpy(pendingPairings[slot].uid, uid, sizeof(pendingPairings[slot].uid));
  pendingPairings[slot].sourceClient = sourceClient;
  pendingPairings[slot].viaUdp = false;
  pendingPairings[slot].sourceIp = IPAddress();
  pendingPairings[slot].sourcePort = 0;
  pendingPairings[slot].lastSeen = millis();
  pendingPairings[slot].used = true;
  return slot;
}

static int pairingRememberUdp(const char* uid, const IPAddress& ip, uint16_t port) {
  int slot = pairingFind(uid);
  if (slot < 0) {
    for (int i = 0; i < MAX_PENDING_PAIRINGS; ++i) {
      if (!pendingPairings[i].used) { slot = i; break; }
    }
  }
  if (slot < 0) return -1;
  strlcpy(pendingPairings[slot].uid, uid, sizeof(pendingPairings[slot].uid));
  pendingPairings[slot].sourceClient = 0;
  pendingPairings[slot].viaUdp = true;
  pendingPairings[slot].sourceIp = ip;
  pendingPairings[slot].sourcePort = port;
  pendingPairings[slot].lastSeen = millis();
  pendingPairings[slot].used = true;
  return slot;
}

static void pairingClear(const char* uid) {
  int slot = pairingFind(uid);
  if (slot >= 0) pendingPairings[slot].used = false;
}

static void pairingDropClient(uint8_t client) {
  for (int i = 0; i < MAX_PENDING_PAIRINGS; ++i) {
    if (pendingPairings[i].used && !pendingPairings[i].viaUdp &&
        pendingPairings[i].sourceClient == client) {
      pendingPairings[i].used = false;
    }
  }
}

static bool pairingIdValid(const char* id) {
  const size_t length = strlen(id);
  if (length == 0 || length > 3) return false;
  if (length == 1 && (id[0] == 'A' || id[0] == 'a' ||
                      id[0] == 'B' || id[0] == 'b')) {
    return true;
  }
  if (id[0] < '1' || id[0] > '9') return false;
  size_t cursor = 1;
  if (cursor < length && id[cursor] >= '0' && id[cursor] <= '9') ++cursor;
  if (cursor < length && (id[cursor] == 'A' || id[cursor] == 'a' ||
                          id[cursor] == 'B' || id[cursor] == 'b')) {
    ++cursor;
  }
  return cursor == length;
}

static void sendPairingRequest(uint8_t headunitClient, const char* uid) {
  char event[128];
  snprintf(event, sizeof(event),
           "{\"event\":\"seatPairingRequired\",\"uid\":\"%s\","
           "\"temporaryId\":\"RandomID\"}", uid);
  webSocket.sendTXT(headunitClient, event);
  Serial.printf("[Pairing] requested head-unit ID for UID %s\n", uid);
}

// ─── Seat roster — tracks every seat node that has reported in ────────
// Updated only from onWsEvent (loop-task context), so no mutex needed.
#define MAX_SEATS        64   // production target: 50 seats + 14 spare slots
#define SEAT_OFFLINE_MS  15000   // no message for 15s → seat considered offline

static_assert(MAX_SEATS >= 50, "Production SmartBus requires at least 50 seat slots");

struct SeatInfo {
  char          id[32];
  bool          occupied;
  bool          calling;      // instantaneous: last reported sensorState (button held)
  bool          callPending;  // latched: a call was raised, not yet acknowledged
  unsigned long lastSeen;     // millis() of last message
  unsigned long callTime;     // millis() when callPending was latched
  bool          online;       // last-logged online state (for ONLINE/OFFLINE edge logging)
  bool          seatbelt;      // last reported buckle state (true = fastened)
  bool          seatbeltAlert; // latched: occupied while unbuckled, warning active
  unsigned long lastSeatbeltWarnTime; // millis() when seatbelt warning was last queued
  bool          used;         // slot in use
  bool          viaUdp;       // false = legacy WebSocket/gateway transport
  IPAddress     sourceIp;      // last UDP source, if viaUdp
  uint32_t      udpSession;    // changes whenever a seat reboots
  uint32_t      udpSeq;        // last accepted packet in that session
  bool          hasUdpSeq;
};
static SeatInfo seats[MAX_SEATS];

static bool rosterHasId(const char* id) {
  for (int i = 0; i < MAX_SEATS; ++i) {
    if (seats[i].used && strcasecmp(seats[i].id, id) == 0) return true;
  }
  return false;
}

// Find a seat slot by id, or claim a free one. Returns index, or -1 if full.
static int rosterFind(const char* id) {
  int freeIdx = -1;
  for (int i = 0; i < MAX_SEATS; i++) {
    if (seats[i].used) {
      if (strcmp(seats[i].id, id) == 0) return i;
    } else if (freeIdx < 0) {
      freeIdx = i;
    }
  }
  if (freeIdx < 0) return -1;
  strncpy(seats[freeIdx].id, id, sizeof(seats[freeIdx].id) - 1);
  seats[freeIdx].id[sizeof(seats[freeIdx].id) - 1] = '\0';
  seats[freeIdx].used = true;
  Serial.printf("[Roster] new seat: %s (slot %d)\n", id, freeIdx);
  return freeIdx;
}

// Upsert a seat from an incoming seat event. Returns true only on the rising
// edge of "occupied while unbuckled", so the caller can queue the warning once
// instead of on every heartbeat.
static bool rosterUpdate(const char* id, int sensorState, bool occupied,
                         bool seatbelt) {
  if (id[0] == '\0') return false;
  int i = rosterFind(id);
  if (i < 0) { Serial.println("[Roster] full — seat ignored"); return false; }
  if (!seats[i].online) {
    seats[i].online = true;
    Serial.printf("[Seat] %s ONLINE\n", seats[i].id);
  }
  seats[i].occupied = occupied;
  seats[i].calling  = (sensorState != 0);

  const bool alert  = occupied && !seatbelt;
  const bool raised = alert && !seats[i].seatbeltAlert;
  if (raised) {
    Serial.printf("[Seatbelt] %s occupied without seatbelt\n", seats[i].id);
  }
  seats[i].seatbelt      = seatbelt;
  seats[i].seatbeltAlert = alert;

  if (sensorState) {
    seats[i].callPending = true;
    seats[i].callTime    = millis();  // record when the call was raised
  }
  seats[i].lastSeen = millis();
  return raised;
}

// Acknowledge/clear a seat's pending call. Returns true if the seat was found.
static bool rosterClear(const char* id) {
  for (int i = 0; i < MAX_SEATS; i++) {
    if (seats[i].used && strcmp(seats[i].id, id) == 0) {
      seats[i].calling = false;
      seats[i].callPending = false;
      Serial.printf("[Roster] call cleared for %s\n", id);
      return true;
    }
  }
  return false;
}

// Build a JSON array of all known seats with full state fields.
// e.g. [{"id":"1A","occupied":false,"calling":false,"callPending":true,"online":true}]
static String rosterJson() {
  DynamicJsonDocument doc(MAX_SEATS * 128);
  JsonArray arr = doc.to<JsonArray>();
  unsigned long now = millis();
  for (int i = 0; i < MAX_SEATS; i++) {
    if (!seats[i].used) continue;
    const bool isOnline = (now - seats[i].lastSeen) < SEAT_OFFLINE_MS;
    JsonObject obj    = arr.createNestedObject();
    obj["id"]          = seats[i].id;
    // Never expose stale state to the head unit. If the seat controller stops
    // reporting, its last "occupied" sample must not remain visible forever.
    obj["occupied"]    = isOnline && seats[i].occupied;
    obj["calling"]     = isOnline && seats[i].calling;
    obj["callPending"] = seats[i].callPending;
    obj["online"]      = isOnline;
    obj["seatbelt"]      = isOnline && seats[i].seatbelt;
    obj["seatbeltAlert"] = isOnline && seats[i].seatbeltAlert;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// ─── MQTT Broker instance ─── reverted ────────────────────────────────
// MqttBroker mqttBroker;

// #include <WiFiManager.h>  // disabled — skipping portal, straight to AP

static void wifiInit() {
  // Final router-centric topology: the Master is a station only. The router
  // handles all Wi-Fi associations; Seat-Monitoring-Bus is no longer started.
  WiFi.setHostname("bus-led");
  WiFi.mode(WIFI_STA);
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  // Fleet-wide fixed router configuration. Ignore legacy hotspot credentials
  // in NVS so an old Head Unit setting cannot send this Master elsewhere.
  String bootSsid = STA_SSID;
  String bootPass = STA_PASS;
  Serial.printf("[STA] fixed fleet router '%s'\n", bootSsid.c_str());
  configuredStaSsid = bootSsid;
  configuredStaPass = bootPass;
  staRecoveryEnabled = configuredStaSsid.length() > 0;

  if (bootSsid.length()) {
    Serial.printf("[STA] boot connect to '%s' (timeout %lus)\n",
                  bootSsid.c_str(), STA_BOOT_TIMEOUT_MS / 1000);
    WiFi.begin(bootSsid.c_str(), bootPass.c_str());
    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < STA_BOOT_TIMEOUT_MS) {
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      staRecoveryRestartMarker = 0;
      staCrossChannelRestartBlocked = false;
      Serial.printf("[STA] connected: IP %s, channel %u\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
    } else {
      Serial.println("[STA] router unavailable; automatic reconnect remains active");
    }
  } else {
    Serial.println("[STA] ERROR: no router credentials configured");
    staRecoveryRestartMarker = 0;
  }

  // mDNS is advertised on the router LAN.
  if (WiFi.status() == WL_CONNECTED && MDNS.begin("bus-led")) {
    mdnsStarted = true;
    MDNS.addService("ws", "tcp", 81);
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] responder up: bus-led.local (WS port 81)");
  } else if (WiFi.status() == WL_CONNECTED) {
    mdnsStarted = false;
    Serial.println("[mDNS] start failed");
  }

  staRecoveryPhase = StaRecoveryPhase::Idle;
  Serial.println("[WiFi] STA-only router mode; Master AP disabled");
}

// ─── STA credential persistence (NVS) ──────────────────────────────────
// Saved whenever the headunit sends an explicit ssid/pass, so the next boot
// can auto-reconnect without the driver re-entering it every day.
static void saveStaCreds(const String& ssid, const String& pass) {
  prefs.begin("sta", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.printf("[STA] credentials saved to NVS: %s\n", ssid.c_str());
}

static bool loadStaCreds(String& ssid, String& pass) {
  prefs.begin("sta", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

// ─── STA control (triggered by headunit request) ─────────────────────
// Save runtime credential changes and apply them through a controlled reboot.
// This prevents a live seat AP from hopping channel without warning.
static void staConnect(const String& ssid, const String& pass) {
  if ((ssid.length() && ssid != STA_SSID) ||
      (pass.length() && pass != STA_PASS)) {
    Serial.printf("[STA] fixed fleet router is '%s'; credential change rejected\n",
                  STA_SSID);
    return;
  }
  const char* s = ssid.length() ? ssid.c_str() : STA_SSID;
  if (s[0] == '\0') {
    Serial.println("[STA] connect requested but no SSID configured/provided");
    return;
  }
  // Explicit credentials must be saved even when the SSID name is unchanged
  // because the hotspot password may have been updated.
  if (ssid.length()) {
    saveStaCreds(ssid, pass);
  } else if (WiFi.status() == WL_CONNECTED &&
             wifiSsidEquals(WiFi.SSID().c_str(), s)) {
    Serial.printf("[STA] already connected to '%s'\n", s);
    return;
  }
  configuredStaSsid = s;
  configuredStaPass = ssid.length() ? pass : String(STA_PASS);
  staRecoveryEnabled = true;
  staRecoveryPhase = StaRecoveryPhase::Idle;
  staRecoveryRestartMarker = 0;  // explicit credentials get one clean boot attempt
  staCrossChannelRestartBlocked = false;
  WiFi.scanDelete();
  Serial.printf("[STA] credentials ready for '%s'; controlled reboot scheduled\n", s);
  wifiRestartPending = true;
  wifiRestartAt = millis() + 700;
}

static void staDisconnect() {
  Serial.println("[STA] disconnect requested");
  staRecoveryEnabled = false;
  staRecoveryPhase = StaRecoveryPhase::Idle;
  WiFi.scanDelete();
  WiFi.disconnect();
}

#ifdef ENABLE_NAPT
// ─── Internet sharing (NAT) — Arduino core 3.x only ───────────────────
// Routes AP clients (headunit, seats, gateway) out through the STA uplink:
//     hotspot ──▶ STA ══NAT══▶ AP ──▶ headunit
// Core 2.0.17's lwIP has NAPT compiled out entirely, which is why this is
// guarded and only built in the `napt` env. Called once the STA link is up
// (NAT needs a live uplink to translate to).
// Tell the AP's DHCP server to hand clients a DNS server. Without this they
// get an IP and a default route but cannot resolve any hostname — which looks
// exactly like "no internet".
//
// Called once from wifiInit(), BEFORE any client has associated. Changing a
// DHCP option requires stopping the DHCP server, so doing it later (when the
// uplink comes up) would kick every connected seat and the headunit off —
// which is exactly what was observed in the field. At boot it costs nothing.
//
// A fixed public resolver is used because the upstream DNS isn't known until
// the STA associates, which is precisely the moment we're avoiding. Once NAT
// is on, queries to it are translated out through the uplink normally.
static void apAdvertiseDns() {
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) {
    Serial.println("[NAT] AP netif lookup failed — DNS not advertised");
    return;
  }

  esp_netif_dns_info_t dns = {};
  dns.ip.type            = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = (uint32_t)IPAddress(8, 8, 8, 8);

  uint8_t offerDns = 2;   // DHCPS_OFFER_DNS
  esp_netif_dhcps_stop(ap);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                         &offerDns, sizeof(offerDns));
  esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_start(ap);
  Serial.println("[NAT] AP DHCP will advertise DNS 8.8.8.8");
}

// Turn on NAT once the uplink is live. Deliberately does NOT touch the DHCP
// server — see apAdvertiseDns() above — so enabling internet no longer
// disconnects the seats or the headunit.
static void enableInternetSharing() {
  if (!WiFi.AP.enableNAPT(true)) {
    Serial.println("[NAT] enableNAPT() failed");
    return;
  }
  Serial.println("[NAT] enabled — AP clients now share internet");
}
#endif  // ENABLE_NAPT

// Build a status line the headunit can parse, e.g.
// {"status":"wifi","connected":true,"ssid":"iot_bus","ip":"192.168.1.42"}
static String wifiStatusJson() {
  DynamicJsonDocument doc(192);
  bool up = (WiFi.status() == WL_CONNECTED);
  doc["status"]    = "wifi";
  doc["connected"] = up;
  doc["ssid"]      = up ? WiFi.SSID()               : String("");
  doc["ip"]        = up ? WiFi.localIP().toString() : String("");
  String out; serializeJson(doc, out); return out;
}

// ─── Safe hotspot recovery ───────────────────────────────────────────
// Scanning is asynchronous so WebSocket, LED effects and seat processing keep
// running. A same-channel hotspot can be joined without moving the seat AP.
// A different-channel hotspot requires a controlled reboot, deferred until no
// call is active/pending and no audio announcement is playing or queued.
static bool recoveryCallBusy() {
  if (audioAnnouncementActive) return true;
  if (callQueue && uxQueueMessagesWaiting(callQueue) > 0) return true;
  for (int i = 0; i < MAX_SEATS; ++i) {
    if (seats[i].used && (seats[i].calling || seats[i].callPending)) return true;
  }
  return false;
}

static void recoveryScheduleScan(unsigned long now, unsigned long delayMs) {
  if (!staRecoveryEnabled || configuredStaSsid.length() == 0) {
    staRecoveryPhase = StaRecoveryPhase::Idle;
    return;
  }
  staRecoveryPhase = StaRecoveryPhase::WaitingToScan;
  staRecoveryAt = now + delayMs;
}

static void handleStaRecovery(unsigned long now) {
  if (!staRecoveryEnabled || wifiRestartPending) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (staRecoveryPhase != StaRecoveryPhase::Idle) {
      WiFi.scanDelete();
      staRecoveryPhase = StaRecoveryPhase::Idle;
      Serial.printf("[Recovery] hotspot restored: %s, IP %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (staRecoveryPhase == StaRecoveryPhase::Idle) {
    recoveryScheduleScan(now, STA_RECOVERY_INITIAL_DELAY_MS);
    Serial.printf("[Recovery] uplink lost; scan scheduled in %lus\n",
                  STA_RECOVERY_INITIAL_DELAY_MS / 1000);
    return;
  }

  if (staRecoveryPhase == StaRecoveryPhase::WaitingToScan) {
    if ((int32_t)(now - staRecoveryAt) < 0) return;
    WiFi.scanDelete();
    const int16_t result = WiFi.scanNetworks(
      true, false, false, 120, 0, configuredStaSsid.c_str(), nullptr);
    if (result == WIFI_SCAN_RUNNING) {
      staRecoveryPhase = StaRecoveryPhase::Scanning;
      Serial.printf("[Recovery] scanning for '%s' without changing AP channel %u\n",
                    configuredStaSsid.c_str(), activeApChannel);
    } else {
      Serial.printf("[Recovery] scan start failed (%d); retrying later\n", result);
      recoveryScheduleScan(now, STA_RECOVERY_SCAN_INTERVAL_MS);
    }
    return;
  }

  if (staRecoveryPhase == StaRecoveryPhase::Scanning) {
    const int16_t count = WiFi.scanComplete();
    if (count == WIFI_SCAN_RUNNING) return;
    if (count <= 0) {
      WiFi.scanDelete();
      Serial.println("[Recovery] hotspot not found; keeping seat AP unchanged");
      recoveryScheduleScan(now, STA_RECOVERY_SCAN_INTERVAL_MS);
      return;
    }

    int best = -1;
    int32_t bestRssi = -1000;
    for (int i = 0; i < count; ++i) {
      if (WiFi.SSID(i) == configuredStaSsid && WiFi.RSSI(i) > bestRssi) {
        best = i;
        bestRssi = WiFi.RSSI(i);
      }
    }

    if (best < 0) {
      WiFi.scanDelete();
      recoveryScheduleScan(now, STA_RECOVERY_SCAN_INTERVAL_MS);
      return;
    }

    recoveryFoundChannel = static_cast<uint8_t>(WiFi.channel(best));
    uint8_t foundBssid[6] = {0};
    const bool haveBssid = WiFi.BSSID(best, foundBssid) != nullptr;
    Serial.printf("[Recovery] found '%s': channel %u, RSSI %ld dBm\n",
                  configuredStaSsid.c_str(), recoveryFoundChannel, (long)bestRssi);

    if (recoveryFoundChannel == activeApChannel && haveBssid) {
      WiFi.scanDelete();
      WiFi.begin(configuredStaSsid.c_str(), configuredStaPass.c_str(),
                 recoveryFoundChannel, foundBssid, true);
      staRecoveryPhase = StaRecoveryPhase::ConnectingSameChannel;
      staRecoveryAt = now + STA_RECOVERY_CONNECT_TIMEOUT_MS;
      Serial.println("[Recovery] same channel; reconnecting without moving seat AP");
    } else if (staCrossChannelRestartBlocked) {
      WiFi.scanDelete();
      Serial.println(
        "[Recovery] hotspot is on another channel, but auto-restart is blocked "
        "after a failed recovery boot; use restart or update credentials");
      recoveryScheduleScan(now, STA_RECOVERY_SCAN_INTERVAL_MS * 4);
    } else {
      WiFi.scanDelete();
      staRecoveryPhase = StaRecoveryPhase::WaitingSafeRestart;
      staRecoveryLogAt = 0;
      Serial.printf("[Recovery] channel change %u -> %u requires controlled restart\n",
                    activeApChannel, recoveryFoundChannel);
    }
    return;
  }

  if (staRecoveryPhase == StaRecoveryPhase::ConnectingSameChannel) {
    if (WiFi.status() == WL_CONNECTED) return;  // handled at top next loop
    if ((int32_t)(now - staRecoveryAt) >= 0) {
      Serial.println("[Recovery] same-channel reconnect timed out");
      WiFi.disconnect();
      recoveryScheduleScan(now, STA_RECOVERY_SCAN_INTERVAL_MS);
    }
    return;
  }

  if (staRecoveryPhase == StaRecoveryPhase::WaitingSafeRestart) {
    if (recoveryCallBusy()) {
      if (staRecoveryLogAt == 0 || now - staRecoveryLogAt >= 10000) {
        staRecoveryLogAt = now;
        Serial.println("[Recovery] restart deferred: active/pending seat call or audio");
      }
      return;
    }
    Serial.printf("[Recovery] call path idle; restarting safely for hotspot channel %u\n",
                  recoveryFoundChannel);
    staRecoveryPhase = StaRecoveryPhase::Idle;
    staRecoveryRestartMarker = STA_RECOVERY_RESTART_MAGIC;
    wifiRestartPending = true;
    wifiRestartAt = now + 1000;
  }
}

/* -- Inverter relay disabled --
// The Inverter module (separate ESP32, plain HTTP :80, GET /data) is a
// station on our own AP. We poll it on its own task and hand the headunit
// the readings over the WebSocket link it already has — one connection for
// both seats and power telemetry, instead of the tablet juggling two.
struct InverterData {
  float vac = 0, cac = 0, pac = 0, eac = 0, pfac = 0, freqac = 0, vdc = 0;
  bool  online = false;
};
static InverterData    inverterData;
static SemaphoreHandle_t inverterMutex;
static IPAddress       inverterIP;
static bool            inverterIPKnown = false;

// Resolve bus-inverter.local via mDNS, falling back to INVERTER_FALLBACK_IP
// if that's set and mDNS comes up empty (some routers/hardware are flaky
// with mDNS between two stations on the same softAP).
static bool resolveInverterIP() {
  IPAddress ip = MDNS.queryHost(INVERTER_HOSTNAME, 3000);
  if (ip != IPAddress(0, 0, 0, 0)) {
    inverterIP = ip;
    inverterIPKnown = true;
    Serial.printf("[Inverter] resolved %s.local → %s\n", INVERTER_HOSTNAME, ip.toString().c_str());
    return true;
  }
  if (INVERTER_FALLBACK_IP[0] != '\0' && inverterIP.fromString(INVERTER_FALLBACK_IP)) {
    inverterIPKnown = true;
    Serial.printf("[Inverter] mDNS failed — using fallback IP %s\n", INVERTER_FALLBACK_IP);
    return true;
  }
  Serial.println("[Inverter] could not resolve bus-inverter.local");
  return false;
}

// Runs on its own task so a slow/unreachable Inverter never stalls the
// WebSocket loop() or seat processing.
void inverterPollTask(void*) {
  for (;;) {
    if (!inverterIPKnown && !resolveInverterIP()) {
      vTaskDelay(pdMS_TO_TICKS(3000));   // back off, then retry resolution
      continue;
    }

    HTTPClient http;
    http.begin("http://" + inverterIP.toString() + "/data");
    http.setTimeout(1500);
    int code = http.GET();

    if (code == 200) {
      DynamicJsonDocument doc(256);
      if (!deserializeJson(doc, http.getString())) {
        xSemaphoreTake(inverterMutex, portMAX_DELAY);
        inverterData.vac    = doc["vac"]    | 0.0f;
        inverterData.cac    = doc["cac"]    | 0.0f;
        inverterData.pac    = doc["pac"]    | 0.0f;
        inverterData.eac    = doc["eac"]    | 0.0f;
        inverterData.pfac   = doc["pfac"]   | 0.0f;
        inverterData.freqac = doc["freqac"] | 0.0f;
        inverterData.vdc    = doc["vdc"]    | 0.0f;
        inverterData.online = true;
        xSemaphoreGive(inverterMutex);
      }
    } else {
      xSemaphoreTake(inverterMutex, portMAX_DELAY);
      inverterData.online = false;
      xSemaphoreGive(inverterMutex);
      inverterIPKnown = false;   // DHCP lease may have changed — re-resolve next round
      Serial.printf("[Inverter] poll failed (code %d) — will re-resolve\n", code);
    }
    http.end();

    vTaskDelay(pdMS_TO_TICKS(2000));   // matches the Inverter's own sensor refresh rate
  }
}

// e.g. {"status":"inverter","online":true,"vac":220.1,"cac":1.234,"pac":271.5,
//       "eac":12.34,"pfac":0.98,"freqac":50.0,"vdc":12.65}
static String inverterStatusJson() {
  DynamicJsonDocument doc(256);
  xSemaphoreTake(inverterMutex, portMAX_DELAY);
  doc["status"]  = "inverter";
  doc["online"]  = inverterData.online;
  doc["vac"]     = inverterData.vac;
  doc["cac"]     = inverterData.cac;
  doc["pac"]     = inverterData.pac;
  doc["eac"]     = inverterData.eac;
  doc["pfac"]    = inverterData.pfac;
  doc["freqac"]  = inverterData.freqac;
  doc["vdc"]     = inverterData.vdc;
  xSemaphoreGive(inverterMutex);
  String out; serializeJson(doc, out); return out;
}
-- end Inverter relay disabled -- */

/* -- reverted: MQTT broker init/publish replaced by HTTP server (handleCommand/handleData) --
static void mqttBrokerInit() {
  mqttBroker.onMessage = [](const String& topic, const String& payload) {
    Serial.printf("[MQTT Broker] %s → %s\n", topic.c_str(), payload.c_str());

    if (topic == MQTT_TOPIC_CMD) {
      String ack = processCommand(payload)
        ? "{\"status\":\"ok\",\"cmd\":\"" + payload + "\"}"
        : "{\"status\":\"error\",\"msg\":\"unknown cmd\"}";
      mqttBroker.publish(MQTT_TOPIC_STATUS, ack);

    } else if (topic == MQTT_TOPIC_SEAT_EVENT) {
      int stIdx = payload.indexOf("\"sensorState\":");
      mqttSeatState    = (stIdx >= 0) ? payload.substring(stIdx + 14).toInt() : 0;
      mqttSeatOccupied = payload.indexOf("\"seatOccupied\":true") >= 0;
      int idIdx = payload.indexOf("\"id\":\"");
      if (idIdx >= 0) {
        int idEnd = payload.indexOf("\"", idIdx + 6);
        payload.substring(idIdx + 6, idEnd).toCharArray(mqttSeatId, sizeof(mqttSeatId));
      }
      mqttSeatReceived = true;
      mqttBroker.publish(MQTT_TOPIC_SENSOR, payload);  // forward to subscribers
    }
  };

  mqttBroker.begin();
  Serial.printf("[MQTT Broker] Running on %s:1883\n",
                WiFi.softAPIP().toString().c_str());
}

void mqttPublishSensor(const char* id, int state, bool occupied) {
  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"id\":\"%s\",\"sensorState\":%d,\"seatOccupied\":%s}",
    id, state, occupied ? "true" : "false");
  mqttBroker.publish(MQTT_TOPIC_SENSOR, String(buf));
  Serial.printf("[MQTT Broker] Published: %s\n", buf);
}
-- end reverted MQTT broker block -- */

// ─── LED settings persistence (NVS / Preferences) ────────────────────
static void saveSettings() {
  prefs.begin("led", false);  // read-write
  prefs.putBool ("on",  ledOn);
  prefs.putUChar("bri", ledBrightness);
  prefs.putUChar("eff", (uint8_t)ledEffect);
  prefs.putUChar("r",   ledR);
  prefs.putUChar("g",   ledG);
  prefs.putUChar("b",   ledB);
  prefs.end();
}

static void loadSettings() {
  prefs.begin("led", true);   // read-only (returns defaults if unset)
  ledOn         = prefs.getBool ("on",  ledOn);
  ledBrightness = prefs.getUChar("bri", ledBrightness);
  ledEffect     = (LedEffect)prefs.getUChar("eff", (uint8_t)ledEffect);
  ledR          = prefs.getUChar("r",   ledR);
  ledG          = prefs.getUChar("g",   ledG);
  ledB          = prefs.getUChar("b",   ledB);
  prefs.end();
  Serial.printf("[NVS] loaded: on=%d bri=%d eff=%d rgb=%d,%d,%d\n",
                ledOn, ledBrightness, ledEffect, ledR, ledG, ledB);
}

// ─── Shared Command Processor ─────────────────────────────────────────
bool processCommand(const String& cmd) {
  if (cmd == "on" || cmd == "ws_on") {
    ledOn = true;
    Serial.println("WS2811 ON");
  }
  else if (cmd == "off" || cmd == "ws_off") {
    ledOn = false;
    Serial.println("WS2811 OFF");
  }
  else if (cmd.startsWith("dimmer_")) {
    int val = cmd.substring(7).toInt();
    ledBrightness = map(constrain(val, 0, 100), 0, 100, 0, 255);
    Serial.printf("Dimmer→brightness: %d%% = %d\n", val, ledBrightness);
  }
  else if (cmd.startsWith("rgb_")) {
    int first  = cmd.indexOf('_');
    int second = cmd.indexOf('_', first + 1);
    int third  = cmd.lastIndexOf('_');
    ledOn = false;
    ledR = cmd.substring(first + 1, second).toInt();
    ledG = cmd.substring(second + 1, third).toInt();
    ledB = cmd.substring(third + 1).toInt();
    ledEffect = EFF_SOLID;
    ledOn = true;
    Serial.printf("RGB→WS2811 color: R:%d G:%d B:%d\n", ledR, ledG, ledB);
  }
  else if (cmd.startsWith("ws_color_")) {
    String vals = cmd.substring(9);
    int first  = vals.indexOf('_');
    int second = vals.lastIndexOf('_');
    ledR = vals.substring(0, first).toInt();
    ledG = vals.substring(first + 1, second).toInt();
    ledB = vals.substring(second + 1).toInt();
    Serial.printf("WS2811 color: R:%d G:%d B:%d\n", ledR, ledG, ledB);
  }
  else if (cmd.startsWith("ws_brightness_")) {
    ledBrightness = constrain(cmd.substring(14).toInt(), 0, 255);
    Serial.printf("WS2811 brightness: %d\n", ledBrightness);
  }
  else if (cmd.startsWith("bright_")) {
    ledBrightness = constrain(cmd.substring(7).toInt(), 0, 255);
    Serial.printf("WS2811 brightness: %d\n", ledBrightness);
  }
  else if (cmd.startsWith("mode_")) {
    String eff = cmd.substring(5);
    if      (eff == "1")     ledEffect = EFF_RAINBOW_GLITTER;
    else if (eff == "2")     ledEffect = EFF_CONFETTI_PARTY;
    else if (eff == "3")     ledEffect = EFF_SINELON_WAVE;
    else if (eff == "4")     ledEffect = EFF_BPM_RHYTHM;
    else if (eff == "5")     ledEffect = EFF_JUGGLE_BOUNCE;
    else if (eff == "6")     ledEffect = EFF_FIRE_FLAME;
    else if (eff == "7")     ledEffect = EFF_AURORA_GLOW;
    else if (eff == "8")     ledEffect = EFF_BINARY_COUNTER;
    else if (eff == "9")     ledEffect = EFF_PALETTE_WAVE;
    else if (eff == "10")    ledEffect = EFF_DUAL_SCANNER;
    else if (eff == "11")    ledEffect = EFF_COLOR_BOUNCE;
    else if (eff == "12")    ledEffect = EFF_SNAKE;
    else return false;
    Serial.printf("WS2811 effect: %s\n", eff.c_str());
  }
  // else if (cmd == "wifi_reset") { }  // WiFiManager removed
  else {
    return false;
  }
  saveSettings();  // persist the change so it survives reboot
  return true;
}

// ─── HTTP Client: POST sensorState to headunit ───────────────────────
// -- reverted: this made the ESP32 an HTTP *client* pushing to the headunit.
//    We want the ESP32 to be the *server* instead (Android hits handleData),
//    so this client function is disabled. --
/*
void sendToHeadunit(int sensorState, long reading, bool seatOccupied = false) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://" + headunitIP + ":8080/data");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  char body[80];
  snprintf(body, sizeof(body), "{\"sensorState\":%d,\"reading\":%ld,\"seatOccupied\":%s}",
           sensorState, reading, seatOccupied ? "true" : "false");
  int code = http.POST(String(body));

  if (code > 0) {
    Serial.printf("[Headunit] POST %d → %s\n", code, body);
  } else {
    Serial.printf("[Headunit] POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}
*/

struct ApClientView {
  char name[32];
  char ip[16];
  char mac[18];
  int8_t rssi;
};

static void setWsPeer(uint8_t client, const char* role, const char* name) {
  if (client >= WEBSOCKETS_SERVER_CLIENT_MAX) return;
  wsPeers[client].used = true;
  strlcpy(wsPeers[client].role, role, sizeof(wsPeers[client].role));
  strlcpy(wsPeers[client].name, name, sizeof(wsPeers[client].name));
}

static const char* apClientNameForIp(const IPAddress& ip) {
  for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (wsPeers[i].used && wsPeers[i].ip == ip && wsPeers[i].name[0]) {
      return wsPeers[i].name;
    }
  }
  if (webMonitorClientIp == ip) return "Web monitor";
  return "Unknown device";
}

static int getApClientViews(ApClientView* output, int capacity) {
  wifi_sta_list_t wifiClients = {};
  wifi_sta_mac_ip_list_t ipClients = {};
  if (!output || capacity <= 0 ||
      esp_wifi_ap_get_sta_list(&wifiClients) != ESP_OK) return 0;
  const bool haveIp = esp_wifi_ap_get_sta_list_with_ip(&wifiClients, &ipClients) == ESP_OK;
  const int count = min((int)wifiClients.num, capacity);

  for (int i = 0; i < count; ++i) {
    IPAddress ip;
    if (haveIp && i < ipClients.num) ip = IPAddress(ipClients.sta[i].ip.addr);
    strlcpy(output[i].name, apClientNameForIp(ip), sizeof(output[i].name));
    strlcpy(output[i].ip, ip.toString().c_str(), sizeof(output[i].ip));
    snprintf(output[i].mac, sizeof(output[i].mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             wifiClients.sta[i].mac[0], wifiClients.sta[i].mac[1],
             wifiClients.sta[i].mac[2], wifiClients.sta[i].mac[3],
             wifiClients.sta[i].mac[4], wifiClients.sta[i].mac[5]);
    output[i].rssi = wifiClients.sta[i].rssi;
  }
  return count;
}

static void printApClientTable() {
  ApClientView clients[MASTER_AP_MAX_CLIENTS];
  const int count = getApClientViews(clients, MASTER_AP_MAX_CLIENTS);
  Serial.println(F("\nAP CLIENT NAME                 IP              MAC                RSSI"));
  Serial.println(F("------------------------------ --------------- ----------------- ----"));
  if (!count) Serial.println(F("(no AP client connected)"));
  for (int i = 0; i < count; ++i) {
    Serial.printf("%-30s %-15s %-17s %d dBm\n", clients[i].name, clients[i].ip,
                  clients[i].mac, (int)clients[i].rssi);
  }
}

// ─── WebSocket event handler — Android app connects as a ws:// client ─
// Client sends a command string as a TEXT frame (same vocabulary as the old
// HTTP GET /command?cmd=...); we run processCommand and reply with a JSON ack.
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[WS] #%u connected from %s\n", num, ip.toString().c_str());
      if (num < WEBSOCKETS_SERVER_CLIENT_MAX) {
        wsPeers[num] = {};
        wsPeers[num].used = true;
        wsPeers[num].ip = ip;
        strlcpy(wsPeers[num].role, "unknown", sizeof(wsPeers[num].role));
        strlcpy(wsPeers[num].name, "WebSocket client", sizeof(wsPeers[num].name));
      }
      // Ask the client to identify its role. Seat events remain active even
      // without a headunit so the local web monitor and call audio still work.
      webSocket.sendTXT(num, "{\"status\":\"identify\"}");
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS] #%u disconnected\n", num);
      pairingDropClient(num);
      if (num < WEBSOCKETS_SERVER_CLIENT_MAX) wsPeers[num] = {};
      if (num == (uint8_t)headunitNum) {
        headunitNum = -1;
        Serial.println("[WS] Headunit gone — local seat monitoring remains active");
      }
      break;
    case WStype_TEXT: {
      String msg = String((char*)payload);
      Serial.println("[WS] rx: " + msg);

      // ── Headunit registration ─────────────────────────────────────
      // Headunit must send {"role":"headunit"} right after connecting.
      if (msg.indexOf("\"role\":\"headunit\"") >= 0) {
        headunitNum = (int)num;
        setWsPeer(num, "headunit", "Head unit");
        Serial.printf("[WS] Headunit registered as client #%u\n", num);
        String roster = rosterJson();
        webSocket.sendTXT(num, roster);   // push current state immediately
        for (int i = 0; i < MAX_PENDING_PAIRINGS; ++i) {
          if (pendingPairings[i].used) sendPairingRequest(num, pendingPairings[i].uid);
        }
        break;
      }

      if (msg.indexOf("\"role\":\"gateway\"") >= 0) {
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, msg) == DeserializationError::Ok) {
          const char* gatewayName = doc["id"] | "Gateway";
          setWsPeer(num, "gateway", gatewayName);
          Serial.printf("[WS] %s registered as client #%u\n", gatewayName, num);
        }
        break;
      }

      // Seat confirms that the selected ID was committed to its own NVS.
      if (msg.indexOf("\"event\":\"seatIdSaved\"") >= 0) {
        StaticJsonDocument<192> doc;
        if (deserializeJson(doc, msg) == DeserializationError::Ok) {
          const char* uid = doc["uid"] | "";
          pairingClear(uid);
          if (headunitNum >= 0) webSocket.sendTXT((uint8_t)headunitNum, msg);
          Serial.printf("[Pairing] seat UID %s saved as %s\n",
                        uid, doc["id"] | "");
        }
        break;
      }

      // ── Seat health report ────────────────────────────────────────
      // Logged unconditionally (not gated on the headunit) — this is the
      // only visibility into a seat once its USB cable is removed, and it
      // must stay readable precisely when things are going wrong.
      if (msg.indexOf("\"diag\":true") >= 0) {
        Serial.printf("[Diag] %s\n", msg.c_str());
        break;
      }

      // ── Seat node events — processed independently of the headunit ───
      int stIdx = msg.indexOf("\"sensorState\":");
      if (stIdx >= 0) {
        int  state    = msg.substring(stIdx + 14).toInt();
        bool occupied = msg.indexOf("\"seatOccupied\":true") >= 0;
        bool seatbelt = msg.indexOf("\"seatbelt\":true") >= 0;

        char id[32] = {0};
        char uid[13] = {0};
        int idIdx = msg.indexOf("\"id\":\"");
        if (idIdx >= 0) {
          int idEnd = msg.indexOf("\"", idIdx + 6);
          msg.substring(idIdx + 6, idEnd).toCharArray(id, sizeof(id));
        }
        int uidIdx = msg.indexOf("\"uid\":\"");
        if (uidIdx >= 0) {
          int uidEnd = msg.indexOf("\"", uidIdx + 7);
          msg.substring(uidIdx + 7, uidEnd).toCharArray(uid, sizeof(uid));
        }

        const bool unassigned = strcmp(id, "RandomID") == 0 ||
                                msg.indexOf("\"unassigned\":true") >= 0;
        if (unassigned) {
          // Heartbeats register connectivity but only a real button call opens
          // the popup, as requested by the provisioning workflow.
          if (state && strlen(uid) == 12) {
            int slot = pairingRemember(uid, num);
            if (slot < 0) {
              Serial.println("[Pairing] pending table full");
            } else if (headunitNum >= 0) {
              sendPairingRequest((uint8_t)headunitNum, uid);
            } else {
              Serial.printf("[Pairing] UID %s waiting for head unit\n", uid);
            }
          }
          break;  // RandomID must never enter the roster or audio queue
        }

        if (num < WEBSOCKETS_SERVER_CLIENT_MAX &&
            strcmp(wsPeers[num].role, "gateway") != 0) {
          setWsPeer(num, "seat", id);
        }

        const bool seatbeltRaised = rosterUpdate(id, state, occupied, seatbelt);

        // -- was: digitalWrite(LED_BUILTIN, state ? HIGH : LOW);
        //    The builtin LED now reports upstream internet status instead
        //    (see loop()), which is far more useful on a headless master
        //    than mirroring the last seat's button. --
        if (seatbeltRaised) {
          int slot = rosterFind(id);
          if (slot >= 0 && (millis() - seats[slot].lastSeatbeltWarnTime >= 5000)) {
            seats[slot].lastSeatbeltWarnTime = millis();
            CallEvent belt;
            strncpy(belt.id, id, sizeof(belt.id) - 1);
            belt.id[sizeof(belt.id) - 1] = '\0';
            belt.kind = CALL_SEATBELT;
            xQueueSend(callQueue, &belt, 0);
          }
        }
        if (state) {
          CallEvent ev;
          strncpy(ev.id, id, sizeof(ev.id) - 1);
          ev.id[sizeof(ev.id) - 1] = '\0';
          ev.kind = CALL_SEAT;
          xQueueSend(callQueue, &ev, 0);
        }

      // ── Headunit commands ─────────────────────────────────────────
      } else if (msg.indexOf("\"action\":\"assignSeatId\"") >= 0) {
        StaticJsonDocument<256> doc;
        const DeserializationError jsonError = deserializeJson(doc, msg);
        const char* uid = doc["uid"] | "";
        const char* id  = doc["id"]  | "";
        int slot = pairingFind(uid);
        String response;

        if ((int)num != headunitNum) {
          response = "{\"event\":\"seatPairingResult\",\"success\":false,"
                     "\"error\":\"headunit_required\"}";
        } else if (jsonError || slot < 0) {
          response = "{\"event\":\"seatPairingResult\",\"success\":false,"
                     "\"error\":\"pairing_not_found\"}";
        } else if (!pairingIdValid(id)) {
          response = "{\"event\":\"seatPairingResult\",\"success\":false,"
                     "\"error\":\"invalid_id\"}";
        } else if (rosterHasId(id)) {
          response = "{\"event\":\"seatPairingResult\",\"success\":false,"
                     "\"error\":\"duplicate_id\"}";
        } else {
          char command[128];
          snprintf(command, sizeof(command),
                   "{\"action\":\"setSeatId\",\"uid\":\"%s\",\"id\":\"%s\"}",
                   uid, id);
          const bool dispatched = pendingPairings[slot].viaUdp
            ? udpSendJson(pendingPairings[slot].sourceIp,
                          pendingPairings[slot].sourcePort, command)
            : (webSocket.sendTXT(pendingPairings[slot].sourceClient, command), true);
          char ok[160];
          snprintf(ok, sizeof(ok),
                   "{\"event\":\"seatPairingResult\",\"success\":true,"
                   "\"uid\":\"%s\",\"id\":\"%s\",\"state\":\"dispatched\"}",
                   uid, id);
          response = ok;
          Serial.printf("[Pairing] dispatched ID %s to UID %s via %s%s\n",
                        id, uid,
                        pendingPairings[slot].viaUdp ? "UDP " : "WS client #",
                        pendingPairings[slot].viaUdp
                          ? pendingPairings[slot].sourceIp.toString().c_str()
                          : String(pendingPairings[slot].sourceClient).c_str());
          if (!dispatched) Serial.println("[Pairing] UDP dispatch failed");
        }
        webSocket.sendTXT(num, response);

      } else if (msg == "roster") {
        String roster = rosterJson();
        webSocket.sendTXT(num, roster);
      } else if (msg.indexOf("\"action\":\"clearCall\"") >= 0) {
        char id[32] = {0};
        int idIdx = msg.indexOf("\"id\":\"");
        if (idIdx >= 0) {
          int idEnd = msg.indexOf("\"", idIdx + 6);
          msg.substring(idIdx + 6, idEnd).toCharArray(id, sizeof(id));
        }
        if (id[0] != '\0' && strcmp(id, "all") != 0) {
          rosterClear(id);
        } else {
          for (int i = 0; i < MAX_SEATS; i++) {
            seats[i].calling = false;
            seats[i].callPending = false;
          }
          Serial.println("[Roster] all calls cleared");
        }
        String roster = rosterJson();
        webSocket.broadcastTXT(roster);
      } else if (msg.startsWith("clearCall_")) {
        // Backward compatibility for older headunit builds, which send
        // `clearCall_<id>` instead of the canonical `clear_<id>` command.
        String id = msg.substring(10);
        if (id == "all") {
          for (int i = 0; i < MAX_SEATS; i++) {
            seats[i].calling = false;
            seats[i].callPending = false;
          }
          Serial.println("[Roster] all calls cleared (legacy clearCall_)");
        } else {
          rosterClear(id.c_str());
        }
        String roster = rosterJson();
        webSocket.broadcastTXT(roster);
      } else if (msg.startsWith("clear_")) {
        String id = msg.substring(6);
        if (id == "all") {
          for (int i = 0; i < MAX_SEATS; i++) {
            seats[i].calling = false;
            seats[i].callPending = false;
          }
          Serial.println("[Roster] all calls cleared");
        } else {
          rosterClear(id.c_str());
        }
        String roster = rosterJson();
        webSocket.broadcastTXT(roster);

      // ── Headunit-requested upstream WiFi control ──────────────────
      // Connect: {"action":"wifiConnect"}  (uses configured STA_SSID), or
      //          {"action":"wifiConnect","ssid":"X","pass":"Y"}  (override)
      } else if (msg.indexOf("\"action\":\"wifiConnect\"") >= 0) {
        StaticJsonDocument<256> doc;
        String ssid, pass;
        if (deserializeJson(doc, msg) == DeserializationError::Ok) {
          ssid = String((const char*)(doc["ssid"] | ""));
          pass = String((const char*)(doc["pass"] | ""));
        }
        staConnect(ssid, pass);
        if (wifiRestartPending) {
          webSocket.sendTXT(num,
            "{\"status\":\"wifi\",\"state\":\"saved\",\"restarting\":true}");
        } else {
          String st = wifiStatusJson();
          webSocket.sendTXT(num, st);
        }
      } else if (msg.indexOf("\"action\":\"wifiDisconnect\"") >= 0) {
        staDisconnect();
        String st = wifiStatusJson();
        webSocket.sendTXT(num, st);
      } else if (msg.indexOf("\"action\":\"wifiStatus\"") >= 0) {
        String st = wifiStatusJson();
        webSocket.sendTXT(num, st);   // headunit polls current state
      // -- Inverter relay disabled --
      // } else if (msg == "inverter") {
      //   String inv = inverterStatusJson();
      //   webSocket.sendTXT(num, inv);   // headunit polls current inverter reading

      } else {
        String ack = processCommand(msg)
          ? "{\"status\":\"ok\",\"cmd\":\"" + msg + "\"}"
          : "{\"status\":\"error\",\"msg\":\"unknown cmd\"}";
        webSocket.sendTXT(num, ack);
      }
      break;
    }
    default:
      break;
  }
}

// ─── Serial command handler (laptop ↔ master) ─────────────────────────
// Line-based, one command per line. Commands are case-insensitive; arguments
// keep their case (SSIDs and passwords are case-sensitive).
// Non-blocking: reads at most one line per loop() pass.
static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout/low voltage";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "unknown";
  }
}

static void printSeatTable() {
  const unsigned long now = millis();
  bool found = false;

  Serial.println(F("\nID         LINK     TRANSPORT  IP              LAST     OCCUPIED  CALL  PENDING BELT"));
  Serial.println(F("---------- -------- ---------- --------------- -------- --------- ----- ------- ----"));
  for (int i = 0; i < MAX_SEATS; ++i) {
    if (!seats[i].used) continue;
    found = true;
    const unsigned long ageSeconds = (now - seats[i].lastSeen) / 1000UL;
    const bool online = (now - seats[i].lastSeen) < SEAT_OFFLINE_MS;
    char age[16];
    snprintf(age, sizeof(age), "%lus", ageSeconds);
    const String sourceIp = seats[i].viaUdp ? seats[i].sourceIp.toString() : "-";
    const char* belt = !online ? "-" : (seats[i].seatbeltAlert ? "OPEN"
                                   : seats[i].seatbelt     ? "ok" : "-");
    Serial.printf("%-10s %-8s %-10s %-15s %-8s %-9s %-5s %-7s %-4s\n",
                  seats[i].id,
                  online ? "ONLINE" : "OFFLINE",
                  seats[i].viaUdp ? "UDP" : "WebSocket",
                  sourceIp.c_str(),
                  age,
                  seats[i].occupied ? "YES" : "no",
                  seats[i].calling ? "YES" : "no",
                  seats[i].callPending ? "YES" : "no",
                  belt);
  }
  if (!found) Serial.println(F("(no seat has reported yet)"));
}

static void printMasterStatus() {
  const unsigned long totalSeconds = millis() / 1000UL;
  const unsigned long hours = totalSeconds / 3600UL;
  const unsigned long minutes = (totalSeconds / 60UL) % 60UL;
  const unsigned long seconds = totalSeconds % 60UL;
  int known = 0, online = 0, occupied = 0, pending = 0;
  const unsigned long now = millis();

  for (int i = 0; i < MAX_SEATS; ++i) {
    if (!seats[i].used) continue;
    ++known;
    if ((now - seats[i].lastSeen) < SEAT_OFFLINE_MS) ++online;
    if (seats[i].occupied) ++occupied;
    if (seats[i].callPending) ++pending;
  }

  Serial.println(F("\n========== SMARTBUS MASTER =========="));
  Serial.printf("firmware     : v1.9  built %s %s\n", __DATE__, __TIME__);
  Serial.printf("uptime       : %02lu:%02lu:%02lu\n", hours, minutes, seconds);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("reset reason : %s (%d)\n", resetReasonName(resetReason), (int)resetReason);
  Serial.printf("chip         : %s, %u MHz\n", ESP.getChipModel(), ESP.getCpuFreqMHz());
  Serial.printf("heap         : %u free, %u minimum\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  Serial.println(F("WiFi mode    : STA-only (Master AP disabled)"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("router       : %s  IP %s  RSSI %d dBm\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  (int)WiFi.RSSI());
  } else {
    Serial.println(F("router       : DOWN"));
  }
  Serial.printf("headunit     : %s\n", headunitNum >= 0 ? "REGISTERED" : "NOT REGISTERED");
  Serial.printf("seats        : %d/%d known, %d online, %d occupied, %d pending\n",
                known, MAX_SEATS, online, occupied, pending);
  printSeatTable();
  Serial.println(F("=====================================\n"));
}

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;

  String cmd = line, arg;
  int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    arg = line.substring(sp + 1);
    arg.trim();
  }
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    Serial.println(F(
      "\n--- MASTER commands ---\n"
      "  help                 this list\n"
      "  status               complete master dashboard + seat table\n"
      "  seats                readable seat status table\n"
      "  roster               seat roster as JSON\n"
      "  wifi                 router link status\n"
      "  connect              reconnect fixed iot_bus router\n"
      "  disconnect           temporarily drop router link\n"
      "  clear <id>|all       acknowledge a seat call\n"
      "  restart              reboot the master\n"
      "  <led cmd>            any LED command (on, off, rgb_R_G_B, mode_N, ...)\n"));

  } else if (cmd == "status") {
    printMasterStatus();

  } else if (cmd == "seats") {
    printSeatTable();

  } else if (cmd == "roster") {
    String r = rosterJson();
    Serial.println(r);

  } else if (cmd == "wifi") {
    String w = wifiStatusJson();
    Serial.println(w);

  } else if (cmd == "connect") {
    // "connect" alone reuses the stored credentials; "connect <ssid> <pass>"
    // overrides and persists them.
    String ssid, pass;
    if (arg.length()) {
      int s2 = arg.indexOf(' ');
      if (s2 > 0) { ssid = arg.substring(0, s2); pass = arg.substring(s2 + 1); pass.trim(); }
      else        { ssid = arg; }
    } else {
      loadStaCreds(ssid, pass);  // empty means staConnect() uses firmware default
    }
    staConnect(ssid, pass);

  } else if (cmd == "disconnect") {
    staDisconnect();

  } else if (cmd == "clear") {
    if (arg.length() == 0)      Serial.println("usage: clear <seatId>|all");
    else if (arg == "all")    { for (int i = 0; i < MAX_SEATS; i++) seats[i].callPending = false;
                                Serial.println("all calls cleared"); }
    else if (!rosterClear(arg.c_str())) Serial.printf("seat '%s' not found\n", arg.c_str());

  } else if (cmd == "restart") {
    Serial.println("restarting...");
    delay(200);
    ESP.restart();

  } else if (processCommand(line)) {
    // Any LED/effect command reaches the same handler the WebSocket uses.
    Serial.println("ok");

  } else {
    Serial.printf("unknown command '%s' — type 'help'\n", cmd.c_str());
  }
}

// ─── HTTP Handlers ─── replaced by WebSocket (onWsEvent above) ────────
/*
// Flutter: GET /command?cmd=xxx
void handleCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"missing cmd\"}");
    return;
  }
  String cmd = server.arg("cmd");
  Serial.println("Perintah masuk: " + cmd);

  if (processCommand(cmd)) {
    server.send(200, "application/json", "{\"status\":\"ok\",\"cmd\":\"" + cmd + "\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"unknown cmd\"}");
  }
}

// Headunit: POST /data  body: {"sensorState":1}
// Also auto-updates headunit IP from the request source
void handleData() {
  String remoteIP = server.client().remoteIP().toString();
  if (headunitIP != remoteIP) {
    headunitIP = remoteIP;
    Serial.println("[Headunit] IP updated to " + headunitIP);
  }

  String body = server.arg("plain");
  Serial.println("[Headunit] POST: " + body);

  int idx = body.indexOf("\"sensorState\":");
  if (idx < 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"missing sensorState\"}");
    return;
  }
  int state = body.substring(idx + 14).toInt();
  ledOn = (state == 1);
  Serial.printf("[Headunit] sensorState %d → LED %s\n", state, ledOn ? "ON" : "OFF");

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}
*/

// DY-HV8F integrated I/O mode (DIP 000). All HIGH is idle. Pulling a
// binary combination LOW for a short pulse selects tracks 1..255.
void audioInit() {
  uint32_t allPinsMask = 0;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    allPinsMask |= 1UL << AUDIO_IO_PINS[bit];
  }
  // Preload every output latch HIGH before connecting the pins to GPIO output.
  REG_WRITE(GPIO_OUT_W1TS_REG, allPinsMask);
  for (uint8_t bit = 0; bit < 8; ++bit) {
    pinMode(AUDIO_IO_PINS[bit], OUTPUT);
  }
  Serial.println("[Audio] integrated I/O ready (DIP 000)");
}

void playTrack(int soundTrack) {
  if (soundTrack < 1 || soundTrack > 255) {
    Serial.printf("[Audio] invalid track %d (valid: 1..255)\n", soundTrack);
    return;
  }

  Serial.printf("[Audio] trigger track %d, mask=0x%02X\n",
                soundTrack, soundTrack);
  uint32_t allPinsMask = 0;
  uint32_t lowPinsMask = 0;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    const uint32_t gpioMask = 1UL << AUDIO_IO_PINS[bit];
    allPinsMask |= gpioMask;
    if (soundTrack & (1 << bit)) lowPinsMask |= gpioMask;
  }

  // Apply the complete binary value atomically. Sequential writes could make
  // the module observe an intermediate track number.
  REG_WRITE(GPIO_OUT_W1TC_REG, lowPinsMask);

  delay(150);  // mode 000 continues playback after the key is released

  REG_WRITE(GPIO_OUT_W1TS_REG, allPinsMask);
}

bool waitForAudioDone(unsigned long startTimeoutMs = 1000,
                      unsigned long playTimeoutMs = 10000) {
  unsigned long startedAt = millis();
  while (digitalRead(BUSY_PIN) == HIGH &&
         millis() - startedAt < startTimeoutMs) {
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }

  if (digitalRead(BUSY_PIN) == HIGH) {
    Serial.println("[Audio] warning: BUSY did not go LOW");
    return false;
  }

  unsigned long playingAt = millis();
  while (digitalRead(BUSY_PIN) == LOW &&
         millis() - playingAt < playTimeoutMs) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  if (digitalRead(BUSY_PIN) == LOW) {
    Serial.println("[Audio] warning: BUSY playback timeout");
    return false;
  }

  // DY-HV8F needs recovery time after BUSY returns idle before accepting the
  // next key combination. A short 100 ms gap caused the second track in a
  // phrase to be ignored, while the following track worked after a timeout.
  vTaskDelay(500 / portTICK_PERIOD_MS);
  return true;
}

// ─── Unused effect library ────────────────────────────────────────────
// void rainbow() {
//   static uint8_t hue = 0;
//   fill_rainbow(leds, NUM_LEDS, hue++, 7);
//   FastLED.show(); FastLED.delay(30);
// }
// void rainbowWithGlitter() {
//   static uint8_t hue = 0;
//   fill_rainbow(leds, NUM_LEDS, hue++, 7);
//   if(random8() < 80) leds[random16(NUM_LEDS)] += CRGB::White;
//   FastLED.show(); FastLED.delay(30);
// }
// void confetti() {
//   fadeToBlackBy(leds, NUM_LEDS, 10);
//   int pos = random16(NUM_LEDS);
//   leds[pos] += CHSV(random8(), 200, 255);
//   FastLED.show(); FastLED.delay(50);
// }
// void sinelon() {
//   static uint8_t hue = 0;
//   fadeToBlackBy(leds, NUM_LEDS, 20);
//   int pos = beatsin16(13, 0, NUM_LEDS-1);
//   leds[pos] += CHSV(hue++, 255, 192);
//   FastLED.show(); FastLED.delay(30);
// }
// void bpm() {
//   static uint8_t hue = 0;
//   uint8_t beat = beatsin8(62, 64, 255);
//   for(int i = 0; i < NUM_LEDS; i++) leds[i] = CHSV(hue + (i*2), 255, beat);
//   hue++; FastLED.show(); FastLED.delay(20);
// }
// void juggle() {
//   static uint8_t hue = 0;
//   fadeToBlackBy(leds, NUM_LEDS, 20);
//   for(int i = 0; i < 8; i++) leds[beatsin16(i+7, 0, NUM_LEDS-1)] |= CHSV(hue + i*32, 200, 255);
//   hue++; FastLED.show(); FastLED.delay(20);
// }
// void fire() {
//   static byte heat[NUM_LEDS];
//   for(int i = 0; i < NUM_LEDS; i++) heat[i] = qsub8(heat[i], random8(0, 30));
//   for(int k = NUM_LEDS-1; k >= 2; k--) heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
//   if(random8() < 40) { int y = random8(7); heat[y] = qadd8(heat[y], random8(160,255)); }
//   for(int j = 0; j < NUM_LEDS; j++) leds[j] = HeatColor(heat[j]);
//   FastLED.show(); FastLED.delay(33);
// }
// void meteor() {
//   static int meteorSize = 10, meteorTrailDecay = 64, pos = 0;
//   for(int i = 0; i < NUM_LEDS; i++) if(random8() > 35) leds[i].fadeToBlackBy(meteorTrailDecay);
//   for(int j = 0; j < meteorSize; j++) if((pos-j < NUM_LEDS) && (pos-j >= 0)) leds[pos-j] = CRGB::White;
//   pos++; if(pos > NUM_LEDS + meteorSize) pos = 0;
//   FastLED.show(); FastLED.delay(30);
// }
// void waveRider() {
//   static uint8_t hue = 0; static int pos = 0;
//   fadeToBlackBy(leds, NUM_LEDS, 30);
//   leds[pos] = CHSV(hue, 255, 255);
//   leds[(pos + NUM_LEDS/2) % NUM_LEDS] = CHSV(hue + 128, 255, 255);
//   if(pos > 1) {
//     leds[pos-1] = CHSV(hue, 255, 150);
//     leds[(pos + NUM_LEDS/2 - 1) % NUM_LEDS] = CHSV(hue + 128, 255, 150);
//   }
//   pos = (pos + 1) % NUM_LEDS; hue += 3;
//   FastLED.show(); FastLED.delay(50);
// }
// void pixelDance() {
//   static uint8_t hue = 0;
//   uint8_t numPixels = random8(3, 6);
//   fill_solid(leds, NUM_LEDS, CRGB::Black);
//   for(int i = 0; i < numPixels; i++) leds[random8(NUM_LEDS)] = CHSV(hue + (i * 30), 255, 255);
//   hue += 10; FastLED.show(); FastLED.delay(200);
// }
// void colorSymphony() {
//   static uint8_t hue = 0;
//   for(int i = 0; i < NUM_LEDS; i++) leds[i] = CHSV(hue + (i * 5), 255, 255);
//   hue += 2; FastLED.show(); FastLED.delay(50);
// }
// void binaryCounter() {
//   static uint8_t counter = 0;
//   for(int i = 0; i < 8; i++) {
//     leds[i]   = (counter & (1<<i)) ? CRGB::Blue  : CRGB::Black;
//     leds[i+8] = (counter & (1<<i)) ? CRGB::Red   : CRGB::Black;
//     if(i < 4) leds[i+16] = (counter & (1<<i)) ? CRGB::Green : CRGB::Black;
//   }
//   counter++; FastLED.show(); FastLED.delay(500);
// }
// void auroraEffect() {
//   static uint8_t hue = 0;
//   for(int i = 0; i < NUM_LEDS; i++) {
//     uint8_t v = (beatsin8(5,0,255,0,i*10) + beatsin8(7,0,255,0,i*10+85) + beatsin8(4,0,255,0,i*10+170)) / 3;
//     leds[i] = CHSV(hue + (i * 2), 200, v);
//   }
//   hue++; FastLED.show(); FastLED.delay(30);
// }
// void matrixRain() {
//   for(int i = NUM_LEDS-1; i > 0; i--) { leds[i] = leds[i-1]; leds[i].fadeToBlackBy(50); }
//   leds[0] = (random8() < 30) ? CRGB::Green : CRGB::Black;
//   FastLED.show(); FastLED.delay(100);
// }
// void colorBounce() {
//   static int pos = 0, dir = 1; static uint8_t hue = 0;
//   fill_solid(leds, NUM_LEDS, CRGB::Black);
//   leds[pos] = CHSV(hue, 255, 255);
//   for(int i = 1; i <= 3; i++) if(pos-i >= 0) leds[pos-i] = CHSV(hue, 255, 255/(i*2));
//   pos += dir;
//   if(pos >= NUM_LEDS-1 || pos <= 0) { dir *= -1; hue += 60; }
//   FastLED.show(); FastLED.delay(50);
// }
// DEFINE_GRADIENT_PALETTE( ocean_gp ) {
//   0,0,0,143, 48,0,0,255, 96,0,255,255,
//   144,42,255,45, 192,255,255,0, 240,255,0,0, 255,143,0,0
// };
// void paletteWave() {
//   static uint8_t startIndex = 0;
//   CRGBPalette16 palette = ocean_gp;
//   for(int i = 0; i < NUM_LEDS; i++) leds[i] = ColorFromPalette(palette, startIndex + i*5, 255, LINEARBLEND);
//   startIndex += 2; FastLED.show(); FastLED.delay(30);
// }
// void dualScanner() {
//   static int pos1=0, pos2=NUM_LEDS/2, dir1=1, dir2=-1;
//   fadeToBlackBy(leds, NUM_LEDS, 30);
//   leds[pos1] = CRGB::Red; leds[pos2] = CRGB::Blue;
//   pos1 += dir1; pos2 += dir2;
//   if(pos1 >= NUM_LEDS || pos1 < 0) dir1 *= -1;
//   if(pos2 >= NUM_LEDS || pos2 < 0) dir2 *= -1;
//   FastLED.show(); FastLED.delay(50);
// }
// void fireworks() {
//   static uint8_t fadeRate = 100;
//   if(random8() < 10) {
//     uint8_t pos = random8(NUM_LEDS/4, NUM_LEDS*3/4), hue = random8(), size = random8(3,6);
//     leds[pos] = CHSV(hue, 255, 255);
//     for(int i=1; i<=size; i++) {
//       if(pos+i < NUM_LEDS) leds[pos+i] = CHSV(hue+random8(30), 255, 255);
//       if(pos-i >= 0)       leds[pos-i] = CHSV(hue+random8(30), 255, 255);
//     }
//   }
//   for(int i = 0; i < NUM_LEDS; i++) leds[i].fadeToBlackBy(fadeRate);
//   FastLED.show(); FastLED.delay(33);
// }
// void trisakti() {
//   static int cycle = 0; static unsigned long counter = 0;
//   counter++;
//   if (cycle == 0) {
//     fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();
//     if (counter >= 50) { cycle = 1; counter = 0; }
//   } else if (cycle == 1) {
//     uint8_t val = (counter<600) ? map(counter,0,599,0,255) : (counter<700) ? 255 : (counter<800) ? map(counter,700,799,255,0) : 0;
//     fill_solid(leds, NUM_LEDS, CRGB(val,0,0)); FastLED.show();
//     if(counter >= 900) { cycle = 2; counter = 0; }
//   } else if (cycle == 2) {
//     uint8_t val = (counter<600) ? map(counter,0,599,0,255) : (counter<700) ? 255 : (counter<800) ? map(counter,700,799,255,0) : 0;
//     fill_solid(leds, NUM_LEDS, CRGB(0,val,0)); FastLED.show();
//     if(counter >= 900) { cycle = 3; counter = 0; }
//   } else if (cycle == 3) {
//     uint8_t val = (counter<600) ? map(counter,0,599,0,255) : (counter<700) ? 255 : (counter<800) ? map(counter,700,799,255,0) : 0;
//     fill_solid(leds, NUM_LEDS, CRGB(val,val/5,0)); FastLED.show();
//     if(counter >= 900) { cycle = 4; counter = 0; }
//   } else if (cycle == 4) {
//     uint8_t val = (counter<600) ? map(counter,0,599,0,150) : (counter<700) ? 150 : (counter<800) ? map(counter,700,799,150,0) : 0;
//     uint8_t red = (val > 130) ? 130 : val;
//     fill_solid(leds, NUM_LEDS, CRGB(red,0,val)); FastLED.show();
//     if(counter >= 900) { cycle = 1; counter = 0; }
//   }
//   delay(1);
// }
// void pickNewColor() { carColor = CHSV(random8(), 255, 255); }
// void car() {
//   fadeToBlackBy(leds, NUM_LEDS, FADE_RATE);
//   for(int i = 0; i < CAR_LENGTH; i++) {
//     int pos = carPosition - i;
//     if(pos < 0) pos += NUM_LEDS;
//     leds[pos] = carColor;
//     leds[pos].fadeToBlackBy(i * (255/CAR_LENGTH));
//   }
//   FastLED.show();
//   carPosition = (carPosition + 1) % NUM_LEDS;
//   if(carPosition == 0) pickNewColor();
//   delay(50);
// }
// void ular() {
//   static int counter = 0, colorIndex = 0;
//   static unsigned long lastColorChange = millis();
//   if(millis() - lastColorChange > colorChangeInterval) { colorIndex = (colorIndex+1) % paletteCount; lastColorChange = millis(); }
//   for(int i = NUM_LEDS-1; i > 0; i--) leds[i] = leds[i-1];
//   if(counter % (snakeLength + gapBetweenSnakes) < snakeLength) {
//     int posInSnake = counter % (snakeLength + gapBetweenSnakes);
//     uint8_t brightness = 255;
//     if(posInSnake < fadeSize) brightness = map(posInSnake, 0, fadeSize-1, 80, 255);
//     else if(posInSnake >= snakeLength-fadeSize) brightness = map(posInSnake, snakeLength-fadeSize, snakeLength-1, 255, 80);
//     leds[0] = colorPalette[colorIndex];
//     leds[0].fadeToBlackBy(255 - brightness);
//   } else { leds[0] = CRGB::Black; }
//   FastLED.show(); delay(moveDelay); counter++;
// }
// void gradasi() {
//   static uint8_t hue = 10;
//   for(int i = 0; i < NUM_LEDS; i++) leds[i] += CHSV(hue + (i*2), 255, 50);
//   fadeToBlackBy(leds, NUM_LEDS, 10);
//   FastLED.show(); delay(50); hue++;
// }
// void controlLed(int r, int g, int b) {
//   fill_solid(leds, NUM_LEDS, CRGB(r, g, b));
//   FastLED.show();
// }

// mode 1: rainbow
// mode 2: confeti
// mode 3: sinefon
// mode 4: bpm
// mode 5: juggle
// mode 6: fire
// mode 7: aurora
// mode 8: binary 
// mode 9: palette
// mode 10: dual scanner
// mode 11: color bounce
// mode 12: snake
