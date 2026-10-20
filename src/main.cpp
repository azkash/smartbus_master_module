#include "main.h"
#include "web_monitor.h"
#include "udp_transport.h"
// #include "esp_mac.h"  // was used by espnowInit — commented with ESP-NOW

// ─── Effect Task (Core 1) ────────────────────────────────────────────
volatile bool effectRunning = false;
static TaskHandle_t effectHandle = NULL;
static TaskHandle_t audioHandle = NULL;

// DY-HV8F track index. Keep this in sync with
// audio/track baru sudah di convert/TRACK_MAPPING.md and copy the files to an
// empty module in the documented order.
static constexpr int TRACK_CALL_PREFIX = 1;   // "Panggilan kursi nomor"
static constexpr int TRACK_DIGIT_1     = 2;   // 2..10 = "satu".."sembilan"
static constexpr int TRACK_TEN         = 11;  // "sepuluh"
static constexpr int TRACK_ELEVEN      = 12;  // "sebelas"
static constexpr int TRACK_TEEN        = 13;  // "belas"
static constexpr int TRACK_TENS        = 14;  // "puluh"
static constexpr int TRACK_SUFFIX_A    = 15;  // "A"
static constexpr int TRACK_SUFFIX_B    = 16;  // "B"

// Seatbelt warning phrase, assembled as
//   TRACK_SEATBELT_PREFIX + <seat number> + TRACK_SEATBELT_NO
//   + TRACK_SEATBELT_USE + TRACK_SEATBELT_BELT
// i.e. "kursi nomor <N> tidak memakai seatbelt".
static constexpr int TRACK_SEATBELT_NO     = 17;  // "Tidak"
static constexpr int TRACK_SEATBELT_USE    = 18;  // "Memakai"
static constexpr int TRACK_SEATBELT_BELT   = 19;  // "Seatbelt"
static constexpr int TRACK_SEATBELT_PREFIX = 20;  // "kursi nomor"

// Parse the production seat-id grammar: A/B, 1..99, or 1..99 followed by A/B.
// A letter-only front seat has no numeric part, so seatNumber is returned as 0
// and the announcement consists of TRACK_CALL_PREFIX followed by A or B.
static bool parseSeatIdForAudio(const char* id, uint8_t& seatNumber,
                                int& suffixTrack) {
  if (id == nullptr || id[0] == '\0') return false;

  seatNumber = 0;
  suffixTrack = 0;
  if (id[1] == '\0') {
    if (id[0] == 'A' || id[0] == 'a') {
      suffixTrack = TRACK_SUFFIX_A;
      return true;
    }
    if (id[0] == 'B' || id[0] == 'b') {
      suffixTrack = TRACK_SUFFIX_B;
      return true;
    }
  }

  if (id[0] < '1' || id[0] > '9') return false;

  unsigned int number = 0;
  const char* cursor = id;
  while (*cursor >= '0' && *cursor <= '9') {
    number = number * 10 + static_cast<unsigned int>(*cursor - '0');
    if (number > 99) return false;
    ++cursor;
  }

  if (*cursor != '\0') {
    if ((cursor[0] == 'A' || cursor[0] == 'a') && cursor[1] == '\0') {
      suffixTrack = TRACK_SUFFIX_A;
    } else if ((cursor[0] == 'B' || cursor[0] == 'b') && cursor[1] == '\0') {
      suffixTrack = TRACK_SUFFIX_B;
    } else {
      return false;
    }
  }

  seatNumber = static_cast<uint8_t>(number);
  return true;
}

static void playAudioPart(int track) {
  playTrack(track);
  waitForAudioDone();
}

// Speak an Indonesian number from 1 through 99 using the reusable digit,
// "belas", and "puluh" recordings in the new track set.
static void playSeatNumber(uint8_t number) {
  if (number <= 9) {
    playAudioPart(TRACK_DIGIT_1 + number - 1);
    return;
  }
  if (number == 10) {
    playAudioPart(TRACK_TEN);
    return;
  }
  if (number == 11) {
    playAudioPart(TRACK_ELEVEN);
    return;
  }
  if (number < 20) {
    playAudioPart(TRACK_DIGIT_1 + (number - 10) - 1);
    playAudioPart(TRACK_TEEN);
    return;
  }

  const uint8_t tens = number / 10;
  const uint8_t ones = number % 10;
  playAudioPart(TRACK_DIGIT_1 + tens - 1);
  playAudioPart(TRACK_TENS);
  if (ones > 0) playAudioPart(TRACK_DIGIT_1 + ones - 1);
}

// Audio is intentionally isolated from effectTask: a complete announcement can
// take several seconds while BUSY is LOW, but LED frames must keep rendering.
void audioTask(void*) {
  while (true) {
    CallEvent call;
    if (xQueueReceive(callQueue, &call, portMAX_DELAY) == pdTRUE) {
      uint8_t seatNumber = 0;
      int suffixTrack = 0;
      if (!parseSeatIdForAudio(call.id, seatNumber, suffixTrack)) {
        Serial.printf("[Audio] cannot announce invalid seat id: %s\n", call.id);
        continue;
      }

      if (seatNumber == 0) {
        Serial.printf("[Audio] parsed seat %s as letter-only %s\n", call.id,
                      suffixTrack == TRACK_SUFFIX_A ? "A" : "B");
      } else {
        Serial.printf("[Audio] parsed seat %s as number %u%s\n",
                      call.id, seatNumber,
                      suffixTrack == TRACK_SUFFIX_A ? "A" :
                      suffixTrack == TRACK_SUFFIX_B ? "B" : "");
      }

      audioAnnouncementActive = true;
      if (call.kind == CALL_SEATBELT) {
        Serial.printf("[Seatbelt] %s occupied without seatbelt — voicing warning\n",
                      call.id);
        // "kursi nomor <N> tidak memakai seatbelt" — played once.
        playAudioPart(TRACK_SEATBELT_PREFIX);
        if (seatNumber != 0) playSeatNumber(seatNumber);
        if (suffixTrack != 0) playAudioPart(suffixTrack);
        playAudioPart(TRACK_SEATBELT_NO);
        playAudioPart(TRACK_SEATBELT_USE);
        playAudioPart(TRACK_SEATBELT_BELT);
      } else {
        Serial.printf("[Seat] call from %s — voicing announcement\n", call.id);
        for (uint8_t repeat = 0; repeat < 2; ++repeat) {
          Serial.printf("[Audio] announcement repeat %u/2\n", repeat + 1);
          playAudioPart(TRACK_CALL_PREFIX);
          if (seatNumber != 0) playSeatNumber(seatNumber);
          if (suffixTrack != 0) playAudioPart(suffixTrack);
        }
      }
      audioAnnouncementActive = false;
    }
  }
}

void effectTask(void*) {
  int hue = 0, chase = 0, fade = 0;
  bool fadeUp = true;

  while (effectRunning) {
    // FastLED's global brightness scaling may preserve a minimum non-zero
    // output for non-black pixels. Explicit black data guarantees true OFF at
    // dimmer 0%, and also fixes the old OFF branch that displayed blue.
    if (!ledOn || ledBrightness == 0) {
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      fill_solid(dimLeds, NUM_DIMS, CRGB::Black);
      FastLED.show();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    FastLED.setBrightness(ledBrightness);

    switch (ledEffect) {
      case EFF_SOLID:
        fill_solid(leds, NUM_LEDS, CRGB(ledR, ledG, ledB));
        FastLED.show();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        break;
      case EFF_RAINBOW_GLITTER:
        fill_rainbow(leds, NUM_LEDS, hue, 7);
        FastLED.show();
        hue = (hue + 3) % 256;
        vTaskDelay(20 / portTICK_PERIOD_MS);
        break;
      case EFF_CONFETTI_PARTY:
        fadeToBlackBy(leds, NUM_LEDS, 10);
        for (int i = 0; i < 3; i++) {
          int pos = random16(NUM_LEDS);
          leds[pos] += CHSV(random8(), 200, 255);
        }
        FastLED.show();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        break;
      case EFF_SINELON_WAVE:
        fadeToBlackBy(leds, NUM_LEDS, 20);
        leds[beatsin16(13, 0, NUM_LEDS - 1)] += CHSV(hue++, 255, 192);
        FastLED.show();
        vTaskDelay(30 / portTICK_PERIOD_MS);
        break;
      case EFF_BPM_RHYTHM: {
        uint8_t beat = beatsin8(62, 64, 255);
        for (int i = 0; i < NUM_LEDS; i++) leds[i] = CHSV(hue + (i * 2), 255, beat);
        hue++;
        FastLED.show();
        vTaskDelay(20 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_JUGGLE_BOUNCE:
        fadeToBlackBy(leds, NUM_LEDS, 20);
        for (int i = 0; i < 8; i++) leds[beatsin16(i + 7, 0, NUM_LEDS - 1)] |= CHSV(hue + i * 32, 200, 255);
        hue++;
        FastLED.show();
        vTaskDelay(20 / portTICK_PERIOD_MS);
        break;
      case EFF_FIRE_FLAME: {
        static byte heat[NUM_LEDS];
        for (int i = 0; i < NUM_LEDS; i++) heat[i] = qsub8(heat[i], random8(0, 30));
        for (int k = NUM_LEDS - 1; k >= 2; k--) heat[k] = (heat[k-1] + heat[k-2] + heat[k-2]) / 3;
        if (random8() < 40) { int y = random8(7); heat[y] = qadd8(heat[y], random8(160, 255)); }
        for (int j = 0; j < NUM_LEDS; j++) leds[j] = HeatColor(heat[j]);
        FastLED.show();
        vTaskDelay(33 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_AURORA_GLOW: {
        for (int i = 0; i < NUM_LEDS; i++) {
          uint8_t v = (beatsin8(5,0,255,0,i*10) + beatsin8(7,0,255,0,i*10+85) + beatsin8(4,0,255,0,i*10+170)) / 3;
          leds[i] = CHSV(hue + (i * 2), 200, v);
        }
        hue++;
        FastLED.show();
        vTaskDelay(30 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_BINARY_COUNTER: {
        static uint8_t counter = 0;
        for (int i = 0; i < 8; i++) {
          leds[i]    = (counter & (1 << i)) ? CRGB::Blue  : CRGB::Black;
          leds[i+8]  = (counter & (1 << i)) ? CRGB::Red   : CRGB::Black;
          if (i < 4) leds[i+16] = (counter & (1 << i)) ? CRGB::Green : CRGB::Black;
        }
        counter++;
        FastLED.show();
        vTaskDelay(500 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_PALETTE_WAVE: {
        static uint8_t startIndex = 0;
        CRGBPalette16 palette = ocean_gp;
        for (int i = 0; i < NUM_LEDS; i++) leds[i] = ColorFromPalette(palette, startIndex + i * 5, 255, LINEARBLEND);
        startIndex += 2;
        FastLED.show();
        vTaskDelay(30 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_DUAL_SCANNER: {
        static int pos1 = 0, pos2 = NUM_LEDS / 2, dir1 = 1, dir2 = -1;
        fadeToBlackBy(leds, NUM_LEDS, 30);
        leds[pos1] = CRGB::Red;
        leds[pos2] = CRGB::Blue;
        pos1 += dir1; pos2 += dir2;
        if (pos1 >= NUM_LEDS || pos1 < 0) dir1 *= -1;
        if (pos2 >= NUM_LEDS || pos2 < 0) dir2 *= -1;
        FastLED.show();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_COLOR_BOUNCE: {
        static int pos = 0, dir = 1;
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[pos] = CHSV(hue, 255, 255);
        for (int i = 1; i <= 3; i++) if (pos - i >= 0) leds[pos - i] = CHSV(hue, 255, 255 / (i * 2));
        pos += dir;
        if (pos >= NUM_LEDS - 1 || pos <= 0) { dir *= -1; hue += 60; }
        FastLED.show();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        break;
      }
      case EFF_SNAKE: {
        static int counter = 0, colorIndex = 0;
        static unsigned long lastColorChange = millis();
        static const CRGB palette[] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::Purple, CRGB::Yellow, CRGB::Cyan};
        const int snakeLen = 10, gap = 1, fadeSz = 5;
        if (millis() - lastColorChange > 10000) { colorIndex = (colorIndex + 1) % 6; lastColorChange = millis(); }
        for (int i = NUM_LEDS - 1; i > 0; i--) leds[i] = leds[i - 1];
        int posInSnake = counter % (snakeLen + gap);
        if (posInSnake < snakeLen) {
          uint8_t brightness = 255;
          if (posInSnake < fadeSz) brightness = map(posInSnake, 0, fadeSz - 1, 80, 255);
          else if (posInSnake >= snakeLen - fadeSz) brightness = map(posInSnake, snakeLen - fadeSz, snakeLen - 1, 255, 80);
          leds[0] = palette[colorIndex];
          leds[0].fadeToBlackBy(255 - brightness);
        } else { leds[0] = CRGB::Black; }
        FastLED.show();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        counter++;
        break;
      }
      case EFF_BLINK:
        fill_solid(leds, NUM_LEDS, CRGB(ledR, ledG, ledB));
        FastLED.show();
        vTaskDelay(500 / portTICK_PERIOD_MS);
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
        vTaskDelay(500 / portTICK_PERIOD_MS);
        break;
      case EFF_CHASE:
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[chase % NUM_LEDS] = CRGB(ledR, ledG, ledB);
        if (chase >= 1) leds[(chase - 1) % NUM_LEDS].fadeToBlackBy(180);
        FastLED.show();
        chase = (chase + 1) % NUM_LEDS;
        vTaskDelay(40 / portTICK_PERIOD_MS);
        break;
      case EFF_FADE: {
        uint8_t bri = fadeUp ? fade : (255 - fade);
        fill_solid(leds, NUM_LEDS, CRGB(ledR, ledG, ledB));
        FastLED.setBrightness(map(bri, 0, 255, 0, ledBrightness));
        FastLED.show();
        fade += 3;
        if (fade >= 255) { fade = 0; fadeUp = !fadeUp; }
        vTaskDelay(15 / portTICK_PERIOD_MS);
        break;
      }
    }
  }
  effectHandle = NULL;
  vTaskDelete(NULL);
}

// void startEffectTask() {
//   if (effectHandle) {
//     effectRunning = false;
//     while (effectHandle) vTaskDelay(10 / portTICK_PERIOD_MS);
//   }
//   effectRunning = true;
//   xTaskCreate(effectTask, "ledEffect", 2048, NULL, 1, &effectHandle);
// }

// ─── ESP-NOW receiver (Core 0) — replaced by MQTT ────────────────────
// typedef struct {
//   char id[13];
//   int  sensorState;
//   bool seatOccupied;
// } espnow_payload_t;
//
// static volatile bool espnow_received      = false;
// static volatile bool espnow_seat_occupied = false;
// static volatile bool espnow_button_state  = false;
// static char          espnow_id[13]        = {0};
//
// static QueueHandle_t espNowQueue;
// static QueueHandle_t httpQueue;
//
// void espNowTask(void*) {
//   espnow_payload_t msg;
//   while (true) {
//     if (xQueueReceive(espNowQueue, &msg, portMAX_DELAY)) {
//       Serial.printf("[ESP-NOW] id: %s sensorState: %d  occupied: %d\n", msg.id, msg.sensorState, msg.seatOccupied);
//       digitalWrite(LED_BUILTIN, msg.sensorState ? HIGH : LOW);
//       xQueueSend(httpQueue, &msg, 0);
//     }
//   }
// }
//
// void httpTask(void*) {
//   espnow_payload_t msg;
//   while (true) {
//     if (xQueueReceive(httpQueue, &msg, portMAX_DELAY) && msg.sensorState != 0) {
//       sendToHeadunit(msg.sensorState, 0, msg.seatOccupied);
//     }
//   }
// }
//
// static void onDataRecv(const esp_now_recv_info* recv_info, const uint8_t* data, int len) {
//   if (len == sizeof(espnow_payload_t)) {
//     const espnow_payload_t* p = (const espnow_payload_t*)data;
//     espnow_seat_occupied = p->seatOccupied;
//     espnow_button_state = p->sensorState;
//     memcpy((void*)espnow_id, p->id, sizeof(espnow_id));
//     espnow_received = true;
//   }
// }
//
// static void espnowInit() {
//   if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
//   esp_now_register_recv_cb(onDataRecv);
//   Serial.println("ESP-NOW receiver ready");
//   uint8_t mac[6];
//   esp_read_mac(mac, ESP_MAC_WIFI_STA);
//   Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
// }

static void actInit() {
  audioInit();
  pinMode(BUSY_PIN, INPUT);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.addLeds<LED_TYPE, DIM_PIN, COLOR_ORDER>(dimLeds, NUM_DIMS);
  FastLED.setBrightness(ledBrightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  fill_solid(dimLeds, NUM_DIMS, CRGB::Black);
  FastLED.show();
  if (effectHandle) {
    effectRunning = false;
    while (effectHandle) vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  effectRunning = true;
  xTaskCreate(
    effectTask,
    "ledEffect",
    4096,
    NULL,
    1,
    &effectHandle
  );
  if (!audioHandle) {
    xTaskCreate(
      audioTask,
      "callAudio",
      4096,
      NULL,
      1,
      &audioHandle
    );
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.println("Master Board Firmware v1.9 RouterUDP STAOnly DynamicID: " __DATE__ " " __TIME__);

  // Phone hotspots using PMF / 802.11w make the WiFi driver log
  //   "CCMP mgmt frame from <ap> used non-zero reserved bit"
  // at ERROR level several times a second. It concerns the STA uplink only —
  // the AP, seats and WebSocket traffic are unaffected — but it buries the
  // [Roster]/[Seat]/[WS] lines we actually need when debugging. Silence the
  // driver's own tag; our messages are unaffected.
  // Delete this line to get the driver chatter back.
  esp_log_level_set("wifi", ESP_LOG_NONE);

  // One entry per roster slot prevents a valid call from being dropped merely
  // because many of the 50 production seats call within the same audio window.
  // Playback remains sequential; this only protects admission to the queue.
  callQueue = xQueueCreate(MAX_SEATS, sizeof(CallEvent));
  if (!callQueue) {
    Serial.println("[Audio] ERROR: could not allocate seat-call queue");
    delay(1000);
    ESP.restart();
  }
  loadSettings();       // restore LED on/brightness/effect/rgb from NVS (before actInit)

  wifiInit();           // join fleet router in STA-only mode
  udpTransportInit();   // one router-facing socket for all UDP seat nodes
  // mqttBrokerInit();  // -- reverted: was local MQTT broker on port 1883 --
  // espnowInit();      // replaced by MQTT
  
  actInit();

  // espNowQueue = xQueueCreate(30, sizeof(espnow_payload_t));
  // xTaskCreatePinnedToCore(espNowTask, "espNowTask", 4096, NULL, 2, NULL, 1);
  // xTaskCreatePinnedToCore(httpTask,   "httpTask",   8192, NULL, 1, NULL, 0);

  // ─── HTTP server ─── replaced by WebSocket server ────────────────────
  // server.on("/command", HTTP_GET,  handleCommand);
  // server.on("/data",    HTTP_POST, handleData);
  // server.onNotFound([]() {
  //   server.send(404, "application/json", "{\"status\":\"error\",\"msg\":\"not found\"}");
  // });
  // server.begin();

  // ─── WebSocket server — ESP32 is the server; Android app is the client ─
  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  // Ping every client every 3s; drop it after 2 missed pongs (~10s). This frees
  // the WS slot of a seat that vanished (out of range / powered off) so it
  // doesn't hold one of the limited client slots, and lets its OFFLINE fire.
  webSocket.enableHeartbeat(3000, 2000, 2);
  Serial.printf("[WS] WebSocket server ready on router LAN %s:81\n",
                WiFi.status() == WL_CONNECTED
                  ? WiFi.localIP().toString().c_str() : "(waiting for DHCP)");
  webMonitorInit();

  // -- Inverter relay disabled --
  // inverterMutex = xSemaphoreCreateMutex();
  // xTaskCreate(inverterPollTask, "InvPoll", 4096, NULL, 1, NULL);
}

// ─── Loop (Core 1) ───────────────────────────────────────────────────
void loop() {
  webSocket.loop();  // serve all connected WebSocket clients
  udpTransportLoop(); // receive router-connected seats without extra sockets
  webMonitorHandle(); // serve the phone dashboard on HTTP port 80
  handleSerial();    // laptop console — type 'help' for the command list

  if (wifiRestartPending &&
      (int32_t)(millis() - wifiRestartAt) >= 0) {
    Serial.println("[STA] restarting to apply router credentials");
    delay(50);
    ESP.restart();
  }

  static unsigned long lastBroadcast = 0;
  unsigned long now = millis();

  for (int i = 0; i < MAX_SEATS; i++) {
    if (!seats[i].used) continue;

    // Log OFFLINE when a seat stops reporting for SEAT_OFFLINE_MS.
    if (seats[i].online && (now - seats[i].lastSeen) >= SEAT_OFFLINE_MS) {
      seats[i].online = false;
      // Do not leave stale occupancy/call state visible on the head unit when
      // a seat controller loses power or its network connection. A heartbeat
      // from the controller will restore the actual state after it reconnects.
      seats[i].occupied = false;
      seats[i].calling = false;
      seats[i].seatbelt = false;
      seats[i].seatbeltAlert = false;
      Serial.printf("[Seat] %s OFFLINE\n", seats[i].id);
    }

    // Auto-expire callPending after 30 s if headunit never sends clear_<id>.
    if (seats[i].callPending && !seats[i].calling && now - seats[i].callTime > 30000) {
      seats[i].callPending = false;
      Serial.printf("[Roster] call auto-expired for %s\n", seats[i].id);
    }
  }

  // STA-only recovery needs no AP-channel coordination or reboot.
  static unsigned long lastStaReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastStaReconnect >= 5000) {
    lastStaReconnect = now;
    Serial.println("[STA] reconnecting to router...");
    WiFi.reconnect();
  }

  // Report router state changes.
  static bool staWasUp = false;
  bool staUp = (WiFi.status() == WL_CONNECTED);
  if (staUp != staWasUp) {
    staWasUp = staUp;
    if (staUp) {
      udpTransportRebind();
      Serial.printf("[WEB] monitor router LAN: http://%s/\n",
                    WiFi.localIP().toString().c_str());
    }
    if (staUp && !mdnsStarted && MDNS.begin("bus-led")) {
      mdnsStarted = true;
      MDNS.addService("ws", "tcp", 81);
      MDNS.addService("http", "tcp", 80);
      Serial.println("[mDNS] responder restored: bus-led.local");
    }
    Serial.printf("[STA] %s\n", staUp
      ? ("connected — IP " + WiFi.localIP().toString()).c_str()
      : "disconnected");
    if (headunitNum >= 0) {
      String st = wifiStatusJson();
      webSocket.sendTXT((uint8_t)headunitNum, st);
    }
  }

  // Builtin LED reports the router link at a glance — the master normally
  // runs with no serial attached, so this is the only local indicator:
  //   solid  → router connected
  //   fast blink → a requested restart is pending
  //   slow blink → reconnecting
  //   off    → no uplink
  if (staUp) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (wifiRestartPending) {
    digitalWrite(LED_BUILTIN, (millis() / 200) % 2);   // ~2.5 Hz while trying
  } else if (staRecoveryPhase != StaRecoveryPhase::Idle) {
    digitalWrite(LED_BUILTIN, (millis() / 700) % 2);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }

  // Broadcast current roster every 0.5 s (500 ms).
  if (now - lastBroadcast >= 500) {
    lastBroadcast = now;
    String roster = rosterJson();
    webSocket.broadcastTXT(roster);

    // -- Inverter relay disabled --
    // if (headunitNum >= 0) {
    //   String inv = inverterStatusJson();
    //   webSocket.sendTXT((uint8_t)headunitNum, inv);
    // }
  }
}
