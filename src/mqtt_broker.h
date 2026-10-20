#pragma once
#include <WiFi.h>
#include <functional>

// Minimal MQTT broker for ESP32 (QoS 0/1, wildcards # and +)
// No external library needed — pure WiFiServer implementation

#define MQTT_MAX_CLIENTS   8
#define MQTT_MAX_SUBS     16
#define MQTT_MAX_TOPIC   128
#define MQTT_MAX_PAYLOAD 512
#define MQTT_BYTE_TIMEOUT 100  // ms to wait for the next byte within a packet

class MqttBroker {
public:
  using MsgCb = std::function<void(const String&, const String&)>;
  MsgCb onMessage;  // fires only for externally received PUBLISH packets

  explicit MqttBroker(uint16_t port = 1883) : _srv(port), _port(port) {}

  void begin() {
    _srv.begin();
    Serial.printf("[MQTT Broker] Listening on port %d\n", _port);
  }

  void loop() {
    WiFiClient inc = _srv.accept();
    if (inc) {
      bool placed = false;
      for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
        if (!_c[i].active) {
          _c[i].tcp    = inc;
          _c[i].active = true;
          _c[i].nSubs  = 0;
          placed = true;
          Serial.printf("[MQTT Broker] New client from %s\n",
                        inc.remoteIP().toString().c_str());
          break;
        }
      }
      if (!placed) { inc.stop(); Serial.println("[MQTT Broker] Max clients — rejected"); }
    }

    for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
      Conn& c = _c[i];
      if (!c.active) continue;
      if (!c.tcp.connected()) { _drop(c); continue; }
      if (!c.tcp.available()) continue;

      uint8_t hdr = c.tcp.read();
      int rem = _readRemLen(c.tcp);
      if (rem < 0) { _drop(c); continue; }

      switch (hdr >> 4) {
        case 1:  _onConnect(c, rem);      break;  // CONNECT
        case 3:  _onPublish(c, hdr, rem); break;  // PUBLISH
        case 8:  _onSubscribe(c, rem);    break;  // SUBSCRIBE
        case 10: _onUnsubscribe(c, rem);  break;  // UNSUBSCRIBE
        case 12: _onPingreq(c);           break;  // PINGREQ
        case 14: _drop(c);                break;  // DISCONNECT
        default: _skip(c.tcp, rem);       break;
      }
    }
  }

  // Publish from the ESP32 itself — routes to subscribers, does NOT fire onMessage
  void publish(const String& topic, const String& payload) {
    _route(topic, (const uint8_t*)payload.c_str(), payload.length());
  }

private:
  WiFiServer _srv;
  uint16_t   _port;

  struct Conn {
    WiFiClient tcp;
    bool active = false;
    int  nSubs  = 0;
    char subs[MQTT_MAX_SUBS][MQTT_MAX_TOPIC];
  };
  Conn _c[MQTT_MAX_CLIENTS];

  void _drop(Conn& c) { c.tcp.stop(); c.active = false; c.nSubs = 0; }

  bool _waitByte(WiFiClient& tcp) {
    uint32_t t = millis();
    while (!tcp.available()) {
      if (!tcp.connected() || millis() - t > MQTT_BYTE_TIMEOUT) return false;
      delay(1);
    }
    return true;
  }

  bool _readN(WiFiClient& tcp, uint8_t* buf, int n) {
    for (int i = 0; i < n; i++) {
      if (!_waitByte(tcp)) return false;
      buf[i] = tcp.read();
    }
    return true;
  }

  void _skip(WiFiClient& tcp, int n) {
    for (int i = 0; i < n; i++) { if (!_waitByte(tcp)) break; tcp.read(); }
  }

  int _readRemLen(WiFiClient& tcp) {
    int val = 0, mult = 1;
    for (int i = 0; i < 4; i++) {
      if (!_waitByte(tcp)) return -1;
      uint8_t b = tcp.read();
      val += (b & 0x7F) * mult;
      if (!(b & 0x80)) return val;
      mult <<= 7;
    }
    return -1;
  }

  void _writeRemLen(WiFiClient& tcp, int len) {
    do {
      uint8_t b = len & 0x7F;
      len >>= 7;
      if (len) b |= 0x80;
      tcp.write(b);
    } while (len);
  }

  String _readStr(WiFiClient& tcp, int& rem) {
    uint8_t hi, lo;
    if (!_waitByte(tcp)) return "";
    hi = tcp.read(); rem--;
    if (!_waitByte(tcp)) return "";
    lo = tcp.read(); rem--;
    int len = (hi << 8) | lo;
    String s; s.reserve(len);
    for (int i = 0; i < len; i++) {
      if (!_waitByte(tcp)) break;
      s += (char)tcp.read(); rem--;
    }
    return s;
  }

  void _writeStr(WiFiClient& tcp, const String& s) {
    tcp.write((uint8_t)(s.length() >> 8));
    tcp.write((uint8_t)(s.length() & 0xFF));
    tcp.write((const uint8_t*)s.c_str(), s.length());
  }

  // CONNECT — skip all variable header + payload, reply CONNACK accepted
  void _onConnect(Conn& c, int rem) {
    _skip(c.tcp, rem);
    uint8_t ack[] = {0x20, 0x02, 0x00, 0x00};
    c.tcp.write(ack, 4);
    Serial.println("[MQTT Broker] Client connected (CONNACK sent)");
  }

  // PUBLISH — route to subscribers, fire onMessage for local processing
  void _onPublish(Conn& c, uint8_t hdr, int rem) {
    uint8_t qos = (hdr >> 1) & 0x03;
    String topic = _readStr(c.tcp, rem);

    uint8_t pktId[2] = {0, 0};
    if (qos > 0 && rem >= 2) {
      if (!_readN(c.tcp, pktId, 2)) return;
      rem -= 2;
    }

    uint8_t payload[MQTT_MAX_PAYLOAD];
    int plen = min(rem, (int)MQTT_MAX_PAYLOAD - 1);
    if (!_readN(c.tcp, payload, plen)) return;
    payload[plen] = 0;
    if (rem > plen) _skip(c.tcp, rem - plen);

    if (qos == 1) {
      uint8_t puback[] = {0x40, 0x02, pktId[0], pktId[1]};
      c.tcp.write(puback, 4);
    }

    _route(topic, payload, plen);
    if (onMessage) onMessage(topic, String((char*)payload));
  }

  // SUBSCRIBE — store filters, reply SUBACK (all granted at QoS 0)
  void _onSubscribe(Conn& c, int rem) {
    uint8_t pktId[2];
    if (!_readN(c.tcp, pktId, 2)) return;
    rem -= 2;

    uint8_t rc[16]; int rcn = 0;
    while (rem > 0 && rcn < 16) {
      String topic = _readStr(c.tcp, rem);
      if (!_waitByte(c.tcp)) break;
      c.tcp.read(); rem--;  // requested QoS byte — we grant QoS 0 regardless
      if (c.nSubs < MQTT_MAX_SUBS) {
        topic.toCharArray(c.subs[c.nSubs++], MQTT_MAX_TOPIC);
        rc[rcn++] = 0x00;
        Serial.printf("[MQTT Broker] Subscribed: %s\n", topic.c_str());
      } else {
        rc[rcn++] = 0x80;  // rejected
      }
    }
    c.tcp.write(0x90);
    _writeRemLen(c.tcp, 2 + rcn);
    c.tcp.write(pktId, 2);
    c.tcp.write(rc, rcn);
  }

  // UNSUBSCRIBE — remove filters, reply UNSUBACK
  void _onUnsubscribe(Conn& c, int rem) {
    uint8_t pktId[2];
    if (!_readN(c.tcp, pktId, 2)) return;
    rem -= 2;
    while (rem > 0) {
      String topic = _readStr(c.tcp, rem);
      for (int i = 0; i < c.nSubs; i++) {
        if (String(c.subs[i]) == topic) {
          for (int j = i; j < c.nSubs - 1; j++) memcpy(c.subs[j], c.subs[j+1], MQTT_MAX_TOPIC);
          c.nSubs--; break;
        }
      }
    }
    uint8_t resp[] = {0xB0, 0x02, pktId[0], pktId[1]};
    c.tcp.write(resp, 4);
  }

  void _onPingreq(Conn& c) {
    uint8_t resp[] = {0xD0, 0x00};
    c.tcp.write(resp, 2);
  }

  static bool _topicMatch(const char* filter, const char* topic) {
    while (*filter && *topic) {
      if (*filter == '#') return true;
      if (*filter == '+') {
        while (*topic && *topic != '/') topic++;
        filter++;
      } else if (*filter != *topic) {
        return false;
      } else { filter++; topic++; }
    }
    return (!*filter && !*topic) || *filter == '#';
  }

  void _route(const String& topic, const uint8_t* payload, int len) {
    int remLen = 2 + (int)topic.length() + len;
    for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
      Conn& s = _c[i];
      if (!s.active || !s.tcp.connected()) continue;
      for (int j = 0; j < s.nSubs; j++) {
        if (_topicMatch(s.subs[j], topic.c_str())) {
          s.tcp.write(0x30);         // PUBLISH QoS 0
          _writeRemLen(s.tcp, remLen);
          _writeStr(s.tcp, topic);
          if (payload && len > 0) s.tcp.write(payload, len);
          break;
        }
      }
    }
  }
};
