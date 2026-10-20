# TODO — Code_Master

## Headunit (Android app)
- [ ] **[headunit]** Create a settings page for configuring the SSID and password the master board uses to connect to an internet WiFi (upstream STA credentials). _Depends on: mDNS discovery._
- [ ] **[headunit]** Combine all applications (LED settings, seat display, AP settings, inverter status) into one app. _Depends on: seat roster, inverter status source, mDNS discovery._
- [ ] **[headunit]** Connection/health indicator (WS connected? how many seats online?).

## Firmware — reliability
- [x] **[firmware]** Seat roster on the master — track each `SEAT_ID`: last-seen timestamp, occupancy, call state. Backbone for the seat-display app + detects offline seats. _(query: send `roster`; auto-broadcast on every seat event)_
- [x] **[firmware]** Announcement queue — FreeRTOS `callQueue` filled in `onWsEvent`, drained in `effectTask`; carries the seat id so simultaneous calls queue instead of colliding/dropping. _(per-seat audio track still needs a track-mapping when content is available)_
- [x] **[firmware]** Call acknowledge / clear — `callPending` latched per seat; crew clears via `clear_<id>` or `clear_all`; roster re-broadcast on change.
- [x] **[firmware]** Persist settings to NVS (`Preferences`) — `loadSettings()` on boot, `saveSettings()` after every successful command (brightness / effect / RGB / on-off).

## Firmware — features
- [ ] **[firmware]** Inverter status data source — read it (ADC voltage/current divider, or serial/Modbus) and broadcast over WS. Needed before the app can show inverter status.
- [ ] **[firmware]** Expose volume over WS — add a `vol_NN` command (wraps existing `playbackVolume()`).
- [ ] **[firmware]** OTA updates (`ArduinoOTA` / WebSocket OTA) — update master & seat nodes over the AP instead of USB.

## Discovery
- [ ] **[firmware]** mDNS on the master (e.g. `bus.local`) so the app stops hardcoding `192.168.4.1` and survives IP changes.

## Suggested sequence
1. Seat roster
2. Call acknowledge / clear
3. Persist settings (NVS)
4. mDNS discovery
5. → then the two headunit app items (they depend on the above)
