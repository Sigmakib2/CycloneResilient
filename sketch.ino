/****************************************************************
  SIMPLE LoRa + WiFi Chat Node (with OLED status) + MESH RELAY
  - ESP32 AP, SSID set per node in WIFI_SSID
  - Open http://192.168.4.1 to chat
  - Browser asks user name (stored locally)
  - Messages sent over LoRa as JSON:
      {
        "v":1,
        "type":"chat",
        "src": <nodeId>,
        "name":"Rakib",
        "seq": <per-node message counter>,
        "hop": <current hop>,
        "maxHop": <ttl>,
        "txt":"Hello"
      }
  - Each node:
      * Displays all valid chat messages
      * Forwards messages if:
          - not from itself
          - not seen before (src,seq)
          - hop < maxHop
****************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>   // <--- ADD THIS

/********** SETTINGS **********/
const long  LORA_FREQ  = 433E6;              // 433E6 / 868E6 / 915E6
const char* WIFI_SSID  = "LoRaChat-Node-A";  // <- CHANGE THIS FOR EACH NODE
const char* WIFI_PASS  = "";                 // open AP

// Unique mesh node ID (1..255) - CHANGE PER NODE
const uint8_t MY_ID = 1;

// Mesh settings
const uint8_t MAX_HOP_DEFAULT = 3;   // max hops for messages from this node

// LoRa pins (adjust if your board is different)
const uint8_t LORA_CS   = 5;
const uint8_t LORA_RST  = 14;
const uint8_t LORA_DIO0 = 26;
/******************************/

// OLED: SH1106 128x64, I2C on SDA 21 / SCL 22
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 22, 21);

WebServer http(80);

/*** tiny message buffer in RAM (for local web clients) ***/
struct Msg {
  uint32_t seq;
  String name;
  String text;
};

Msg messages[50];
uint8_t msgHead = 0;
uint32_t nextSeq = 1;      // local sequence for web UI

/*** mesh-level sequence for this node ***/
uint32_t meshSeq = 1;      // used in LoRa JSON "seq"

/*** duplicate detection buffer for mesh ***/
struct Seen {
  uint8_t  src;
  uint32_t seq;
};

Seen seenBuf[40];
uint8_t seenHead = 0;

/*** OLED helper ***/
void drawOLED(const String &l1, const String &l2 = "") {
  oled.clearBuffer();
  oled.drawStr(0, 12, l1.c_str());
  oled.drawStr(0, 28, l2.c_str());
  oled.sendBuffer();
}

/*** helper: store message into ring buffer (for HTTP /poll) ***/
void storeMsg(const String &name, const String &text) {
  messages[msgHead].seq  = nextSeq++;
  messages[msgHead].name = name;
  messages[msgHead].text = text;
  msgHead = (msgHead + 1) % 50;
}

/*** helper: escape text for JSON string (HTTP response) ***/
String jsonEscape(const String &in) {
  String out = in;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  return out;
}

/*** duplicate detection helpers (for mesh) ***/
bool alreadySeen(uint8_t src, uint32_t seq) {
  for (uint8_t i = 0; i < 40; i++) {
    if (seenBuf[i].src == src && seenBuf[i].seq == seq) return true;
  }
  return false;
}

void markSeen(uint8_t src, uint32_t seq) {
  seenBuf[seenHead].src = src;
  seenBuf[seenHead].seq = seq;
  seenHead = (seenHead + 1) % 40;
}

/*** LoRa send helper: send JSON chat packet with mesh fields ***/
void loraSendChat(const String &name, const String &text) {
  StaticJsonDocument<192> doc;
  doc["v"]      = 1;
  doc["type"]   = "chat";
  doc["src"]    = MY_ID;
  doc["name"]   = name;
  doc["seq"]    = meshSeq++;          // per-node counter
  doc["hop"]    = 0;                  // origin node
  doc["maxHop"] = MAX_HOP_DEFAULT;    // TTL
  doc["txt"]    = text;

  String payload;
  serializeJson(doc, payload);

  Serial.print("[LoRa] TX: ");
  Serial.println(payload);

  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();
}

/*** Handle incoming LoRa packets: display + decide kill/pass ***/
void handleLoRaRx() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  String payload;
  while (LoRa.available()) {
    payload += (char)LoRa.read();
  }

  Serial.print("[LoRa] RX: ");
  Serial.println(payload);

  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[LoRa] JSON error: ");
    Serial.println(err.c_str());
    return; // kill
  }

  uint8_t v        = doc["v"]   | 1;
  const char* type = doc["type"] | "chat";
  uint8_t src      = doc["src"] | 0;
  uint32_t seq     = doc["seq"] | 0;
  uint8_t hop      = doc["hop"] | 0;
  uint8_t maxHop   = doc["maxHop"] | MAX_HOP_DEFAULT;
  const char* nameC = doc["name"] | "";
  const char* txtC  = doc["txt"]  | "";

  // basic validation
  if (strcmp(type, "chat") != 0) {
    Serial.println("[LoRa] Ignored non-chat packet");
    return;
  }
  if (src == 0 || seq == 0) {
    Serial.println("[LoRa] Invalid src/seq");
    return;
  }

  String name = String(nameC);
  String text = String(txtC);
  name.trim();
  text.trim();
  if (name.length() == 0 || text.length() == 0) {
    Serial.println("[LoRa] Empty name/text");
    return;
  }

  // LOCAL HANDLING: always store & display valid chat packets
  storeMsg(name, text);
  drawOLED("RX from: " + name, text.substring(0, 16));

  // FORWARDING DECISION (mesh)
  // 1) if packet originated from this node, don't forward
  if (src == MY_ID) {
    Serial.println("[LoRa] Not forwarding own packet");
    return;
  }

  // 2) if already seen this (src,seq), don't forward
  if (alreadySeen(src, seq)) {
    Serial.println("[LoRa] Duplicate, not forwarding");
    return;
  }

  // mark as seen
  markSeen(src, seq);

  // 3) hop limit
  if (hop >= maxHop) {
    Serial.println("[LoRa] TTL exceeded, not forwarding");
    return;
  }

  // PASS: forward with hop+1
  hop++;
  doc["hop"] = hop;

  String out;
  serializeJson(doc, out);

  // small random delay to reduce collision chance
  delay(random(20, 150));

  Serial.print("[LoRa] FWD: ");
  Serial.println(out);

  LoRa.beginPacket();
  LoRa.print(out);
  LoRa.endPacket();
}

/*** HTML/JS page ***/
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>LoRa Chat</title>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{
      font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      max-width:680px;
      margin:0 auto;
      height:100vh;
      display:flex;
      flex-direction:column;
      background:#fafafa;
    }
    header{
      background:#333;color:#fff;padding:8px 10px;font-size:15px;
    }
    #me{
      font-size:13px;opacity:0.9;margin-top:4px;
    }
    #log{
      flex:1;overflow-y:auto;padding:8px;background:#eee;
    }
    .m{
      margin:4px 0;padding:6px 10px;border-radius:12px;
      max-width:80%;font-size:14px;line-height:1.3;
      word-wrap:break-word;
    }
    .me{background:#cfefff;margin-left:auto;}
    .them{background:#fff;margin-right:auto;}
    form{
      display:flex;padding:8px;background:#ddd;gap:6px;
    }
    input[type=text]{
      flex:1;border-radius:16px;border:1px solid #bbb;
      padding:8px 10px;font-size:14px;
    }
    button{
      border:none;border-radius:16px;padding:8px 14px;
      font-size:14px;background:#333;color:#fff;cursor:pointer;
    }
  </style>
</head>
<body>
  <header>
    <div>LoRa Chat</div>
    <div id="me">You: <span id="myName">...</span></div>
  </header>

  <div id="log"></div>

  <form id="f">
    <input id="msg" type="text" autocomplete="off" placeholder="Type a message">
    <button type="submit">Send</button>
  </form>

<script>
  const log = document.getElementById('log');
  const form = document.getElementById('f');
  const msg  = document.getElementById('msg');
  const myNameSpan = document.getElementById('myName');
  let lastSeq = 0;
  let myName = "";

  function askName(){
    myName = localStorage.getItem('loraName') || "";
    while(!myName){
      myName = prompt("Enter your name:");
      if(myName) myName = myName.trim();
      if(myName) localStorage.setItem('loraName', myName);
    }
    myNameSpan.textContent = myName;
  }

  function addMessage(m){
    const d = document.createElement('div');
    d.className = 'm ' + (m.name === myName ? 'me' : 'them');
    d.textContent = m.name + ": " + m.text;
    log.appendChild(d);
    log.scrollTop = log.scrollHeight;
  }

  async function poll(){
    try{
      const r = await fetch('/poll?since=' + lastSeq);
      const arr = await r.json();
      arr.forEach(m => {
        addMessage(m);
        if(m.seq > lastSeq) lastSeq = m.seq;
      });
    }catch(e){
      console.log(e);
    }
    setTimeout(poll, 1000);
  }

  form.onsubmit = async e => {
    e.preventDefault();
    const t = msg.value.trim();
    if(!t) return;
    msg.value = "";
    try{
      await fetch('/send?name=' + encodeURIComponent(myName) +
                  '&txt=' + encodeURIComponent(t));
    }catch(e){
      console.log(e);
    }
  };

  askName();
  poll();
</script>
</body>
</html>
)HTML";

/*** HTTP handlers ***/
void handleRoot() {
  http.send_P(200, "text/html", INDEX_HTML);
}

void handleSend() {
  if (!http.hasArg("name") || !http.hasArg("txt")) {
    http.send(400, "text/plain", "missing params");
    return;
  }

  String name = http.arg("name");
  String text = http.arg("txt");

  name.trim();
  text.trim();

  if (name.length() == 0 || text.length() == 0) {
    http.send(400, "text/plain", "empty");
    return;
  }

  // simple length limits
  if (name.length() > 20)  name = name.substring(0, 20);
  if (text.length() > 200) text = text.substring(0, 200);

  // local store for web UI
  storeMsg(name, text);

  // send to mesh as JSON
  loraSendChat(name, text);

  http.send(200, "text/plain", "ok");
}

void handlePoll() {
  uint32_t since = 0;
  if (http.hasArg("since")) {
    since = (uint32_t) http.arg("since").toInt();
  }

  String out = "[";
  bool first = true;

  for (uint8_t i = 0; i < 50; i++) {
    uint8_t idx = (msgHead + i) % 50;
    if (messages[idx].seq == 0) continue; // empty
    if (messages[idx].seq <= since) continue;

    if (!first) out += ',';
    first = false;

    out += "{\"seq\":" + String(messages[idx].seq) +
           ",\"name\":\"" + jsonEscape(messages[idx].name) + "\"," +
           "\"text\":\"" + jsonEscape(messages[idx].text) + "\"}";
  }

  out += "]";
  http.send(200, "application/json", out);
}

void handleNotFound() {
  http.send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Simple LoRa Chat Mesh Node Boot ===");

  // random seed for forward delay
  randomSeed(analogRead(0));

  // OLED init
  Wire.begin(21, 22, 400000);
  oled.begin();
  oled.setFont(u8g2_font_6x12_tr);
  drawOLED("Booting...", "");

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[WiFi] AP SSID: ");
  Serial.print(WIFI_SSID);
  Serial.print("  IP: ");
  Serial.println(ip);

  drawOLED("SSID: " + String(WIFI_SSID), "IP: " + ip.toString());

  // HTTP routes
  http.on("/", handleRoot);
  http.on("/send", handleSend);
  http.on("/poll", handlePoll);
  http.onNotFound(handleNotFound);
  http.begin();
  Serial.println("[HTTP] Server started");

  // LoRa
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[LoRa] Failed to start");
    drawOLED("LoRa FAILED", "");
    while (true) {
      delay(1000);
    }
  }
  LoRa.setSyncWord(0x12);   // all nodes must match
  Serial.print("[LoRa] Started at ");
  Serial.print(LORA_FREQ);
  Serial.println(" Hz");

  drawOLED("SSID: " + String(WIFI_SSID), "LoRa OK");
}

void loop() {
  http.handleClient();
  handleLoRaRx();   // <--- mesh logic (receive + forward)
}