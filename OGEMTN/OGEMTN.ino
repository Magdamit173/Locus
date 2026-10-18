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
const size_t MAX_NODES = 32;
const size_t MAX_GROUPS = 12;

const uint32_t PACKET_MAGIC = 0x534D4553;
const uint8_t PUBLIC_MESSAGE = 1;
const uint8_t PRIVATE_MESSAGE = 2;
const uint8_t PRESENCE_MESSAGE = 3;

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

struct GroupKey {
  uint8_t key[32];
  uint8_t channelId[8];
  char name[32];
  bool valid;
};

SeenEntry seenCache[SEEN_CACHE_SIZE] = {};
LiveEvent liveEvents[LIVE_EVENT_COUNT] = {};
ActiveNode meshNodes[MAX_NODES] = {};
RateLimit clientLimits[8] = {};
GroupKey groupKeys[MAX_GROUPS] = {};

size_t seenIndex = 0;
size_t liveIndex = 0;
uint32_t liveEventId = 0;
uint32_t lastHeartbeatTx = 0;

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
#menuToggle { display: none; background: none; border: none; font-size: 1.5rem; color: var(--text); cursor: pointer; padding: 0 15px 0 0; line-height: 1; width: auto; margin: 0; }
.app-container { display: flex; height: calc(100% - 50px); overflow: hidden; position: relative; width: 100%; }
.sidebar { width: 280px; min-width: 280px; background: var(--surface); border-right: 1px solid var(--border); display: flex; flex-direction: column; z-index: 200; height: 100%; }
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
.msg { padding: 10px 14px; border-radius: 12px; max-width: 80%; background: var(--msg-bg); align-self: flex-start; word-wrap: break-word; overflow-wrap: anywhere; }
.msg.self { background: var(--my-msg); align-self: flex-end; }
.msg-time { font-size: 0.75rem; opacity: 0.7; margin-top: 4px; text-align: right; }
.composer { background: var(--surface); padding: 15px; border-top: 1px solid var(--border); flex-shrink: 0; }
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
.settings-panel { position: absolute; top:0; left:0; right:0; bottom:0; background: var(--surface); padding: 30px; display: none; overflow-y: auto; z-index: 10; }
.settings-panel.active { display: block; }
.setting-group { margin-bottom: 25px; padding-bottom: 20px; border-bottom: 1px solid var(--border); }
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
</header>
<div class="app-container">
  <div class="sidebar">
    <div class="channel-list" id="channelList"></div>
    <div class="add-channel">
      <input type="text" id="newSecret" placeholder="New Secret Group" maxlength="31">
      <button id="addGroupBtn">Add Channel</button>
    </div>
  </div>
  <div class="chat-area">
    <div class="messages" id="messageFeed"></div>
    <div class="composer" id="composerBox">
      <details>
        <summary>Quick Chat (Disaster Alerts)</summary>
        <div class="quick-chat" id="quickChat">
          <button class="quick-btn" data-q="🆘 I am trapped inside! Need rescue!">🆘 Trapped</button>
          <button class="quick-btn" data-q="🌊 Flood water is rising quickly here!">🌊 Flood Rising</button>
          <button class="quick-btn" data-q="🏊 Deep water/Drowning! Need a boat!">🏊 Need Boat</button>
          <button class="quick-btn" data-q="🚑 Severe injury! I need urgent medical help!">🚑 Need Medic</button>
          <button class="quick-btn" data-q="🔥 Fire nearby, path is blocked!">🔥 Fire Blocked</button>
          <button class="quick-btn" data-q="🏃 I am abandoning this location now.">🏃 Moving out</button>
          <button class="quick-btn" data-q="🔋 My battery is dying. Last message.">🔋 Low Battery</button>
          <button class="quick-btn" data-q="✅ I am safe and have supplies.">✅ I am safe</button>
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

$('messageFeed').onclick = () => {
  document.querySelector('.sidebar').classList.remove('open');
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

function renderChannels() {
  let html = `<div class="channel ${activeChannel === 'public' ? 'active' : ''}" data-ch="public"><span class="name">Public Broadcast</span> 🌐</div>`;
  groups.forEach((g) => {
    html += `<div class="channel ${activeChannel === g ? 'active' : ''}" data-ch="${esc(g)}">
      <span class="name">🔒 ${esc(g)}</span>
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
  renderChannels();
  document.querySelector('.sidebar').classList.remove('open');
  if (ch === 'settings') {
    $('messageFeed').style.display = 'none';
    $('composerBox').style.display = 'none';
    $('settingsPanel').classList.add('active');
  } else {
    $('settingsPanel').classList.remove('active');
    $('messageFeed').style.display = 'flex';
    $('composerBox').style.display = 'block';
    renderMessages();
  }
}

async function joinGroup(secret) {
  try {
    const res = await timedFetch('/join', { method: 'POST', body: new URLSearchParams({ secret }) }, 4000);
    return res.ok;
  } catch (e) {
    return false;
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

function renderMessages() {
  if (activeChannel === 'settings') return;
  const feed = $('messageFeed');
  const filtered = chatLog.filter((m) => m.channel === activeChannel);
  feed.innerHTML = filtered.map((m) => `
    <div class="msg ${m.isSelf ? 'self' : ''}">
      <div style="font-weight:bold; font-size:0.8rem; margin-bottom:4px;">${esc(m.senderName)}</div>
      <div>${esc(m.text)}</div>
      <div class="msg-time">${esc(m.time)}</div>
    </div>
  `).join('');
  feed.scrollTop = feed.scrollHeight;
}

function pushMessage(channel, text, senderId, senderName, isSelf, eventKey) {
  if (eventKey && chatLog.some((m) => m.eventId === eventKey)) return;

  chatLog.push({
    id: Date.now() + Math.random(),
    eventId: eventKey || null,
    channel,
    text,
    senderId,
    senderName,
    isSelf,
    time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  });
  if (chatLog.length > 300) chatLog = chatLog.slice(-300);
  store.set(HISTORY_KEY, chatLog);

  if (!isSelf && hasNotif) {
    try {
      if (Notification.permission === 'granted') {
        new Notification('OGEMTN Alert', { body: `${senderName}: ${text}` });
      }
    } catch (e) {}
  }
  if (channel === activeChannel) renderMessages();
}

async function sendText(raw, fromInput) {
  if (sending) return;
  const text = raw.trim();
  if (!text) return;

  const myName = settings.username || 'Anon';
  const payloadText = `${myName}: ${text}`;

  if (byteLen(payloadText) > 127) {
    showModal('Limit Reached', 'Message too long with username attached.', false);
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

async function poll() {
  if (isPolling) return;
  isPolling = true;
  try {
    const res = await timedFetch('/events?cursor=' + cursor, {}, 4000);
    const data = await res.json();

    if (data.mac) $('macDisplay').innerText = data.mac;

    if (data.nodes !== undefined) {
      const isAlone = data.nodes <= 1;
      $('nodeBadge').innerHTML = `<span class="dot ${isAlone ? 'offline' : ''}"></span>${Number(data.nodes)} node(s)`;
    }

    const bootChanged = lastBoot !== null && data.boot !== lastBoot;
    if (lastBoot === null || bootChanged) {
      lastBoot = data.boot;
      syncGroups();
    }
    cursor = bootChanged ? 0 : data.cursor;

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
        else pushMessage(ch, msgBody, ev.sender, displayName, false, key);
      });
    }
  } catch (e) {}
  isPolling = false;
}

renderChannels();
renderBlocked();
renderMessages();
poll();
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
  liveEventId = 0;
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
    event.valid = true;
    liveIndex = (liveIndex + 1) % LIVE_EVENT_COUNT;
    xSemaphoreGive(stateMutex);
  }
  return id;
}

uint32_t nextSequence() {
  uint32_t value = 0;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    value = ++txSequence;
    xSemaphoreGive(stateMutex);
  }
  return value;
}

void updateNodePresence(const uint8_t *mac) {
  if (memcmp(mac, nodeMac, 6) == 0) return;
  uint32_t now = millis();
  if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
    int emptySlot = -1;
    bool updated = false;
    for (size_t i = 0; i < MAX_NODES; i++) {
      if (meshNodes[i].lastSeen != 0 && memcmp(meshNodes[i].mac, mac, 6) == 0) {
        meshNodes[i].lastSeen = now;
        updated = true;
        break;
      }
      if (meshNodes[i].lastSeen == 0 && emptySlot == -1) {
        emptySlot = (int)i;
      }
    }
    if (!updated && emptySlot != -1) {
      memcpy(meshNodes[emptySlot].mac, mac, 6);
      meshNodes[emptySlot].lastSeen = now;
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

void processPacket(MeshPacket &packet) {
  if (packet.magic != PACKET_MAGIC || packet.version != 1) return;
  if (packet.type != PUBLIC_MESSAGE && packet.type != PRIVATE_MESSAGE && packet.type != PRESENCE_MESSAGE) return;
  if (packet.type != PRESENCE_MESSAGE && packet.length == 0) return;
  if (packet.length > MAX_TEXT || packet.hops == 0) return;
  if (seenBeforeAndRemember(packet)) return;

  updateNodePresence(packet.origin);

  String origin = hashId(packet.origin);
  if (packet.type == PUBLIC_MESSAGE) {
    char message[128] = {};
    memcpy(message, packet.payload, packet.length);
    message[packet.length] = '\0';
    publishLive(String(message), "public", "", origin);
  } else if (packet.type == PRIVATE_MESSAGE) {
    uint8_t plaintext[129] = {};
    char groupName[32] = {};
    if (decryptPrivate(packet, plaintext, groupName, sizeof(groupName))) {
      plaintext[packet.length] = '\0';
      publishLive(String((char *)plaintext), "private", String(groupName), origin);
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
  if (status != 0) {
    Serial.printf("ESP-NOW TX Error: %d\n", status);
  }
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
  uint32_t nodeCount = getActiveNodesCount();
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
           ",\"mac\":\"" + macString(nodeMac) + "\",\"nodes\":" + String(nodeCount) + ",\"events\":[";

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
    xSemaphoreGive(stateMutex);
    json += "]}";
  } else {
    json = "{\"boot\":" + String(bootId) + ",\"cursor\":0,\"nodes\":1,\"events\":[]}";
  }
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
  server.send(200, "text/plain", "Joined.");
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
    hb.sequence = nextSequence();
    seenBeforeAndRemember(hb);
    broadcastPacket(hb);
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

  #ifdef CONFIG_IDF_TARGET_ESP32C3
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    Serial.println("Hardware: ESP32-C3 detected. TX Power set to 8.5dBm.");
  #else
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.println("Hardware: Standard ESP32 detected. TX Power set to 19.5dBm (Max).");
  #endif

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

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(MeshPacket));
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

  server.begin();

  lastHeartbeatTx = millis() - 25000;

  #if !OGEMTN_SINGLE_CORE
    Serial.println("Dual-Core Supported: Pinning Heavy Crypto/Mesh processing to Core 0.");
    xTaskCreatePinnedToCore(meshTaskCore0, "MeshTask", 10240, NULL, 1, &meshTaskHandle, 0);
  #endif

  Serial.println("OGEMTN Standard WebServer started.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  #if OGEMTN_SINGLE_CORE
    meshBackgroundLogic();
  #endif

  delay(1);
}
