#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <string.h>

const uint8_t WIFI_CHANNEL = 1;
const uint8_t MAX_HOPS = 6;
const uint8_t MAX_TEXT = 127;
const size_t RX_QUEUE_DEPTH = 12;
const size_t SEEN_CACHE_SIZE = 96;
const size_t LIVE_EVENT_COUNT = 8;
const size_t MAX_NODES = 32;

const uint32_t PACKET_MAGIC = 0x534D4553;
const uint8_t PUBLIC_MESSAGE = 1;
const uint8_t PRIVATE_MESSAGE = 2;
const uint8_t PRESENCE_MESSAGE = 3;

const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

WebServer server(80);
QueueHandle_t rxQueue = nullptr;

uint8_t nodeMac[6] = {};
uint8_t activePrivateKey[32] = {};
uint8_t activeChannelId[8] = {};
bool privateKeyUnlocked = false;
uint32_t txSequence = 0;

#pragma pack(push, 1)
struct MeshPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t hops;
  uint8_t length;
  uint8_t origin[6];
  uint32_t sequence;
  uint8_t channelId[8];
  uint8_t nonce[12];
  uint8_t payload[128];
  uint8_t tag[16];
};
#pragma pack(pop)

struct SeenEntry {
  uint8_t origin[6];
  uint32_t sequence;
  uint32_t seenAt;
  bool valid;
};

struct LiveEvent {
  uint32_t id;
  char text[190];
  bool valid;
};

struct ActiveNode {
  uint8_t mac[6];
  uint32_t lastSeen;
};

SeenEntry seenCache[SEEN_CACHE_SIZE] = {};
LiveEvent liveEvents[LIVE_EVENT_COUNT] = {};
ActiveNode meshNodes[MAX_NODES] = {};

size_t seenIndex = 0;
size_t liveIndex = 0;
uint32_t liveEventId = 0;
uint8_t previousClientCount = 0;
uint32_t lastHeartbeatTx = 0;

const char PAGE[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OGEMTN</title>
<style>
body{font-family:system-ui,Arial,sans-serif;max-width:720px;margin:22px auto;padding:14px;background:#f2f5fa;color:#182230}
main{background:#fff;padding:20px;border-radius:14px;box-shadow:0 2px 12px #0001}
input,select,textarea,button{box-sizing:border-box;width:100%;padding:12px;margin:7px 0;font:inherit;border-radius:8px}
input,select,textarea{border:1px solid #b8c2d0}
button{background:#175cd3;color:white;border:0;cursor:pointer;font-weight:600}
button.secondary{background:#667085}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#101828;color:#e6edf3;padding:13px;border-radius:8px;min-height:110px}
small,.muted{color:#667085;font-size:.9rem}
.hidden{display:none}
.checkrow{display:flex;align-items:center;gap:9px}
.checkrow input{width:auto}
.header-container{display:flex;justify-content:space-between;align-items:center;margin-bottom:5px}
h1{margin:0}
.badge{font-size:.9rem;font-weight:600;background:#eef2f6;color:#344054;padding:6px 12px;border-radius:20px;display:flex;align-items:center;gap:6px}
.dot{width:8px;height:8px;background:#12b76a;border-radius:50%;display:inline-block}
.dot.offline{background:#f04438}
.mac-display{font-family:monospace;background:#f2f5fa;padding:3px 6px;border-radius:4px;color:#182230}
</style>
</head>
<body>
<main>
<div class="header-container">
  <h1>OGEMTN</h1>
  <div id="nodeBadge" class="badge"><span class="dot offline"></span>Scanning...</div>
</div>
<div class="muted" style="margin-bottom:20px;">Node MAC: <span id="myMac" class="mac-display">Loading...</span></div>
<p class="muted">Decentralized ESP-NOW messaging. The ESP32 only buffers a few live events in RAM. This browser keeps its own local message history.</p>
<form id="messageForm">
<label for="mode">Channel</label>
<select id="mode" name="type">
<option value="public">Public broadcast</option>
<option value="private">Private group</option>
</select>
<div id="privateBox" class="hidden">
<label for="secret">Private group secret</label>
<input id="secret" name="secret" type="password" maxlength="120" autocomplete="off" placeholder="Enter the shared secret">
<label class="checkrow"><input id="rememberSecret" type="checkbox" checked> Remember secret in this browser</label>
<button id="unlockButton" type="button">Unlock private group</button>
<div id="unlockStatus" class="muted"></div>
</div>
<label for="message">Message</label>
<textarea id="message" name="message" maxlength="127" rows="3" required placeholder="Type a message"></textarea>
<button type="submit">Send message</button>
<div id="sendStatus" class="muted" aria-live="polite"></div>
</form>
<h2 style="margin-top:30px;">Messages received by this browser</h2>
<button type="button" class="secondary" id="clearHistory">Clear browser history</button>
<pre id="liveFeed">No messages saved in this browser yet.</pre>
</main>
<script>
const mode=document.getElementById('mode');
const privateBox=document.getElementById('privateBox');
const secret=document.getElementById('secret');
const rememberSecret=document.getElementById('rememberSecret');
const unlockStatus=document.getElementById('unlockStatus');
const sendStatus=document.getElementById('sendStatus');
const feed=document.getElementById('liveFeed');
const form=document.getElementById('messageForm');
const nodeBadge=document.getElementById('nodeBadge');
const macDisplay=document.getElementById('myMac');
const HISTORY_KEY='ogemtn.messageHistory.v1';
const MAX_HISTORY=100;
let cursor=0;
let polling=false;
let messageHistory=[];

function loadHistory(){
  try{
    const saved=JSON.parse(localStorage.getItem(HISTORY_KEY)||'[]');
    messageHistory=Array.isArray(saved)?saved.filter(x=>x&&typeof x.text==='string'&&typeof x.time==='string').slice(-MAX_HISTORY):[];
  }catch(e){
    messageHistory=[];
  }
  renderHistory();
}

function saveHistory(){
  try{
    localStorage.setItem(HISTORY_KEY,JSON.stringify(messageHistory.slice(-MAX_HISTORY)));
  }catch(e){}
}

function renderHistory(){
  if(!messageHistory.length){
    feed.textContent='No messages saved in this browser yet.';
    return;
  }
  feed.textContent=messageHistory.map(item=>'['+item.time+'] '+item.text).join('\n');
}

function addBrowserMessage(text){
  messageHistory.push({time:new Date().toLocaleString(),text:String(text)});
  if(messageHistory.length>MAX_HISTORY) messageHistory=messageHistory.slice(-MAX_HISTORY);
  saveHistory();
  renderHistory();
}

function updatePrivateBox(){
  privateBox.classList.toggle('hidden',mode.value!=='private');
  try{localStorage.setItem('ogemtn.mode',mode.value);}catch(e){}
}

function saveSecret(){
  try{
    if(rememberSecret.checked){
      if(secret.value) localStorage.setItem('ogemtn.secret',secret.value);
      else localStorage.removeItem('ogemtn.secret');
      localStorage.setItem('ogemtn.rememberSecret','1');
    }else{
      localStorage.removeItem('ogemtn.secret');
      localStorage.setItem('ogemtn.rememberSecret','0');
    }
  }catch(e){}
}

async function unlockPrivate(){
  if(!secret.value){
    unlockStatus.textContent='Enter a private group secret first.';
    return false;
  }
  saveSecret();
  try{
    const body=new URLSearchParams({secret:secret.value});
    const response=await fetch('/unlock',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
    unlockStatus.textContent=await response.text();
    return response.ok;
  }catch(e){
    unlockStatus.textContent='Could not unlock private group.';
    return false;
  }
}

try{
  mode.value=localStorage.getItem('ogemtn.mode')||'public';
  const remember=localStorage.getItem('ogemtn.rememberSecret')!=='0';
  rememberSecret.checked=remember;
  secret.value=remember?localStorage.getItem('ogemtn.secret')||'':'';
}catch(e){
  mode.value='public';
  rememberSecret.checked=false;
}
updatePrivateBox();
loadHistory();

mode.addEventListener('change',updatePrivateBox);
secret.addEventListener('input',saveSecret);
rememberSecret.addEventListener('change',saveSecret);
document.getElementById('unlockButton').addEventListener('click',unlockPrivate);
document.getElementById('clearHistory').addEventListener('click',function(){
  messageHistory=[];
  try{localStorage.removeItem(HISTORY_KEY);}catch(e){}
  renderHistory();
});

form.addEventListener('submit',async function(e){
  e.preventDefault();
  sendStatus.textContent='';
  const message=document.getElementById('message').value.trim();
  if(!message){sendStatus.textContent='Enter a message first.';return;}
  if(mode.value==='private'){
    if(!secret.value){unlockStatus.textContent='Enter a private group secret first.';return;}
    saveSecret();
  }
  const body=new URLSearchParams({type:mode.value,message});
  if(mode.value==='private') body.set('secret',secret.value);
  try{
    const response=await fetch('/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
    sendStatus.textContent=await response.text();
    if(response.ok) document.getElementById('message').value='';
  }catch(e){
    sendStatus.textContent='Send failed.';
  }
});

async function pollLive(){
  if(polling)return;
  polling=true;
  try{
    const r=await fetch('/events?cursor='+encodeURIComponent(cursor),{cache:'no-store'});
    if(r.ok){
      const data=await r.json();
      const latest=Number(data.cursor);
      if(Number.isFinite(latest)) cursor=latest;
      
      if(data.mac) macDisplay.textContent = data.mac;
      
      if(data.nodes !== undefined) {
        const isAlone = data.nodes <= 1;
        nodeBadge.innerHTML = '<span class="dot ' + (isAlone ? 'offline' : '') + '"></span>' + data.nodes + ' node' + (data.nodes === 1 ? '' : 's');
      }

      if(Array.isArray(data.events)){
        for(const item of data.events){
          if(item&&typeof item.text==='string') addBrowserMessage(item.text);
        }
      }
    }
  }catch(e){}
  polling=false;
}

async function start(){
  if(secret.value&&mode.value==='private') await unlockPrivate();
  try{
    const r=await fetch('/cursor',{cache:'no-store'});
    cursor=Number(await r.text())||0;
  }catch(e){}
  setInterval(pollLive,800);
}
start();
</script>
</body>
</html>
)html";

String macString(const uint8_t *mac) {
  char value[18];
  snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(value);
}

String jsonEscape(const char *input) {
  String out;
  while (*input) {
    uint8_t c = (uint8_t)*input++;
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) out += ' ';
    else out += (char)c;
  }
  return out;
}

void clearLiveEvents() {
  for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {
    liveEvents[i].valid = false;
    liveEvents[i].id = 0;
    liveEvents[i].text[0] = '\0';
  }
  liveIndex = 0;
}

void publishLive(const String &text) {
  if (WiFi.softAPgetStationNum() == 0) return;
  LiveEvent &event = liveEvents[liveIndex];
  event.id = ++liveEventId;
  text.toCharArray(event.text, sizeof(event.text));
  event.valid = true;
  liveIndex = (liveIndex + 1) % LIVE_EVENT_COUNT;
}

void updateNodePresence(const uint8_t *mac) {
  uint32_t now = millis();
  int emptySlot = -1;
  for (size_t i = 0; i < MAX_NODES; i++) {
    if (meshNodes[i].lastSeen != 0 && memcmp(meshNodes[i].mac, mac, 6) == 0) {
      meshNodes[i].lastSeen = now;
      return;
    }
    if (meshNodes[i].lastSeen == 0 && emptySlot == -1) {
      emptySlot = i;
    }
  }
  if (emptySlot != -1) {
    memcpy(meshNodes[emptySlot].mac, mac, 6);
    meshNodes[emptySlot].lastSeen = now;
  }
}

uint32_t getActiveNodesCount() {
  uint32_t count = 1; // Always include self
  uint32_t now = millis();
  for (size_t i = 0; i < MAX_NODES; i++) {
    if (meshNodes[i].lastSeen != 0) {
      if (now - meshNodes[i].lastSeen > 90000) {
        meshNodes[i].lastSeen = 0; // Expire stale node
      } else {
        count++;
      }
    }
  }
  return count;
}

bool deriveKeys(const String &secret, uint8_t aesKey[32], uint8_t channelId[8]) {
  if (secret.length() == 0) return false;
  uint8_t digest[32];
  String keyMaterial = "OGEMTN-AES-v1:" + secret;
  if (mbedtls_sha256((const unsigned char *)keyMaterial.c_str(), keyMaterial.length(), aesKey, 0) != 0) return false;
  String channelMaterial = "OGEMTN-Channel-v1:" + secret;
  if (mbedtls_sha256((const unsigned char *)channelMaterial.c_str(), channelMaterial.length(), digest, 0) != 0) {
    memset(aesKey, 0, 32);
    return false;
  }
  memcpy(channelId, digest, 8);
  memset(digest, 0, sizeof(digest));
  return true;
}

bool unlockKey(const String &secret) {
  uint8_t key[32];
  uint8_t channelId[8];
  if (!deriveKeys(secret, key, channelId)) return false;
  memset(activePrivateKey, 0, sizeof(activePrivateKey));
  memcpy(activePrivateKey, key, sizeof(activePrivateKey));
  memcpy(activeChannelId, channelId, sizeof(activeChannelId));
  privateKeyUnlocked = true;
  memset(key, 0, sizeof(key));
  memset(channelId, 0, sizeof(channelId));
  return true;
}

size_t makeAAD(const MeshPacket &p, uint8_t *out) {
  uint8_t *q = out;
  memcpy(q, &p.magic, 4);
  q += 4;
  *q++ = p.version;
  *q++ = p.type;
  *q++ = p.length;
  memcpy(q, p.origin, 6);
  q += 6;
  memcpy(q, &p.sequence, 4);
  q += 4;
  memcpy(q, p.channelId, 8);
  q += 8;
  return (size_t)(q - out);
}

bool encryptPrivate(MeshPacket &packet, const String &secret) {
  uint8_t key[32];
  uint8_t channelId[8];
  uint8_t aad[25];
  uint8_t plaintext[128] = {};
  if (!deriveKeys(secret, key, channelId)) return false;
  memcpy(plaintext, packet.payload, packet.length);
  memcpy(packet.channelId, channelId, sizeof(packet.channelId));
  esp_fill_random(packet.nonce, sizeof(packet.nonce));
  size_t aadLength = makeAAD(packet, aad);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0) {
    result = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
      packet.length, packet.nonce, sizeof(packet.nonce), aad, aadLength,
      plaintext, packet.payload, sizeof(packet.tag), packet.tag);
  }
  mbedtls_gcm_free(&ctx);
  memset(key, 0, sizeof(key));
  memset(channelId, 0, sizeof(channelId));
  memset(plaintext, 0, sizeof(plaintext));
  return result == 0;
}

bool decryptPrivate(const MeshPacket &packet, uint8_t *plaintext) {
  if (!privateKeyUnlocked) return false;
  if (memcmp(packet.channelId, activeChannelId, sizeof(activeChannelId)) != 0) return false;
  uint8_t aad[25];
  size_t aadLength = makeAAD(packet, aad);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, activePrivateKey, 256);
  if (result == 0) {
    result = mbedtls_gcm_auth_decrypt(&ctx, packet.length,
      packet.nonce, sizeof(packet.nonce), aad, aadLength,
      packet.tag, sizeof(packet.tag), packet.payload, plaintext);
  }
  mbedtls_gcm_free(&ctx);
  return result == 0;
}

bool isDuplicate(const MeshPacket &packet) {
  uint32_t now = millis();
  for (size_t i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seenCache[i].valid && (uint32_t)(now - seenCache[i].seenAt) > 120000) {
      seenCache[i].valid = false;
    }
    if (seenCache[i].valid && seenCache[i].sequence == packet.sequence &&
        memcmp(seenCache[i].origin, packet.origin, 6) == 0) return true;
  }
  return false;
}

void rememberPacket(const MeshPacket &packet) {
  SeenEntry &entry = seenCache[seenIndex];
  memcpy(entry.origin, packet.origin, 6);
  entry.sequence = packet.sequence;
  entry.seenAt = millis();
  entry.valid = true;
  seenIndex = (seenIndex + 1) % SEEN_CACHE_SIZE;
}

bool broadcastPacket(const MeshPacket &packet) {
  return esp_now_send(BROADCAST_MAC, (const uint8_t *)&packet, sizeof(packet)) == ESP_OK;
}

void processPacket(MeshPacket &packet) {
  if (packet.magic != PACKET_MAGIC || packet.version != 1) return;
  if (packet.type != PUBLIC_MESSAGE && packet.type != PRIVATE_MESSAGE && packet.type != PRESENCE_MESSAGE) return;
  if (packet.type != PRESENCE_MESSAGE && packet.length == 0) return;
  if (packet.length > MAX_TEXT || packet.hops == 0) return;
  if (isDuplicate(packet)) return;
  rememberPacket(packet);

  // Update presence regardless of message type
  updateNodePresence(packet.origin);

  String origin = macString(packet.origin);
  if (packet.type == PUBLIC_MESSAGE) {
    char message[128] = {};
    memcpy(message, packet.payload, packet.length);
    message[packet.length] = '\0';
    publishLive("[PUBLIC] " + origin + ": " + String(message));
  } else if (packet.type == PRIVATE_MESSAGE) {
    uint8_t plaintext[128] = {};
    if (decryptPrivate(packet, plaintext)) {
      plaintext[packet.length] = '\0';
      publishLive("[PRIVATE] " + origin + ": " + String((char *)plaintext));
      memset(plaintext, 0, sizeof(plaintext));
    }
  }

  if (packet.hops > 1) {
    packet.hops--;
    broadcastPacket(packet);
  }
}

// Cross-core compatibility for ESP32 v2.x and v3.x callback signatures
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void receiveCallback(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
#else
void receiveCallback(const uint8_t *macAddr, const uint8_t *data, int length) {
#endif
  if (rxQueue == nullptr || length != (int)sizeof(MeshPacket)) return;
  MeshPacket packet;
  memcpy(&packet, data, sizeof(packet));
  xQueueSend(rxQueue, &packet, 0);
}

void handleHome() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleCursor() {
  server.send(200, "text/plain", String(liveEventId));
}

void handleEvents() {
  uint32_t cursor = (uint32_t)server.arg("cursor").toInt();
  uint32_t first = liveEventId > LIVE_EVENT_COUNT ? liveEventId - LIVE_EVENT_COUNT + 1 : 1;
  if (cursor + 1 > first) first = cursor + 1;
  
  uint32_t nodeCount = getActiveNodesCount();
  // Include the node MAC in the JSON payload
  String json = "{\"cursor\":" + String(liveEventId) + ",\"mac\":\"" + macString(nodeMac) + "\",\"nodes\":" + String(nodeCount) + ",\"events\":[";
  
  bool firstItem = true;
  for (uint32_t id = first; id <= liveEventId; id++) {
    for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {
      if (!liveEvents[i].valid || liveEvents[i].id != id) continue;
      if (!firstItem) json += ",";
      json += "{\"id\":" + String(liveEvents[i].id) + ",\"text\":\"";
      json += jsonEscape(liveEvents[i].text);
      json += "\"}";
      firstItem = false;
      break;
    }
  }
  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleUnlock() {
  String secret = server.arg("secret");
  if (secret.length() == 0) {
    server.send(400, "text/plain", "Enter a private group secret.");
    return;
  }
  if (!unlockKey(secret)) {
    server.send(500, "text/plain", "Could not derive private key.");
    return;
  }
  server.send(200, "text/plain", "Private group unlocked in RAM.");
}

void handleSend() {
  String message = server.arg("message");
  String type = server.arg("type");
  String secret = server.arg("secret");
  message.trim();

  if (message.length() == 0 || message.length() > MAX_TEXT) {
    server.send(400, "text/plain", "Message must contain 1 to 127 bytes.");
    return;
  }
  if (type != "public" && type != "private") {
    server.send(400, "text/plain", "Invalid channel.");
    return;
  }
  if (type == "private" && secret.length() == 0) {
    server.send(400, "text/plain", "Private group secret required.");
    return;
  }

  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = 1;
  packet.type = type == "private" ? PRIVATE_MESSAGE : PUBLIC_MESSAGE;
  packet.hops = MAX_HOPS;
  packet.length = message.length();
  memcpy(packet.origin, nodeMac, sizeof(nodeMac));
  packet.sequence = ++txSequence;
  memcpy(packet.payload, message.c_str(), packet.length);

  if (packet.type == PRIVATE_MESSAGE) {
    if (!encryptPrivate(packet, secret)) {
      server.send(500, "text/plain", "Encryption failed.");
      return;
    }
    unlockKey(secret); // Re-lock active key using secret
    publishLive("[PRIVATE SENT] " + message);
  } else {
    publishLive("[PUBLIC SENT] " + macString(nodeMac) + ": " + message);
  }

  rememberPacket(packet);
  if (!broadcastPacket(packet)) {
    server.send(500, "text/plain", "ESP-NOW broadcast request failed.");
    return;
  }
  
  lastHeartbeatTx = millis(); // Reset heartbeat timer since we just transmitted
  server.send(200, "text/plain", "Message sent.");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  memset(seenCache, 0, sizeof(seenCache));
  memset(meshNodes, 0, sizeof(meshNodes));
  clearLiveEvents();
  txSequence = esp_random();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_wifi_get_mac(WIFI_IF_STA, nodeMac) != ESP_OK) {
    Serial.println("Could not read ESP-NOW MAC.");
    while (true) delay(1000);
  }

  String ssid = "ESP_NOW_" + macString(nodeMac);
  if (!WiFi.softAP(ssid.c_str(), nullptr, WIFI_CHANNEL, false, 4)) {
    Serial.println("Access point startup failed.");
    while (true) delay(1000);
  }
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, sizeof(BROADCAST_MAC));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t peerResult = esp_now_add_peer(&peer);
  if (peerResult != ESP_OK && peerResult != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("Broadcast peer registration failed: %d\n", (int)peerResult);
    while (true) delay(1000);
  }

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(MeshPacket));
  if (rxQueue == nullptr) {
    Serial.println("Receive queue creation failed.");
    while (true) delay(1000);
  }

// Core compatibility hook
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  if (esp_now_register_recv_cb(receiveCallback) != ESP_OK) {
#else
  if (esp_now_register_recv_cb((esp_now_recv_cb_t)receiveCallback) != ESP_OK) {
#endif
    Serial.println("Receive callback registration failed.");
    while (true) delay(1000);
  }

  server.on("/", HTTP_GET, handleHome);
  server.on("/cursor", HTTP_GET, handleCursor);
  server.on("/events", HTTP_GET, handleEvents);
  server.on("/unlock", HTTP_POST, handleUnlock);
  server.on("/send", HTTP_POST, handleSend);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found.");
  });
  server.begin();

  Serial.println();
  Serial.println("OGEMTN started.");
  Serial.printf("SSID: %s\n", ssid.c_str());
  Serial.printf("ESP-NOW MAC: %s\n", macString(nodeMac).c_str());
  Serial.printf("Web address: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Wi-Fi channel: %u\n", WIFI_CHANNEL);
}

void loop() {
  server.handleClient();

  uint8_t clients = WiFi.softAPgetStationNum();
  if (clients == 0 && previousClientCount > 0) {
    clearLiveEvents();
  }
  previousClientCount = clients;

  // Process incoming queue
  MeshPacket packet;
  while (xQueueReceive(rxQueue, &packet, 0) == pdTRUE) {
    processPacket(packet);
  }

  // Automatic Presence Heartbeat (every 30 seconds)
  uint32_t now = millis();
  if (now - lastHeartbeatTx > 30000) {
    lastHeartbeatTx = now;
    MeshPacket hb = {};
    hb.magic = PACKET_MAGIC;
    hb.version = 1;
    hb.type = PRESENCE_MESSAGE;
    hb.hops = MAX_HOPS;
    hb.length = 0;
    memcpy(hb.origin, nodeMac, sizeof(nodeMac));
    hb.sequence = ++txSequence;
    
    rememberPacket(hb);
    broadcastPacket(hb);
  }

  delay(1);
}