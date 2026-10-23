#pragma once

// Router-centric transport: one UDP PCB serves every seat, avoiding one TCP /
// WebSocket socket per node. Legacy WS seats and gateways remain supported.
static WiFiUDP smartbusUdp;

static bool udpSendJson(const IPAddress& ip, uint16_t port, const char* json) {
  if (ip == IPAddress() || port == 0 || json == nullptr) return false;
  if (!smartbusUdp.beginPacket(ip, port)) return false;
  smartbusUdp.write(reinterpret_cast<const uint8_t*>(json), strlen(json));
  return smartbusUdp.endPacket() == 1;
}

static void udpSendAck(const IPAddress& ip, uint16_t port, const char* uid,
                       uint32_t session, uint32_t seq) {
  char ack[160];
  snprintf(ack, sizeof(ack),
           "{\"type\":\"ack\",\"uid\":\"%s\",\"session\":%lu,\"seq\":%lu}",
           uid, (unsigned long)session, (unsigned long)seq);
  udpSendJson(ip, port, ack);
}

static void udpTransportInit() {
  if (smartbusUdp.begin(SMARTBUS_UDP_PORT)) {
    Serial.printf("[UDP] seat receiver ready on port %u", SMARTBUS_UDP_PORT);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf(" at %s", WiFi.localIP().toString().c_str());
    }
    Serial.println();
  } else {
    Serial.println("[UDP] ERROR: could not open seat receiver");
  }
}

static void udpTransportRebind() {
  smartbusUdp.stop();
  if (smartbusUdp.begin(SMARTBUS_UDP_PORT)) {
    Serial.printf("[UDP] rebound on router LAN %s:%u\n",
                  WiFi.localIP().toString().c_str(), SMARTBUS_UDP_PORT);
  } else {
    Serial.println("[UDP] ERROR: rebind failed");
  }
}

static void udpHandleSeat(JsonDocument& doc, const IPAddress& remoteIp,
                          uint16_t remotePort) {
  const char* id = doc["id"] | "";
  const char* uid = doc["uid"] | "";
  const int state = doc["sensorState"] | 0;
  const bool occupied = doc["seatOccupied"] | false;
  const bool seatbelt = doc["seatbelt"] | false;
  const uint32_t session = doc["session"] | 0U;
  const uint32_t seq = doc["seq"] | 0U;
  const uint16_t pressure = doc["pressure"] | (occupied ? 800 : 0);

  if (strlen(uid) != 12 || seq == 0 || session == 0) {
    Serial.printf("[UDP] invalid seat packet from %s\n", remoteIp.toString().c_str());
    return;
  }

  // ACK first: even a duplicate means the previous ACK was what got lost.
  udpSendAck(remoteIp, remotePort, uid, session, seq);

  const bool unassigned = strcmp(id, "RandomID") == 0 ||
                          (doc["unassigned"] | false);
  if (unassigned) {
    if (state || occupied) {
      const int slot = pairingRememberUdp(uid, remoteIp, remotePort);
      if (slot < 0) {
        Serial.println("[Pairing] pending table full");
      } else if (headunitNum >= 0) {
        sendPairingRequest((uint8_t)headunitNum, uid);
      } else {
        Serial.printf("[Pairing] UDP UID %s waiting for head unit\n", uid);
      }
    }
    return;
  }

  int slot = rosterFind(id);
  if (slot < 0) {
    Serial.println("[Roster] full - UDP seat ignored");
    return;
  }

  SeatInfo& seat = seats[slot];
  if (strlen(uid) == 12) {
    strlcpy(seat.uid, uid, sizeof(seat.uid));
  }
  seat.pressure = pressure;
  const bool sameSession = seat.hasUdpSeq && seat.udpSession == session;
  const bool duplicateOrOld = sameSession &&
                              static_cast<int32_t>(seq - seat.udpSeq) <= 0;

  seat.viaUdp = true;
  seat.sourceIp = remoteIp;
  if (!duplicateOrOld) {
    seat.udpSession = session;
    seat.udpSeq = seq;
    seat.hasUdpSeq = true;
  }

  // Duplicates still refresh connectivity/occupancy, but must never replay
  // call audio, clear the instantaneous call flag, or extend its timestamp.
  bool seatbeltRaised = false;
  if (duplicateOrOld) {
    if (!seat.online) {
      seat.online = true;
      Serial.printf("[Seat] %s ONLINE\n", seat.id);
    }
    seat.occupied = occupied;
    seat.lastSeen = millis();
  } else {
    seatbeltRaised = rosterUpdate(id, state, occupied, seatbelt);
  }
  // Rising edge of "occupied while unbuckled" → one warning announcement.
  // Gated by a 5-second lockout per seat to prevent sound queue spamming.
  if (seatbeltRaised) {
    if (millis() - seat.lastSeatbeltWarnTime >= 5000) {
      seat.lastSeatbeltWarnTime = millis();
      CallEvent belt;
      strlcpy(belt.id, id, sizeof(belt.id));
      belt.kind = CALL_SEATBELT;
      xQueueSend(callQueue, &belt, 0);
      Serial.printf("[UDP] SEATBELT %s seq=%lu from %s (audio queued)\n", id,
                    (unsigned long)seq, remoteIp.toString().c_str());
    } else {
      Serial.printf("[UDP] SEATBELT %s audio suppressed (5s cooldown active)\n", id);
    }
  }
  if (state && !duplicateOrOld) {
    CallEvent ev;
    strlcpy(ev.id, id, sizeof(ev.id));
    ev.kind = CALL_SEAT;
    xQueueSend(callQueue, &ev, 0);
    Serial.printf("[UDP] CALL %s seq=%lu from %s\n", id,
                  (unsigned long)seq, remoteIp.toString().c_str());
  }
}

static void udpTransportLoop() {
  int packetSize;
  while ((packetSize = smartbusUdp.parsePacket()) > 0) {
    const IPAddress remoteIp = smartbusUdp.remoteIP();
    const uint16_t remotePort = smartbusUdp.remotePort();
    char payload[512];
    const int readLen = smartbusUdp.read(
      reinterpret_cast<uint8_t*>(payload), sizeof(payload) - 1);
    if (readLen <= 0) continue;
    payload[readLen] = '\0';

    StaticJsonDocument<512> doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.printf("[UDP] bad JSON from %s: %s\n",
                    remoteIp.toString().c_str(), error.c_str());
      continue;
    }

    const char* type = doc["type"] | "";
    if (strcmp(type, "discover") == 0) {
      char response[128];
      snprintf(response, sizeof(response),
               "{\"type\":\"master\",\"name\":\"bus-led\",\"port\":%u}",
               SMARTBUS_UDP_PORT);
      udpSendJson(remoteIp, remotePort, response);
    } else if (strcmp(type, "seat") == 0) {
      udpHandleSeat(doc, remoteIp, remotePort);
    } else if (strcmp(type, "diag") == 0) {
      Serial.printf("[UDP Diag] %s from %s\n", payload,
                    remoteIp.toString().c_str());
    } else if (strcmp(type, "seatIdSaved") == 0) {
      const char* uid = doc["uid"] | "";
      pairingClear(uid);
      if (headunitNum >= 0) webSocket.sendTXT((uint8_t)headunitNum, payload);
      Serial.printf("[Pairing] UDP seat UID %s saved as %s\n",
                    uid, doc["id"] | "");
    }
  }
}
