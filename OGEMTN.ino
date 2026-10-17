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
  char text[130];
  char type[10];
  char group[16];
  char sender[20]; 
  bool valid;
};

struct ActiveNode {
  uint8_t mac[6];
  uint32_t lastSeen;
};

struct RateLimit {
  uint32_t ip;
  uint32_t lastPost;
};

SeenEntry seenCache[SEEN_CACHE_SIZE] = {};
LiveEvent liveEvents[LIVE_EVENT_COUNT] = {};
ActiveNode meshNodes[MAX_NODES] = {};
RateLimit clientLimits[8] = {};

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
:root { --bg: #f2f5fa; --surface: #ffffff; --text: #182230; --border: #e2e8f0; --primary: #175cd3; --msg-bg: #f1f5f9; --my-msg: #dbeafe; --danger: #ef4444; --overlay: rgba(0,0,0,0.4); }
body.dark { --bg: #0f172a; --surface: #1e293b; --text: #f8fafc; --border: #334155; --primary: #3b82f6; --msg-bg: #334155; --my-msg: #1e3a8a; --danger: #dc2626; --overlay: rgba(0,0,0,0.7); }
body { font-family: system-ui, Arial, sans-serif; margin: 0; background: var(--bg); color: var(--text); height: 100vh; display: flex; flex-direction: column; }
header { background: var(--surface); padding: 12px 20px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); }
.app-container { display: flex; flex: 1; overflow: hidden; }
.sidebar { width: 280px; background: var(--surface); border-right: 1px solid var(--border); display: flex; flex-direction: column; }
.channel-list { flex: 1; overflow-y: auto; padding: 10px; }
.channel { padding: 12px; border-radius: 8px; cursor: pointer; margin-bottom: 5px; font-weight: 500; display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.channel:hover { background: var(--bg); }
.channel.active { background: var(--primary); color: white; }
.channel-delete { background: var(--danger); color: white; border: none; cursor: pointer; font-size: 1.2rem; width: 26px; height: 26px; border-radius: 50%; display: flex; align-items: center; justify-content: center; padding: 0; line-height: 0; margin-left: auto; transition: opacity 0.2s; }
.channel-delete:hover { opacity: 0.8; }
.add-channel { padding: 15px; border-top: 1px solid var(--border); }
input, button, select { width: 100%; box-sizing: border-box; padding: 10px; margin-top: 5px; border-radius: 6px; border: 1px solid var(--border); background: var(--surface); color: var(--text); }
button { background: var(--primary); color: white; border: none; cursor: pointer; font-weight: bold; }
button.danger { background: var(--danger); }
button.secondary { background: var(--border); color: var(--text); }
.chat-area { flex: 1; display: flex; flex-direction: column; background: var(--bg); position: relative; }
.messages { flex: 1; padding: 20px; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; }
.msg { padding: 10px 14px; border-radius: 12px; max-width: 80%; background: var(--msg-bg); align-self: flex-start; word-wrap: break-word; }
.msg.self { background: var(--my-msg); align-self: flex-end; }
.msg-time { font-size: 0.75rem; opacity: 0.7; margin-top: 4px; text-align: right; }
.composer { background: var(--surface); padding: 15px; border-top: 1px solid var(--border); }
.quick-chat { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; padding: 5px 0; }
.quick-btn { width: auto; white-space: nowrap; padding: 6px 12px; font-size: 0.85rem; background: var(--border); color: var(--text); border-radius: 20px; }
.input-row { display: flex; gap: 10px; }
.input-row input { margin: 0; }
.input-row button { margin: 0; width: 80px; }
details { margin-bottom: 10px; background: var(--bg); padding: 10px; border-radius: 8px; border: 1px solid var(--border); }
summary { cursor: pointer; font-weight: bold; font-size: 0.9rem; }
.badge { background: #12b76a; color: white; padding: 4px 8px; border-radius: 12px; font-size: 0.8rem; }
.settings-panel { position: absolute; top:0; left:0; right:0; bottom:0; background: var(--surface); padding: 30px; display: none; overflow-y: auto; }
.settings-panel.active { display: block; }
.setting-group { margin-bottom: 25px; padding-bottom: 20px; border-bottom: 1px solid var(--border); }
.modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: var(--overlay); display: flex; align-items: center; justify-content: center; z-index: 9999; }
.modal-overlay.hidden { display: none; }
.modal-box { background: var(--surface); padding: 25px; border-radius: 12px; width: 90%; max-width: 350px; text-align: center; box-shadow: 0 10px 25px rgba(0,0,0,0.2); }
.modal-box h3 { margin-top: 0; }
.modal-actions { display: flex; gap: 10px; margin-top: 20px; justify-content: center; }
@media (max-width: 768px) { .sidebar { width: 80px; } .channel span.name { display: none; } .add-channel { padding: 5px; } }
</style>
</head>
<body>
<header>
  <div style="font-weight:bold; font-size:1.2rem;">OGEMTN <span id="nodeBadge" class="badge">1 node</span></div>
</header>
<div class="app-container">
  <div class="sidebar">
    <div class="channel-list" id="channelList"></div>
    <div class="add-channel">
      <input type="text" id="newSecret" placeholder="New Secret Group">
      <button onclick="addGroup()">Add Channel</button>
    </div>
  </div>
  <div class="chat-area">
    <div class="messages" id="messageFeed"></div>
    <div class="composer" id="composerBox">
      <details>
        <summary>Quick Chat (Disaster Alerts)</summary>
        <div class="quick-chat">
          <button class="quick-btn" onclick="sendQuick('🆘 I am trapped inside! Need rescue!')">🆘 Trapped</button>
          <button class="quick-btn" onclick="sendQuick('🌊 Flood water is rising quickly here!')">🌊 Flood Rising</button>
          <button class="quick-btn" onclick="sendQuick('🏊 Deep water/Drowning! Need a boat!')">🏊 Need Boat</button>
          <button class="quick-btn" onclick="sendQuick('🚑 Severe injury! I need urgent medical help!')">🚑 Need Medic</button>
          <button class="quick-btn" onclick="sendQuick('🔥 Fire nearby, path is blocked!')">🔥 Fire Blocked</button>
          <button class="quick-btn" onclick="sendQuick('🏃 I am abandoning this location now.')">🏃 Moving out</button>
          <button class="quick-btn" onclick="sendQuick('🔋 My battery is dying. Last message.')">🔋 Low Battery</button>
          <button class="quick-btn" onclick="sendQuick('✅ I am safe and have supplies.')">✅ I am safe</button>
        </div>
      </details>
      <form id="msgForm" class="input-row">
        <input type="text" id="msgText" placeholder="Type message..." required autocomplete="off" maxlength="127">
        <button type="submit">Send</button>
      </form>
    </div>
    
    <div class="settings-panel" id="settingsPanel">
      <h2>Settings</h2>
      
      <div class="setting-group">
        <label><b>Node MAC Address</b></label>
        <div id="macDisplay" style="margin-top:5px; font-family:monospace; font-size:1.1rem; opacity:0.8;">Waiting for connection...</div>
      </div>

      <div class="setting-group">
        <label><b>Display Name</b></label>
        <input type="text" id="userName" placeholder="e.g. Engr. Smith" onchange="saveSettings()">
        <small style="display:block; margin-top:5px; opacity:0.7;">This name will be attached to your messages.</small>
      </div>
      <div class="setting-group">
        <label><b>Appearance</b></label>
        <div style="margin-top:10px;">
          <button class="quick-btn" onclick="toggleTheme()">Toggle Dark/Light Mode</button>
        </div>
      </div>
      <div class="setting-group">
        <label><b>Manage Data</b></label>
        <div style="display:flex; gap:10px; margin-top:10px;">
          <button class="danger" onclick="clearChannelHistory(activeChannel)">Clear Current Channel</button>
          <button class="danger" onclick="clearAllHistory()">Clear ALL History</button>
        </div>
      </div>
    </div>
  </div>
</div>

<div id="modalOverlay" class="modal-overlay hidden">
  <div class="modal-box">
    <h3 id="modalTitle">Title</h3>
    <p id="modalText">Message</p>
    <div class="modal-actions">
      <button id="modalBtnCancel" class="secondary hidden">Cancel</button>
      <button id="modalBtnOk">OK</button>
    </div>
  </div>
</div>

<script>
const HISTORY_KEY = 'ogemtn.history';
const GROUPS_KEY = 'ogemtn.groups';
const SETTINGS_KEY = 'ogemtn.settings';
const clientId = Math.random().toString(36).substr(2, 9); 

let activeChannel = 'public';
let cursor = 0;
let isPolling = false;

let groups = JSON.parse(localStorage.getItem(GROUPS_KEY) || '[]');
let history = JSON.parse(localStorage.getItem(HISTORY_KEY) || '[]');
let settings = JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{"theme":"light", "username":""}');

if(settings.theme === 'dark') document.body.classList.add('dark');
document.getElementById('userName').value = settings.username || '';

if(Notification.permission === 'default') Notification.requestPermission();

function showModal(title, text, isConfirm, callback) {
  document.getElementById('modalTitle').innerText = title;
  document.getElementById('modalText').innerText = text;
  document.getElementById('modalOverlay').classList.remove('hidden');
  
  const btnOk = document.getElementById('modalBtnOk');
  const btnCancel = document.getElementById('modalBtnCancel');
  
  btnOk.onclick = () => {
    document.getElementById('modalOverlay').classList.add('hidden');
    if(callback) callback(true);
  };
  
  if(isConfirm) {
    btnCancel.classList.remove('hidden');
    btnCancel.onclick = () => {
      document.getElementById('modalOverlay').classList.add('hidden');
      if(callback) callback(false);
    };
  } else {
    btnCancel.classList.add('hidden');
  }
}

function toggleTheme() {
  document.body.classList.toggle('dark');
  settings.theme = document.body.classList.contains('dark') ? 'dark' : 'light';
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
}

function saveSettings() {
  settings.username = document.getElementById('userName').value.trim();
  localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
}

function renderChannels() {
  const list = document.getElementById('channelList');
  let html = `<div class="channel ${activeChannel==='public'?'active':''}" onclick="setChannel('public')"><span class="name">Public Broadcast</span> 🌐</div>`;
  groups.forEach(g => {
    const isActive = activeChannel === g;
    html += `<div class="channel ${isActive?'active':''}" onclick="setChannel('${g}')">
      <span class="name">🔒 ${g}</span>
      <button class="channel-delete" onclick="event.stopPropagation(); removeGroup('${g}')">×</button>
    </div>`;
  });
  html += `<div class="channel ${activeChannel==='settings'?'active':''}" onclick="setChannel('settings')" style="margin-top:20px; border-top:1px solid var(--border); border-radius:0;"><span class="name">⚙️ Settings</span></div>`;
  list.innerHTML = html;
}

function setChannel(ch) {
  activeChannel = ch;
  renderChannels();
  
  if (ch === 'settings') {
    document.getElementById('messageFeed').style.display = 'none';
    document.getElementById('composerBox').style.display = 'none';
    document.getElementById('settingsPanel').classList.add('active');
  } else {
    document.getElementById('settingsPanel').classList.remove('active');
    document.getElementById('messageFeed').style.display = 'flex';
    document.getElementById('composerBox').style.display = 'block';
    renderMessages();
  }
}

function addGroup() {
  const val = document.getElementById('newSecret').value.trim();
  if(val && !groups.includes(val)) {
    groups.push(val);
    localStorage.setItem(GROUPS_KEY, JSON.stringify(groups));
    document.getElementById('newSecret').value = '';
    setChannel(val);
  }
}

function removeGroup(ch) {
  showModal('Delete Channel', `Are you sure you want to delete the channel "${ch}"?`, true, (confirmed) => {
    if(confirmed) {
      groups = groups.filter(g => g !== ch);
      localStorage.setItem(GROUPS_KEY, JSON.stringify(groups));
      if(activeChannel === ch) setChannel('public');
      else renderChannels();
    }
  });
}

function clearChannelHistory(ch) {
  if (ch === 'settings') return;
  showModal('Clear History', `Clear all saved messages for ${ch}?`, true, (confirmed) => {
    if(confirmed) {
      history = history.filter(m => m.channel !== ch);
      localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
      showModal('Success', 'Channel history cleared.', false);
      renderMessages();
    }
  });
}

function clearAllHistory() {
  showModal('Clear All', 'Delete ALL messages across all channels on this device?', true, (confirmed) => {
    if(confirmed) {
      history = [];
      localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
      showModal('Success', 'All history cleared.', false);
    }
  });
}

function renderMessages() {
  if(activeChannel === 'settings') return;
  const feed = document.getElementById('messageFeed');
  const filtered = history.filter(m => m.channel === activeChannel);
  feed.innerHTML = filtered.map(m => `
    <div class="msg ${m.isSelf ? 'self' : ''}">
      <div style="font-weight:bold; font-size:0.8rem; margin-bottom:4px;">${m.senderName}</div>
      <div>${m.text}</div>
      <div class="msg-time">${m.time}</div>
    </div>
  `).join('');
  feed.scrollTop = feed.scrollHeight;
}

function pushMessage(channel, text, senderId, senderName, isSelf) {
  history.push({ 
    id: Date.now()+Math.random(), 
    channel, 
    text, 
    senderId,
    senderName, 
    isSelf, 
    time: new Date().toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'}) 
  });
  if(history.length > 300) history = history.slice(-300);
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
  
  if(!isSelf && Notification.permission === 'granted') {
    new Notification("OGEMTN Alert", { body: `${senderName}: ${text}` });
  }
  if(channel === activeChannel) renderMessages();
}

async function sendQuick(txt) {
  document.getElementById('msgText').value = txt;
  document.getElementById('msgForm').dispatchEvent(new Event('submit'));
}

document.getElementById('msgForm').onsubmit = async (e) => {
  e.preventDefault();
  const input = document.getElementById('msgText');
  let text = input.value.trim();
  if(!text) return;
  
  const myName = settings.username || 'Anonymous';
  const payloadText = settings.username ? `${settings.username}: ${text}` : text;
  
  if (payloadText.length > 127) {
    showModal("Limit Reached", "Message too long with username attached.", false);
    return;
  }

  const type = activeChannel === 'public' ? 'public' : 'private';
  const body = new URLSearchParams({ 
    type, 
    message: payloadText,
    clientId: clientId 
  });
  if(type === 'private') body.set('secret', activeChannel);
  
  try {
    const res = await fetch('/send', { method: 'POST', body });
    if(res.ok) {
      input.value = '';
      pushMessage(activeChannel, text, clientId, myName + " (Me)", true);
    } else {
      showModal('Send Failed', await res.text(), false);
    }
  } catch(err) { 
    showModal('Network Error', 'Could not reach the mesh node.', false); 
  }
};

async function poll() {
  if(isPolling) return;
  isPolling = true;
  try {
    const res = await fetch('/events?cursor=' + cursor);
    const data = await res.json();
    cursor = Math.max(cursor, data.cursor || 0);
    
    if(data.mac) {
      document.getElementById('macDisplay').innerText = data.mac;
    }

    if(data.nodes !== undefined) {
      const isAlone = data.nodes <= 1;
      document.getElementById('nodeBadge').innerHTML = `<span class="dot ${isAlone ? 'offline' : ''}"></span> ${data.nodes} node(s)`;
    }
    
    if(data.events) {
      data.events.forEach(ev => {
        if(ev.sender !== clientId) {
          const ch = ev.type === 'public' ? 'public' : ev.group;
          
          let displayName = ev.sender; 
          let msgBody = ev.text;
          const colonIdx = ev.text.indexOf(': ');
          if (colonIdx > 0 && colonIdx < 30) {
            displayName = ev.text.substring(0, colonIdx);
            msgBody = ev.text.substring(colonIdx + 2);
          }

          pushMessage(ch, msgBody, ev.sender, displayName, false);
        }
      });
    }
  } catch(e) {}
  isPolling = false;
}

renderChannels();
setInterval(poll, 1000);
</script>
</body>
</html>
)html";

void safeDelay(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    yield();
  }
}

void fatalError(const char* msg) {
  Serial.println(msg);
  safeDelay(3000); 
  ESP.restart(); 
}

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

bool checkRateLimit(uint32_t ip) {
  uint32_t now = millis();
  for (int i = 0; i < 8; i++) {
    if (clientLimits[i].ip == ip) {
      if (now - clientLimits[i].lastPost < 1500) return true;
      clientLimits[i].lastPost = now;
      return false;
    }
  }
  for (int i = 0; i < 8; i++) {
    if (clientLimits[i].ip == 0 || now - clientLimits[i].lastPost > 10000) {
      clientLimits[i].ip = ip;
      clientLimits[i].lastPost = now;
      return false;
    }
  }
  return false;
}

void clearLiveEvents() {
  for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {
    liveEvents[i].valid = false;
    liveEvents[i].id = 0;
    liveEvents[i].text[0] = '\0';
  }
  liveIndex = 0;
}

void publishLive(const String &text, const String &type, const String &group, const String &senderId) {
  if (WiFi.softAPgetStationNum() == 0) return;
  LiveEvent &event = liveEvents[liveIndex];
  event.id = ++liveEventId;
  text.toCharArray(event.text, sizeof(event.text));
  type.toCharArray(event.type, sizeof(event.type));
  group.toCharArray(event.group, sizeof(event.group));
  senderId.toCharArray(event.sender, sizeof(event.sender));
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
  uint32_t count = 1;
  uint32_t now = millis();
  for (size_t i = 0; i < MAX_NODES; i++) {
    if (meshNodes[i].lastSeen != 0) {
      if (now - meshNodes[i].lastSeen > 90000) {
        meshNodes[i].lastSeen = 0;
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

  updateNodePresence(packet.origin);

  String origin = macString(packet.origin);
  if (packet.type == PUBLIC_MESSAGE) {
    char message[128] = {};
    memcpy(message, packet.payload, packet.length);
    message[packet.length] = '\0';
    publishLive(String(message), "public", "", origin);
  } else if (packet.type == PRIVATE_MESSAGE) {
    uint8_t plaintext[128] = {};
    if (decryptPrivate(packet, plaintext)) {
      plaintext[packet.length] = '\0';
      publishLive(String((char *)plaintext), "private", activeChannelId ? "unlocked" : "", origin);
    }
  }

  if (packet.hops > 1) {
    packet.hops--;
    broadcastPacket(packet);
  }
}

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

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void sendCallback(const uint8_t *macAddr, esp_now_send_status_t status) {
#else
void sendCallback(const uint8_t *macAddr, uint8_t status) {
#endif
  if (status == 0) {
    
  } else {
    Serial.printf("ESP-NOW TX Error: %d\n", status);
  }
}

void handleHome() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleEvents() {
  uint32_t cursor = (uint32_t)server.arg("cursor").toInt();
  uint32_t first = liveEventId > LIVE_EVENT_COUNT ? liveEventId - LIVE_EVENT_COUNT + 1 : 1;
  if (cursor + 1 > first) first = cursor + 1;

  uint32_t nodeCount = getActiveNodesCount();
  String json = "{\"cursor\":" + String(liveEventId) + ",\"mac\":\"" + macString(nodeMac) + "\",\"nodes\":" + String(nodeCount) + ",\"events\":[";

  bool firstItem = true;
  for (uint32_t id = first; id <= liveEventId; id++) {
    for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {
      if (!liveEvents[i].valid || liveEvents[i].id != id) continue;
      if (!firstItem) json += ",";
      json += "{\"id\":" + String(liveEvents[i].id) + ",\"text\":\"" + jsonEscape(liveEvents[i].text) + "\",\"type\":\"" + jsonEscape(liveEvents[i].type) + "\",\"group\":\"" + jsonEscape(liveEvents[i].group) + "\",\"sender\":\"" + jsonEscape(liveEvents[i].sender) + "\"}";
      firstItem = false;
      break;
    }
  }
  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleSend() {
  uint32_t clientIP = (uint32_t)server.client().remoteIP();
  if (checkRateLimit(clientIP)) {
    server.send(429, "text/plain", "Rate limit: 1 message per 1.5 seconds.");
    return;
  }

  String message = server.arg("message");
  String type = server.arg("type");
  String secret = server.arg("secret");
  String clientId = server.arg("clientId"); 
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
    unlockKey(secret); 
  }

  publishLive(message, type, secret, clientId);

  rememberPacket(packet);
  if (!broadcastPacket(packet)) {
    server.send(500, "text/plain", "ESP-NOW broadcast request failed.");
    return;
  }

  lastHeartbeatTx = millis();
  server.send(200, "text/plain", "Message sent.");
}

void setup() {
  Serial.begin(115200);
  safeDelay(200); 
  
  memset(seenCache, 0, sizeof(seenCache));
  memset(meshNodes, 0, sizeof(meshNodes));
  memset(clientLimits, 0, sizeof(clientLimits));
  clearLiveEvents();
  txSequence = esp_random();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  
  if (esp_wifi_get_mac(WIFI_IF_STA, nodeMac) != ESP_OK) {
    fatalError("Could not read ESP-NOW MAC. Restarting...");
  }

  String ssid = "ESP_NOW_" + macString(nodeMac);
  
  if (!WiFi.softAP(ssid.c_str(), nullptr, WIFI_CHANNEL, false, 8)) {
    fatalError("Access point startup failed. Restarting...");
  }
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    fatalError("ESP-NOW initialization failed. Restarting...");
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, sizeof(BROADCAST_MAC));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(MeshPacket));

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_now_register_recv_cb(receiveCallback);
  esp_now_register_send_cb(sendCallback);
#else
  esp_now_register_recv_cb((esp_now_recv_cb_t)receiveCallback);
  esp_now_register_send_cb((esp_now_send_cb_t)sendCallback);
#endif

  server.on("/", HTTP_GET, handleHome);
  server.on("/events", HTTP_GET, handleEvents);
  server.on("/send", HTTP_POST, handleSend);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found."); });
  server.begin();

  Serial.println("OGEMTN started.");
}

void loop() {
  server.handleClient();

  uint8_t clients = WiFi.softAPgetStationNum();
  if (clients == 0 && previousClientCount > 0) {
    clearLiveEvents();
  }
  previousClientCount = clients;

  MeshPacket packet;
  while (xQueueReceive(rxQueue, &packet, 0) == pdTRUE) {
    processPacket(packet);
  }

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

  yield();
}