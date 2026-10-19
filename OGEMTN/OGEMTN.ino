#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

#ifndef NETIF_FOREACH
#define NETIF_FOREACH(n) for ((n) = netif_list; (n) != NULL; (n) = (n)->next)
#endif
#include <string.h>

#if defined(CONFIG_FREERTOS_UNICORE) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define OGEMTN_SINGLE_CORE 1
#else
#define OGEMTN_SINGLE_CORE 0
#endif

const uint8_t WIFI_CHANNEL = 1;
const uint8_t MAX_HOPS = 6;
const uint8_t MAX_TEXT = 127;
const size_t MAX_SECRET = 31;
const size_t RX_QUEUE_DEPTH = 12;
const size_t SEEN_CACHE_SIZE = 96;
const size_t LIVE_EVENT_COUNT = 100;
const size_t EVENTS_PER_REPLY = 40;
const size_t MAX_NODES = 64;
const size_t MAX_GROUPS = 12;
const size_t MAX_PENDING_ACKS = 8;

const size_t MAX_GROUP_PEERS = 96;
const size_t MAX_LISTENERS = 24;
const uint32_t PEER_TTL_MS = 90000;
const uint32_t LISTENER_TTL_MS = 30000;
const uint32_t GROUP_PRESENCE_EVERY_MS = 5000;

const bool BATTERY_SENSE_ENABLED = true;

#if defined(CONFIG_IDF_TARGET_ESP32C3)
const int BATTERY_PIN = 3;
#else
const int BATTERY_PIN = 34;
#endif

const float DIVIDER_R_TOP_OHM = 100000.0f;
const float DIVIDER_R_BOTTOM_OHM = 100000.0f;
const float BATTERY_CAL_FACTOR = 1.0f;
const float NO_BATTERY_BELOW_V = 1.0f;
const float STEP_HYSTERESIS_PCT = 3.0f;
const float SOC_TABLE_V[11] = {
  2.50f, 3.00f, 3.20f, 3.22f, 3.25f, 3.26f, 3.27f, 3.30f, 3.32f, 3.35f, 3.65f
};

const uint32_t PACKET_MAGIC = 0x534D4553;
const uint8_t PUBLIC_MESSAGE = 1;
const uint8_t PRIVATE_MESSAGE = 2;
const uint8_t PRESENCE_MESSAGE = 3;
const uint8_t ACK_MESSAGE = 4;
const uint8_t GROUP_PRESENCE_MESSAGE = 5;

const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

WebServer server(80);
DNSServer dnsServer;
QueueHandle_t rxQueue = nullptr;
TaskHandle_t meshTaskHandle = nullptr;
SemaphoreHandle_t stateMutex = nullptr;

uint8_t nodeMac[6] = {};
uint32_t txSequence = 0;
uint32_t bootId = 0;
uint8_t idSalt[16] = {};

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

struct RxItem {
  MeshPacket packet;
  uint8_t src[6];
  int8_t rssi;
  bool hasRssi;
};

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
  char group[32];
  char sender[20];
  uint32_t seq;
  uint32_t ackVer;
  bool mine;
  bool acked;
  bool valid;
};

struct ActiveNode {
  uint8_t mac[6];
  uint32_t lastSeen;
  uint32_t lastDirect;
  int8_t rssi;
  bool hasRssi;
  uint8_t hops;
  uint8_t phones;
};

struct RateLimit {
  uint32_t ip;
  uint32_t lastPost;
};

struct GroupKey {
  uint8_t key[32];
  uint8_t channelId[8];
  char name[32];
  bool valid;
};

struct PendingAck {
  uint8_t target[6];
  uint32_t sequence;
  uint32_t dueAt;
  uint8_t hops;
  bool valid;
};

struct GroupPeer {
  uint8_t mac[6];
  uint8_t channelId[8];
  uint8_t phones;
  uint32_t lastSeen;
};

struct LocalListener {
  uint8_t channelId[8];
  uint32_t ip;
  uint32_t lastSeen;
  bool valid;
};

SeenEntry seenCache[SEEN_CACHE_SIZE] = {};
LiveEvent liveEvents[LIVE_EVENT_COUNT] = {};
ActiveNode meshNodes[MAX_NODES] = {};
RateLimit clientLimits[8] = {};
GroupKey groupKeys[MAX_GROUPS] = {};
PendingAck pendingAcks[MAX_PENDING_ACKS] = {};
GroupPeer groupPeers[MAX_GROUP_PEERS] = {};
LocalListener listeners[MAX_LISTENERS] = {};

size_t seenIndex = 0;
size_t liveIndex = 0;
uint32_t liveEventId = 0;
uint32_t ackVersion = 0;
uint32_t lastHeartbeatTx = 0;
size_t groupCursor = 0;
uint32_t nextGroupPresenceAt = 0;

volatile uint32_t statRx = 0;
volatile uint32_t statRxBadLen = 0;
volatile uint32_t statRxQueueFull = 0;
volatile uint32_t statProcessed = 0;
volatile uint32_t statTxOk = 0;
volatile uint32_t statTxFail = 0;
volatile int statLastLen = 0;
volatile int statLastRssi = 0;

const char PAGE[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=0,interactive-widget=resizes-content">
<title>OGEMTN</title>
<style>
:root { --bg: #f2f5fa; --surface: #ffffff; --text: #182230; --border: #e2e8f0; --primary: #175cd3; --msg-bg: #f1f5f9; --my-msg: #dbeafe; --danger: #ef4444; --overlay: rgba(0,0,0,0.4); }
body.dark { --bg: #0f172a; --surface: #1e293b; --text: #f8fafc; --border: #334155; --primary: #3b82f6; --msg-bg: #334155; --my-msg: #1e3a8a; --danger: #dc2626; --overlay: rgba(0,0,0,0.7); }
html, body { font-family: system-ui, Arial, sans-serif; margin: 0; padding: 0; background: var(--bg); color: var(--text); height: 100%; width: 100%; position: fixed; overflow: hidden; overscroll-behavior: none; }
header { background: var(--surface); padding: 12px 20px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); z-index: 101; flex-shrink: 0; height: 50px; box-sizing: border-box; }
#menuToggle, #infoToggle { background: none; border: none; font-size: 1.4rem; color: var(--text); cursor: pointer; padding: 0 10px; line-height: 1; width: auto; margin: 0; }
#menuToggle { display: none; }
.app-container { display: flex; height: calc(100% - 50px); overflow: hidden; position: relative; width: 100%; }
.sidebar { width: 280px; min-width: 280px; background: var(--surface); border-right: 1px solid var(--border); display: flex; flex-direction: column; z-index: 200; height: 100%; }
.sidebar-right { position: absolute; right: -300px; top: 0; bottom: 0; width: 300px; background: var(--surface); border-left: 1px solid var(--border); transition: right 0.3s ease; z-index: 250; padding: 15px; overflow-y: auto; box-sizing: border-box; display: flex; flex-direction: column; gap: 15px; }
.sidebar-right.open { right: 0; box-shadow: -2px 0 10px rgba(0,0,0,0.3); }
.channel-list { flex: 1; overflow-y: auto; padding: 10px; }
.channel { padding: 12px; border-radius: 8px; cursor: pointer; margin-bottom: 5px; font-weight: 500; display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.channel:hover { background: var(--bg); }
.channel.active { background: var(--primary); color: white; }
.channel-delete { background: var(--danger); color: white; border: none; cursor: pointer; font-size: 1.2rem; width: 26px; height: 26px; border-radius: 50%; display: flex; align-items: center; justify-content: center; padding: 0; line-height: 0; margin: 0 0 0 auto; transition: opacity 0.2s; }
.channel-delete:hover { opacity: 0.8; }
.add-channel { padding: 15px; border-top: 1px solid var(--border); }
input, button, select { width: 100%; box-sizing: border-box; padding: 10px; margin-top: 5px; border-radius: 6px; border: 1px solid var(--border); background: var(--surface); color: var(--text); font-size: 1rem; outline: none; }
input:focus { border-color: var(--primary); }
button { background: var(--primary); color: white; border: none; cursor: pointer; font-weight: bold; }
button.danger { background: var(--danger); }
button.secondary { background: var(--border); color: var(--text); }
.hidden { display: none !important; }
.chat-area { flex: 1; display: flex; flex-direction: column; background: var(--bg); position: relative; overflow: hidden; min-width: 0; height: 100%; }
.messages { flex: 1; padding: 20px; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; }
.msg { padding: 10px 14px; border-radius: 12px; max-width: 80%; background: var(--msg-bg); align-self: flex-start; word-wrap: break-word; overflow-wrap: anywhere; position: relative; group: relative; }
.msg.self { background: var(--my-msg); align-self: flex-end; }
.msg-header { display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 4px; }
.msg-sender { font-weight: bold; font-size: 0.8rem; }
.msg-reply-btn { background: none; border: none; color: var(--primary); cursor: pointer; font-size: 0.75rem; padding: 0 4px; margin: 0; width: auto; opacity: 0.8; }
.msg-reply-btn:hover { opacity: 1; text-decoration: underline; }
.msg-quote { background: rgba(0,0,0,0.06); border-left: 3px solid var(--primary); padding: 4px 8px; border-radius: 4px; font-size: 0.8rem; margin-bottom: 6px; opacity: 0.9; }
body.dark .msg-quote { background: rgba(255,255,255,0.08); }
.msg-time { font-size: 0.75rem; opacity: 0.7; margin-top: 4px; text-align: right; }
.composer { background: var(--surface); padding: 15px; border-top: 1px solid var(--border); flex-shrink: 0; }
.reply-preview { display: flex; justify-content: space-between; align-items: center; background: var(--msg-bg); border-left: 3px solid var(--primary); padding: 6px 10px; border-radius: 6px; font-size: 0.85rem; margin-bottom: 8px; }
.reply-cancel { background: none; border: none; color: var(--danger); font-size: 1.1rem; cursor: pointer; padding: 0 5px; width: auto; margin: 0; line-height: 1; }
.quick-chat { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; padding: 5px 0; }
.quick-btn { width: auto; white-space: nowrap; padding: 6px 12px; font-size: 0.85rem; background: var(--border); color: var(--text); border-radius: 20px; }
.input-row { display: flex; gap: 10px; }
.input-row input { margin: 0; }
.input-row button { margin: 0; width: 80px; }
details { margin-bottom: 10px; background: var(--bg); padding: 10px; border-radius: 8px; border: 1px solid var(--border); }
summary { cursor: pointer; font-weight: bold; font-size: 0.9rem; padding: 5px 0; }
.badge { background: #12b76a; color: white; padding: 4px 8px; border-radius: 12px; font-size: 0.8rem; }
.dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #ffffff; margin-right: 4px; }
.dot.offline { background: #fca5a5; }
.unread { background: #ef4444; color: white; border-radius: 10px; padding: 1px 7px; font-size: 0.75rem; font-weight: bold; }
.pdot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; margin-right: 6px; background: #9ca3af; flex-shrink: 0; }
.pdot.on { background: #12b76a; box-shadow: 0 0 6px #12b76a; }
.pdot.off { background: #ef4444; }
.pcount { font-size: 0.75rem; opacity: 0.85; margin-left: auto; }
#presenceBar { padding: 6px 14px; font-size: 0.85rem; background: var(--surface); border-bottom: 1px solid var(--border); flex-shrink: 0; }
#presenceBar.warn { background: #fee2e2; color: #991b1b; }
body.dark #presenceBar.warn { background: #450a0a; color: #fecaca; }
.settings-panel { position: absolute; top:0; left:0; right:0; bottom:0; background: var(--surface); padding: 30px; display: none; overflow-y: auto; z-index: 10; }
.settings-panel.active { display: block; }
.setting-group { margin-bottom: 25px; padding-bottom: 20px; border-bottom: 1px solid var(--border); }
.bat-bar { display: flex; gap: 3px; margin-top: 8px; }
.bat-seg { flex: 1; height: 18px; border-radius: 3px; background: var(--border); }
.bat-seg.on { background: #12b76a; }
.bat-seg.on.mid { background: #f79009; }
.bat-seg.on.low { background: #ef4444; }
.node-row { display: flex; justify-content: space-between; align-items: center; gap: 10px; padding: 8px 10px; margin-top: 6px; border: 1px solid var(--border); border-radius: 8px; background: var(--bg); }
.node-name { font-weight: bold; font-family: monospace; }
.node-sig { text-align: right; font-size: 0.85rem; white-space: nowrap; }
.sig-good { color: #12b76a; }
.sig-fair { color: #f79009; }
.sig-weak { color: #ef4444; }
.modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: var(--overlay); display: flex; align-items: center; justify-content: center; z-index: 9999; }
.modal-box { background: var(--surface); padding: 25px; border-radius: 12px; width: 90%; max-width: 350px; text-align: center; box-shadow: 0 10px 25px rgba(0,0,0,0.2); }
.modal-box h3 { margin-top: 0; }
.modal-actions { display: flex; gap: 10px; margin-top: 20px; justify-content: center; }
@media (max-width: 768px) {
  #menuToggle { display: block; }
  .sidebar { position: absolute; left: -280px; top: 0; bottom: 0; transition: left 0.3s ease; box-shadow: 2px 0 10px rgba(0,0,0,0.5); z-index: 999; }
  .sidebar.open { left: 0; }
  .msg { max-width: 92%; }
  .composer { padding: 10px; }
}
</style>
</head>
<body>
<header>
  <div style="display:flex; align-items:center;">
    <button id="menuToggle">☰</button>
    <div style="font-weight:bold; font-size:1.2rem;">OGEMTN <span id="nodeBadge" class="badge">1 node</span></div>
  </div>
  <button id="infoToggle" title="Channel Info & Network Nodes">👥</button>
</header>
<div class="app-container">
  <div class="sidebar">
    <div class="channel-list" id="channelList"></div>
    <div class="add-channel">
      <input type="text" id="newSecret" placeholder="New Secret Group" maxlength="31">
      <button id="addGroupBtn">Add Channel</button>
    </div>
  </div>
  
  <div class="sidebar-right" id="rightDrawer">
    <div style="display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--border); padding-bottom:10px;">
      <h3 style="margin:0;">Channel & Mesh Info</h3>
      <button id="closeDrawerBtn" style="width:auto; padding:2px 8px; background:none; color:var(--text); font-size:1.2rem;">×</button>
    </div>
    <div>
      <label style="font-size:0.85rem; opacity:0.7; font-weight:bold;">CURRENT CHANNEL</label>
      <div id="drawerChannelName" style="font-weight:bold; font-size:1.1rem; margin-top:4px;">Public Broadcast</div>
      <div id="drawerChannelStatus" style="font-size:0.85rem; margin-top:2px;">🌐 Unencrypted Mesh</div>
    </div>
    <div>
      <label style="font-size:0.85rem; opacity:0.7; font-weight:bold;">ACTIVE LISTENERS</label>
      <div id="drawerListeners" style="font-size:1rem; margin-top:4px;">Checking...</div>
    </div>
    <div style="border-top:1px solid var(--border); padding-top:10px;">
      <label style="font-size:0.85rem; opacity:0.7; font-weight:bold;">MESH NODES & PEERS</label>
      <div id="drawerNodeList" style="margin-top:8px; display:flex; flex-direction:column; gap:6px;">
        <small>Listening for nearby nodes...</small>
      </div>
    </div>
  </div>

  <div class="chat-area">
    <div id="presenceBar">Checking who's online...</div>
    <div class="messages" id="messageFeed"></div>
    <div class="composer" id="composerBox">
      <div id="replyBar" class="reply-preview hidden">
        <div>
          <span style="font-weight:bold;" id="replyTargetSender"></span>: 
          <span id="replyTargetText" style="opacity:0.8;"></span>
        </div>
        <button class="reply-cancel" id="cancelReplyBtn">×</button>
      </div>
      <details>
        <summary>Quick Emergency Alerts</summary>
        <div class="quick-chat" id="quickChat">
          <button class="quick-btn" data-q="🆘 SOS: Trapped & Need Rescue!">🆘 Trapped & Rescue</button>
          <button class="quick-btn" data-q="🚑 Medical: Severe Injury / Medic Needed!">🚑 Medic Needed</button>
          <button class="quick-btn" data-q="🌊 Hazard: Rising Water / Flood Danger!">🌊 Rising Flood</button>
          <button class="quick-btn" data-q="🔥 Hazard: Fire Nearby / Path Blocked!">🔥 Fire Blocked</button>
          <button class="quick-btn" data-q="💧 Resource: Requesting Water & Food!">💧 Water/Food Needed</button>
          <button class="quick-btn" data-q="📍 Status: Holding Position / Waiting Here">📍 Holding Position</button>
          <button class="quick-btn" data-q="🏃 Status: Relocating to Higher Ground">🏃 Relocating</button>
          <button class="quick-btn" data-q="🔋 Battery: Critical Power (<5%)">🔋 Critical Battery</button>
          <button class="quick-btn" data-q="✅ Status: Safe & Accounted For">✅ Safe & Accounted</button>
          <button class="quick-btn" data-q="👀 Net: Monitoring Emergency Frequency">👀 Monitoring Net</button>
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
        <label><b>Your Signal</b></label>
        <div id="mySignal" style="margin-top:5px; font-size:1.05rem;">Checking...</div>
        <small style="display:block; margin-top:5px; opacity:0.7;">Signal strength between your phone and this node.</small>
      </div>
      <div class="setting-group">
        <label><b>Battery</b></label>
        <div id="batText" style="margin-top:5px; font-size:1.05rem;">Checking...</div>
        <div id="batBar" class="bat-bar"></div>
        <small id="batVolt" style="display:block; margin-top:5px; opacity:0.7;"></small>
      </div>
      <div class="setting-group">
        <label><b>Nearby Nodes <span id="nodeTotal"></span></b></label>
        <div id="nodeSummary" style="margin-top:5px; opacity:0.8; font-size:0.9rem;">Listening...</div>
        <div id="nodeList"></div>
        <button id="moreNodesBtn" class="secondary hidden">Show more</button>
      </div>
      <div class="setting-group">
        <label><b>Display Name</b></label>
        <input type="text" id="userName" placeholder="e.g. Engr. Smith" maxlength="24">
        <small style="display:block; margin-top:5px; opacity:0.7;">This name will be attached to your messages.</small>
      </div>
      <div class="setting-group">
        <label><b>Block Annoying Users</b></label>
        <div style="display:flex; gap:10px; margin-top:5px;">
          <input type="text" id="blockInput" placeholder="Exact Display Name or ID">
          <button id="blockBtn" style="width:80px;" class="danger">Block</button>
        </div>
        <small style="display:block; margin-top:5px; opacity:0.7;">Messages from these names will be silently ignored on your screen.</small>
        <div id="blockList" style="margin-top:10px; display:flex; flex-wrap:wrap; gap:5px;"></div>
      </div>
      <div class="setting-group">
        <label><b>Appearance</b></label>
        <div style="margin-top:10px;">
          <button class="quick-btn" id="themeBtn">Toggle Dark/Light Mode</button>
          <button class="quick-btn" id="soundBtn" style="margin-left:8px;">Alerts: On</button>
        </div>
      </div>
      <div class="setting-group">
        <label><b>Manage Data</b></label>
        <div style="display:flex; gap:10px; margin-top:10px;">
          <button class="danger" id="clearBtn">Clear ALL Device History</button>
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
const BLOCKED_KEY = 'ogemtn.blocked';

const $ = (id) => document.getElementById(id);

let storageOk = true;
try {
  localStorage.setItem('ogemtn.test', '1');
  localStorage.removeItem('ogemtn.test');
} catch (e) {
  storageOk = false;
}

const store = {
  get(key, fallback) {
    try {
      const raw = localStorage.getItem(key);
      return raw === null ? fallback : JSON.parse(raw);
    } catch (e) {
      return fallback;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem(key, JSON.stringify(value));
    } catch (e) {}
  }
};

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function byteLen(s) {
  try {
    return unescape(encodeURIComponent(s)).length;
  } catch (e) {
    return s.length;
  }
}

let activeChannel = 'public';
let cursor = storageOk ? 0 : -1;
let lastBoot = null;
let isPolling = false;
let sending = false;
let ackVer = 0;
let firstPoll = true;
let nodeLimit = 20;
let nodesBusy = false;
let activeReply = null;

const deliveredKeys = new Set();
const unread = {};
const presence = {};

let groups = store.get(GROUPS_KEY, []);
let chatLog = store.get(HISTORY_KEY, []);
let settings = store.get(SETTINGS_KEY, { theme: 'light', username: '' });
let blocked = store.get(BLOCKED_KEY, []);

if (!Array.isArray(groups)) groups = [];
if (!Array.isArray(chatLog)) chatLog = [];
if (!Array.isArray(blocked)) blocked = [];
if (!settings || typeof settings !== 'object') settings = { theme: 'light', username: '' };

if (settings.theme === 'dark') document.body.classList.add('dark');
$('userName').value = settings.username || '';

const hasNotif = typeof Notification !== 'undefined';
try {
  if (hasNotif && Notification.permission === 'default') Notification.requestPermission();
} catch (e) {}

let audioCtx = null;

function unlockAudio() {
  try {
    if (!audioCtx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (AC) audioCtx = new AC();
    }
    if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
  } catch (e) {}
}

['touchstart', 'click', 'keydown'].forEach((evt) => document.addEventListener(evt, unlockAudio, { passive: true }));

function beep() {
  try {
    if (!audioCtx) return;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = 'sine';
    osc.frequency.value = 880;
    gain.gain.value = 0.15;
    osc.connect(gain);
    gain.connect(audioCtx.destination);
    osc.start();
    osc.stop(audioCtx.currentTime + 0.18);
  } catch (e) {}
}

function badge(ch) {
  return unread[ch] ? `<span class="unread">${unread[ch]}</span>` : '';
}

function updateTitle() {
  const n = Object.values(unread).reduce((a, b) => a + b, 0);
  document.title = n > 0 ? `(${n}) OGEMTN` : 'OGEMTN';
}

function notify(channel, name, text, silent) {
  const viewing = !document.hidden && channel === activeChannel;
  if (!viewing) {
    unread[channel] = (unread[channel] || 0) + 1;
    renderChannels();
    updateTitle();
  }
  if (silent || settings.sound === false) return;
  try {
    if (navigator.vibrate) navigator.vibrate([200, 100, 200]);
  } catch (e) {}
  beep();
  if (!viewing && hasNotif) {
    try {
      if (Notification.permission === 'granted') {
        new Notification('OGEMTN Alert', { body: `${name}: ${text}` });
      }
    } catch (e) {}
  }
}

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) {
    unread[activeChannel] = 0;
    renderChannels();
    updateTitle();
  }
});

function renderSoundBtn() {
  $('soundBtn').innerText = settings.sound === false ? 'Alerts: Off' : 'Alerts: On';
}

$('soundBtn').onclick = () => {
  settings.sound = settings.sound === false;
  store.set(SETTINGS_KEY, settings);
  renderSoundBtn();
  unlockAudio();
  if (settings.sound !== false) beep();
};

function timedFetch(url, opts, ms) {
  if (typeof AbortController === 'undefined') return fetch(url, opts);
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), ms);
  return fetch(url, Object.assign({ signal: ctl.signal }, opts || {})).then(
    (r) => { clearTimeout(timer); return r; },
    (e) => { clearTimeout(timer); throw e; }
  );
}

function showModal(title, text, isConfirm, callback) {
  $('modalTitle').innerText = title;
  $('modalText').innerText = text;
  $('modalOverlay').classList.remove('hidden');
  const btnOk = $('modalBtnOk');
  const btnCancel = $('modalBtnCancel');
  btnOk.onclick = () => {
    $('modalOverlay').classList.add('hidden');
    if (callback) callback(true);
  };
  if (isConfirm) {
    btnCancel.classList.remove('hidden');
    btnCancel.onclick = () => {
      $('modalOverlay').classList.add('hidden');
      if (callback) callback(false);
    };
  } else {
    btnCancel.classList.add('hidden');
  }
}

$('menuToggle').onclick = (e) => {
  e.stopPropagation();
  document.querySelector('.sidebar').classList.toggle('open');
};

$('infoToggle').onclick = (e) => {
  e.stopPropagation();
  $('rightDrawer').classList.toggle('open');
  if ($('rightDrawer').classList.contains('open')) updateDrawer();
};

$('closeDrawerBtn').onclick = () => {
  $('rightDrawer').classList.remove('open');
};

$('messageFeed').onclick = () => {
  document.querySelector('.sidebar').classList.remove('open');
  $('rightDrawer').classList.remove('open');
};

$('themeBtn').onclick = () => {
  document.body.classList.toggle('dark');
  settings.theme = document.body.classList.contains('dark') ? 'dark' : 'light';
  store.set(SETTINGS_KEY, settings);
};

$('userName').onchange = () => {
  settings.username = $('userName').value.replace(/:/g, '').trim();
  store.set(SETTINGS_KEY, settings);
};

function renderBlocked() {
  $('blockList').innerHTML = blocked.map((b) =>
    `<span class="badge" style="background:var(--danger)">${esc(b)} <span style="cursor:pointer;margin-left:4px" data-unblock="${esc(b)}">×</span></span>`
  ).join('');
}

$('blockList').onclick = (e) => {
  const el = e.target.closest('[data-unblock]');
  if (!el) return;
  const val = el.getAttribute('data-unblock');
  blocked = blocked.filter((b) => b !== val);
  store.set(BLOCKED_KEY, blocked);
  renderBlocked();
};

$('blockBtn').onclick = () => {
  const val = $('blockInput').value.trim();
  if (val && !blocked.includes(val)) {
    blocked.push(val);
    store.set(BLOCKED_KEY, blocked);
    $('blockInput').value = '';
    renderBlocked();
  }
};

function presLabel(ch) {
  const n = presence[ch];
  if (n === undefined) return { cls: '', txt: '' };
  return { cls: n > 0 ? 'on' : 'off', txt: n > 0 ? String(n) : '0' };
}

function updatePresenceBar() {
  const bar = $('presenceBar');
  if (activeChannel === 'settings') { bar.classList.add('hidden'); return; }
  bar.classList.remove('hidden');
  const n = presence[activeChannel];
  bar.classList.toggle('warn', n === 0);
  if (n === undefined) bar.innerHTML = '<span class="pdot"></span>Checking who\'s online...';
  else if (n === 0) bar.innerHTML = '<span class="pdot off"></span>Nobody else online here. Your message may not be heard yet.';
  else bar.innerHTML = `<span class="pdot on"></span>${n} other${n > 1 ? 's' : ''} listening on this channel`;
  updateDrawer();
}

function setPresence(ch, total) {
  const v = Math.max(0, total - 1);
  const changed = presence[ch] !== v;
  presence[ch] = v;
  if (changed) renderChannels();
  updatePresenceBar();
}

function updateDrawer() {
  if (!$('rightDrawer').classList.contains('open')) return;
  if (activeChannel === 'settings') {
    $('drawerChannelName').innerText = 'Settings Panel';
    $('drawerChannelStatus').innerText = '⚙️ System Configuration';
    $('drawerListeners').innerText = 'N/A';
  } else {
    $('drawerChannelName').innerText = activeChannel === 'public' ? 'Public Broadcast' : '🔒 ' + activeChannel;
    $('drawerChannelStatus').innerText = activeChannel === 'public' ? '🌐 Unencrypted Mesh' : '🔐 AES-256 Encrypted';
    const n = presence[activeChannel];
    $('drawerListeners').innerText = n === undefined ? 'Checking...' : `${n + 1} device(s) total (${n} remote)`;
  }
}

function renderChannels() {
  const pp = presLabel('public');
  let html = `<div class="channel ${activeChannel === 'public' ? 'active' : ''}" data-ch="public"><span class="name"><span class="pdot ${pp.cls}"></span>Public Broadcast</span><span class="pcount">${pp.txt}</span> ${badge('public')} 🌐</div>`;
  groups.forEach((g) => {
    const p = presLabel(g);
    html += `<div class="channel ${activeChannel === g ? 'active' : ''}" data-ch="${esc(g)}">
      <span class="name"><span class="pdot ${p.cls}"></span>🔒 ${esc(g)}</span><span class="pcount">${p.txt}</span>${badge(g)}
      <button class="channel-delete" data-del="${esc(g)}">×</button>
    </div>`;
  });
  html += `<div class="channel ${activeChannel === 'settings' ? 'active' : ''}" data-ch="settings" style="margin-top:20px; border-top:1px solid var(--border); border-radius:0;"><span class="name">⚙️ Settings</span></div>`;
  $('channelList').innerHTML = html;
}

$('channelList').onclick = (e) => {
  const del = e.target.closest('[data-del]');
  if (del) {
    e.stopPropagation();
    removeGroup(del.getAttribute('data-del'));
    return;
  }
  const ch = e.target.closest('[data-ch]');
  if (ch) setChannel(ch.getAttribute('data-ch'));
};

function setChannel(ch) {
  activeChannel = ch;
  unread[ch] = 0;
  clearReply();
  updateTitle();
  renderChannels();
  document.querySelector('.sidebar').classList.remove('open');
  if (ch === 'settings') {
    $('messageFeed').style.display = 'none';
    $('composerBox').style.display = 'none';
    $('settingsPanel').classList.add('active');
    nodeLimit = 20;
    pollNodes();
  } else {
    $('settingsPanel').classList.remove('active');
    $('messageFeed').style.display = 'flex';
    $('composerBox').style.display = 'block';
    renderMessages();
  }
  updatePresenceBar();
}

async function joinGroup(secret) {
  try {
    const res = await timedFetch('/join', { method: 'POST', body: new URLSearchParams({ secret }) }, 4000);
    if (!res.ok) return null;
    let d = {};
    try { d = await res.json(); } catch (e) {}
    if (typeof d.n === 'number') setPresence(secret, d.n);
    return d;
  } catch (e) {
    return null;
  }
}

async function syncGroups() {
  for (const g of groups) await joinGroup(g);
}

$('addGroupBtn').onclick = async () => {
  const val = $('newSecret').value.trim();
  if (!val) return;
  if (byteLen(val) > 31) {
    showModal('Too Long', 'Group secret must be 31 bytes or less.', false);
    return;
  }
  if (val === 'public' || val === 'settings') {
    showModal('Reserved', 'Please choose a different secret.', false);
    return;
  }
  if (groups.includes(val)) return;
  const ok = await joinGroup(val);
  if (!ok) {
    showModal('Cannot Join', 'The node could not register this group. It may be full or unreachable.', false);
    return;
  }
  groups.push(val);
  store.set(GROUPS_KEY, groups);
  $('newSecret').value = '';
  setChannel(val);
};

function removeGroup(ch) {
  showModal('Purge Channel', `Are you sure you want to purge and leave the secret channel "${ch}"? Saved messages for this channel will be removed from your device.`, true, (confirmed) => {
    if (confirmed) {
      groups = groups.filter((g) => g !== ch);
      store.set(GROUPS_KEY, groups);
      delete presence[ch];
      chatLog = chatLog.filter((m) => m.channel !== ch);
      store.set(HISTORY_KEY, chatLog);
      if (activeChannel === ch) setChannel('public');
      else renderChannels();
    }
  });
}

$('clearBtn').onclick = () => {
  showModal('Clear All', 'Delete ALL messages across all channels on this device?', true, (confirmed) => {
    if (confirmed) {
      chatLog = [];
      store.set(HISTORY_KEY, chatLog);
      renderMessages();
      showModal('Success', 'All history cleared.', false);
    }
  });
};

function setReply(sender, text) {
  activeReply = { sender, text: text.length > 20 ? text.substring(0,20) + '...' : text };
  $('replyTargetSender').innerText = esc(sender);
  $('replyTargetText').innerText = esc(activeReply.text);
  $('replyBar').classList.remove('hidden');
  $('msgText').focus();
}

function clearReply() {
  activeReply = null;
  $('replyBar').classList.add('hidden');
}

$('cancelReplyBtn').onclick = clearReply;

function renderMessages() {
  if (activeChannel === 'settings') return;
  const feed = $('messageFeed');
  const filtered = chatLog.filter((m) => m.channel === activeChannel);
  feed.innerHTML = filtered.map((m) => {
    let quoteHtml = '';
    let msgText = m.text;
    if (msgText.startsWith('↳ [') && msgText.includes(']: ')) {
      const idx = msgText.indexOf(']: ');
      const qSender = msgText.substring(3, idx);
      const rest = msgText.substring(idx + 3);
      const spaceIdx = rest.indexOf(' ');
      if (spaceIdx > 0) {
        const qText = rest.substring(0, spaceIdx);
        msgText = rest.substring(spaceIdx + 1);
        quoteHtml = `<div class="msg-quote"><b>${esc(qSender)}</b>: ${esc(qText)}</div>`;
      }
    }
    return `
      <div class="msg ${m.isSelf ? 'self' : ''}">
        <div class="msg-header">
          <span class="msg-sender">${esc(m.senderName)}</span>
          <button class="msg-reply-btn" onclick="setReply('${esc(m.senderName)}', '${esc(msgText)}')">↩ reply</button>
        </div>
        ${quoteHtml}
        <div>${esc(msgText)}</div>
        <div class="msg-time">${esc(m.time)}${m.isSelf ? (m.delivered ? ' · ✓✓ Delivered' : ' · ✓ Sent') : ''}</div>
      </div>
    `;
  }).join('');
  feed.scrollTop = feed.scrollHeight;
}

function pushMessage(channel, text, senderId, senderName, isSelf, eventKey, silent) {
  if (eventKey && chatLog.some((m) => m.eventId === eventKey)) return;

  chatLog.push({
    id: Date.now() + Math.random(),
    eventId: eventKey || null,
    channel,
    text,
    senderId,
    senderName,
    isSelf,
    delivered: !!(eventKey && deliveredKeys.has(eventKey)),
    time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  });
  if (chatLog.length > 300) chatLog = chatLog.slice(-300);
  store.set(HISTORY_KEY, chatLog);

  if (!isSelf) notify(channel, senderName, text, !!silent);
  if (channel === activeChannel) renderMessages();
}

async function sendText(raw, fromInput) {
  if (sending) return;
  let text = raw.trim();
  if (!text) return;

  if (activeReply) {
    text = `↳ [${activeReply.sender}]: ${activeReply.text} ${text}`;
  }

  const myName = settings.username || 'Anon';
  const payloadText = `${myName}: ${text}`;

  if (byteLen(payloadText) > 127) {
    showModal('Limit Reached', 'Message too long with quote attachment.', false);
    return;
  }

  const type = activeChannel === 'public' ? 'public' : 'private';
  const body = new URLSearchParams({ type, message: payloadText });
  if (type === 'private') body.set('secret', activeChannel);

  sending = true;
  try {
    const res = await timedFetch('/send', { method: 'POST', body }, 6000);
    if (res.ok) {
      let info = {};
      try { info = await res.json(); } catch (e) {}
      if (fromInput) $('msgText').value = '';
      clearReply();
      const key = info.id ? `${info.boot}:${info.id}` : null;
      pushMessage(activeChannel, text, 'me', myName + ' (Me)', true, key);
    } else {
      showModal('Send Failed', await res.text(), false);
    }
  } catch (err) {
    showModal('Network Error', 'Could not reach the mesh node.', false);
  }
  sending = false;
}

$('quickChat').onclick = (e) => {
  const btn = e.target.closest('[data-q]');
  if (btn) sendText(btn.getAttribute('data-q'), false);
};

$('msgForm').onsubmit = (e) => {
  e.preventDefault();
  sendText($('msgText').value, true);
};

function renderBattery(b) {
  let on = 0;
  let cls = '';
  let text = 'Checking...';
  let volt = '';
  if (!b || b.s === 'off') {
    text = 'Battery sensing is disabled in firmware.';
  } else if (b.s === 'none') {
    text = 'No battery detected on the sense pin.';
  } else if (b.s === 'pin') {
    text = 'Invalid ADC pin. Use an ADC1 pin.';
  } else if (b.s === 'ok') {
    on = Math.round(b.pct / 10);
    cls = b.pct <= 20 ? 'low' : (b.pct <= 40 ? 'mid' : '');
    text = `${b.pct}%${b.pct <= 10 ? ' · Low battery' : ''}`;
    volt = `Cell voltage: ${Number(b.v).toFixed(2)} V`;
  }
  $('batText').innerText = text;
  $('batVolt').innerText = volt;
  $('batBar').innerHTML = Array.from({ length: 10 }, (_, i) =>
    `<div class="bat-seg ${i < on ? 'on ' + cls : ''}"></div>`
  ).join('');
}

function sigInfo(dbm) {
  if (dbm >= -55) return { label: 'Excellent', cls: 'sig-good', bars: '▂▄▆█' };
  if (dbm >= -67) return { label: 'Good', cls: 'sig-good', bars: '▂▄▆' };
  if (dbm >= -75) return { label: 'Fair', cls: 'sig-fair', bars: '▂▄' };
  return { label: 'Weak', cls: 'sig-weak', bars: '▂' };
}

function sigHtml(dbm) {
  const s = sigInfo(dbm);
  return `<span class="${s.cls}">${s.bars}</span> ${dbm} dBm · ${s.label}`;
}

async function pollNodes() {
  if (nodesBusy) return;
  nodesBusy = true;
  try {
    const res = await timedFetch('/nodes?limit=' + nodeLimit, {}, 4000);
    const d = await res.json();
    $('mySignal').innerHTML = (d.myRssi === null || d.myRssi === undefined) ? 'Unavailable' : sigHtml(d.myRssi);
    renderBattery(d.bat);
    $('nodeTotal').innerText = `(${d.total}${d.full ? '+' : ''})`;
    let summary = `${d.direct} direct · ${d.total - d.direct} via relay`;
    if (d.shown < d.total) summary += ` · showing ${d.shown} of ${d.total}`;
    $('nodeSummary').innerText = d.total === 0 ? 'No other nodes heard yet.' : summary;
    
    const nodeRowsHtml = d.nodes.map((n) => {
      const where = n.h <= 1 ? 'Direct' : `${n.h} hops`;
      const sig = n.r === null ? `<small>${n.h <= 1 ? 'signal n/a' : 'via relay'}</small>` : sigHtml(n.r);
      return `<div class="node-row"><div><div class="node-name">${esc(n.n)}</div><small>${where} · ${Number(n.a)}s ago</small></div><div class="node-sig">${sig}</div></div>`;
    }).join('');
    
    $('nodeList').innerHTML = nodeRowsHtml;
    $('drawerNodeList').innerHTML = d.nodes.length ? nodeRowsHtml : '<small>No other nodes in radio range.</small>';
    $('moreNodesBtn').classList.toggle('hidden', d.shown >= d.total);
  } catch (e) {}
  nodesBusy = false;
}

$('moreNodesBtn').onclick = () => {
  nodeLimit = Math.min(nodeLimit + 20, 64);
  pollNodes();
};

async function poll() {
  if (isPolling) return;
  isPolling = true;
  try {
    const res = await timedFetch('/events?cursor=' + cursor + '&av=' + ackVer, {}, 4000);
    const data = await res.json();

    if (data.mac) $('macDisplay').innerText = data.mac;

    if (data.nodes !== undefined) {
      const isAlone = data.nodes <= 1;
      $('nodeBadge').innerHTML = `<span class="dot ${isAlone ? 'offline' : ''}"></span>${Number(data.nodes)} node(s)`;
    }

    if (data.pub !== undefined) setPresence('public', data.pub);

    const bootChanged = lastBoot !== null && data.boot !== lastBoot;
    if (lastBoot === null || bootChanged) {
      lastBoot = data.boot;
      syncGroups();
    }
    cursor = bootChanged ? 0 : data.cursor;
    if (bootChanged) {
      ackVer = 0;
      deliveredKeys.clear();
    }

    if (data.events) {
      data.events.forEach((ev) => {
        const ch = ev.type === 'public' ? 'public' : ev.group;
        if (ch !== 'public' && !groups.includes(ch)) return;

        const isMine = ev.sender === data.me;
        let displayName = ev.sender;
        let msgBody = ev.text;
        const colonIdx = ev.text.indexOf(': ');
        if (colonIdx > 0 && colonIdx < 30) {
          displayName = ev.text.substring(0, colonIdx);
          msgBody = ev.text.substring(colonIdx + 2);
        }

        if (!isMine && (blocked.includes(displayName) || blocked.includes(ev.sender))) return;

        const key = `${data.boot}:${ev.id}`;
        if (isMine) pushMessage(ch, msgBody, ev.sender, displayName + ' (Me)', true, key);
        else pushMessage(ch, msgBody, ev.sender, displayName, false, key, firstPoll);
      });
    }

    if (data.acks && data.acks.length) {
      let changed = false;
      data.acks.forEach((id) => {
        const key = `${data.boot}:${id}`;
        deliveredKeys.add(key);
        const m = chatLog.find((x) => x.eventId === key);
        if (m && !m.delivered) {
          m.delivered = true;
          changed = true;
        }
      });
      if (changed) {
        store.set(HISTORY_KEY, chatLog);
        renderMessages();
      }
    }
    ackVer = bootChanged ? 0 : (data.av || 0);
    firstPoll = false;
  } catch (e) {}
  isPolling = false;
}

renderSoundBtn();
renderChannels();
renderBlocked();
renderMessages();
updatePresenceBar();
poll();
setInterval(poll, 1000);
setInterval(pollNodes, 3000);
setInterval(() => { groups.forEach((g) => joinGroup(g)); }, 8000);
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
    liveEvents[i].seq = 0;
    liveEvents[i].ackVer = 0;
    liveEvents[i].mine = false;
    liveEvents[i].acked = false;
  }
  liveIndex = 0;
  liveEventId = 0;
  ackVersion = 0;
}

uint32_t publishLive(const String &text, const String &type, const String &group, const String &senderId) {
  if (WiFi.softAPgetStationNum() == 0) return 0;
  uint32_t id = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    LiveEvent &event = liveEvents[liveIndex];
    id = ++liveEventId;
    event.id = id;
    text.toCharArray(event.text, sizeof(event.text));
    type.toCharArray(event.type, sizeof(event.type));
    group.toCharArray(event.group, sizeof(event.group));
    senderId.toCharArray(event.sender, sizeof(event.sender));
    event.seq = 0;
    event.ackVer = 0;
    event.mine = false;
    event.acked = false;
    event.valid = true;
    liveIndex = (liveIndex + 1) % LIVE_EVENT_COUNT;
    xSemaphoreGive(stateMutex);
  }
  return id;
}

void markEventMine(uint32_t eventId, uint32_t sequence) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    LiveEvent &event = liveEvents[(eventId - 1) % LIVE_EVENT_COUNT];
    if (event.valid && event.id == eventId) {
      event.mine = true;
      event.seq = sequence;
    }
    xSemaphoreGive(stateMutex);
  }
}

void applyAck(uint32_t sequence) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {
      LiveEvent &event = liveEvents[i];
      if (event.valid && event.mine && !event.acked && event.seq == sequence) {
        event.acked = true;
        event.ackVer = ++ackVersion;
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

uint32_t nextSequence() {
  uint32_t value = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    value = ++txSequence;
    xSemaphoreGive(stateMutex);
  }
  return value;
}

void updateNodePresence(const MeshPacket &packet, const RxItem &rx) {
  const uint8_t *mac = packet.origin;
  if (memcmp(mac, nodeMac, 6) == 0) return;
  uint32_t now = millis();
  bool direct = memcmp(rx.src, mac, 6) == 0;
  uint8_t hopsAway = 1;

  if (!direct) {
    if (packet.type == ACK_MESSAGE) hopsAway = 2;
    else if (packet.hops <= MAX_HOPS) hopsAway = (uint8_t)(MAX_HOPS - packet.hops + 1);
    if (hopsAway < 2) hopsAway = 2;
  }

  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    int slot = -1;
    int emptySlot = -1;
    int oldest = -1;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < MAX_NODES; i++) {
      if (meshNodes[i].lastSeen == 0) {
        if (emptySlot == -1) emptySlot = (int)i;
        continue;
      }
      if (memcmp(meshNodes[i].mac, mac, 6) == 0) {
        slot = (int)i;
        break;
      }
      uint32_t age = now - meshNodes[i].lastSeen;
      if (oldest == -1 || age > oldestAge) {
        oldest = (int)i;
        oldestAge = age;
      }
    }

    if (slot == -1) {
      slot = emptySlot != -1 ? emptySlot : oldest;
      if (slot != -1) {
        ActiveNode &fresh = meshNodes[slot];
        memset(&fresh, 0, sizeof(fresh));
        memcpy(fresh.mac, mac, 6);
        fresh.hops = hopsAway;
      }
    }

    if (slot != -1) {
      ActiveNode &node = meshNodes[slot];
      node.lastSeen = now;
      if (direct) {
        node.hops = 1;
        node.lastDirect = now;
        if (rx.hasRssi) {
          node.rssi = node.hasRssi ? (int8_t)(((int)node.rssi * 3 + rx.rssi) / 4) : rx.rssi;
          node.hasRssi = true;
        }
      } else if (node.lastDirect == 0 || now - node.lastDirect > 60000) {
        node.hops = hopsAway;
        node.hasRssi = false;
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

uint32_t getActiveNodesCount() {
  uint32_t count = 1;
  uint32_t now = millis();
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_NODES; i++) {
      if (meshNodes[i].lastSeen != 0) {
        if (now - meshNodes[i].lastSeen > 90000) {
          meshNodes[i].lastSeen = 0;
        } else {
          count++;
        }
      }
    }
    xSemaphoreGive(stateMutex);
  }
  return count;
}

bool sha256Bytes(const uint8_t *input, size_t length, uint8_t out[32]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  return mbedtls_sha256(input, length, out, 0) == 0;
#else
  return mbedtls_sha256_ret(input, length, out, 0) == 0;
#endif
}

bool sha256Raw(const String &input, uint8_t out[32]) {
  return sha256Bytes((const uint8_t *)input.c_str(), input.length(), out);
}

String hashId(const uint8_t *mac) {
  uint8_t buf[22];
  uint8_t digest[32];
  memcpy(buf, idSalt, sizeof(idSalt));
  memcpy(buf + sizeof(idSalt), mac, 6);
  if (!sha256Bytes(buf, sizeof(buf), digest)) return String("unknown");
  char out[9];
  snprintf(out, sizeof(out), "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
  return String(out);
}

bool lookupClientMac(uint32_t ip, uint8_t *mac) {
  ip4_addr_t target;
  target.addr = ip;
  struct netif *iface = NULL;
  NETIF_FOREACH(iface) {
    struct eth_addr *found = NULL;
    const ip4_addr_t *foundIp = NULL;
    if (etharp_find_addr(iface, &target, &found, &foundIp) >= 0 && found != NULL) {
      memcpy(mac, found->addr, 6);
      return true;
    }
  }
  return false;
}

String clientIdentity() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  uint8_t id[6] = {};
  if (!lookupClientMac(ip, id)) {
    memcpy(id, &ip, 4);
    id[4] = 0xA5;
    id[5] = 0x5A;
  }
  return hashId(id);
}

bool deriveKeys(const String &secret, uint8_t aesKey[32], uint8_t channelId[8]) {
  if (secret.length() == 0) return false;
  uint8_t digest[32];
  String keyMaterial = "OGEMTN-AES-v1:" + secret;
  if (!sha256Raw(keyMaterial, aesKey)) return false;
  String channelMaterial = "OGEMTN-Channel-v1:" + secret;
  if (!sha256Raw(channelMaterial, digest)) {
    memset(aesKey, 0, 32);
    return false;
  }
  memcpy(channelId, digest, 8);
  memset(digest, 0, sizeof(digest));
  return true;
}

bool registerGroup(const String &secret) {
  if (secret.length() == 0 || secret.length() > MAX_SECRET) return false;
  uint8_t key[32];
  uint8_t channelId[8];
  if (!deriveKeys(secret, key, channelId)) return false;
  bool ok = false;

  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    int emptySlot = -1;
    bool exists = false;
    for (size_t i = 0; i < MAX_GROUPS; i++) {
      if (groupKeys[i].valid && memcmp(groupKeys[i].channelId, channelId, 8) == 0) {
        exists = true;
        break;
      }
      if (!groupKeys[i].valid && emptySlot == -1) emptySlot = (int)i;
    }
    if (exists) {
      ok = true;
    } else if (emptySlot != -1) {
      GroupKey &slot = groupKeys[emptySlot];
      memcpy(slot.key, key, 32);
      memcpy(slot.channelId, channelId, 8);
      memset(slot.name, 0, sizeof(slot.name));
      secret.toCharArray(slot.name, sizeof(slot.name));
      slot.valid = true;
      ok = true;
    }
    xSemaphoreGive(stateMutex);
  }

  memset(key, 0, sizeof(key));
  memset(channelId, 0, sizeof(channelId));
  return ok;
}

void setNodePhones(const uint8_t *mac, uint8_t phones) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_NODES; i++) {
      if (meshNodes[i].lastSeen != 0 && memcmp(meshNodes[i].mac, mac, 6) == 0) {
        meshNodes[i].phones = phones;
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

bool touchListener(const uint8_t *cid, uint32_t ip) {
  uint32_t now = millis();
  bool isNew = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    int slot = -1, freeSlot = -1, oldest = 0;
    for (size_t i = 0; i < MAX_LISTENERS; i++) {
      if (listeners[i].valid && now - listeners[i].lastSeen > LISTENER_TTL_MS) listeners[i].valid = false;
      if (listeners[i].valid && listeners[i].ip == ip && memcmp(listeners[i].channelId, cid, 8) == 0) slot = (int)i;
      else if (!listeners[i].valid && freeSlot == -1) freeSlot = (int)i;
      if (now - listeners[i].lastSeen > now - listeners[oldest].lastSeen) oldest = (int)i;
    }
    if (slot == -1) {
      isNew = true;
      slot = freeSlot != -1 ? freeSlot : oldest;
    }
    LocalListener &l = listeners[slot];
    memcpy(l.channelId, cid, 8);
    l.ip = ip;
    l.lastSeen = now;
    l.valid = true;
    xSemaphoreGive(stateMutex);
  }
  return isNew;
}

uint8_t localPhones(const uint8_t *cid) {
  uint32_t now = millis();
  uint8_t n = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_LISTENERS; i++) {
      if (listeners[i].valid && now - listeners[i].lastSeen <= LISTENER_TTL_MS &&
          memcmp(listeners[i].channelId, cid, 8) == 0 && n < 255) n++;
    }
    xSemaphoreGive(stateMutex);
  }
  return n;
}

void updateGroupPeer(const uint8_t *origin, const uint8_t *cid, uint8_t phones) {
  uint32_t now = millis();
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    int slot = -1, freeSlot = -1, oldest = 0;
    for (size_t i = 0; i < MAX_GROUP_PEERS; i++) {
      GroupPeer &p = groupPeers[i];
      if (p.lastSeen != 0 && now - p.lastSeen > PEER_TTL_MS) p.lastSeen = 0;
      if (p.lastSeen != 0 && memcmp(p.mac, origin, 6) == 0 && memcmp(p.channelId, cid, 8) == 0) slot = (int)i;
      else if (p.lastSeen == 0 && freeSlot == -1) freeSlot = (int)i;
      if (now - p.lastSeen > now - groupPeers[oldest].lastSeen) oldest = (int)i;
    }
    if (slot == -1) slot = freeSlot != -1 ? freeSlot : oldest;
    GroupPeer &p = groupPeers[slot];
    memcpy(p.mac, origin, 6);
    memcpy(p.channelId, cid, 8);
    p.phones = phones;
    p.lastSeen = now;
    xSemaphoreGive(stateMutex);
  }
}

uint32_t remoteGroupPhones(const uint8_t *cid) {
  uint32_t now = millis(), total = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_GROUP_PEERS; i++) {
      if (groupPeers[i].lastSeen != 0 && now - groupPeers[i].lastSeen <= PEER_TTL_MS &&
          memcmp(groupPeers[i].channelId, cid, 8) == 0) total += groupPeers[i].phones;
    }
    xSemaphoreGive(stateMutex);
  }
  return total;
}

uint32_t publicListeners() {
  uint32_t now = millis();
  uint32_t total = (uint32_t)WiFi.softAPgetStationNum();
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_NODES; i++) {
      if (meshNodes[i].lastSeen != 0 && now - meshNodes[i].lastSeen <= PEER_TTL_MS) total += meshNodes[i].phones;
    }
    xSemaphoreGive(stateMutex);
  }
  return total;
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

bool decryptPrivate(const MeshPacket &packet, uint8_t *plaintext, char *groupName, size_t groupNameSize) {
  uint8_t key[32];
  bool found = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_GROUPS; i++) {
      if (groupKeys[i].valid && memcmp(groupKeys[i].channelId, packet.channelId, 8) == 0) {
        memcpy(key, groupKeys[i].key, 32);
        strncpy(groupName, groupKeys[i].name, groupNameSize - 1);
        groupName[groupNameSize - 1] = '\0';
        found = true;
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
  if (!found) return false;
  uint8_t aad[25];
  size_t aadLength = makeAAD(packet, aad);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0) {
    result = mbedtls_gcm_auth_decrypt(&ctx, packet.length,
      packet.nonce, sizeof(packet.nonce), aad, aadLength,
      packet.tag, sizeof(packet.tag), packet.payload, plaintext);
  }
  mbedtls_gcm_free(&ctx);
  memset(key, 0, sizeof(key));
  return result == 0;
}

bool seenBeforeAndRemember(const MeshPacket &packet) {
  uint32_t now = millis();
  bool duplicate = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < SEEN_CACHE_SIZE; i++) {
      if (seenCache[i].valid && (uint32_t)(now - seenCache[i].seenAt) > 120000) {
        seenCache[i].valid = false;
      }
      if (seenCache[i].valid && seenCache[i].sequence == packet.sequence &&
          memcmp(seenCache[i].origin, packet.origin, 6) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      SeenEntry &entry = seenCache[seenIndex];
      memcpy(entry.origin, packet.origin, 6);
      entry.sequence = packet.sequence;
      entry.seenAt = now;
      entry.valid = true;
      seenIndex = (seenIndex + 1) % SEEN_CACHE_SIZE;
    }
    xSemaphoreGive(stateMutex);
  }
  return duplicate;
}

bool broadcastPacket(const MeshPacket &packet) {
  return esp_now_send(BROADCAST_MAC, (const uint8_t *)&packet, sizeof(packet)) == ESP_OK;
}

void sendGroupPresence() {
  char name[32] = {};
  uint8_t cid[8] = {};
  bool have = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t k = 0; k < MAX_GROUPS; k++) {
      size_t idx = (groupCursor + k) % MAX_GROUPS;
      if (groupKeys[idx].valid) {
        memcpy(name, groupKeys[idx].name, sizeof(name));
        memcpy(cid, groupKeys[idx].channelId, 8);
        groupCursor = idx + 1;
        have = true;
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
  if (!have) return;
  uint8_t phones = localPhones(cid);
  if (phones == 0) return;
  MeshPacket p = {};
  p.magic = PACKET_MAGIC;
  p.version = 1;
  p.type = GROUP_PRESENCE_MESSAGE;
  p.hops = MAX_HOPS;
  p.length = 1;
  memcpy(p.origin, nodeMac, 6);
  p.sequence = nextSequence();
  p.payload[0] = phones;
  if (!encryptPrivate(p, String(name))) return;
  seenBeforeAndRemember(p);
  broadcastPacket(p);
}

void scheduleAck(const uint8_t *target, uint32_t sequence, uint8_t hopsLeft) {
  if (memcmp(target, nodeMac, 6) == 0) return;
  int slot = -1;
  for (size_t i = 0; i < MAX_PENDING_ACKS; i++) {
    if (!pendingAcks[i].valid) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) return;
  uint8_t used = MAX_HOPS > hopsLeft ? (uint8_t)(MAX_HOPS - hopsLeft) : 0;
  uint8_t ackHops = used + 2;
  if (ackHops > MAX_HOPS) ackHops = MAX_HOPS;
  PendingAck &pending = pendingAcks[slot];
  memcpy(pending.target, target, 6);
  pending.sequence = sequence;
  pending.hops = ackHops;
  pending.dueAt = millis() + 60 + (esp_random() % 400);
  pending.valid = true;
}

void cancelPendingAck(const uint8_t *target, uint32_t sequence) {
  for (size_t i = 0; i < MAX_PENDING_ACKS; i++) {
    if (pendingAcks[i].valid && pendingAcks[i].sequence == sequence &&
        memcmp(pendingAcks[i].target, target, 6) == 0) {
      pendingAcks[i].valid = false;
    }
  }
}

void processPacket(MeshPacket &packet, const RxItem &rx) {
  if (packet.magic != PACKET_MAGIC || packet.version != 1) return;
  if (packet.type != PUBLIC_MESSAGE && packet.type != PRIVATE_MESSAGE && packet.type != PRESENCE_MESSAGE &&
      packet.type != ACK_MESSAGE && packet.type != GROUP_PRESENCE_MESSAGE) return;
  if (packet.type != PRESENCE_MESSAGE && packet.length == 0) return;
  if (packet.length > MAX_TEXT || packet.hops == 0) return;
  if (seenBeforeAndRemember(packet)) return;
  statProcessed++;
  updateNodePresence(packet, rx);

  if (packet.type == PRESENCE_MESSAGE) {
    setNodePhones(packet.origin, packet.length == 1 ? packet.payload[0] : 0);
  }
  if (packet.type == ACK_MESSAGE) {
    if (packet.length != 10) return;
    uint8_t target[6];
    uint32_t originalSequence;
    memcpy(target, packet.payload, 6);
    memcpy(&originalSequence, packet.payload + 6, 4);
    cancelPendingAck(target, originalSequence);
    if (memcmp(target, nodeMac, 6) == 0) {
      applyAck(originalSequence);
      return;
    }
  }
  String origin = hashId(packet.origin);
  if (packet.type == PUBLIC_MESSAGE) {
    char message[128] = {};
    memcpy(message, packet.payload, packet.length);
    message[packet.length] = '\0';
    publishLive(String(message), "public", "", origin);
    scheduleAck(packet.origin, packet.sequence, packet.hops);
  } else if (packet.type == PRIVATE_MESSAGE) {
    uint8_t plaintext[129] = {};
    char groupName[32] = {};
    if (decryptPrivate(packet, plaintext, groupName, sizeof(groupName))) {
      plaintext[packet.length] = '\0';
      publishLive(String((char *)plaintext), "private", String(groupName), origin);
      scheduleAck(packet.origin, packet.sequence, packet.hops);
    }
  } else if (packet.type == GROUP_PRESENCE_MESSAGE) {
    if (packet.length == 1) {
      uint8_t pt[129] = {};
      char gn[32] = {};
      if (decryptPrivate(packet, pt, gn, sizeof(gn))) {
        updateGroupPeer(packet.origin, packet.channelId, pt[0]);
      }
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
  statRx++;
  statLastLen = length;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  if (info != nullptr && info->rx_ctrl != nullptr) statLastRssi = info->rx_ctrl->rssi;
#endif
  if (rxQueue == nullptr) return;
  if (length != (int)sizeof(MeshPacket)) {
    statRxBadLen++;
    return;
  }
  RxItem item;
  memset(&item, 0, sizeof(item));
  memcpy(&item.packet, data, sizeof(MeshPacket));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  if (info != nullptr) {
    memcpy(item.src, info->src_addr, 6);
    if (info->rx_ctrl != nullptr) {
      item.rssi = (int8_t)info->rx_ctrl->rssi;
      item.hasRssi = true;
    }
  }
#else
  memcpy(item.src, macAddr, 6);
#endif
  if (xQueueSend(rxQueue, &item, 0) != pdTRUE) statRxQueueFull++;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void sendCallback(const uint8_t *macAddr, esp_now_send_status_t status) {
#else
void sendCallback(const uint8_t *macAddr, uint8_t status) {
#endif
  if (status == 0) statTxOk++;
  else statTxFail++;
}

void handleHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleEvents() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  long requested = server.hasArg("cursor") ? server.arg("cursor").toInt() : 0;
  uint32_t ackSeen = server.hasArg("av") ? (uint32_t)server.arg("av").toInt() : 0;
  uint32_t nodeCount = getActiveNodesCount();
  uint32_t pubListeners = publicListeners();
  String me = clientIdentity();
  String json;

  json.reserve(6000);
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    uint32_t current = liveEventId;
    uint32_t oldest = current > LIVE_EVENT_COUNT ? current - LIVE_EVENT_COUNT + 1 : 1;
    uint32_t first;

    if (requested < 0) {
      first = current + 1;
    } else {
      first = (uint32_t)requested + 1;
      if (first < oldest) first = oldest;
    }
    uint32_t last = current;
    if (first <= current && last - first + 1 > EVENTS_PER_REPLY) {
      last = first + EVENTS_PER_REPLY - 1;
    }
    uint32_t replyCursor = (requested < 0 || first > current) ? current : last;
    json = "{\"boot\":" + String(bootId) + ",\"me\":\"" + me + "\",\"cursor\":" + String(replyCursor) +  
           ",\"mac\":\"" + macString(nodeMac) + "\",\"nodes\":" + String(nodeCount) +  
           ",\"pub\":" + String(pubListeners) + ",\"events\":[";

    bool firstItem = true;  
    for (uint32_t id = first; id <= last; id++) {  
      LiveEvent &ev = liveEvents[(id - 1) % LIVE_EVENT_COUNT];  
      if (!ev.valid || ev.id != id) continue;  
      if (!firstItem) json += ",";  
      json += "{\"id\":" + String(ev.id) + ",\"text\":\"" + jsonEscape(ev.text) +  
              "\",\"type\":\"" + jsonEscape(ev.type) + "\",\"group\":\"" + jsonEscape(ev.group) +  
              "\",\"sender\":\"" + jsonEscape(ev.sender) + "\"}";  
      firstItem = false;  
    }  
    json += "],\"av\":" + String(ackVersion) + ",\"acks\":[";  
    bool firstAck = true;  
    for (size_t i = 0; i < LIVE_EVENT_COUNT; i++) {  
      LiveEvent &ev = liveEvents[i];  
      if (ev.valid && ev.mine && ev.acked && ev.ackVer > ackSeen) {  
        if (!firstAck) json += ",";  
        json += String(ev.id);  
        firstAck = false;  
      }  
    }  
    xSemaphoreGive(stateMutex);  
    json += "]}";  
  } else {
    json = "{\"boot\":" + String(bootId) + ",\"cursor\":0,\"nodes\":1,\"events\":[]}";
  }
  server.send(200, "application/json; charset=utf-8", json);
}

enum BatteryState { BAT_DISABLED = 0, BAT_OK = 1, BAT_NONE = 2, BAT_BAD_PIN = 3 };
BatteryState batteryState = BAT_DISABLED;
float batteryCellV = 0.0f;
int batteryStep = 0;
bool batteryHaveSample = false;
uint32_t batteryNextAt = 0;

bool adcPinUsable(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  return pin >= 0 && pin <= 4;
#elif defined(CONFIG_IDF_TARGET_ESP32)
  return pin >= 32 && pin <= 39;
#else
  return pin >= 0;
#endif
}

float socPercentFromVolts(float volts) {
  if (volts <= SOC_TABLE_V[0]) return 0.0f;
  if (volts >= SOC_TABLE_V[10]) return 100.0f;
  for (int i = 0; i < 10; i++) {
    if (volts < SOC_TABLE_V[i + 1]) {
      float span = SOC_TABLE_V[i + 1] - SOC_TABLE_V[i];
      if (span <= 0.0f) return (float)(i * 10);
      return (float)(i * 10) + 10.0f * (volts - SOC_TABLE_V[i]) / span;
    }
  }
  return 100.0f;
}

float dividerRatio() {
  return (DIVIDER_R_TOP_OHM + DIVIDER_R_BOTTOM_OHM) / DIVIDER_R_BOTTOM_OHM;
}

float readCellVolts() {
  uint32_t samples[24];
  for (int i = 0; i < 24; i++) {
    samples[i] = analogReadMilliVolts(BATTERY_PIN);
    delayMicroseconds(200);
  }
  for (int i = 1; i < 24; i++) {
    uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  uint32_t sum = 0;
  for (int i = 4; i < 20; i++) sum += samples[i];
  float pinVolts = ((float)sum / 16.0f) / 1000.0f;
  return pinVolts * dividerRatio() * BATTERY_CAL_FACTOR;
}

void batterySetup() {
  if (!BATTERY_SENSE_ENABLED) {
    batteryState = BAT_DISABLED;
    return;
  }
  if (!adcPinUsable(BATTERY_PIN)) {
    batteryState = BAT_BAD_PIN;
    Serial.printf("Battery pin GPIO%d is not an ADC1 pin. Battery sensing disabled.\n", BATTERY_PIN);
    return;
  }
  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  batteryState = BAT_NONE;
  Serial.printf("Battery sense on GPIO%d. Expected ADC range %.3f V (empty) to %.3f V (full).\n",
    BATTERY_PIN, SOC_TABLE_V[0] / dividerRatio(), SOC_TABLE_V[10] / dividerRatio());
}

void updateBattery() {
  if (batteryState == BAT_DISABLED || batteryState == BAT_BAD_PIN) return;
  float volts = readCellVolts();
  if (volts < NO_BATTERY_BELOW_V) {
    batteryState = BAT_NONE;
    batteryHaveSample = false;
    return;
  }
  batteryCellV = batteryHaveSample ? (batteryCellV * 0.8f + volts * 0.2f) : volts;
  float pct = socPercentFromVolts(batteryCellV);
  if (!batteryHaveSample) {
    batteryStep = ((int)(pct / 10.0f)) * 10;
  } else {
    float h = STEP_HYSTERESIS_PCT;
    if (pct >= 99.0f) batteryStep = 100;
    else if (pct >= (float)batteryStep + 10.0f + h) batteryStep = ((int)((pct - h) / 10.0f)) * 10;
    else if (pct < (float)batteryStep - h) batteryStep = ((int)((pct + h) / 10.0f)) * 10;
  }
  if (batteryStep > 100) batteryStep = 100;
  if (batteryStep < 0) batteryStep = 0;
  batteryHaveSample = true;
  batteryState = BAT_OK;
}

String batteryJson() {
  if (batteryState == BAT_OK) {
    return "{\"s\":\"ok\",\"pct\":" + String(batteryStep) + ",\"v\":" + String(batteryCellV, 2) + "}";
  }
  if (batteryState == BAT_NONE) return "{\"s\":\"none\"}";
  if (batteryState == BAT_BAD_PIN) return "{\"s\":\"pin\"}";
  return "{\"s\":\"off\"}";
}

bool clientRssi(int *rssiOut) {
  wifi_sta_list_t list;
  memset(&list, 0, sizeof(list));
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) return false;
  uint32_t ip = (uint32_t)server.client().remoteIP();
  uint8_t mac[6];
  if (lookupClientMac(ip, mac)) {
    for (int i = 0; i < list.num; i++) {
      if (memcmp(list.sta[i].mac, mac, 6) == 0) {
        *rssiOut = list.sta[i].rssi;
        return true;
      }
    }
  }
  if (list.num == 1) {
    *rssiOut = list.sta[0].rssi;
    return true;
  }
  return false;
}

struct NodeRow {
  uint8_t mac[6];
  int8_t rssi;
  bool hasRssi;
  uint8_t hops;
  uint32_t age;
};

bool rowBefore(const NodeRow &a, const NodeRow &b) {
  bool aDirect = a.hops <= 1;
  bool bDirect = b.hops <= 1;
  if (aDirect != bDirect) return aDirect;
  if (aDirect) {
    int ar = a.hasRssi ? a.rssi : -127;
    int br = b.hasRssi ? b.rssi : -127;
    if (ar != br) return ar > br;
  } else if (a.hops != b.hops) {
    return a.hops < b.hops;
  }
  return a.age < b.age;
}

void handleNodes() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  long limit = server.hasArg("limit") ? server.arg("limit").toInt() : 20;
  if (limit < 1) limit = 1;
  if (limit > (long)MAX_NODES) limit = (long)MAX_NODES;
  int myRssi = 0;
  bool haveMyRssi = clientRssi(&myRssi);
  NodeRow rows[MAX_NODES];
  size_t count = 0;
  uint32_t now = millis();

  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_NODES; i++) {
      const ActiveNode &node = meshNodes[i];
      if (node.lastSeen == 0 || now - node.lastSeen > 90000) continue;
      NodeRow &row = rows[count++];
      memset(&row, 0, sizeof(row));
      memcpy(row.mac, node.mac, 6);
      row.rssi = node.rssi;
      row.hasRssi = node.hasRssi;
      row.hops = node.hops;
      row.age = (now - node.lastSeen) / 1000;
    }
    xSemaphoreGive(stateMutex);
  }

  for (size_t i = 1; i < count; i++) {
    NodeRow key = rows[i];
    size_t j = i;
    while (j > 0 && rowBefore(key, rows[j - 1])) {
      rows[j] = rows[j - 1];
      j--;
    }
    rows[j] = key;
  }

  size_t direct = 0;
  for (size_t i = 0; i < count; i++) {
    if (rows[i].hops <= 1) direct++;
  }
  size_t shown = count < (size_t)limit ? count : (size_t)limit;
  String json;

  json.reserve(300 + shown * 80);
  json = "{\"myRssi\":";
  json += haveMyRssi ? String(myRssi) : String("null");
  json += ",\"total\":" + String((uint32_t)count) + ",\"direct\":" + String((uint32_t)direct) +
          ",\"shown\":" + String((uint32_t)shown) + ",\"full\":" + String(count >= MAX_NODES ? "true" : "false") +
          ",\"bat\":" + batteryJson() + ",\"nodes\":[";

  for (size_t i = 0; i < shown; i++) {
    if (i > 0) json += ",";
    json += "{\"n\":\"" + macString(rows[i].mac) + "\",\"r\":";
    json += rows[i].hasRssi ? String((int)rows[i].rssi) : String("null");
    json += ",\"h\":" + String((uint32_t)rows[i].hops) + ",\"a\":" + String((uint32_t)rows[i].age) + "}";
  }
  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleJoin() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String secret = server.hasArg("secret") ? server.arg("secret") : "";
  if (secret.length() == 0 || secret.length() > MAX_SECRET) {
    server.send(400, "text/plain", "Secret must be 1 to 31 bytes.");
    return;
  }
  if (!registerGroup(secret)) {
    server.send(507, "text/plain", "Too many groups on this node.");
    return;
  }
  uint8_t key[32], cid[8];
  uint32_t n = 0;
  if (deriveKeys(secret, key, cid)) {
    bool isNew = touchListener(cid, (uint32_t)server.client().remoteIP());
    if (isNew) nextGroupPresenceAt = 0;
    n = (uint32_t)localPhones(cid) + remoteGroupPhones(cid);
  }
  memset(key, 0, sizeof(key));
  server.send(200, "application/json", "{\"n\":" + String(n) + "}");
}

void handleSend() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  uint32_t clientIP = (uint32_t)server.client().remoteIP();
  if (checkRateLimit(clientIP)) {
    server.send(429, "text/plain", "Rate limit: 1 message per 1.5 seconds.");
    return;
  }

  String message = server.hasArg("message") ? server.arg("message") : "";
  String type = server.hasArg("type") ? server.arg("type") : "";
  String secret = server.hasArg("secret") ? server.arg("secret") : "";
  String sender = clientIdentity();
  message.trim();

  if (message.length() == 0 || message.length() > MAX_TEXT) {
    server.send(400, "text/plain", "Message must contain 1 to 127 bytes.");
    return;
  }
  if (type != "public" && type != "private") {
    server.send(400, "text/plain", "Invalid channel.");
    return;
  }
  if (type == "private" && (secret.length() == 0 || secret.length() > MAX_SECRET)) {
    server.send(400, "text/plain", "Private group secret must be 1 to 31 bytes.");
    return;
  }

  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = 1;
  packet.type = type == "private" ? PRIVATE_MESSAGE : PUBLIC_MESSAGE;
  packet.hops = MAX_HOPS;
  packet.length = message.length();
  memcpy(packet.origin, nodeMac, sizeof(nodeMac));
  packet.sequence = nextSequence();
  memcpy(packet.payload, message.c_str(), packet.length);

  if (packet.type == PRIVATE_MESSAGE) {
    if (!registerGroup(secret)) {
      server.send(507, "text/plain", "Too many groups on this node.");
      return;
    }
    if (!encryptPrivate(packet, secret)) {
      server.send(500, "text/plain", "Encryption failed.");
      return;
    }
  }

  uint32_t eventId = publishLive(message, type, type == "private" ? secret : String(""), sender);
  if (eventId != 0) markEventMine(eventId, packet.sequence);

  seenBeforeAndRemember(packet);
  if (!broadcastPacket(packet)) {
    server.send(500, "text/plain", "ESP-NOW broadcast request failed.");
    return;
  }

  lastHeartbeatTx = millis();
  String reply = "{\"id\":" + String(eventId) + ",\"boot\":" + String(bootId) + "}";
  server.send(200, "application/json", reply);
}

void meshBackgroundLogic() {
  RxItem item;
  while (xQueueReceive(rxQueue, &item, 0) == pdTRUE) {
    processPacket(item.packet, item);
  }
  uint32_t now = millis();

  for (size_t i = 0; i < MAX_PENDING_ACKS; i++) {
    if (pendingAcks[i].valid && (int32_t)(now - pendingAcks[i].dueAt) >= 0) {
      pendingAcks[i].valid = false;
      MeshPacket ack = {};
      ack.magic = PACKET_MAGIC;
      ack.version = 1;
      ack.type = ACK_MESSAGE;
      ack.hops = pendingAcks[i].hops;
      ack.length = 10;
      memcpy(ack.origin, nodeMac, sizeof(nodeMac));
      ack.sequence = nextSequence();
      memcpy(ack.payload, pendingAcks[i].target, 6);
      memcpy(ack.payload + 6, &pendingAcks[i].sequence, 4);
      seenBeforeAndRemember(ack);
      broadcastPacket(ack);
    }
  }

  static uint8_t lastStations = 0;
  uint8_t stations = WiFi.softAPgetStationNum();
  if (stations != lastStations) {
    lastStations = stations;
    lastHeartbeatTx = now - 30001;
  }

  if (now - lastHeartbeatTx > 30000) {
    lastHeartbeatTx = now;
    MeshPacket hb = {};
    hb.magic = PACKET_MAGIC;
    hb.version = 1;
    hb.type = PRESENCE_MESSAGE;
    hb.hops = MAX_HOPS;
    hb.length = 1;
    hb.payload[0] = stations;
    memcpy(hb.origin, nodeMac, sizeof(nodeMac));
    hb.sequence = nextSequence();
    seenBeforeAndRemember(hb);
    broadcastPacket(hb);
  }

  if ((int32_t)(now - nextGroupPresenceAt) >= 0) {
    nextGroupPresenceAt = now + GROUP_PRESENCE_EVERY_MS;
    sendGroupPresence();
  }
}

#if !OGEMTN_SINGLE_CORE
void meshTaskCore0(void *pvParameters) {
  for (;;) {
    meshBackgroundLogic();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

void setup() {
  Serial.begin(115200);
  safeDelay(200);

  stateMutex = xSemaphoreCreateMutex();
  if (stateMutex == nullptr) {
    fatalError("Failed to create FreeRTOS mutex. Restarting...");
  }

  memset(seenCache, 0, sizeof(seenCache));
  memset(meshNodes, 0, sizeof(meshNodes));
  memset(clientLimits, 0, sizeof(clientLimits));
  memset(groupKeys, 0, sizeof(groupKeys));
  memset(groupPeers, 0, sizeof(groupPeers));
  memset(listeners, 0, sizeof(listeners));

  clearLiveEvents();
  txSequence = esp_random();
  bootId = esp_random();
  esp_fill_random(idSalt, sizeof(idSalt));

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  if (esp_wifi_get_mac(WIFI_IF_STA, nodeMac) != ESP_OK) {
    fatalError("Could not read ESP-NOW MAC. Restarting...");
  }

  String ssid = "ESP_NOW_" + macString(nodeMac);
  if (!WiFi.softAP(ssid.c_str(), nullptr, WIFI_CHANNEL, false, 8)) {
    fatalError("Access point startup failed. Restarting...");
  }

  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.println("TX Power set to 19.5dBm on all boards.");

  dnsServer.start(53, "*", apIP);

  if (esp_now_init() != ESP_OK) {
    fatalError("ESP-NOW initialization failed. Restarting...");
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, sizeof(BROADCAST_MAC));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxItem));
  if (rxQueue == nullptr) {
    fatalError("Failed to create RX queue. Restarting...");
  }

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
  server.on("/join", HTTP_POST, handleJoin);
  server.on("/nodes", HTTP_GET, handleNodes);

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(204);
    } else {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    }
  });

  batterySetup();
  server.begin();
  lastHeartbeatTx = millis() - 25000;

#if !OGEMTN_SINGLE_CORE
  Serial.println("Dual-Core Supported: Pinning Heavy Crypto/Mesh processing to Core 0.");
  xTaskCreatePinnedToCore(meshTaskCore0, "MeshTask", 10240, NULL, 1, &meshTaskHandle, 0);
#endif

  Serial.println("OGEMTN Standard WebServer started.");
}

void loop() {
  static uint32_t lastStat = 0;
  if (millis() - lastStat > 5000) {
    lastStat = millis();
    Serial.printf("[STAT] rx=%u badLen=%u qFull=%u processed=%u txOk=%u txFail=%u lastLen=%d rssi=%d ch=%u stations=%d\n",
      (unsigned)statRx, (unsigned)statRxBadLen, (unsigned)statRxQueueFull, (unsigned)statProcessed,
      (unsigned)statTxOk, (unsigned)statTxFail, statLastLen, statLastRssi,
      (unsigned)WiFi.channel(), (int)WiFi.softAPgetStationNum());
  }

  if ((int32_t)(millis() - batteryNextAt) >= 0) {
    batteryNextAt = millis() + 2000;
    updateBattery();
  }

  dnsServer.processNextRequest();
  server.handleClient();

#if OGEMTN_SINGLE_CORE
  meshBackgroundLogic();
#endif

  delay(1);
}