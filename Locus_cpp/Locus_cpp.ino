#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_idf_version.h>
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

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Locus firmware supports classic ESP32 only. Select ESP32 Dev Module or a compatible ESP32-WROOM board."
#endif

#define LOCUS_SINGLE_CORE 0

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
const char *ADMIN_PIN = "58427319";
const char *OTA_USERNAME = "admin";
const size_t ADMIN_SESSION_COUNT = 8;
const uint32_t ADMIN_SESSION_IDLE_MS = 1800000UL;
const char *LOCUS_BUILD_NAME = "Locus Admin Dashboard - 2026-10-24";

const int BATTERY_PIN = 34;

const int EMERGENCY_BUTTON_PIN = 4;
const int BUZZER_PIN = 16;
uint32_t lastHardwareEmergencyAt = 0;
bool hardwareEmergencySent = false;
bool emergencyButtonWasDown = false;
bool buzzerActive = false;
uint32_t buzzerStopTime = 0;

const float DIVIDER_R_TOP_OHM = 100000.0f;
const float DIVIDER_R_BOTTOM_OHM = 200000.0f;
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
const uint8_t EMERGENCY_MESSAGE = 6;
const uint32_t EMERGENCY_COOLDOWN_MS = 30000;

const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

WebServer server(80);
DNSServer dnsServer;
Preferences nodePreferences;
String nodeLocationLabel = "Unconfigured location";
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

struct AdminSession {
  char token[33];
  uint32_t lastActivity;
  bool valid;
};

struct EmergencyRateEntry {
  uint32_t ip;
  uint32_t lastAt;
  bool valid;
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
AdminSession adminSessions[ADMIN_SESSION_COUNT] = {};
EmergencyRateEntry emergencyRateLimits[16] = {};
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
<title>Locus Mesh</title>
<style>
:root { --bg: #f2f5fa; --surface: #ffffff; --text: #182230; --border: #e2e8f0; --primary: #175cd3; --msg-bg: #f1f5f9; --my-msg: #dbeafe; --danger: #ef4444; --overlay: rgba(0,0,0,0.4); }
body.dark { --bg: #0f172a; --surface: #1e293b; --text: #f8fafc; --border: #334155; --primary: #3b82f6; --msg-bg: #334155; --my-msg: #1e3a8a; --danger: #dc2626; --overlay: rgba(0,0,0,0.7); }
html, body { font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 0; padding: 0; background: var(--bg); color: var(--text); height: 100%; width: 100%; position: fixed; overflow: hidden; overscroll-behavior: none; }

::-webkit-scrollbar { width: 6px; height: 6px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }
::-webkit-scrollbar-thumb:hover { background: var(--primary); }
* { scrollbar-width: thin; scrollbar-color: var(--border) transparent; }

header { background: var(--surface); padding: 0 16px; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); z-index: 101; flex-shrink: 0; height: 50px; box-sizing: border-box; }
#menuToggle, #infoToggle { background: none; border: none; font-size: 1.3rem; color: var(--text); cursor: pointer; padding: 6px 10px; border-radius: 6px; line-height: 1; width: auto; margin: 0; transition: background 0.2s; }
#menuToggle:hover, #infoToggle:hover { background: var(--bg); }
#menuToggle { display: none; }

.app-container { display: flex; height: calc(100% - 50px); overflow: hidden; position: relative; width: 100%; }
.sidebar { width: 280px; min-width: 280px; background: var(--surface); border-right: 1px solid var(--border); display: flex; flex-direction: column; z-index: 200; height: 100%; }

.sidebar-right { position: absolute; right: -320px; top: 0; bottom: 0; width: 320px; background: var(--surface); border-left: 1px solid var(--border); transition: right 0.3s cubic-bezier(0.4, 0, 0.2, 1); z-index: 250; padding: 18px; overflow-y: auto; box-sizing: border-box; display: flex; flex-direction: column; gap: 16px; }
.sidebar-right.open { right: 0; box-shadow: -4px 0 20px rgba(0,0,0,0.15); }

.channel-list { flex: 1; overflow-y: auto; padding: 10px; }
.channel { padding: 12px; border-radius: 8px; cursor: pointer; margin-bottom: 5px; font-weight: 500; display: flex; align-items: center; justify-content: space-between; gap: 10px; transition: background 0.15s; }
.channel:hover { background: var(--bg); }
.channel.active { background: var(--primary); color: white; }
.channel-delete { background: var(--danger); color: white; border: none; cursor: pointer; font-size: 1rem; width: 24px; height: 24px; border-radius: 50%; display: flex; align-items: center; justify-content: center; padding: 0; line-height: 0; margin: 0 0 0 auto; transition: opacity 0.2s; }
.channel-delete:hover { opacity: 0.85; }

.add-channel { padding: 15px; border-top: 1px solid var(--border); }
input, button, select { width: 100%; box-sizing: border-box; padding: 10px; margin-top: 5px; border-radius: 6px; border: 1px solid var(--border); background: var(--surface); color: var(--text); font-size: 0.95rem; outline: none; }
input:focus { border-color: var(--primary); }
button { background: var(--primary); color: white; border: none; cursor: pointer; font-weight: bold; transition: opacity 0.2s; }
button:hover { opacity: 0.9; }
button.danger { background: var(--danger); }
button.secondary { background: var(--border); color: var(--text); }
.hidden { display: none !important; }

.chat-area { flex: 1; display: flex; flex-direction: column; background: var(--bg); position: relative; overflow: hidden; min-width: 0; height: 100%; }
.messages { flex: 1; padding: 16px 20px; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; }

.msg { padding: 10px 14px; border-radius: 12px; max-width: 80%; background: var(--msg-bg); align-self: flex-start; word-wrap: break-word; overflow-wrap: anywhere; position: relative; box-shadow: 0 1px 2px rgba(0,0,0,0.05); }
.msg.self { background: var(--my-msg); align-self: flex-end; }
.msg.emergency { border: 2px solid #ef4444; animation: locusEmergencyPulse 1s ease-in-out infinite; }
@keyframes locusEmergencyPulse { 0%,100% { box-shadow: 0 0 0 0 rgba(239,68,68,.30); } 50% { box-shadow: 0 0 0 5px rgba(239,68,68,.08); } }
#sosToggle { width:auto; margin:0 8px; padding:7px 11px; background:#dc2626; color:white; border:0; border-radius:7px; font-weight:800; }
.msg-header { display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 4px; }
.msg-sender { font-weight: 700; font-size: 0.8rem; opacity: 0.9; }
.msg-reply-btn { background: none; border: none; color: var(--primary); cursor: pointer; font-size: 0.75rem; padding: 0; margin: 0; width: auto; opacity: 0.75; font-weight: bold; }
.msg-reply-btn:hover { opacity: 1; text-decoration: underline; }
.msg-quote { background: rgba(0,0,0,0.06); border-left: 3px solid var(--primary); padding: 4px 8px; border-radius: 4px; font-size: 0.8rem; margin-bottom: 6px; opacity: 0.9; }
body.dark .msg-quote { background: rgba(255,255,255,0.08); }
.msg-time { font-size: 0.72rem; opacity: 0.65; margin-top: 4px; text-align: right; }

.composer { background: var(--surface); padding: 12px 15px; border-top: 1px solid var(--border); flex-shrink: 0; }
.reply-preview { display: flex; justify-content: space-between; align-items: center; background: var(--msg-bg); border-left: 3px solid var(--primary); padding: 6px 10px; border-radius: 6px; font-size: 0.85rem; margin-bottom: 8px; }
.reply-cancel { background: none; border: none; color: var(--danger); font-size: 1.1rem; cursor: pointer; padding: 0 4px; width: auto; margin: 0; line-height: 1; }

.quick-chat { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 8px; padding: 4px 0; }
.quick-btn { width: auto; white-space: nowrap; padding: 5px 10px; font-size: 0.8rem; background: var(--border); color: var(--text); border-radius: 16px; font-weight: normal; }
.input-row { display: flex; gap: 8px; }
.input-row input { margin: 0; }
.input-row button { margin: 0; width: 80px; flex-shrink: 0; }

details { margin-bottom: 8px; background: var(--bg); padding: 8px 12px; border-radius: 8px; border: 1px solid var(--border); }
summary { cursor: pointer; font-weight: bold; font-size: 0.85rem; padding: 2px 0; user-select: none; }

.badge { background: #12b76a; color: white; padding: 3px 8px; border-radius: 12px; font-size: 0.75rem; font-weight: 600; }
.dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #ffffff; margin-right: 5px; }
.dot.offline { background: #fca5a5; }
.unread { background: #ef4444; color: white; border-radius: 10px; padding: 1px 6px; font-size: 0.72rem; font-weight: bold; }
.pdot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; background: #9ca3af; flex-shrink: 0; }
.pdot.on { background: #12b76a; box-shadow: 0 0 6px #12b76a; }
.pdot.off { background: #ef4444; }
.pcount { font-size: 0.75rem; opacity: 0.85; margin-left: auto; }

#presenceBar { padding: 6px 14px; font-size: 0.82rem; background: var(--surface); border-bottom: 1px solid var(--border); flex-shrink: 0; }
#presenceBar.warn { background: #fee2e2; color: #991b1b; }
body.dark #presenceBar.warn { background: #450a0a; color: #fecaca; }

.settings-panel { position: absolute; top:0; left:0; right:0; bottom:0; background: var(--surface); padding: 24px; display: none; overflow-y: auto; z-index: 10; }
.settings-panel.active { display: block; }
.setting-group { margin-bottom: 20px; padding-bottom: 16px; border-bottom: 1px solid var(--border); }
.settings-nav-button { display:flex; align-items:center; justify-content:space-between; gap:12px; width:100%; padding:12px 14px; margin-top:8px; text-align:left; background:var(--surface); border:1px solid var(--border); color:var(--text); border-radius:8px; font-weight:700; }
.settings-nav-button:hover { border-color:var(--primary); opacity:1; }

.bat-bar { display: flex; gap: 3px; margin-top: 8px; }
.bat-seg { flex: 1; height: 16px; border-radius: 3px; background: var(--border); }
.bat-seg.on { background: #12b76a; }
.bat-seg.on.mid { background: #f79009; }
.bat-seg.on.low { background: #ef4444; }

.card-box { border: 1px solid var(--border); border-radius: 8px; padding: 10px; background: var(--bg); margin-top: 6px; }
.node-row { display: flex; justify-content: space-between; align-items: center; gap: 10px; padding: 8px; margin-top: 6px; border: 1px solid var(--border); border-radius: 6px; background: var(--surface); }
.node-name { font-weight: bold; font-family: monospace; font-size: 0.88rem; }
.node-sig { text-align: right; font-size: 0.82rem; white-space: nowrap; }
.sig-good { color: #12b76a; font-weight: bold; }
.sig-fair { color: #f79009; font-weight: bold; }
.sig-weak { color: #ef4444; font-weight: bold; }

.modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: var(--overlay); display: flex; align-items: center; justify-content: center; z-index: 9999; }
.modal-box { background: var(--surface); padding: 22px; border-radius: 12px; width: 90%; max-width: 340px; text-align: center; box-shadow: 0 10px 25px rgba(0,0,0,0.2); }
.modal-box h3 { margin-top: 0; font-size: 1.1rem; }
.modal-actions { display: flex; gap: 10px; margin-top: 18px; justify-content: center; }

@media (max-width: 768px) {
  #menuToggle { display: block; }
  .sidebar { position: absolute; left: -280px; top: 0; bottom: 0; transition: left 0.3s ease; box-shadow: 2px 0 10px rgba(0,0,0,0.3); z-index: 999; }
  .sidebar.open { left: 0; }
  .msg { max-width: 90%; }
  .composer { padding: 10px; }
}
</style>
</head>
<body>
<header>
  <div style="display:flex; align-items:center;">
    <button id="menuToggle">☰</button>
    <div style="font-weight:bold; font-size:1.15rem; margin-left:4px;">Locus <span id="nodeBadge" class="badge">1 node</span></div>
  </div>
  <div style="display:flex;align-items:center;gap:6px;">
    <button id="sosToggle" type="button" title="Open emergency quick actions">SOS</button>
    <button id="infoToggle" title="Network & Device Status">📶 Info</button>
  </div>
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
    <div style="display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--border); padding-bottom:8px;">
      <h3 style="margin:0; font-size:1rem;">Network Inspector</h3>
      <button id="closeDrawerBtn" style="width:auto; padding:0 6px; background:none; color:var(--text); font-size:1.2rem;">×</button>
    </div>

    <div>
      <label style="font-size:0.75rem; opacity:0.7; font-weight:bold; letter-spacing:0.5px;">LAYER 3 · PHONE TO NODE LINK</label>
      <div class="card-box">
        <div style="font-size:0.85rem; opacity:0.8;">Local AP Direct Signal:</div>
        <div id="drawerL3Signal" style="font-size:1rem; margin-top:3px;">Measuring...</div>
      </div>
    </div>

    <div>
      <label style="font-size:0.75rem; opacity:0.7; font-weight:bold; letter-spacing:0.5px;">CURRENT CHANNEL</label>
      <div class="card-box">
        <div id="drawerChannelName" style="font-weight:bold; font-size:0.95rem;">Public Broadcast</div>
        <div id="drawerChannelStatus" style="font-size:0.8rem; opacity:0.8; margin-top:2px;">🌐 Unencrypted Mesh</div>
        <div id="drawerListeners" style="font-size:0.8rem; margin-top:6px; font-weight:600;">Checking active devices...</div>
      </div>
    </div>

    <div style="flex:1; display:flex; flex-direction:column; min-height:0;">
      <label style="font-size:0.75rem; opacity:0.7; font-weight:bold; letter-spacing:0.5px;">LAYER 2 · ESP-NOW MESH NODES</label>
      <div id="drawerNodeList" style="margin-top:6px; overflow-y:auto; flex:1; display:flex; flex-direction:column; gap:4px;">
        <small style="opacity:0.7;">Scanning RF environment...</small>
      </div>
    </div>
  </div>

  <div class="chat-area">
    <div id="presenceBar">Checking who's online...</div>
    <div class="messages" id="messageFeed"></div>
    <div class="composer" id="composerBox">
      <div id="replyBar" class="reply-preview hidden">
        <div style="overflow:hidden; text-overflow:ellipsis; white-space:nowrap;">
          <span style="font-weight:bold;" id="replyTargetSender"></span>: 
          <span id="replyTargetText" style="opacity:0.85;"></span>
        </div>
        <button class="reply-cancel" id="cancelReplyBtn">×</button>
      </div>
      <details>
        <summary>Emergency Quick Alerts</summary>
        <div class="quick-chat" id="quickChat">
          <button class="quick-btn" data-sos="MEDICAL: NEED MEDICAL ASSISTANCE">🚑 Medical assistance</button>
          <button class="quick-btn" data-sos="MEDICAL: SEVERE BLEEDING / MAJOR INJURY">🩺 Severe injury</button>
          <button class="quick-btn" data-sos="FIRE/RESCUE: FIRE OR SMOKE REPORTED">🔥 Fire / smoke</button>
          <button class="quick-btn" data-sos="FIRE/RESCUE: TRAPPED / CANNOT EVACUATE">🆘 Trapped / rescue</button>
          <button class="quick-btn" data-sos="PUBLIC SAFETY: IMMEDIATE THREAT / SECURITY HELP NEEDED">🚨 Public safety threat</button>
          <button class="quick-btn" data-sos="PUBLIC SAFETY: MISSING OR VULNERABLE PERSON">🔎 Missing person</button>
          <button class="quick-btn" data-sos="NATURAL/ENVIRONMENTAL: EARTHQUAKE DAMAGE OR AFTERSHOCK DANGER">🌎 Earthquake damage</button>
          <button class="quick-btn" data-sos="NATURAL/ENVIRONMENTAL: FLOODING OR RISING WATER">🌊 Flooding / rising water</button>
          <button class="quick-btn" data-sos="NATURAL/ENVIRONMENTAL: LANDSLIDE OR SEVERE WEATHER DANGER">⛈️ Severe weather / landslide</button>
          <button class="quick-btn" data-sos="SUPPLIES: NEED CLEAN WATER OR FIRST-AID SUPPLIES">💧 Need supplies</button>
          <button class="quick-btn" data-sos="STATUS: HOLDING POSITION">📍 Holding position</button>
          <button class="quick-btn" data-sos="STATUS: MOVING TO HIGH GROUND">🏃 Moving to safety</button>
          <button class="quick-btn" data-sos="STATUS: CLEAR / SAFE HERE">✅ Safe here</button>
        </div>
      </details>
      <form id="msgForm" class="input-row">
        <input type="text" id="msgText" placeholder="Type message..." required autocomplete="off" maxlength="127">
        <button type="submit">Send</button>
      </form>
    </div>

    <div class="settings-panel" id="settingsPanel">
      <h2 style="margin-top:0;">Node Settings</h2>
      <div class="setting-group">
        <label><b>Node MAC Address</b></label>
        <div id="macDisplay" style="margin-top:5px; font-family:monospace; font-size:1rem; opacity:0.8;">Waiting...</div>
      </div>
      <div class="setting-group">
        <label><b>Client Link Signal (Phone to Node)</b></label>
        <div id="mySignal" style="margin-top:5px; font-size:0.95rem;">Checking...</div>
      </div>
      <div class="setting-group">
        <label><b>Node Battery</b></label>
        <div id="batText" style="margin-top:5px; font-size:0.95rem;">Checking...</div>
        <div id="batBar" class="bat-bar"></div>
        <small id="batVolt" style="display:block; margin-top:4px; opacity:0.7;"></small>
      </div>
      <div class="setting-group">
        <label><b>Discovered Mesh Nodes <span id="nodeTotal"></span></b></label>
        <div id="nodeSummary" style="margin-top:4px; opacity:0.8; font-size:0.85rem;">Listening...</div>
        <div id="nodeList" style="margin-top:6px;"></div>
        <button id="moreNodesBtn" class="secondary hidden" style="margin-top:8px;">Show more</button>
      </div>
      <div class="setting-group">
        <label><b>Administrator Console</b></label>
        <p class="muted">Node labels and firmware maintenance are restricted to the password-protected admin panel.</p>
        <button type="button" id="openAdminBtn" class="settings-nav-button"><span>Open Locus Admin Console</span><span aria-hidden="true">→</span></button>
        <p class="muted">Each ESP32 has its own admin console. A central controller for all nodes would need a separate authenticated mesh-management protocol.</p>
      </div>
      <div class="setting-group">
        <label><b>Display Name</b></label>
        <input type="text" id="userName" placeholder="e.g. Rescuer 1" maxlength="24">
      </div>
      <div class="setting-group">
        <label><b>Block Users</b></label>
        <div style="display:flex; gap:8px; margin-top:5px;">
          <input type="text" id="blockInput" placeholder="Name or ID">
          <button id="blockBtn" style="width:75px;" class="danger">Block</button>
        </div>
        <div id="blockList" style="margin-top:8px; display:flex; flex-wrap:wrap; gap:5px;"></div>
      </div>
      <div class="setting-group">
        <label><b>Appearance & Alerts</b></label>
        <div style="margin-top:8px; display:flex; gap:8px;">
          <button class="quick-btn" id="themeBtn">Toggle Dark/Light</button>
          <button class="quick-btn" id="soundBtn">Alerts: On</button>
        </div>
      </div>
      <div class="setting-group">
        <label><b>Storage</b></label>
        <div style="margin-top:8px;">
          <button class="danger" id="clearBtn">Clear Device History</button>
        </div>
      </div>
    </div>
  </div>
</div>

<div id="modalOverlay" class="modal-overlay hidden">
  <div class="modal-box">
    <h3 id="modalTitle">Title</h3>
    <p id="modalText" style="font-size:0.9rem; opacity:0.85;"></p>
    <div class="modal-actions">
      <button id="modalBtnCancel" class="secondary hidden">Cancel</button>
      <button id="modalBtnOk">OK</button>
    </div>
  </div>
</div>

<div id="sosInitialModal" class="modal-overlay hidden">
  <div class="modal-box" style="border-top: 5px solid var(--danger); width: 95%; max-width: 400px;">
    <h3 style="margin-top:0; color:var(--danger);">EMERGENCY CHECK-IN</h3>
    <p style="font-size:0.85rem;">State your situation and location.</p>
    <input type="text" id="sosLocInput" placeholder="Enter location (e.g., Room 302)..." style="margin-bottom: 12px; font-weight:bold;">
    <div style="display:flex; flex-direction:column; gap:8px; max-height:65vh; overflow-y:auto;">
      <h4 style="margin:4px 0 0;text-align:left;">Medical emergencies</h4>
      <button style="background:#dc2626;" onclick="sendSos('MEDICAL: NEED MEDICAL ASSISTANCE')">🚑 Need medical assistance</button>
      <button style="background:#dc2626;" onclick="sendSos('MEDICAL: SEVERE BLEEDING / MAJOR INJURY')">🩺 Severe bleeding / major injury</button>
      <button style="background:#dc2626;" onclick="sendSos('MEDICAL: UNCONSCIOUS PERSON')">🫀 Unconscious person</button>
      <h4 style="margin:8px 0 0;text-align:left;">Fire and rescue</h4>
      <button style="background:#b91c1c;" onclick="sendSos('FIRE/RESCUE: FIRE OR SMOKE REPORTED')">🔥 Fire or smoke</button>
      <button style="background:#b45309;" onclick="sendSos('FIRE/RESCUE: TRAPPED / CANNOT EVACUATE')">🆘 Trapped / cannot evacuate</button>
      <button style="background:#b45309;" onclick="sendSos('FIRE/RESCUE: CHEMICAL SPILL / GAS LEAK')">☣️ Chemical spill / gas leak</button>
      <h4 style="margin:8px 0 0;text-align:left;">Public safety and security</h4>
      <button style="background:#7c3aed;" onclick="sendSos('PUBLIC SAFETY: IMMEDIATE THREAT / SECURITY HELP NEEDED')">🚨 Immediate threat / security help</button>
      <button style="background:#7c3aed;" onclick="sendSos('PUBLIC SAFETY: MISSING OR VULNERABLE PERSON')">🔎 Missing / vulnerable person</button>
      <h4 style="margin:8px 0 0;text-align:left;">Natural and environmental disasters</h4>
      <button style="background:#2563eb;" onclick="sendSos('NATURAL/ENVIRONMENTAL: EARTHQUAKE DAMAGE OR AFTERSHOCK DANGER')">🌎 Earthquake damage / aftershock danger</button>
      <button style="background:#2563eb;" onclick="sendSos('NATURAL/ENVIRONMENTAL: FLOODING OR RISING WATER')">🌊 Flooding / rising water</button>
      <button style="background:#2563eb;" onclick="sendSos('NATURAL/ENVIRONMENTAL: LANDSLIDE OR SEVERE WEATHER DANGER')">⛈️ Landslide / severe weather</button>
      <h4 style="margin:8px 0 0;text-align:left;">Status and other</h4>
      <button style="background:#3b82f6;" onclick="sendSos('STATUS: CLEAR / SAFE HERE')">✅ Clear / safe here</button>
      <label for="sosCustomInput" style="text-align:left;font-weight:700;">Other emergency</label>
      <input type="text" id="sosCustomInput" maxlength="50" placeholder="Describe another urgent problem">
      <button style="background:#991b1b;" onclick="sendSosCustom()">Send Custom SOS</button>
      <button class="secondary" style="margin-top:8px;" onclick="closeSosModal()">Skip / Close</button>
    </div>
  </div>
</div>

<script>
const HISTORY_KEY = 'locus.history';
const GROUPS_KEY = 'locus.groups';
const SETTINGS_KEY = 'locus.settings';
const BLOCKED_KEY = 'locus.blocked';

const $ = (id) => document.getElementById(id);

let storageOk = true;
try {
  localStorage.setItem('locus.test', '1');
  localStorage.removeItem('locus.test');
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

function safeSessionGet(key) {
  try { return sessionStorage.getItem(key); } catch (e) { return null; }
}

function safeSessionSet(key, value) {
  try { sessionStorage.setItem(key, value); } catch (e) {}
}

function esc(s) {
  const map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  return String(s).replace(/[&<>"']/g, (c) => map[c]);
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
let settings = store.get(SETTINGS_KEY, { theme: 'dark', username: '' });
let blocked = store.get(BLOCKED_KEY, []);

if (!Array.isArray(groups)) groups = [];
if (!Array.isArray(chatLog)) chatLog = [];
if (!Array.isArray(blocked)) blocked = [];
if (!settings || typeof settings !== 'object') settings = { theme: 'dark', username: '' };

if (settings.theme !== 'light' && settings.theme !== 'dark') settings.theme = 'dark';
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
  document.title = n > 0 ? `(${n}) Locus` : 'Locus Mesh';
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
        new Notification('Locus Alert', { body: `${name}: ${text}` });
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
  if ($('rightDrawer').classList.contains('open')) {
    updateDrawer();
    pollNodes();
  }
};

$('closeDrawerBtn').onclick = () => {
  $('rightDrawer').classList.remove('open');
};

$('messageFeed').onclick = (e) => {
  const replyButton = e.target.closest('[data-reply-id]');
  if (replyButton) {
    const target = chatLog.find((m) => String(m.id) === replyButton.getAttribute('data-reply-id'));
    if (target) setReply(target.senderName, target.text);
    return;
  }
  document.querySelector('.sidebar').classList.remove('open');
  $('rightDrawer').classList.remove('open');
};

$('themeBtn').onclick = () => {
  document.body.classList.toggle('dark');
  settings.theme = document.body.classList.contains('dark') ? 'dark' : 'light';
  store.set(SETTINGS_KEY, settings);
};

$('openAdminBtn').onclick = () => { window.location.href = '/admin'; };

window.addEventListener('storage', (event) => {
  if (event.key !== SETTINGS_KEY) return;
  settings = store.get(SETTINGS_KEY, settings);
  document.body.classList.toggle('dark', settings.theme === 'dark');
});

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
  if (connectionWarningShown) { bar.classList.remove('hidden'); bar.classList.add('warn'); bar.textContent = 'Connection interrupted. Reconnecting to Locus automatically…'; return; }
  if (activeChannel === 'settings') { bar.classList.add('hidden'); return; }
  bar.classList.remove('hidden');
  const n = presence[activeChannel];
  bar.classList.toggle('warn', n === 0);
  if (n === undefined) bar.innerHTML = '<span class="pdot"></span>Checking channel presence...';
  else if (n === 0) bar.innerHTML = '<span class="pdot off"></span>No other active listeners detected on channel.';
  else bar.innerHTML = `<span class="pdot on"></span>${n} other node listener${n > 1 ? 's' : ''} online`;
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
    $('drawerChannelName').innerText = 'System Settings';
    $('drawerChannelStatus').innerText = '⚙️ Hardware Config';
    $('drawerListeners').innerText = 'N/A';
  } else {
    $('drawerChannelName').innerText = activeChannel === 'public' ? 'Public Broadcast' : '🔒 ' + activeChannel;
    $('drawerChannelStatus').innerText = activeChannel === 'public' ? '🌐 Unencrypted Mesh' : '🔐 AES-256 GCM Encrypted';
    const n = presence[activeChannel];
    $('drawerListeners').innerText = n === undefined ? 'Checking presence...' : `${n + 1} device(s) total (${n} remote listeners)`;
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
    showModal('Reserved', 'Please choose a different secret name.', false);
    return;
  }
  if (groups.includes(val)) return;
  const ok = await joinGroup(val);
  if (!ok) {
    showModal('Cannot Join', 'Node capacity reached or communication failed.', false);
    return;
  }
  groups.push(val);
  store.set(GROUPS_KEY, groups);
  $('newSecret').value = '';
  setChannel(val);
};

function removeGroup(ch) {
  showModal('Purge Channel', `Purge secret channel "${ch}" and clear stored messages on this device?`, true, (confirmed) => {
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
  showModal('Clear History', 'Delete ALL message history saved locally on this phone?', true, (confirmed) => {
    if (confirmed) {
      chatLog = [];
      store.set(HISTORY_KEY, chatLog);
      renderMessages();
    }
  });
};

function setReply(sender, text) {
  const cleanSender = String(sender || 'Unknown').slice(0, 24);
  const cleanText = String(text || '');
  activeReply = { sender: cleanSender, text: cleanText.length > 25 ? cleanText.substring(0, 25) + '...' : cleanText };
  $('replyTargetSender').textContent = activeReply.sender;
  $('replyTargetText').textContent = activeReply.text;
  $('replyBar').classList.remove('hidden');
  $('msgText').focus();
}

function clearReply() {
  activeReply = null;
  $('replyBar').classList.add('hidden');
}

$('cancelReplyBtn').onclick = clearReply;

$('sosToggle').onclick = () => $('sosInitialModal').classList.remove('hidden');


function isEmergencyMessage(text) {
  const critical = /\[SOS\]|\[EMERGENCY BUTTON\]|NEED MEDICAL ASSISTANCE|TRAPPED\s*\/\s*CANNOT EVACUATE|NEED RESCUE|HARDWARE SOS/i.test(String(text));
  const safe = /CLEAR \/ SAFE HERE|SAFE & FULLY ACCOUNTED/i.test(String(text));
  return critical && !safe;
}

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
      const separator = ' ⟡ ';
      const separatorIndex = rest.indexOf(separator);
      if (separatorIndex > 0) {
        const qText = rest.substring(0, separatorIndex);
        msgText = rest.substring(separatorIndex + separator.length);
        quoteHtml = `<div class="msg-quote"><b>${esc(qSender)}</b>: ${esc(qText)}</div>`;
      }
    }
    const emergencyClass = isEmergencyMessage(msgText) ? ' emergency' : '';
    return `
      <div class="msg ${m.isSelf ? 'self' : ''}${emergencyClass}">
        <div class="msg-header">
          <span class="msg-sender">${esc(m.senderName)}</span>
          <button class="msg-reply-btn" data-reply-id="${m.id}">↩ reply</button>
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

async function sendText(raw, fromInput, emergency = false) {
  if (sending) return;
  let text = raw.trim();
  if (!text) return;

  if (activeReply) {
    text = `↳ [${activeReply.sender}]: ${activeReply.text} ⟡ ${text}`;
  }

  const myName = settings.username || 'Anon';
  const payloadText = `${myName}: ${text}`;

  if (byteLen(payloadText) > 127) {
    showModal('Limit Reached', 'Message too long. Try shortening your display name or response.', false);
    return;
  }

  const type = activeChannel === 'public' ? 'public' : 'private';
  const body = new URLSearchParams({ type, message: payloadText });
  if (type === 'private') body.set('secret', activeChannel);
  if (emergency) body.set('emergency', '1');

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
    showModal('Network Error', 'Target mesh node unreachable.', false);
  }
  sending = false;
}

$('quickChat').onclick = (e) => {
  const btn = e.target.closest('[data-sos], [data-q]');
  if (!btn) return;
  const sos = btn.getAttribute('data-sos');
  if (sos) sendSos(sos);
  else sendText(btn.getAttribute('data-q'), false);
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
    text = 'Battery sense disabled in firmware.';
  } else if (b.s === 'none') {
    text = 'No battery detected on sense pin.';
  } else if (b.s === 'pin') {
    text = 'Invalid ADC pin specified.';
  } else if (b.s === 'ok') {
    on = Math.round(b.pct / 10);
    cls = b.pct <= 20 ? 'low' : (b.pct <= 40 ? 'mid' : '');
    text = `${b.pct}%${b.pct <= 10 ? ' · Low Battery' : ''}`;
    volt = `Voltage: ${Number(b.v).toFixed(2)} V`;
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
    
    const l3Sig = (d.myRssi === null || d.myRssi === undefined) ? 'Unavailable' : sigHtml(d.myRssi);
    $('mySignal').innerHTML = l3Sig;
    $('drawerL3Signal').innerHTML = l3Sig;

    renderBattery(d.bat);
    $('nodeTotal').innerText = `(${d.total}${d.full ? '+' : ''})`;
    
    let summary = `${d.direct} direct · ${d.total - d.direct} relayed`;
    if (d.shown < d.total) summary += ` · showing ${d.shown} of ${d.total}`;
    $('nodeSummary').innerText = d.total === 0 ? 'No other mesh nodes heard.' : summary;
    
    const nodeRowsHtml = d.nodes.length ? d.nodes.map((n) => {
      const where = n.h <= 1 ? 'Direct' : `${n.h} hops`;
      const sig = n.r === null ? `<small style="opacity:0.6">${n.h <= 1 ? 'N/A' : 'Relayed'}</small>` : sigHtml(n.r);
      return `<div class="node-row"><div><div class="node-name">${esc(n.n)}</div><small style="opacity:0.75">${where} · ${Number(n.a)}s ago</small></div><div class="node-sig">${sig}</div></div>`;
    }).join('') : '<small style="opacity:0.75">No remote nodes heard yet.</small>';
    
    $('nodeList').innerHTML = nodeRowsHtml;
    $('drawerNodeList').innerHTML = nodeRowsHtml;
    $('moreNodesBtn').classList.toggle('hidden', d.shown >= d.total);
  } catch (e) {}
  nodesBusy = false;
}

$('moreNodesBtn').onclick = () => {
  nodeLimit = Math.min(nodeLimit + 20, 64);
  pollNodes();
};

let pollFailureCount = 0;
let connectionWarningShown = false;

async function poll() {
  if (isPolling) return;
  isPolling = true;
  try {
    const res = await timedFetch('/events?cursor=' + cursor + '&av=' + ackVer, { cache: 'no-store' }, 5000);
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const data = await res.json();
    pollFailureCount = 0;
    if (connectionWarningShown) { connectionWarningShown = false; updatePresenceBar(); }

    if (data.mac) $('macDisplay').innerText = data.mac;
    if (data.label) {
      const locationKey = 'locus.sosLocation.' + (data.mac || 'default');
      if (!$('sosLocInput').dataset.loaded) {
        let savedLocation = '';
        try { savedLocation = localStorage.getItem(locationKey) || ''; } catch (e) {}
        $('sosLocInput').value = savedLocation || data.label;
        $('sosLocInput').dataset.loaded = '1';
        $('sosLocInput').addEventListener('input', () => {
          try { localStorage.setItem(locationKey, $('sosLocInput').value); } catch (e) {}
        });
      }
    }

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
  } catch (e) {
    pollFailureCount++;
    if (pollFailureCount >= 3) {
      connectionWarningShown = true;
      const bar = $('presenceBar');
      bar.classList.remove('hidden');
      bar.classList.add('warn');
      bar.textContent = 'Connection interrupted. Reconnecting to Locus automatically…';
    }
  }
  isPolling = false;
}

function sendSos(problem) {
  let loc = $('sosLocInput').value.trim() || 'Location unknown';
  let issue = String(problem || 'GENERAL EMERGENCY').trim();
  while (byteLen(loc) > 24) loc = loc.slice(0, -1);
  while (byteLen(issue) > 48) issue = issue.slice(0, -1);
  const msg = `[SOS] ${issue} | Loc: ${loc}`;
  if (activeChannel !== 'public') setChannel('public');
  $('msgText').value = msg;
  const isClearStatus = /CLEAR \/ SAFE HERE|SAFE & FULLY ACCOUNTED/i.test(issue);
  sendText(msg, true, !isClearStatus);
  closeSosModal();
}

function sendSosCustom() {
  const custom = $('sosCustomInput').value.trim();
  if (!custom) {
    showModal('Describe the emergency', 'Enter a short emergency description first.', false);
    return;
  }
  sendSos(custom);
  $('sosCustomInput').value = '';
}

function closeSosModal() {
  $('sosInitialModal').classList.add('hidden');
  safeSessionSet('sosShown', '1');
}

if (!safeSessionGet('sosShown')) {
  $('sosInitialModal').classList.remove('hidden');
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

const char ADMIN_LOGIN_PAGE[] PROGMEM = R"login(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark light"><meta http-equiv="Cache-Control" content="no-store"><title>Locus Admin Sign In</title>
<style>:root{color-scheme:dark;--bg:#0b1220;--panel:#121d30;--text:#e8eef8;--muted:#9baec7;--border:#293b55;--accent:#70a5ff}body.light{color-scheme:light;--bg:#f2f5fa;--panel:#fff;--text:#172033;--muted:#667085;--border:#dce4ef;--accent:#175cd3}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:18px;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI",sans-serif}.card{width:min(100%,420px);background:var(--panel);border:1px solid var(--border);border-radius:16px;padding:24px;box-shadow:0 20px 55px #0004}.top{display:flex;justify-content:space-between;align-items:center;gap:8px}.mark{font-weight:900;font-size:1.4rem;background:var(--accent);color:#071326;border-radius:10px;padding:8px 12px}h1{font-size:1.35rem;margin:20px 0 8px}p,small{color:var(--muted);line-height:1.5}label{display:block;margin:18px 0 6px;font-weight:700;font-size:.9rem}input,button{font:inherit;width:100%;padding:12px;border-radius:9px;border:1px solid var(--border);margin-top:8px}input{background:var(--bg);color:var(--text)}button{border:0;background:var(--accent);color:#071326;font-weight:800;cursor:pointer}.eyebrow{font-size:.72rem;color:var(--muted);font-weight:800;letter-spacing:.1em;text-align:right}.note{font-size:.82rem;margin-top:18px}.modal-backdrop[hidden]{display:none!important}.modal-backdrop{position:fixed;inset:0;z-index:9999;background:#0009;display:flex;align-items:center;justify-content:center;padding:18px}.modal{background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:20px;width:min(100%,400px)}.modal h2{font-size:1.1rem}.modal-actions{display:flex;justify-content:flex-end;margin-top:16px}.modal-actions button{width:auto;min-width:90px}</style></head>
<body><main class="card"><div class="top"><div class="mark">L</div><div class="eyebrow">LOCAL DEVICE ADMINISTRATION</div></div><h1>Locus Administrator</h1><p>Sign in to manage this ESP32 node, view diagnostics, and update firmware.</p><form id="loginForm"><label for="username">Username</label><input id="username" name="username" type="text" value="admin" autocomplete="username" readonly><label for="password">Administrator password</label><input id="password" name="password" type="password" minlength="8" maxlength="64" autocomplete="current-password" required autofocus><button type="submit">Sign in</button></form><p class="note">There is no web password-reset option. If you changed and lost the password, local device recovery may be required.</p></main>
<div id="appModal" class="modal-backdrop" hidden role="dialog" aria-modal="true" aria-labelledby="modalTitle"><div class="modal"><h2 id="modalTitle">Locus</h2><p id="modalMessage"></p><div class="modal-actions"><button id="modalOk" type="button">OK</button></div></div></div>
<script>
const $=id=>document.getElementById(id);const SETTINGS_KEY='locus.settings';function readSettings(){try{const s=JSON.parse(localStorage.getItem(SETTINGS_KEY)||'{}');return s&&typeof s==='object'?s:{}}catch(e){return {}}}function currentTheme(){return readSettings().theme==='light'?'light':'dark'}function applySharedTheme(){document.body.classList.toggle('light',currentTheme()==='light')}applySharedTheme();window.addEventListener('storage',e=>{if(e.key===SETTINGS_KEY)applySharedTheme()});function modal(title,message){return new Promise(resolve=>{const overlay=$('appModal');$('modalTitle').textContent=title;$('modalMessage').textContent=message;overlay.hidden=false;const finish=()=>{overlay.hidden=true;$('modalOk').removeEventListener('click',finish);document.removeEventListener('keydown',key);resolve()};const key=e=>{if(e.key==='Escape'||e.key==='Enter')finish()};$('modalOk').addEventListener('click',finish);document.addEventListener('keydown',key);$('modalOk').focus()})}
$('loginForm').addEventListener('submit',async e=>{e.preventDefault();const btn=$('loginForm').querySelector('button');btn.disabled=true;try{const body=new URLSearchParams({username:$('username').value,password:$('password').value});const r=await fetch('/admin/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body,cache:'no-store',credentials:'same-origin'});if(!r.ok)throw new Error(await r.text());window.location.replace('/admin')}catch(err){await modal('Sign-in failed',err.message||'Could not sign in.');$('password').select()}finally{btn.disabled=false}});
const params=new URLSearchParams(location.search);if(params.has('changed'))modal('Password changed','Sign in with your new administrator password.');else if(params.has('logout'))modal('Signed out','Your Locus administrator session has ended.');
</script></body></html>
)login";

const char ADMIN_PAGE[] PROGMEM = R"admin(
<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark light"><title>Locus Admin Console</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#121d30;--panel2:#17253b;--text:#e8eef8;--muted:#9baec7;--border:#293b55;--accent:#70a5ff;--good:#34d399;--warn:#fbbf24;--bad:#fb7185;--shadow:#0005}
body.light{color-scheme:light;--bg:#f2f5fa;--panel:#fff;--panel2:#f8fafc;--text:#172033;--muted:#667085;--border:#dce4ef;--accent:#175cd3;--good:#079455;--warn:#b54708;--bad:#d92d20;--shadow:#0001}
*{box-sizing:border-box}body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;background:var(--bg);color:var(--text);transition:background .15s,color .15s}.wrap{max-width:1100px;margin:0 auto;padding:20px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:18px}.brand{display:flex;align-items:center;gap:12px}.mark{width:46px;height:46px;display:grid;place-items:center;border-radius:13px;background:var(--accent);color:#071326;font-size:1.35rem;font-weight:900}.brand h1{font-size:1.35rem;margin:0}.sub{font-size:.88rem;color:var(--muted);margin-top:3px}.actions{display:flex;gap:8px;flex-wrap:wrap}.btn,button{font:inherit;border:1px solid var(--border);border-radius:9px;padding:10px 13px;background:var(--panel2);color:var(--text);cursor:pointer;font-weight:650}.btn.primary,button.primary{background:var(--accent);color:#071326;border-color:transparent}.btn.danger,button.danger{color:white;background:#c93652;border-color:transparent}.btn:hover,button:hover{filter:brightness(1.08)}a{color:var(--accent)}.banner{background:var(--panel);border:1px solid var(--border);padding:13px 15px;border-radius:12px;color:var(--muted);margin-bottom:16px}.banner strong{color:var(--text)}.section-title{margin:24px 0 10px;font-size:1.03rem}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}.stat,.panel{background:var(--panel);border:1px solid var(--border);border-radius:13px;padding:15px;min-width:0;box-shadow:0 4px 14px var(--shadow)}.stat .k{color:var(--muted);font-size:.8rem}.stat .v{font-size:1.35rem;font-weight:750;overflow-wrap:anywhere;margin-top:7px}.stat .hint{color:var(--muted);font-size:.76rem;margin-top:5px}.layout{display:grid;grid-template-columns:1.2fr .8fr;gap:12px}.panel h3{margin:0 0 12px;font-size:1rem}.kv{display:grid;grid-template-columns:minmax(120px,.7fr) minmax(0,1.3fr);gap:9px;border-bottom:1px solid var(--border);padding:8px 0;font-size:.88rem}.kv:last-child{border-bottom:0}.kv span:first-child{color:var(--muted)}.kv span:last-child{overflow-wrap:anywhere;text-align:right}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;font-size:.85rem;min-width:480px}th,td{text-align:left;padding:10px 8px;border-bottom:1px solid var(--border)}th{color:var(--muted);font-weight:650}td{overflow-wrap:anywhere}.pill{display:inline-block;border-radius:20px;padding:3px 8px;font-size:.75rem;font-weight:700;background:var(--panel2);border:1px solid var(--border)}.pill.good{color:var(--good)}.pill.warn{color:var(--warn)}.pill.bad{color:var(--bad)}.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.field{margin-bottom:10px}label{display:block;font-size:.84rem;font-weight:650;margin-bottom:6px}input{font:inherit;width:100%;padding:11px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--text)}input:focus{outline:2px solid var(--accent);outline-offset:1px}.note{color:var(--muted);font-size:.8rem;line-height:1.5}.status{margin-top:10px;font-size:.85rem;min-height:1.3em}.status.good{color:var(--good)}.status.bad{color:var(--bad)}.panel-actions{display:flex;gap:8px;flex-wrap:wrap}.panel-actions button{width:auto}.progressline{height:5px;background:var(--border);border-radius:10px;overflow:hidden;margin-top:10px}.progressline div{height:100%;width:0;background:var(--accent)}footer{color:var(--muted);font-size:.8rem;padding:20px 2px 6px}.danger-note{border-left:3px solid var(--warn);padding:9px 11px;background:var(--panel2);color:var(--muted);font-size:.8rem;margin-top:12px;border-radius:4px}
.modal-backdrop[hidden]{display:none!important}.modal-backdrop{position:fixed;inset:0;z-index:9999;background:#0009;display:flex;align-items:center;justify-content:center;padding:18px}.modal-card{width:min(100%,430px);background:var(--panel);color:var(--text);border:1px solid var(--border);border-radius:15px;padding:20px;box-shadow:0 20px 55px #0005}.modal-card h2{font-size:1.1rem;margin:0 0 10px}.modal-card p{font-size:.92rem;line-height:1.5;color:var(--muted);white-space:pre-wrap}.modal-actions{display:flex;justify-content:flex-end;gap:8px;flex-wrap:wrap;margin-top:18px}.modal-actions button{width:auto;min-width:90px}.modal-actions .modal-cancel{background:var(--panel2)}
@media(max-width:850px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.layout{grid-template-columns:1fr}}
@media(max-width:520px){.wrap{padding:12px}.grid{grid-template-columns:1fr 1fr;gap:7px}.stat{padding:11px}.stat .v{font-size:1.12rem}.form-grid{grid-template-columns:1fr}.kv{grid-template-columns:1fr;gap:3px}.kv span:last-child{text-align:left}.brand h1{font-size:1.15rem}}
</style></head>
<body><div class="wrap">
<header class="top"><div class="brand"><div class="mark">L</div><div><h1>Locus Admin Console</h1><div class="sub">Embedded node maintenance and diagnostics</div></div></div><div class="actions"><button id="refreshBtn" type="button">↻ Refresh</button><button id="openOtaBtn" class="btn primary" type="button">Firmware OTA →</button><button id="openChatBtn" class="btn" type="button">Open chat</button><button id="logoutBtn" class="danger" type="button">Log out</button></div></header>
<div class="banner"><strong>Connected node:</strong> <span id="identityLine">__MAC__ · __LABEL_TEXT__</span><br>Live diagnostics refresh automatically every 3 seconds. Changes made here affect this ESP32 node only.</div>
<h2 class="section-title">Live overview</h2>
<div class="grid">
 <div class="stat"><div class="k">Uptime</div><div class="v" id="uptime">Loading…</div><div class="hint">Since last reboot</div></div>
 <div class="stat"><div class="k">Connected phones</div><div class="v" id="phones">—</div><div class="hint">Wi-Fi clients connected to this node</div></div>
 <div class="stat"><div class="k">Known mesh nodes</div><div class="v" id="nodeCount">—</div><div class="hint">Including this node</div></div>
 <div class="stat"><div class="k">Free heap</div><div class="v" id="heap">—</div><div class="hint" id="heapHint">Current free RAM</div></div>
</div>
<h2 class="section-title">Device and network details</h2>
<div class="layout">
 <section class="panel"><h3>Device information</h3>
  <div class="kv"><span>Node MAC</span><span id="mac">__MAC__</span></div>
  <div class="kv"><span>Location label</span><span id="currentLabel">__LABEL_TEXT__</span></div>
  <div class="kv"><span>Chip</span><span id="chip">—</span></div>
  <div class="kv"><span>CPU frequency</span><span id="cpu">—</span></div>
  <div class="kv"><span>Flash capacity</span><span id="flash">—</span></div>
  <div class="kv"><span>Firmware size</span><span id="firmwareSize">—</span></div>
  <div class="kv"><span>OTA app partition capacity</span><span id="otaSpace">—</span></div>
  <div class="kv"><span>Locus build</span><span id="locusBuild">—</span></div>
  <div class="kv"><span>Arduino core / ESP-IDF</span><span id="sdk">—</span></div>
  <div class="kv"><span>Last reset reason</span><span id="resetReason">—</span></div>
 </section>
 <section class="panel"><h3>Radio and battery</h3>
  <div class="kv"><span>Access point IP</span><span id="apIp">—</span></div>
  <div class="kv"><span>Wi-Fi channel</span><span id="channel">—</span></div>
  <div class="kv"><span>AP clients</span><span id="apClients">—</span></div>
  <div class="kv"><span>Battery status</span><span id="batteryStatus">—</span></div>
  <div class="kv"><span>Battery voltage</span><span id="batteryVoltage">—</span></div>
  <div class="kv"><span>Battery estimate</span><span id="batteryPercent">—</span></div>
  <div class="kv"><span>Minimum free heap</span><span id="minHeap">—</span></div>
  <div class="kv"><span>Receive queue depth</span><span id="queueDepth">—</span></div>
  <p class="note">Battery percentage is an estimate from the configured voltage lookup table. Calibrate it against your actual battery chemistry before relying on it.</p>
 </section>
</div>
<h2 class="section-title">ESP-NOW diagnostics</h2>
<div class="grid">
 <div class="stat"><div class="k">Packets received</div><div class="v" id="rx">—</div></div>
 <div class="stat"><div class="k">Packets processed</div><div class="v" id="processed">—</div></div>
 <div class="stat"><div class="k">Successful sends</div><div class="v" id="txOk">—</div></div>
 <div class="stat"><div class="k">Send failures</div><div class="v" id="txFail">—</div></div>
 <div class="stat"><div class="k">Invalid packet lengths</div><div class="v" id="badLen">—</div></div>
 <div class="stat"><div class="k">RX queue overflow</div><div class="v" id="queueFull">—</div></div>
 <div class="stat"><div class="k">Last packet length</div><div class="v" id="lastLen">—</div></div>
 <div class="stat"><div class="k">Last RSSI</div><div class="v" id="lastRssi">—</div></div>
</div>
<section class="panel" style="margin-top:12px"><div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap"><h3 style="margin:0">Connected phone / browser devices</h3><span class="pill" id="clientHealth">Waiting for data</span></div>
 <div class="table-wrap"><table><thead><tr><th>Client MAC</th><th>Access point signal</th></tr></thead><tbody id="clientTable"><tr><td colspan="2">Loading connected clients…</td></tr></tbody></table></div>
 <p class="note">The ESP32 can report connected client MAC addresses and their Wi-Fi RSSI. It cannot identify a person's name from this radio data.</p>
</section>
<section class="panel" style="margin-top:12px"><div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap"><h3 style="margin:0">Discovered mesh nodes</h3><span class="pill" id="meshHealth">Waiting for data</span></div>
 <div class="table-wrap"><table><thead><tr><th>Node MAC</th><th>Path</th><th>Signal</th><th>Last seen</th></tr></thead><tbody id="meshTable"><tr><td colspan="4">Loading node table…</td></tr></tbody></table></div>
 <div class="panel-actions" style="margin-top:12px"><button id="testPacketBtn" class="primary" type="button">Send mesh test packet</button><button id="rebootBtn" class="danger" type="button">Reboot this node</button></div>
 <div id="diagStatus" class="status" role="status"></div><p class="note">A test packet is a normal diagnostic broadcast, not an SOS. A reboot temporarily disconnects phones using this node.</p>
</section>
<h2 class="section-title">Node configuration</h2>
<div class="layout">
 <section class="panel"><h3>Location label</h3><form id="labelForm"><div class="field"><label for="label">Installation location</label><input id="label" name="label" maxlength="48" value="__LABEL_ATTR__" required placeholder="e.g. Engineering Building - Floor 2"></div><button class="primary" type="submit">Save location label</button><div id="labelStatus" class="status" role="status"></div></form><p class="note">The label is stored in this node's NVS preferences and is used by physical-button SOS alerts.</p></section>
 <section class="panel"><h3>Change administrator password</h3><form id="passwordForm" autocomplete="off"><div class="field"><label for="currentPassword">Current password</label><input id="currentPassword" name="currentPassword" type="password" minlength="8" maxlength="64" required autocomplete="current-password"></div><div class="field"><label for="newPassword">New password</label><input id="newPassword" name="newPassword" type="password" minlength="8" maxlength="64" required autocomplete="new-password"></div><div class="field"><label for="confirmPassword">Confirm new password</label><input id="confirmPassword" name="confirmPassword" type="password" minlength="8" maxlength="64" required autocomplete="new-password"></div><button class="primary" type="submit">Change password</button><div id="passwordStatus" class="status" role="status"></div></form><div class="danger-note">There is intentionally no web-based password reset. Keep the new password somewhere safe. The updater and admin console use the same password.</div></section>
</div>
<h2 class="section-title">Firmware maintenance</h2>
<section class="panel"><h3>Over-the-air update</h3><p class="note">Compile the sketch for the classic ESP32 and export its compiled <code>.bin</code> file. Upload it through the OTA page. The firmware must fit an OTA-capable partition layout.</p><div class="panel-actions"><button class="btn primary" id="openOtaPanelBtn" type="button">Open OTA firmware updater →</button></div><div class="danger-note">Keep the node powered during an update. The access point is open and this prototype uses session cookies over plain HTTP. Credentials and session traffic are not encrypted, so do not use these controls on an untrusted network.</div></section>
<footer>Locus local administration · <span id="lastUpdated">Waiting for first update…</span></footer>
</div>
<div id="appModal" class="modal-backdrop" hidden role="dialog" aria-modal="true" aria-labelledby="modalTitle" aria-describedby="modalMessage"><div class="modal-card"><h2 id="modalTitle">Locus</h2><p id="modalMessage"></p><div class="modal-actions"><button id="modalCancel" class="modal-cancel" type="button">Cancel</button><button id="modalOk" class="primary" type="button">OK</button></div></div></div>
<script>
const $=id=>document.getElementById(id);
const SETTINGS_KEY='locus.settings';
function readSettings(){try{const v=JSON.parse(localStorage.getItem(SETTINGS_KEY)||'{}');return v&&typeof v==='object'?v:{}}catch(e){return {}}}
function currentTheme(){return readSettings().theme==='light'?'light':'dark'}
function applySharedTheme(){document.body.classList.toggle('light',currentTheme()==='light')}
applySharedTheme();
window.addEventListener('storage',e=>{if(e.key===SETTINGS_KEY)applySharedTheme()});
$('openOtaBtn').addEventListener('click',()=>{window.location.href='/ota'});
$('openOtaPanelBtn').addEventListener('click',()=>{window.location.href='/ota'});
$('openChatBtn').addEventListener('click',()=>{window.location.href='/'});
function txt(id,value){$(id).textContent=value===undefined||value===null?'—':String(value)}
function num(n){return Number(n||0).toLocaleString()}
function bytes(n){n=Number(n||0);if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB'}
function duration(ms){ms=Math.max(0,Number(ms)||0);const d=Math.floor(ms/86400000);ms%=86400000;const h=Math.floor(ms/3600000);ms%=3600000;const m=Math.floor(ms/60000);const sec=Math.floor((ms%60000)/1000);return (d?d+'d ':'')+h+'h '+m+'m '+sec+'s'}
function setStatus(id,msg,good){const el=$(id);el.textContent=msg;el.className='status '+(good===true?'good':good===false?'bad':'')}
function showModal(title,message,needChoice=false,okLabel='OK'){return new Promise(resolve=>{const overlay=$('appModal');$('modalTitle').textContent=title;$('modalMessage').textContent=message;$('modalCancel').hidden=!needChoice;$('modalCancel').style.display=needChoice?'':'none';$('modalOk').textContent=okLabel;overlay.hidden=false;const finish=value=>{overlay.hidden=true;$('modalOk').removeEventListener('click',onOk);$('modalCancel').removeEventListener('click',onCancel);document.removeEventListener('keydown',onKey);resolve(value)};const onOk=()=>finish(true);const onCancel=()=>finish(false);const onKey=e=>{if(e.key==='Escape')finish(false);if(e.key==='Enter'&&e.target===overlay)finish(true)};$('modalOk').addEventListener('click',onOk);$('modalCancel').addEventListener('click',onCancel);document.addEventListener('keydown',onKey);$('modalOk').focus()})}
async function request(url,options){const opts=Object.assign({cache:'no-store',credentials:'same-origin'},options||{});const r=await fetch(url,opts);if(r.status===401){window.location.replace('/admin/login');throw new Error('Your admin session expired. Sign in again.')}if(!r.ok)throw new Error(await r.text());return r}
let busy=false;
async function refresh(){if(busy)return;busy=true;try{const r=await request('/admin/status',{cache:'no-store'});const d=await r.json();txt('identityLine',d.mac+' · '+d.label);txt('mac',d.mac);txt('currentLabel',d.label);txt('uptime',duration(d.uptimeMs));txt('phones',num(d.stations));txt('apClients',num(d.stations));const clients=Array.isArray(d.clients)?d.clients:[];$('clientTable').innerHTML=clients.length?clients.map(c=>'<tr><td>'+String(c.mac)+'</td><td>'+c.rssi+' dBm</td></tr>').join(''):'<tr><td colspan="2">No phones or browser clients currently connected.</td></tr>';const clientHealth=$('clientHealth');clientHealth.textContent=clients.length+' connected client'+(clients.length===1?'':'s');clientHealth.className='pill '+(clients.length?'good':'warn');txt('nodeCount',num(d.meshCount+1));txt('heap',bytes(d.freeHeap));txt('heapHint','Minimum observed: '+bytes(d.minFreeHeap));txt('chip',d.chip+' · revision '+d.chipRev);txt('cpu',d.cpuMHz+' MHz');txt('flash',bytes(d.flashBytes));txt('firmwareSize',bytes(d.sketchBytes));txt('otaSpace',bytes(d.freeSketchBytes));txt('locusBuild',d.locusBuild);txt('sdk',d.coreVersion+' / '+d.sdk);txt('resetReason',d.resetReason);txt('apIp',d.apIp);txt('channel',d.channel);txt('minHeap',bytes(d.minFreeHeap));txt('queueDepth',d.queueDepth);txt('rx',num(d.rx));txt('processed',num(d.processed));txt('txOk',num(d.txOk));txt('txFail',num(d.txFail));txt('badLen',num(d.badLen));txt('queueFull',num(d.queueFull));txt('lastLen',d.lastLen);txt('lastRssi',d.lastRssi===null?'Unavailable':d.lastRssi+' dBm');const b=d.battery||{};txt('batteryStatus',b.s==='ok'?'Available':b.s==='none'?'No battery detected':b.s==='off'?'Disabled':'Invalid ADC pin');txt('batteryVoltage',b.s==='ok'?Number(b.v).toFixed(2)+' V':'—');txt('batteryPercent',b.s==='ok'?b.pct+'%':'—');const mesh=Array.isArray(d.mesh)?d.mesh:[];$('meshTable').innerHTML=mesh.length?mesh.map(n=>'<tr><td>'+String(n.mac)+'</td><td>'+(Number(n.hops)<=1?'Direct':Number(n.hops)+' hops')+'</td><td>'+(n.rssi===null?'—':n.rssi+' dBm')+'</td><td>'+n.age+' s ago</td></tr>').join(''):'<tr><td colspan="4">No other mesh nodes heard recently.</td></tr>';const health=$('meshHealth');health.textContent=mesh.length?'Mesh peers: '+mesh.length:'No peer nodes';health.className='pill '+(mesh.length?'good':'warn');txt('lastUpdated','Updated '+new Date().toLocaleTimeString());if(!$('label').dataset.edited)$('label').value=d.label}catch(e){txt('lastUpdated','Unable to refresh: '+e.message)}finally{busy=false}}
$('refreshBtn').addEventListener('click',refresh);refresh();setInterval(refresh,5000);
$('label').addEventListener('input',()=>$('label').dataset.edited='1');
$('labelForm').addEventListener('submit',async e=>{e.preventDefault();setStatus('labelStatus','Saving…');try{const body=new URLSearchParams({label:$('label').value});const r=await request('/admin/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const msg=await r.text();setStatus('labelStatus',msg,true);delete $('label').dataset.edited;await showModal('Location saved',msg);refresh()}catch(err){setStatus('labelStatus',err.message,false);await showModal('Unable to save location',err.message)}});
$('passwordForm').addEventListener('submit',async e=>{e.preventDefault();const old=$('currentPassword').value;const next=$('newPassword').value;const confirmValue=$('confirmPassword').value;if(next.length<8||next.length>64){await showModal('Password not changed','Use a password from 8 to 64 characters.');return}if(next!==confirmValue){await showModal('Passwords do not match','Enter the same new password in both fields.');return}if(old===next){await showModal('Choose another password','The new password must differ from the current password.');return}const yes=await showModal('Change administrator password?','The new password will protect both this admin console and the OTA updater. You will be signed out after it is saved.',true,'Change password');if(!yes)return;setStatus('passwordStatus','Changing password…');try{const body=new URLSearchParams({currentPassword:old,newPassword:next,confirmPassword:confirmValue});const r=await request('/admin/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const msg=await r.text();$('passwordForm').reset();setStatus('passwordStatus',msg,true);await showModal('Password changed',msg+' You will now return to the login screen.');window.location.replace('/admin/login?changed=1')}catch(err){setStatus('passwordStatus',err.message,false);await showModal('Password change failed',err.message)}});
$('testPacketBtn').addEventListener('click',async()=>{const yes=await showModal('Send mesh test packet?','This broadcasts a diagnostic message to reachable Locus nodes. It is not an SOS alert.',true,'Send test');if(!yes)return;setStatus('diagStatus','Sending test packet…');try{const r=await request('/admin/test',{method:'POST'});const msg=await r.text();setStatus('diagStatus',msg,true);await showModal('Diagnostic packet',msg);setTimeout(refresh,1000)}catch(e){setStatus('diagStatus',e.message,false);await showModal('Test packet failed',e.message)}});
$('rebootBtn').addEventListener('click',async()=>{const yes=await showModal('Reboot this node?','Phones connected to this ESP32 will disconnect temporarily. Reconnect after the access point returns.',true,'Reboot');if(!yes)return;setStatus('diagStatus','Requesting reboot…');try{const r=await request('/admin/reboot',{method:'POST'});const msg=await r.text();setStatus('diagStatus',msg,true);await showModal('Node rebooting',msg)}catch(e){setStatus('diagStatus',e.message,false);await showModal('Reboot request',e.message)}});
$('logoutBtn').addEventListener('click',async()=>{const yes=await showModal('Log out of Locus Admin?','Your admin session on this browser will be ended.',true,'Log out');if(!yes)return;try{await fetch('/admin/logout',{method:'POST',cache:'no-store',credentials:'same-origin'});window.location.replace('/admin/login?logout=1')}catch(e){await showModal('Could not log out',e.message)}});
</script></body></html>
)admin";
const char OTA_PAGE[] PROGMEM = R"ota(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark light"><meta http-equiv="Cache-Control" content="no-store"><title>Locus OTA Update</title>
<style>:root{color-scheme:dark;--bg:#0b1220;--panel:#121d30;--text:#e8eef8;--muted:#9baec7;--border:#293b55;--accent:#70a5ff;--warn:#fbbf24}body.light{color-scheme:light;--bg:#f2f5fa;--panel:#fff;--text:#172033;--muted:#667085;--border:#dce4ef;--accent:#175cd3;--warn:#b54708}*{box-sizing:border-box}body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;max-width:650px;margin:0 auto;padding:18px;background:var(--bg);color:var(--text)}main{margin-top:20px;background:var(--panel);padding:22px;border:1px solid var(--border);border-radius:14px}h1{font-size:1.45rem}p,small{color:var(--muted);line-height:1.5}input,button{font:inherit;box-sizing:border-box;width:100%;padding:12px;margin:8px 0;border-radius:8px;border:1px solid var(--border)}input{background:var(--bg);color:var(--text)}button{background:var(--accent);color:#071326;border:0;font-weight:750;cursor:pointer}.top{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}.top button{width:auto;padding:8px 11px}.warn{padding:12px;border-left:3px solid var(--warn);background:var(--bg);border-radius:4px}a{color:var(--accent)}.actions{display:flex;gap:8px;flex-wrap:wrap}.modal-backdrop[hidden]{display:none!important}.modal-backdrop{position:fixed;inset:0;z-index:9999;background:#0009;display:flex;align-items:center;justify-content:center;padding:18px}.modal-card{width:min(100%,430px);background:var(--panel);border:1px solid var(--border);border-radius:15px;padding:20px;box-shadow:0 20px 55px #0005}.modal-card h2{font-size:1.1rem;margin:0 0 10px}.modal-card p{color:var(--muted);line-height:1.5}.modal-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:18px}.modal-actions button{width:auto;min-width:90px}.modal-cancel{background:var(--panel);color:var(--text);border:1px solid var(--border)}</style></head>
<body><div class="top"><button id="backAdminBtn" type="button">← Admin dashboard</button><div class="actions"><button id="logoutBtn" type="button">Log out</button></div></div><main><h1>Locus OTA Firmware Update</h1><p>Upload a compiled <code>.bin</code> file for a classic ESP32 with an OTA-capable partition layout.</p><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><label for="firmware">Compiled firmware (.bin)</label><input id="firmware" type="file" name="firmware" accept=".bin,application/octet-stream" required><button id="uploadBtn" type="submit">Upload and Install Firmware</button></form><div id="uploadState" role="status"></div><p class="warn">Keep the node powered throughout the update. Do not disconnect while uploading. The node will restart after a successful update.</p><small>Use only firmware compiled for this classic ESP32 model and compatible OTA partition layout.</small></main>
<div id="appModal" class="modal-backdrop" hidden role="dialog" aria-modal="true" aria-labelledby="modalTitle"><div class="modal-card"><h2 id="modalTitle">Locus</h2><p id="modalMessage"></p><div class="modal-actions"><button id="modalCancel" class="modal-cancel" type="button">Cancel</button><button id="modalOk" type="button">OK</button></div></div></div>
<script>
const $=id=>document.getElementById(id);const SETTINGS_KEY='locus.settings';
function readSettings(){try{const v=JSON.parse(localStorage.getItem(SETTINGS_KEY)||'{}');return v&&typeof v==='object'?v:{}}catch(e){return {}}}
function currentTheme(){return readSettings().theme==='light'?'light':'dark'}
function applySharedTheme(){document.body.classList.toggle('light',currentTheme()==='light')}
applySharedTheme();
window.addEventListener('storage',e=>{if(e.key===SETTINGS_KEY)applySharedTheme()});
$('backAdminBtn').addEventListener('click',()=>{window.location.href='/admin'});
function showModal(title,message,needChoice=false,okLabel='OK'){return new Promise(resolve=>{const overlay=$('appModal');$('modalTitle').textContent=title;$('modalMessage').textContent=message;$('modalCancel').hidden=!needChoice;$('modalCancel').style.display=needChoice?'':'none';$('modalOk').textContent=okLabel;overlay.hidden=false;const finish=v=>{overlay.hidden=true;$('modalOk').removeEventListener('click',onOk);$('modalCancel').removeEventListener('click',onCancel);document.removeEventListener('keydown',onKey);resolve(v)};const onOk=()=>finish(true),onCancel=()=>finish(false),onKey=e=>{if(e.key==='Escape')finish(false)};$('modalOk').addEventListener('click',onOk);$('modalCancel').addEventListener('click',onCancel);document.addEventListener('keydown',onKey);$('modalOk').focus()})}
$('otaForm').addEventListener('submit',async e=>{if($('otaForm').dataset.approved==='1')return;e.preventDefault();const file=$('firmware').files&&$('firmware').files[0];if(!file){await showModal('Choose firmware','Select a compiled .bin file first.');return}if(!file.name.toLowerCase().endsWith('.bin')){await showModal('Unsupported file','Please select a compiled .bin firmware file.');return}const yes=await showModal('Install this firmware?','File: '+file.name+'\nSize: '+file.size.toLocaleString()+' bytes\n\nThe node will restart after a successful update. Keep it powered throughout.',true,'Upload firmware');if(yes){$('otaForm').dataset.approved='1';$('uploadState').textContent='Uploading firmware… do not close this page.';$('uploadBtn').disabled=true;$('otaForm').submit()}});
$('logoutBtn').addEventListener('click',async()=>{const yes=await showModal('Log out of Locus Admin?','Your admin session on this browser will be ended.',true,'Log out');if(!yes)return;try{await fetch('/admin/logout',{method:'POST',cache:'no-store',credentials:'same-origin'});window.location.replace('/admin/login?logout=1')}catch(e){await showModal('Could not log out',e.message)}});
</script></body></html>
)ota";

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

bool emergencyCooldownActive(uint32_t ip, uint32_t &secondsRemaining) {
  const uint32_t now = millis();
  secondsRemaining = 0;
  for (size_t i = 0; i < 16; i++) {
    EmergencyRateEntry &entry = emergencyRateLimits[i];
    if (entry.valid && entry.ip == ip) {
      uint32_t elapsed = now - entry.lastAt;
      if (elapsed < EMERGENCY_COOLDOWN_MS) {
        secondsRemaining = (EMERGENCY_COOLDOWN_MS - elapsed + 999) / 1000;
        return true;
      }
      return false;
    }
  }
  return false;
}

void rememberEmergencySent(uint32_t ip) {
  const uint32_t now = millis();
  int slot = -1;
  uint32_t oldestAge = 0;
  for (size_t i = 0; i < 16; i++) {
    if (emergencyRateLimits[i].valid && emergencyRateLimits[i].ip == ip) {
      slot = (int)i;
      break;
    }
    if (!emergencyRateLimits[i].valid) {
      slot = (int)i;
      break;
    }
    uint32_t age = now - emergencyRateLimits[i].lastAt;
    if (age >= oldestAge) {
      oldestAge = age;
      slot = (int)i;
    }
  }
  if (slot >= 0) {
    emergencyRateLimits[slot].ip = ip;
    emergencyRateLimits[slot].lastAt = now;
    emergencyRateLimits[slot].valid = true;
  }
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
  String keyMaterial = "Locus-AES-v1:" + secret;
  if (!sha256Raw(keyMaterial, aesKey)) return false;
  String channelMaterial = "Locus-Channel-v1:" + secret;
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

void triggerBuzzer(uint32_t durationMs) {
  buzzerActive = true;
  buzzerStopTime = millis() + durationMs;
  digitalWrite(BUZZER_PIN, HIGH);
}

bool isUrgentEmergencyText(const char *message) {
  bool safeStatus = strstr(message, "CLEAR / SAFE HERE") != nullptr ||
                    strstr(message, "SAFE & FULLY ACCOUNTED") != nullptr;
  if (safeStatus) return false;
  return strstr(message, "[SOS]") != nullptr ||
         strstr(message, "[EMERGENCY BUTTON]") != nullptr ||
         strstr(message, "NEED MEDICAL ASSISTANCE") != nullptr ||
         strstr(message, "TRAPPED / CANNOT EVACUATE") != nullptr ||
         strstr(message, "NEED RESCUE") != nullptr ||
         strstr(message, "HARDWARE SOS") != nullptr;
}

void processPacket(MeshPacket &packet, const RxItem &rx) {
  if (packet.magic != PACKET_MAGIC || packet.version != 1) return;
  if (packet.type != PUBLIC_MESSAGE && packet.type != PRIVATE_MESSAGE && packet.type != PRESENCE_MESSAGE &&
      packet.type != ACK_MESSAGE && packet.type != GROUP_PRESENCE_MESSAGE && packet.type != EMERGENCY_MESSAGE) return;
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
  if (packet.type == PUBLIC_MESSAGE || packet.type == EMERGENCY_MESSAGE) {
    char message[128] = {};
    memcpy(message, packet.payload, packet.length);
    message[packet.length] = '\0';
    publishLive(String(message), "public", "", origin);
    if ((packet.type == EMERGENCY_MESSAGE && isUrgentEmergencyText(message)) ||
        (packet.type == PUBLIC_MESSAGE && isUrgentEmergencyText(message))) triggerBuzzer(5000);
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
  BaseType_t queued;
  if (item.packet.type == EMERGENCY_MESSAGE) queued = xQueueSendToFront(rxQueue, &item, 0);
  else queued = xQueueSend(rxQueue, &item, 0);
  if (queued != pdTRUE) statRxQueueFull++;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void sendCallback(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void sendCallback(const uint8_t *macAddr, esp_now_send_status_t status) {
#else
void sendCallback(const uint8_t *macAddr, uint8_t status) {
#endif
  if (status == 0) statTxOk++;
  else statTxFail++;
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void handleHome() {
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleEvents() {
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
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
           ",\"mac\":\"" + macString(nodeMac) + "\",\"label\":\"" + jsonEscape(nodeLocationLabel.c_str()) + "\",\"nodes\":" + String(nodeCount) +
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

String htmlEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '\"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String currentAdminPassword() {
  String configured = nodePreferences.getString("adminPass", ADMIN_PIN);
  if (configured.length() < 8 || configured.length() > 64) return String(ADMIN_PIN);
  return configured;
}

String requestAdminToken() {
  String cookies = server.header("Cookie");
  const String marker = "LocusSession=";
  int start = cookies.indexOf(marker);
  if (start < 0) return String("");
  start += marker.length();
  int end = cookies.indexOf(';', start);
  String token = end < 0 ? cookies.substring(start) : cookies.substring(start, end);
  token.trim();
  if (token.length() != 32) return String("");
  return token;
}

int findAdminSession(const String &token) {
  if (token.length() != 32) return -1;
  uint32_t now = millis();
  for (size_t i = 0; i < ADMIN_SESSION_COUNT; i++) {
    AdminSession &session = adminSessions[i];
    if (session.valid && (uint32_t)(now - session.lastActivity) > ADMIN_SESSION_IDLE_MS) {
      session.valid = false;
      session.token[0] = '\0';
    }
    if (session.valid && strncmp(session.token, token.c_str(), sizeof(session.token)) == 0) {
      return (int)i;
    }
  }
  return -1;
}

bool adminSessionAuthenticated() {
  String token = requestAdminToken();
  int slot = findAdminSession(token);
  if (slot < 0) return false;
  adminSessions[slot].lastActivity = millis();
  return true;
}

void invalidateAdminSession(const String &token) {
  int slot = findAdminSession(token);
  if (slot >= 0) {
    adminSessions[slot].valid = false;
    adminSessions[slot].token[0] = '\0';
  }
}

void invalidateAllAdminSessions() {
  for (size_t i = 0; i < ADMIN_SESSION_COUNT; i++) {
    adminSessions[i].valid = false;
    adminSessions[i].token[0] = '\0';
    adminSessions[i].lastActivity = 0;
  }
}

String createAdminSession() {
  uint32_t now = millis();
  int slot = -1;
  int oldestSlot = 0;
  uint32_t oldestAge = 0;
  for (size_t i = 0; i < ADMIN_SESSION_COUNT; i++) {
    AdminSession &session = adminSessions[i];
    if (session.valid && (uint32_t)(now - session.lastActivity) > ADMIN_SESSION_IDLE_MS) session.valid = false;
    if (!session.valid) { slot = (int)i; break; }
    uint32_t age = now - session.lastActivity;
    if (age >= oldestAge) { oldestAge = age; oldestSlot = (int)i; }
  }
  if (slot < 0) slot = oldestSlot;
  uint8_t randomBytes[16];
  esp_fill_random(randomBytes, sizeof(randomBytes));
  char token[33];
  for (size_t i = 0; i < sizeof(randomBytes); i++) snprintf(token + i * 2, 3, "%02x", randomBytes[i]);
  token[32] = '\0';
  memset(&adminSessions[slot], 0, sizeof(adminSessions[slot]));
  strncpy(adminSessions[slot].token, token, sizeof(adminSessions[slot].token) - 1);
  adminSessions[slot].lastActivity = now;
  adminSessions[slot].valid = true;
  return String(token);
}

void clearAdminCookie() {
  server.sendHeader("Set-Cookie", "LocusSession=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
}

bool requireAdminAuthentication() {
  if (adminSessionAuthenticated()) return true;
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  if (server.uri() == "/admin" || server.uri() == "/ota") {
    server.sendHeader("Location", "/admin/login", true);
    server.send(303, "text/plain; charset=utf-8", "Sign in to Locus Admin.");
  } else {
    server.send(401, "text/plain; charset=utf-8", "Admin session expired. Sign in again.");
  }
  return false;
}

void handleAdminLoginPage() {
  if (adminSessionAuthenticated()) {
    server.sendHeader("Location", "/admin", true);
    server.send(303, "text/plain", "Already signed in.");
    return;
  }
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", ADMIN_LOGIN_PAGE);
}

void handleAdminLogin() {
  String username = server.hasArg("username") ? server.arg("username") : "";
  String supplied = server.hasArg("password") ? server.arg("password") : "";
  String expected = currentAdminPassword();
  if (username != OTA_USERNAME || supplied.length() != expected.length() || supplied != expected) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(401, "text/plain; charset=utf-8", "Incorrect username or administrator password.");
    return;
  }
  String token = createAdminSession();
  server.sendHeader("Set-Cookie", "LocusSession=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleAdminLogout() {
  invalidateAdminSession(requestAdminToken());
  clearAdminCookie();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "Signed out.");
}

void handleAdminPage() {
  if (!requireAdminAuthentication()) return;
  String page = FPSTR(ADMIN_PAGE);
  String safeLabel = htmlEscape(nodeLocationLabel);
  page.replace("__MAC__", macString(nodeMac));
  page.replace("__LABEL_TEXT__", safeLabel);
  page.replace("__LABEL_ATTR__", safeLabel);
  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", page);
}

void handleAdminConfig() {
  if (!requireAdminAuthentication()) return;
  sendNoCacheHeaders();
  String label = server.hasArg("label") ? server.arg("label") : "";
  label.trim();
  if (label.length() < 1 || label.length() > 48) {
    server.send(400, "text/plain", "Label must be 1 to 48 characters.");
    return;
  }
  for (size_t i = 0; i < label.length(); i++) {
    if ((uint8_t)label[i] < 0x20) {
      server.send(400, "text/plain", "Control characters are not allowed.");
      return;
    }
  }
  nodeLocationLabel = label;
  nodePreferences.putString("location", nodeLocationLabel);
  server.send(200, "text/plain; charset=utf-8", "Saved node label: " + nodeLocationLabel);
  Serial.println("Admin updated node label: " + nodeLocationLabel);
}

void handleAdminPassword() {
  if (!requireAdminAuthentication()) return;
  sendNoCacheHeaders();
  String current = server.hasArg("currentPassword") ? server.arg("currentPassword") : "";
  String next = server.hasArg("newPassword") ? server.arg("newPassword") : "";
  String confirm = server.hasArg("confirmPassword") ? server.arg("confirmPassword") : "";
  String actual = currentAdminPassword();

  if (current != actual) {
    server.send(403, "text/plain; charset=utf-8", "Current password is incorrect.");
    return;
  }
  if (next.length() < 8 || next.length() > 64) {
    server.send(400, "text/plain; charset=utf-8", "New password must be 8 to 64 characters.");
    return;
  }
  if (next != confirm) {
    server.send(400, "text/plain; charset=utf-8", "New passwords do not match.");
    return;
  }
  if (next == actual) {
    server.send(400, "text/plain; charset=utf-8", "Choose a password different from the current password.");
    return;
  }
  for (size_t i = 0; i < next.length(); i++) {
    if ((uint8_t)next[i] < 0x20 || (uint8_t)next[i] == 0x7F) {
      server.send(400, "text/plain; charset=utf-8", "Control characters are not allowed in passwords.");
      return;
    }
  }
  size_t written = nodePreferences.putString("adminPass", next);
  if (written == 0 || nodePreferences.getString("adminPass", "") != next) {
    server.send(500, "text/plain; charset=utf-8", "Could not save password. The old password remains active.");
    return;
  }
  Serial.println("Administrator password changed through the admin console.");
  invalidateAllAdminSessions();
  clearAdminCookie();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "Password changed and saved. Sign in again with the new password. The OTA updater uses the same password.");
}

String resetReasonText() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON: return String("Power-on");
    case ESP_RST_EXT: return String("External reset");
    case ESP_RST_SW: return String("Software reset");
    case ESP_RST_PANIC: return String("Exception or panic");
    case ESP_RST_INT_WDT: return String("Interrupt watchdog");
    case ESP_RST_TASK_WDT: return String("Task watchdog");
    case ESP_RST_WDT: return String("Watchdog reset");
    case ESP_RST_DEEPSLEEP: return String("Deep-sleep wake");
    case ESP_RST_BROWNOUT: return String("Brownout / supply voltage dip");
    case ESP_RST_SDIO: return String("SDIO reset");
    default: return String("Other reset (code ") + String((int)reason) + ")";
  }
}

String batteryJson();

String makeAdminStatusJson() {
  String resetReason = resetReasonText();
  String chip = ESP.getChipModel();
  String sdk = ESP.getSdkVersion();
  IPAddress apIP = WiFi.softAPIP();
  String json;
  json.reserve(7000);
  json = "{\"uptimeMs\":" + String((uint32_t)millis()) +
         ",\"mac\":\"" + macString(nodeMac) +
         "\",\"label\":\"" + jsonEscape(nodeLocationLabel.c_str()) +
         "\",\"chip\":\"" + jsonEscape(chip.c_str()) +
         "\",\"chipRev\":" + String(ESP.getChipRevision()) +
         ",\"cpuMHz\":" + String(ESP.getCpuFreqMHz()) +
         ",\"flashBytes\":" + String(ESP.getFlashChipSize()) +
         ",\"sketchBytes\":" + String(ESP.getSketchSize()) +
         ",\"freeSketchBytes\":" + String(ESP.getFreeSketchSpace()) +
         ",\"freeHeap\":" + String(ESP.getFreeHeap()) +
         ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) +
         ",\"locusBuild\":\"" + String(LOCUS_BUILD_NAME) +
         "\",\"coreVersion\":\"" + String(ESP.getCoreVersion()) +
         "\",\"sdk\":\"" + jsonEscape(sdk.c_str()) +
         "\",\"resetReason\":\"" + jsonEscape(resetReason.c_str()) +
         "\",\"apIp\":\"" + apIP.toString() +
         "\",\"channel\":" + String(WiFi.channel()) +
         ",\"stations\":" + String((uint32_t)WiFi.softAPgetStationNum()) +
         ",\"queueDepth\":" + String(rxQueue ? (uint32_t)uxQueueMessagesWaiting(rxQueue) : 0) +
         ",\"rx\":" + String((uint32_t)statRx) +
         ",\"processed\":" + String((uint32_t)statProcessed) +
         ",\"txOk\":" + String((uint32_t)statTxOk) +
         ",\"txFail\":" + String((uint32_t)statTxFail) +
         ",\"badLen\":" + String((uint32_t)statRxBadLen) +
         ",\"queueFull\":" + String((uint32_t)statRxQueueFull) +
         ",\"lastLen\":" + String((int)statLastLen) +
         ",\"lastRssi\":";
  if (statLastRssi == 0) json += "null";
  else json += String((int)statLastRssi);
  json += ",\"clients\":[";
  wifi_sta_list_t clientList;
  memset(&clientList, 0, sizeof(clientList));
  bool firstClient = true;
  if (esp_wifi_ap_get_sta_list(&clientList) == ESP_OK) {
    for (int i = 0; i < clientList.num; i++) {
      if (!firstClient) json += ",";
      char clientMac[18];
      snprintf(clientMac, sizeof(clientMac), "%02X:%02X:%02X:%02X:%02X:%02X",
        clientList.sta[i].mac[0], clientList.sta[i].mac[1], clientList.sta[i].mac[2],
        clientList.sta[i].mac[3], clientList.sta[i].mac[4], clientList.sta[i].mac[5]);
      json += "{\"mac\":\"" + String(clientMac) + "\",\"rssi\":" + String((int)clientList.sta[i].rssi) + "}";
      firstClient = false;
    }
  }
  json += "],\"battery\":" + batteryJson() + ",\"meshCount\":0,\"mesh\":[";

  uint32_t now = millis();
  uint32_t meshCount = 0;
  bool first = true;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    for (size_t i = 0; i < MAX_NODES; i++) {
      const ActiveNode &node = meshNodes[i];
      if (node.lastSeen == 0 || now - node.lastSeen > PEER_TTL_MS) continue;
      meshCount++;
      if (!first) json += ",";
      json += "{\"mac\":\"" + macString(node.mac) + "\",\"hops\":" + String((uint32_t)node.hops) + ",\"age\":" + String((uint32_t)((now - node.lastSeen) / 1000)) + ",\"rssi\":";
      json += node.hasRssi ? String((int)node.rssi) : String("null");
      json += "}";
      first = false;
    }
    xSemaphoreGive(stateMutex);
  }
  json += "]}";
  String marker = "\"meshCount\":0";
  json.replace(marker, "\"meshCount\":" + String(meshCount));
  return json;
}

void handleAdminStatus() {
  if (!requireAdminAuthentication()) return;
  sendNoCacheHeaders();
  server.send(200, "application/json; charset=utf-8", makeAdminStatusJson());
}

void handleAdminTestPacket() {
  if (!requireAdminAuthentication()) return;
  sendNoCacheHeaders();
  const char testMessage[] = "[LOCUS TEST] Diagnostic packet from node";
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = 1;
  packet.type = PUBLIC_MESSAGE;
  packet.hops = MAX_HOPS;
  packet.length = sizeof(testMessage) - 1;
  memcpy(packet.origin, nodeMac, sizeof(nodeMac));
  packet.sequence = nextSequence();
  memcpy(packet.payload, testMessage, packet.length);
  uint32_t id = publishLive(String(testMessage), "public", "", hashId(nodeMac));
  if (id != 0) markEventMine(id, packet.sequence);
  seenBeforeAndRemember(packet);
  if (!broadcastPacket(packet)) {
    server.send(500, "text/plain; charset=utf-8", "ESP-NOW broadcast request failed.");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "Diagnostic test packet broadcast request accepted.");
}

void handleAdminReboot() {
  if (!requireAdminAuthentication()) return;
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain; charset=utf-8", "Rebooting this Locus node. Reconnect after the access point returns.");
  Serial.println("Administrator requested node reboot.");
  delay(500);
  ESP.restart();
}

bool requireOtaAuthentication() {
  return requireAdminAuthentication();
}

void handleOtaPage() {
  if (!requireOtaAuthentication()) return;
  sendNoCacheHeaders();
  server.send_P(200, "text/html; charset=utf-8", OTA_PAGE);
}

void handleOtaUpload() {
  if (!adminSessionAuthenticated()) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("OTA complete: %u bytes\n", upload.totalSize);
    else Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("OTA upload aborted");
  }
  yield();
}

void handleOtaComplete() {
  if (!requireOtaAuthentication()) return;
  server.sendHeader("Connection", "close");
  sendNoCacheHeaders();
  if (Update.hasError()) {
    server.send(500, "text/plain", "Firmware update failed. Keep the device connected and check Serial Monitor.");
    return;
  }
  server.send(200, "text/plain", "Firmware updated. Locus is restarting.");
  delay(800);
  ESP.restart();
}

enum BatteryState { BAT_DISABLED = 0, BAT_OK = 1, BAT_NONE = 2, BAT_BAD_PIN = 3 };
BatteryState batteryState = BAT_DISABLED;
float batteryCellV = 0.0f;
int batteryStep = 0;
bool batteryHaveSample = false;
uint32_t batteryNextAt = 0;

bool adcPinUsable(int pin) {
  return pin >= 32 && pin <= 39;
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
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
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

  bool requestedEmergency = (server.hasArg("emergency") && server.arg("emergency") == "1") ||
                            message.startsWith("[SOS]") || message.startsWith("[EMERGENCY BUTTON]");
  bool safeStatus = message.indexOf("CLEAR / SAFE HERE") >= 0 ||
                    message.indexOf("SAFE & FULLY ACCOUNTED") >= 0;
  bool emergency = requestedEmergency && !safeStatus;

  if (emergency && type != "public") {
    server.send(400, "text/plain", "Emergency alerts must be sent to the public emergency feed.");
    return;
  }

  if (emergency) {
    uint32_t secondsRemaining = 0;
    if (emergencyCooldownActive(clientIP, secondsRemaining)) {
      server.sendHeader("Retry-After", String(secondsRemaining));
      server.send(429, "text/plain", "Emergency cooldown active. Try again in " +
                  String(secondsRemaining) + " seconds.");
      return;
    }
  } else if (checkRateLimit(clientIP)) {
    server.send(429, "text/plain", "Rate limit: 1 ordinary message per 1.5 seconds.");
    return;
  }

  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = 1;
  packet.type = emergency ? EMERGENCY_MESSAGE : (type == "private" ? PRIVATE_MESSAGE : PUBLIC_MESSAGE);
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
  if (emergency) rememberEmergencySent(clientIP);

  lastHeartbeatTx = millis();
  String reply = "{\"id\":" + String(eventId) + ",\"boot\":" + String(bootId) +
                 ",\"emergency\":" + String(emergency ? "true" : "false") + "}";
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

#if !LOCUS_SINGLE_CORE
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

  nodePreferences.begin("locus", false);
  nodeLocationLabel = nodePreferences.getString("location", "Unconfigured location");

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

  String ssid = "Locus_" + macString(nodeMac);
  if (!WiFi.softAP(ssid.c_str(), nullptr, WIFI_CHANNEL, false, 8)) {
    fatalError("Access point startup failed. Restarting...");
  }

  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.println("Configured Wi-Fi TX power: 19.5 dBm. Actual RF output depends on mode and hardware.");

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

  const char *headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", HTTP_GET, handleHome);
  server.on("/generate_204", HTTP_GET, handleHome);
  server.on("/gen_204", HTTP_GET, handleHome);
  server.on("/hotspot-detect.html", HTTP_GET, handleHome);
  server.on("/library/test/success.html", HTTP_GET, handleHome);
  server.on("/connecttest.txt", HTTP_GET, handleHome);
  server.on("/ncsi.txt", HTTP_GET, handleHome);
  server.on("/canonical.html", HTTP_GET, handleHome);
  server.on("/success.txt", HTTP_GET, handleHome);
  server.on("/fwlink", HTTP_GET, handleHome);
  server.on("/redirect", HTTP_GET, handleHome);
  server.on("/admin/login", HTTP_GET, handleAdminLoginPage);
  server.on("/admin/login", HTTP_POST, handleAdminLogin);
  server.on("/admin/logout", HTTP_POST, handleAdminLogout);
  server.on("/events", HTTP_GET, handleEvents);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/join", HTTP_POST, handleJoin);
  server.on("/nodes", HTTP_GET, handleNodes);
  server.on("/admin", HTTP_GET, handleAdminPage);
  server.on("/admin/status", HTTP_GET, handleAdminStatus);
  server.on("/admin/config", HTTP_POST, handleAdminConfig);
  server.on("/admin/password", HTTP_POST, handleAdminPassword);
  server.on("/admin/test", HTTP_POST, handleAdminTestPacket);
  server.on("/admin/reboot", HTTP_POST, handleAdminReboot);
  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, handleOtaComplete, handleOtaUpload);

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(204);
    } else {
      sendNoCacheHeaders();
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain; charset=utf-8", "Opening Locus portal…");
    }
  });

  pinMode(EMERGENCY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  batterySetup();
  server.begin();
  lastHeartbeatTx = millis() - 25000;

#if !LOCUS_SINGLE_CORE
  Serial.println("Dual-Core Supported: Pinning Heavy Crypto/Mesh processing to Core 0.");
  xTaskCreatePinnedToCore(meshTaskCore0, "MeshTask", 10240, NULL, 1, &meshTaskHandle, 0);
#endif

  Serial.println("Locus web server started.");
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

  bool buttonDown = digitalRead(EMERGENCY_BUTTON_PIN) == LOW;
  if (buttonDown && !emergencyButtonWasDown) {
    uint32_t now = millis();
    bool cooldownExpired = !hardwareEmergencySent || (uint32_t)(now - lastHardwareEmergencyAt) >= EMERGENCY_COOLDOWN_MS;
    if (cooldownExpired) {
      lastHardwareEmergencyAt = now;
      hardwareEmergencySent = true;
      triggerBuzzer(5000);

      String hwSosMsg = "[EMERGENCY BUTTON] Location: " + nodeLocationLabel + " | Assistance requested. Node: " + hashId(nodeMac);
      if (hwSosMsg.length() > MAX_TEXT) hwSosMsg = hwSosMsg.substring(0, MAX_TEXT);

      MeshPacket p = {};
      p.magic = PACKET_MAGIC;
      p.version = 1;
      p.type = EMERGENCY_MESSAGE;
      p.hops = MAX_HOPS;
      p.length = hwSosMsg.length();
      memcpy(p.origin, nodeMac, 6);
      p.sequence = nextSequence();
      memcpy(p.payload, hwSosMsg.c_str(), p.length);

      uint32_t evId = publishLive(hwSosMsg, "public", "", hashId(nodeMac));
      if (evId != 0) markEventMine(evId, p.sequence);
      seenBeforeAndRemember(p);
      broadcastPacket(p);
      Serial.println("Physical SOS button: emergency packet broadcast.");
    } else {
      Serial.printf("Physical SOS cooldown active for %lu more seconds.\n",
        (unsigned long)((EMERGENCY_COOLDOWN_MS - (now - lastHardwareEmergencyAt) + 999) / 1000));
    }
  }
  emergencyButtonWasDown = buttonDown;

  if (buzzerActive && (int32_t)(millis() - buzzerStopTime) >= 0) {
    buzzerActive = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  if ((int32_t)(millis() - batteryNextAt) >= 0) {
    batteryNextAt = millis() + 2000;
    updateBattery();
  }

  dnsServer.processNextRequest();
  server.handleClient();

#if LOCUS_SINGLE_CORE
  meshBackgroundLogic();
#endif

  delay(1);
}
