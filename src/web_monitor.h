#pragma once

#include <WebServer.h>

static WebServer webMonitorServer(80);

static const char WEB_MONITOR_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SmartBus Master Monitor</title>
  <style>
    :root{color-scheme:dark;--bg:#08111f;--panel:#101c2e;--line:#263750;--text:#e6edf7;--muted:#91a4bd;--good:#48d597;--bad:#ff6b7a;--accent:#5fb3ff}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px system-ui,-apple-system,sans-serif}
    main{width:min(1100px,100%);margin:auto;padding:18px}header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:16px}
    h1{font-size:21px;margin:0}.sub{color:var(--muted);font-size:12px;margin-top:4px}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--good);margin-right:7px;box-shadow:0 0 10px var(--good)}
    .cards{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;margin-bottom:12px}.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px}.label{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.08em}.value{font-size:19px;font-weight:700;margin-top:5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.good{color:var(--good)}.bad{color:var(--bad)}
    .toolbar{display:flex;align-items:center;gap:8px;background:var(--panel);border:1px solid var(--line);border-bottom:0;border-radius:10px 10px 0 0;padding:9px 11px}.toolbar strong{margin-right:auto}button{border:1px solid var(--line);background:#17263b;color:var(--text);padding:7px 11px;border-radius:7px}button:active{transform:translateY(1px)}
    .clientbox{background:var(--panel);border:1px solid var(--line);border-radius:10px;margin:0 0 12px;overflow:auto}.clientbox h2{font-size:14px;margin:0;padding:11px 13px;border-bottom:1px solid var(--line)}table{width:100%;border-collapse:collapse;min-width:620px}th,td{text-align:left;padding:8px 12px;border-bottom:1px solid #1e2d42;font:12px ui-monospace,SFMono-Regular,Consolas,monospace}th{color:var(--muted);font-size:10px;text-transform:uppercase}tr:last-child td{border:0}
    pre{height:58vh;min-height:330px;margin:0;padding:14px;overflow:auto;background:#030811;border:1px solid var(--line);border-radius:0 0 10px 10px;color:#b9f6d2;font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;white-space:pre-wrap;word-break:break-word}
    footer{color:var(--muted);font-size:11px;text-align:center;margin-top:10px}@media(max-width:760px){main{padding:11px}.cards{grid-template-columns:repeat(2,1fr)}.cards .card:last-child{grid-column:span 2}h1{font-size:18px}pre{height:62vh}}
  </style>
</head>
<body><main>
  <header><div><h1>SmartBus Master</h1><div class="sub">Web Serial Monitor &middot; Router LAN iot_bus</div></div><div id="connection"><span class="dot"></span>Terhubung</div></header>
  <section class="cards">
    <div class="card"><div class="label">Router IP</div><div class="value" id="routerip">-</div></div>
    <div class="card"><div class="label">Seats online</div><div class="value" id="seats">-</div></div>
    <div class="card"><div class="label">Head unit</div><div class="value" id="headunit">-</div></div>
    <div class="card"><div class="label">Uplink</div><div class="value" id="uplink">-</div></div>
    <div class="card"><div class="label">Free heap</div><div class="value" id="heap">-</div></div>
  </section>
  <section class="clientbox"><h2>Seat SmartBus (router UDP + legacy)</h2><table><thead><tr><th>ID</th><th>Status</th><th>Transport</th><th>IP</th><th>Occupancy</th><th>Call</th><th>Seatbelt</th><th>Last seen</th></tr></thead><tbody id="seatrows"><tr><td colspan="8">Memuat...</td></tr></tbody></table></section>
  <div class="toolbar"><strong>Live serial output</strong><button id="pause">Pause</button><button id="bottom">Ke bawah</button></div>
  <pre id="terminal">Menghubungkan ke ESP Master...</pre>
  <footer>Log menyimpan 80 baris terakhir dan diperbarui setiap 2 detik.</footer>
</main><script>
const terminal=document.getElementById('terminal'),pauseButton=document.getElementById('pause');let paused=false;
const setState=(id,text,ok)=>{const el=document.getElementById(id);el.textContent=text;el.className='value '+(ok===undefined?'':ok?'good':'bad')};
function renderClients(items){const body=document.getElementById('clientrows');body.textContent='';if(!items.length){const row=body.insertRow();const cell=row.insertCell();cell.colSpan=4;cell.textContent='Tidak ada klien';return}for(const c of items){const row=body.insertRow();for(const value of [c.name,c.ip,c.mac,c.rssi+' dBm'])row.insertCell().textContent=value}}
function renderSeats(items){const body=document.getElementById('seatrows');body.textContent='';if(!items.length){const row=body.insertRow();const cell=row.insertCell();cell.colSpan=8;cell.textContent='Belum ada seat';return}for(const s of items){const belt=!s.online?'-':s.seatbeltAlert?'NO BELT':s.seatbelt?'ok':'-';const row=body.insertRow();for(const value of [s.id,s.online?'ONLINE':'OFFLINE',s.transport,s.ip||'-',s.occupied?'YES':'no',s.call?'YES':'no',belt,s.age+' s'])row.insertCell().textContent=value}}
async function updateStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;const s=await r.json();setState('routerip',s.uplink?s.uplinkIp:'DOWN',s.uplink);setState('seats',s.seatsOnline+'/'+s.seatsKnown,s.seatsOnline===s.seatsKnown);setState('headunit',s.headunit?'ONLINE':'OFFLINE',s.headunit);setState('uplink',s.uplink?s.uplinkSsid+' · '+s.rssi+' dBm':'DOWN',s.uplink);setState('heap',Math.round(s.heap/1024)+' KB');renderSeats(s.seatList||[]);document.getElementById('connection').innerHTML='<span class="dot"></span>Terhubung'}catch(e){document.getElementById('connection').innerHTML='<span class="dot" style="background:var(--bad);box-shadow:none"></span>Terputus'}}
async function updateLog(){if(paused)return;try{const nearBottom=terminal.scrollHeight-terminal.scrollTop-terminal.clientHeight<55;const r=await fetch('/api/log',{cache:'no-store'});if(!r.ok)throw 0;terminal.textContent=await r.text();if(nearBottom)terminal.scrollTop=terminal.scrollHeight}catch(e){}}
pauseButton.onclick=()=>{paused=!paused;pauseButton.textContent=paused?'Lanjut':'Pause';if(!paused)updateLog()};document.getElementById('bottom').onclick=()=>terminal.scrollTop=terminal.scrollHeight;
updateStatus();updateLog();setInterval(updateStatus,3000);setInterval(updateLog,2000);
</script></body></html>
)HTML";

static void webMonitorSendStatus() {
  webMonitorClientIp = webMonitorServer.client().remoteIP();
  int known = 0;
  int online = 0;
  const unsigned long now = millis();
  for (int i = 0; i < MAX_SEATS; ++i) {
    if (!seats[i].used) continue;
    ++known;
    if ((now - seats[i].lastSeen) < SEAT_OFFLINE_MS) ++online;
  }

  // Sized for the 64-slot production roster. The monitor object carries more
  // fields per seat than the compact WebSocket roster, so 8 KB is too tight
  // once the fleet grows beyond the original 32-seat prototype.
  DynamicJsonDocument doc(16384);
  doc["uptime"] = millis() / 1000UL;
  doc["heap"] = ESP.getFreeHeap();
  doc["seatsKnown"] = known;
  doc["seatsOnline"] = online;
  doc["headunit"] = headunitNum >= 0;
  const bool uplink = WiFi.status() == WL_CONNECTED;
  doc["uplink"] = uplink;
  doc["uplinkSsid"] = uplink ? WiFi.SSID() : String("");
  doc["uplinkIp"] = uplink ? WiFi.localIP().toString() : String("");
  doc["rssi"] = uplink ? WiFi.RSSI() : 0;
  JsonArray seatList = doc.createNestedArray("seatList");
  for (int i = 0; i < MAX_SEATS; ++i) {
    if (!seats[i].used) continue;
    JsonObject seat = seatList.createNestedObject();
    const bool isOnline = (now - seats[i].lastSeen) < SEAT_OFFLINE_MS;
    seat["id"] = seats[i].id;
    seat["online"] = isOnline;
    seat["transport"] = seats[i].viaUdp ? "UDP" : "WebSocket";
    seat["ip"] = seats[i].viaUdp ? seats[i].sourceIp.toString() : String("");
    seat["occupied"] = isOnline && seats[i].occupied;
    seat["call"] = isOnline && (seats[i].calling || seats[i].callPending);
    seat["seatbelt"] = isOnline && seats[i].seatbelt;
    seat["seatbeltAlert"] = isOnline && seats[i].seatbeltAlert;
    seat["age"] = (now - seats[i].lastSeen) / 1000UL;
  }
  String json;
  serializeJson(doc, json);
  webMonitorServer.sendHeader("Cache-Control", "no-store");
  webMonitorServer.send(200, "application/json", json);
}

static void webMonitorInit() {
  webMonitorServer.on("/", HTTP_GET, []() {
    webMonitorClientIp = webMonitorServer.client().remoteIP();
    webMonitorServer.sendHeader("Cache-Control", "no-store");
    webMonitorServer.send_P(200, "text/html; charset=utf-8", WEB_MONITOR_HTML);
  });
  webMonitorServer.on("/api/status", HTTP_GET, webMonitorSendStatus);
  webMonitorServer.on("/api/log", HTTP_GET, []() {
    webMonitorClientIp = webMonitorServer.client().remoteIP();
    webMonitorServer.sendHeader("Cache-Control", "no-store");
    webMonitorServer.send(200, "text/plain; charset=utf-8", SmartBusSerial.snapshot());
  });
  webMonitorServer.onNotFound([]() {
    webMonitorServer.send(404, "text/plain", "Not found");
  });
  webMonitorServer.begin();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WEB] monitor router LAN: http://%s/\n",
                  WiFi.localIP().toString().c_str());
  }
}

static void webMonitorHandle() {
  webMonitorServer.handleClient();
}
