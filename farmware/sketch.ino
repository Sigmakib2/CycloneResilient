/****************************************************************
  LoRa-Wi-Fi Chat Node (core-only, no WebSockets)
  - Each node has a configurable name via web panel
  - SSID = "LoRaChat-" + nodeName (changes when name changes)
  - Web UI is mobile-responsive
  - Deduplication removed: all RX packets are stored
****************************************************************/
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>

/* ----- user settings ----- */
const long     LORA_FREQ = 433E6;   // make sure this matches your modules (433/868/915)
const uint8_t  LORA_TTL  = 3;       // hop limit
/* ------------------------- */

/* OLED on SDA 21 / SCL 22 */
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 22, 21);

/* LoRa pins: change if your board uses different pins */
const uint8_t PIN_CS  = 5;
const uint8_t PIN_RST = 14;
const uint8_t PIN_DIO0= 26;

/* Web server */
WebServer http(80);

/* node ID from MAC (last 2 hex chars) */
char nodeID[3] = "00";

/* persistent node name */
Preferences prefs;
String nodeName = "Node-00";

/* ----- tiny message store ----- */
struct Msg { uint32_t seq; String id; String txt; };
Msg buf[50];
uint8_t head = 0;
uint32_t nextSeq = 1;

/* push into ring buffer */
void store(const String &id, const String &txt) {
  buf[head] = { nextSeq++, id, txt };
  head = (head + 1) % 50;
}

/* LoRa send helper */
void loraSend(const String &j, uint8_t ttl) {
  Serial.print("[LoRa] TX ttl=");
  Serial.print(ttl);
  Serial.print(" payload=");
  Serial.println(j);

  LoRa.beginPacket();
  LoRa.write(ttl);
  LoRa.print(j);
  LoRa.endPacket();
}

/* OLED helper */
void draw(const String &l1, const String &l2 = "") {
  oled.clearBuffer();
  oled.drawStr(0, 12, l1.c_str());
  oled.drawStr(0, 28, l2.c_str());
  oled.sendBuffer();
}

/* ----- HTML page (in flash) ----- */
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>LoRa Chat</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    html,body{height:100%}
    body{
      font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      margin:0;
      display:flex;
      flex-direction:column;
      height:100vh;
      max-width:680px;
      margin-left:auto;
      margin-right:auto;
      background:#fafafa;
    }
    header{
      background:#333;
      color:#fff;
      padding:10px 12px;
      font-size:15px;
      position:sticky;
      top:0;
      z-index:10;
    }
    #nodeInfo{
      margin-top:4px;
      font-size:13px;
      opacity:0.9;
    }
    #settings{
      padding:8px 10px;
      background:#f5f5f5;
      border-bottom:1px solid #ddd;
      font-size:13px;
    }
    #nameForm{
      display:flex;
      align-items:center;
      gap:6px;
      width:100%;
    }
    #nameForm label{
      white-space:nowrap;
    }
    #nameInput{
      flex:1;
      padding:6px 8px;
      border-radius:4px;
      border:1px solid #ccc;
      font-size:14px;
    }
    #nameForm button{
      padding:6px 10px;
      border:none;
      border-radius:4px;
      background:#333;
      color:#fff;
      font-size:13px;
      cursor:pointer;
      flex-shrink:0;
    }
    #nameForm button:active{
      transform:scale(0.98);
    }

    #log{
      flex:1;
      overflow-y:auto;
      padding:10px;
      background:#eee;
    }
    .m{
      margin:4px 0;
      padding:6px 10px;
      border-radius:12px;
      max-width:80%;
      font-size:14px;
      line-height:1.3;
      word-wrap:break-word;
    }
    .me{
      background:#cfefff;
      margin-left:auto;
    }
    .them{
      background:#fff;
      margin-right:auto;
    }

    form#f{
      display:flex;
      background:#ddd;
      padding:8px;
      gap:6px;
    }
    form#f input[type=text]{
      flex:1;
      padding:8px 10px;
      border-radius:16px;
      border:1px solid #bbb;
      font-size:14px;
    }
    form#f button{
      padding:8px 14px;
      border-radius:16px;
      border:none;
      background:#333;
      color:#fff;
      font-size:14px;
      cursor:pointer;
      flex-shrink:0;
    }
    form#f button:active{
      transform:scale(0.98);
    }

    @media (max-width:480px){
      header{
        padding:8px 10px;
        font-size:14px;
      }
      #settings{
        padding:6px 8px;
      }
      #nameForm{
        flex-direction:column;
        align-items:stretch;
      }
      #nameForm label{
        margin-bottom:2px;
      }
      #nameInput{
        width:100%;
      }
      #nameForm button{
        width:100%;
        margin-top:4px;
      }
      #log{
        padding:8px;
      }
      .m{
        font-size:13px;
      }
      form#f{
        padding:6px;
      }
      form#f button{
        padding:8px 10px;
      }
    }
  </style>
</head>
<body>
  <header>
    <div>LoRa Chat Node</div>
    <div id="nodeInfo">Node name: <strong id="nodeName">...</strong></div>
  </header>

  <div id="settings">
    <form id="nameForm">
      <label for="nameInput">Change node name:</label>
      <input id="nameInput" type="text" maxlength="20" />
      <button type="submit">Save</button>
    </form>
  </div>

  <div id="log"></div>

  <form id="f">
    <input id="msg" type="text" autocomplete="off" placeholder="Type a message">
    <button>Send</button>
  </form>

  <script>
    let nodeName = "Node";

    const log = document.getElementById('log');
    const nameSpan = document.getElementById('nodeName');
    const nameForm = document.getElementById('nameForm');
    const nameInput = document.getElementById('nameInput');
    const form = document.getElementById('f');
    const msg = document.getElementById('msg');
    let last = 0;

    function add(j, me){
      const d = document.createElement('div');
      d.className = 'm ' + (me ? 'me' : 'them');
      d.textContent = j.id + ': ' + j.txt;
      log.appendChild(d);
      log.scrollTop = log.scrollHeight;
    }

    async function loadName(){
      try {
        const r = await fetch('/name');
        const j = await r.json();
        nodeName = j.name || 'Node';
        nameSpan.textContent = nodeName;
        nameInput.value = nodeName;
      } catch(e) {
        console.log(e);
      }
    }

    async function poll(){
      try {
        const r = await fetch('/poll?since=' + last);
        const a = await r.json();
        a.forEach(m => {
          const isMe = (m.id === nodeName);
          add(m, isMe);
          last = m.seq;
        });
      } catch(e) {}
      setTimeout(poll, 1000);
    }

    nameForm.onsubmit = async e => {
      e.preventDefault();
      const n = nameInput.value.trim();
      if (!n) return;
      try {
        await fetch('/setname?name=' + encodeURIComponent(n));
        nodeName = n;
        nameSpan.textContent = nodeName;
      } catch(e) {}
    };

    form.onsubmit = async e => {
      e.preventDefault();
      const t = msg.value.trim();
      if (!t) return;
      msg.value = '';
      try {
        await fetch('/send?txt=' + encodeURIComponent(t));
      } catch(e) {}
    };

    loadName();
    poll();
  </script>
</body>
</html>
)HTML";

/* ----- HTTP handlers ----- */
void handleRoot() {
  http.send_P(200, "text/html", INDEX_HTML);
}

/* return current node name as JSON */
void handleName() {
  String out = "{\"name\":\"" + nodeName + "\"}";
  http.send(200, "application/json", out);
}

/* set node name via ?name= and also change SSID */
void handleSetName() {
  if (!http.hasArg("name")) {
    http.send(400, "text/plain", "missing name");
    return;
  }
  String n = http.arg("name");
  n.trim();
  if (n.length() == 0 || n.length() > 20) {
    http.send(400, "text/plain", "invalid length");
    return;
  }

  nodeName = n;
  prefs.putString("name", nodeName);

  String ssid = "LoRaChat-" + nodeName;
  WiFi.softAP(ssid.c_str());
  IPAddress ip = WiFi.softAPIP();

  Serial.print("[WiFi] New SSID: ");
  Serial.print(ssid);
  Serial.print("  IP: ");
  Serial.println(ip);

  draw("Name: " + nodeName, "SSID: " + ssid);

  http.send(200, "text/plain", "ok");
}

/* send chat message (id = nodeName) */
void handleSend() {
  if (!http.hasArg("txt")) {
    http.send(400, "text/plain", "bad");
    return;
  }
  String txt = http.arg("txt");

  String js = "{\"id\":\"" + nodeName + "\",\"txt\":\"" + txt + "\"}";
  store(nodeName, txt);        // store locally
  loraSend(js, LORA_TTL);      // send over LoRa

  http.send(200, "text/plain", "ok");
}

/* poll recent messages */
void handlePoll() {
  uint32_t since = http.hasArg("since") ? http.arg("since").toInt() : 0;
  String out = "[";

  for (uint8_t i = 0; i < 50; i++) {
    uint8_t idx = (head + i) % 50;
    if (buf[idx].seq > since) {
      if (out.length() > 1) out += ',';
      out += "{\"seq\":" + String(buf[idx].seq) +
             ",\"id\":\"" + buf[idx].id +
             "\",\"txt\":\"" + buf[idx].txt + "\"}";
    }
  }
  out += "]";
  http.send(200, "application/json", out);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== LoRa Chat Node Boot ===");

  Wire.begin(21, 22, 400000);
  oled.begin();
  oled.setFont(u8g2_font_6x12_tr);

  String mac = WiFi.macAddress();
  String last2 = mac.substring(15);   // last byte "FF"
  snprintf(nodeID, sizeof(nodeID), "%s", last2.c_str());

  prefs.begin("loraChat", false);
  String defaultName = "Node-" + String(nodeID);
  nodeName = prefs.getString("name", defaultName);
  if (nodeName.length() == 0) nodeName = defaultName;

  String ssid = "LoRaChat-" + nodeName;
  WiFi.softAP(ssid.c_str());
  IPAddress ip = WiFi.softAPIP();
  draw("SSID: " + ssid, "IP " + ip.toString());

  Serial.print("[WiFi] SSID: ");
  Serial.print(ssid);
  Serial.print("  IP: ");
  Serial.println(ip);

  http.on("/", handleRoot);
  http.on("/name", handleName);
  http.on("/setname", handleSetName);
  http.on("/send", handleSend);
  http.on("/poll", handlePoll);
  http.begin();
  Serial.println("[HTTP] Server started");

  LoRa.setPins(PIN_CS, PIN_RST, PIN_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] FAIL to start");
    draw("LoRa FAIL");
    while (true) { delay(1000); }
  }

  LoRa.setSyncWord(0x12);
  LoRa.setTxPower(17); // OK with antenna attached
  Serial.print("[LoRa] Started at ");
  Serial.print(LORA_FREQ);
  Serial.println(" Hz");
  draw("LoRa OK", nodeName);
}

void loop() {
  http.handleClient();

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    uint8_t ttl = LoRa.read();
    if (ttl == 0) {
      Serial.println("[LoRa] RX with ttl=0, dropping");
      while (LoRa.available()) LoRa.read();
      return;
    }

    String js;
    while (LoRa.available()) {
      js += (char)LoRa.read();
    }

    Serial.print("[LoRa] RX raw: ");
    Serial.println(js);
    Serial.print("      RSSI=");
    Serial.print(rssi);
    Serial.print(" dBm SNR=");
    Serial.println(snr);

    // parse {"id":"xx","txt":"..."}
    int i = js.indexOf("\"id\":\"");
    int j = js.indexOf("\",\"txt\":\"");
    int k = js.lastIndexOf("\"}");

    if (i < 0 || j < i || k < j) {
      Serial.println("[LoRa] Malformed JSON, ignoring");
      return;
    }

    String id  = js.substring(i + 6, j);
    String txt = js.substring(j + 10, k);

    store(id, txt);                       // store every received message
    draw("RX " + id, txt.substring(0, 16));

    if (ttl > 1) {
      Serial.println("[LoRa] Rebroadcasting with ttl-1");
      loraSend(js, ttl - 1);             // forward
    }
  }
}
