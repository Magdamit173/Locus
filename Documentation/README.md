# Locus / OGEMTN Mesh

**Off-Grid Emergency Communication Network using ESP32 and ESP-NOW**

Locus is a prototype communication system intended to help people exchange short messages and emergency alerts when internet service, cellular networks, and ordinary Wi-Fi routers are unavailable. Each Locus node is an ESP32 that creates its own Wi-Fi access point for nearby phones and uses ESP-NOW to exchange messages with other ESP32 nodes.

This README explains what the supplied firmware is designed to do, how its parts work together, how to build and use it, and which limitations should be understood before relying on it.

> **Important scope note:** The supplied source contains `#if !defined(CONFIG_IDF_TARGET_ESP32) #error ...`, so this version is for a **classic dual-core ESP32**, such as an ESP32-WROOM-32 or compatible ESP32 Dev Module. It does **not** compile for an ESP32-C3 as written, even though the source includes a conditional single-core code path. The current battery pin is GPIO 34, not the C3 pin configuration described in some project notes.

---

## 1. What Locus does

When powered on, a Locus node:

1. Starts a local Wi-Fi access point that phones can join directly.
2. Runs a small web server that serves the chat interface from the ESP32's own flash memory.
3. Starts ESP-NOW on Wi-Fi channel 1 to communicate with other Locus nodes nearby.
4. Receives and relays valid mesh packets, up to the configured hop limit.
5. Displays messages, node information, battery estimates, and connection status in the web interface.
6. Can send emergency alerts from the web interface or from a physical SOS button.
7. Offers an authenticated administrator area for diagnostics, node labeling, password changes, rebooting, and firmware updates.

No internet connection or external Wi-Fi router is required for communication between nodes. Phones connect to the access point of a nearby node. ESP-NOW carries the mesh traffic between ESP32 nodes.

### What it is not

Locus is currently a prototype, not a certified emergency service or a replacement for emergency responders. Message delivery, radio range, battery estimates, captive-portal behavior, and hardware reliability need real-world tests. Do not assume an SOS reached every node or responder just because the interface accepted it.

---

## 2. Main system architecture

Locus has two wireless links working together on the same Wi-Fi channel.

### Phone to ESP32: Wi-Fi access point and HTTP

Each ESP32 creates a Wi-Fi network whose name follows this pattern:

`Locus_<ESP32 station MAC address>`

The current configuration uses:

- **Wi-Fi channel:** 1
- **Local IP address:** `192.168.4.1`
- **Maximum connected Wi-Fi clients:** 8
- **Wi-Fi password:** none; the access point is open
- **Web server port:** HTTP port 80

A phone connects to the access point and opens `http://192.168.4.1/`. The ESP32 serves the HTML, CSS, and JavaScript embedded in the firmware. The application does not download frameworks, stylesheets, or scripts from an external website.

### ESP32 to ESP32: ESP-NOW

ESP-NOW sends small data frames directly between ESP32 devices. The firmware registers the broadcast MAC address `FF:FF:FF:FF:FF:FF` as a peer and broadcasts mesh packets on channel 1.

Each receiving node checks a packet, processes any data it can understand, and forwards it when the packet still has hops remaining. This lets packets travel through intermediate nodes rather than requiring every node to be within direct radio range of the original sender.

### Simplified data flow

```text
Phone A
   |
   | Wi-Fi + HTTP
   v
Locus Node A ===== ESP-NOW ===== Locus Node B ===== ESP-NOW ===== Locus Node C
   ^                                  ^                                  ^
   |                                  |                                  |
Phone A's browser                 Phone B's browser                 Phone C's browser
```

Each node independently hosts the web application. There is no central server or master node in this design. The current admin console manages only the ESP32 to which the administrator is connected.

---

## 3. Board support and dual-core processing

The source sets:

```cpp
#define LOCUS_SINGLE_CORE 0
```

For the supported classic ESP32, the firmware creates a FreeRTOS task named `MeshTask` and pins it to **Core 0**. This task repeatedly runs `meshBackgroundLogic()`, which drains the received-packet queue, processes packets, handles pending acknowledgments, transmits presence heartbeats, and updates private-channel presence.

The Arduino `loop()` handles the local web server, DNS requests, physical SOS button, buzzer timing, battery sampling, and periodic Serial diagnostics. On the normal classic ESP32 Arduino setup, the Arduino loop task runs separately from the mesh task.

A FreeRTOS mutex named `stateMutex` protects many shared structures, such as node information, message events, group keys, and tracking tables. The ESP-NOW receive callback copies incoming packet data into an `rxQueue` instead of performing the full message-processing work immediately. This keeps the callback relatively short and allows processing to happen in the mesh task.

**Do not select ESP32-C3 for this version.** The compile-time guard rejects targets that are not the classic ESP32. To support the C3 properly, the target guard, pin map, task setup, and any target-specific APIs would need to be reviewed and changed together.

---

## 4. Startup sequence

During `setup()`, the firmware performs the following work:

1. Starts Serial output at **115200 baud**.
2. Creates the FreeRTOS mutex and clears runtime tracking arrays.
3. Opens the Preferences namespace `locus` and loads the saved node location label.
4. Generates a boot identifier, initial packet sequence seed, and random identity salt.
5. Enables Wi-Fi access-point/station mode and disables Wi-Fi sleep.
6. Configures the access point IP as `192.168.4.1`.
7. Reads the ESP32 station MAC address and builds the AP name `Locus_<MAC>`.
8. Starts the open access point with up to 8 clients.
9. Sets the requested Wi-Fi transmit-power level using `WiFi.setTxPower(WIFI_POWER_19_5dBm)`.
10. Starts wildcard DNS redirection for the captive portal.
11. Initializes ESP-NOW and registers the broadcast peer.
12. Creates the receive queue and registers ESP-NOW callbacks.
13. Registers the HTTP routes for chat, diagnostics, administrator functions, captive-portal requests, and OTA updates.
14. Configures GPIO 4 for the physical SOS button, GPIO 16 for buzzer control, and the battery ADC.
15. Starts the web server and the mesh processing task.

`fatalError()` prints a message, gives the serial output a short opportunity to flush, and restarts the ESP32 if certain critical initialization steps fail.

The requested transmit-power setting is not a guarantee that every board emits exactly 19.5 dBm. Real output depends on the hardware, antenna, regulatory limits, and operating conditions.

---

## 5. Mesh packet format

The firmware defines a packed structure named `MeshPacket`. Packing removes normal compiler-added padding between fields.

**The structure in this source is 182 bytes, not 177 bytes.** Its field sizes add up as follows:

| Field | Size | Purpose |
|---|---:|---|
| `magic` | 4 bytes | Identifies the packet format (`0x534D4553`) |
| `version` | 1 byte | Protocol version |
| `type` | 1 byte | Kind of packet |
| `hops` | 1 byte | Remaining relay allowance |
| `length` | 1 byte | Number of meaningful payload bytes |
| `origin` | 6 bytes | MAC address of the node that originated the packet |
| `sequence` | 4 bytes | Packet sequence number |
| `channelId` | 8 bytes | Identifier for the message channel |
| `nonce` | 12 bytes | Random initialization value for AES-GCM |
| `payload` | 128 bytes | Message or encrypted data buffer |
| `tag` | 16 bytes | AES-GCM authentication tag |
| **Total** | **182 bytes** | Total packed structure size |

Although the payload buffer is 128 bytes long, the firmware limits ordinary text messages to **127 bytes**. The browser checks UTF-8 byte length before sending, rather than counting only visible characters. This matters because an emoji or some non-English characters can use multiple UTF-8 bytes.

The receiver checks that the incoming ESP-NOW frame length equals `sizeof(MeshPacket)`. Nodes running incompatible packet layouts or versions will not communicate correctly.

### Packet types used by the source

| Type constant | Value | Function |
|---|---:|---|
| `PUBLIC_MESSAGE` | 1 | Ordinary public-channel message |
| `PRIVATE_MESSAGE` | 2 | AES-GCM encrypted private-group message |
| `PRESENCE_MESSAGE` | 3 | Node heartbeat and local Wi-Fi station count |
| `ACK_MESSAGE` | 4 | Mesh message acknowledgment |
| `GROUP_PRESENCE_MESSAGE` | 5 | Encrypted private-group listener count |
| `EMERGENCY_MESSAGE` | 6 | Emergency or SOS packet |

---

## 6. How multi-hop forwarding works

The firmware sets `MAX_HOPS` to **6**. A newly created packet starts with its hop count set to 6.

When a node receives a packet, it:

1. Checks the magic value and protocol version.
2. Checks that the packet type, payload length, and remaining hop count are acceptable.
3. Checks the duplicate cache. A packet already seen recently is ignored.
4. Updates its information about the originating mesh node.
5. Processes the message or control information if appropriate.
6. If more than one hop remains, decrements the hop count and rebroadcasts the packet.

The hop count limits how many relay steps the firmware permits. It is not a guarantee of physical range, successful delivery, or an optimal route. The network floods packets rather than calculating a full shortest-path route.

### Duplicate suppression

`seenCache[96]` is a circular tracking array. Each entry records an origin MAC address, sequence number, and timestamp. Entries older than about 120 seconds are expired. If the same origin/sequence pair is seen again while still recorded, the firmware drops it rather than processing and relaying it repeatedly.

This reduces broadcast loops, but the cache is finite. Very busy networks can replace older entries before their expiry period ends.

### Radio acknowledgments

The firmware schedules ACK packets with a random delay of approximately 60 to 460 milliseconds. This reduces the likelihood that multiple relays attempt to send their acknowledgments at exactly the same moment.

The UI's `Sent` and `Delivered` markers should be read as **best-effort mesh acknowledgment status**, not proof that every phone or intended responder displayed the message. An acknowledgment can be generated by a mesh node that received the packet; the current protocol does not provide a separate delivery receipt from every individual phone.

---

## 7. Public, private, and emergency messages

### Public Broadcast

Public messages can be read by nodes and browsers participating in the public feed. Their contents are **not encrypted** by the private-group AES-GCM layer. Do not put confidential information in the public channel.

### Private secret groups

A user enters a secret phrase to create or join a private group. The firmware calculates:

- An AES-256 key from a SHA-256 hash of the text prefix `Locus-AES-v1:` plus the secret.
- A channel identifier from a different SHA-256 hash using the prefix `Locus-Channel-v1:` plus the secret. The channel identifier is the first 8 bytes of that hash.

The message payload is encrypted and authenticated with AES-GCM. The packet contains a random 12-byte nonce and a 16-byte authentication tag. A node that does not have the matching secret cannot decrypt the private payload, although it can still forward the packet as part of the mesh.

The key derivation in this version is **a pair of prefixed SHA-256 hashes, not a formal HKDF or a password-hardening algorithm**. Choose a long, unique secret phrase. Short or predictable phrases can be guessed by an attacker who can observe channel identifiers. The secret phrase is also stored in the browser's local storage as part of the saved group list, so it is not protected from someone who can access that browser profile.

The code's AES-GCM associated data (`makeAAD`) is 25 bytes: magic, version, type, length, origin, sequence, and channel ID. It deliberately does **not** include `hops`, because relays change the hop count. As a result, the GCM tag does not detect a change to the hop-count field by itself. The source summary should not claim that hop-count tampering is rejected by GCM authentication.

### Emergency alerts

The interface includes quick emergency phrases for:

- Medical assistance, severe injury, and an unconscious person
- Fire, smoke, entrapment, and chemical spill/gas leak
- Immediate public-safety threats and missing or vulnerable persons
- Earthquake damage, flooding, landslides, and severe weather
- Safe-status updates and custom emergency text

SOS messages are moved to the public channel before sending. The message includes a manually entered location label or location text, because the firmware does not collect GPS coordinates.

Emergency messages use packet type `EMERGENCY_MESSAGE`. They are given priority when the receive callback adds packets to the RX queue. If a received public/emergency message matches the firmware's emergency-text checks, the node activates its buzzer for 5 seconds.

The web emergency endpoint applies a **30-second cooldown per client IP address**. The physical SOS button has its own separate 30-second cooldown in RAM. These cooldowns are rate limits, not an emergency dispatch guarantee. The physical-button cooldown is not saved across a reboot.

Not every custom SOS phrase is guaranteed to trigger the buzzer on other nodes: the buzzer condition checks for particular patterns such as `[SOS]`, `[EMERGENCY BUTTON]`, `NEED MEDICAL ASSISTANCE`, or `TRAPPED / CANNOT EVACUATE`.

---

## 8. Presence and node tracking

### Node heartbeats

The firmware periodically broadcasts a `PRESENCE_MESSAGE`, normally every 30 seconds. The payload includes the number of phones connected to that node's access point. If the local station count changes, the firmware brings the next heartbeat forward so other mesh nodes can learn about the change sooner.

The web interface polls the `/events` endpoint and displays the known node count. The code considers a mesh node active for up to roughly 90 seconds after it was last heard.

### Private-group presence

Private-group listener updates use `GROUP_PRESENCE_MESSAGE`. The firmware cycles through its registered groups and, when a group has local listeners, sends an encrypted presence packet that contains the listener count. The normal group-presence interval is 5 seconds.

The browser re-sends `/join` requests for saved groups periodically to keep its local listener presence refreshed. Listener records time out after around 30 seconds if they are no longer refreshed.

### Signal strength

The firmware reports RSSI from the ESP-NOW receive callback when the installed Arduino-ESP32/ESP-IDF API provides it. Directly heard nodes can have a measured RSSI. A relayed node normally does not have a direct RSSI measurement, so its signal is shown as unavailable or relayed.

The admin dashboard can also show connected Wi-Fi client MAC addresses and the access-point RSSI reported by the ESP32. These MAC addresses do not identify a person's real name.

---

## 9. Browser application features

The main web interface is the HTML string `PAGE` stored in firmware flash. Its JavaScript runs on the connected phone's browser.

### Chats and groups

- Public Broadcast channel
- Private groups created from secret phrases
- Custom display name attached to outgoing message text
- Reply-to-message interface with a quoted snippet
- Local blocklist that hides messages matching selected names or IDs on that browser
- Unread counters for channels
- `Sent` and best-effort `Delivered` status markers

Blocking is performed by the browser. It does not stop a blocked device from transmitting packets over ESP-NOW, and it does not block the person for everyone else.

### Emergency interface

The SOS button opens a custom emergency modal. Users select a prepared emergency type or enter a short custom problem and a location. The message is sent to the public feed. The user can also use the physical button on the hardware without having the web interface open.

### Notifications and alert sounds

The browser can synthesize a short tone with the Web Audio API, use vibration through `navigator.vibrate()` when supported, and request browser notifications. Browser restrictions may prevent sound, vibration, or notifications unless the user has interacted with the page or granted permission. A mobile operating system may also suspend background browser activity, so background alert delivery is not guaranteed.

### Theme and stored browser data

Dark mode is the default when no theme preference has been saved. Users change the theme through the **main app's Settings panel**. The Admin, login, and OTA pages read the same `locus.settings` theme value and do not have their own theme switch.

The browser attempts to store these keys in `localStorage`:

| Key | Saved information |
|---|---|
| `locus.history` | Locally saved chat history, limited to the newest 300 messages |
| `locus.groups` | Saved private-group secret phrases |
| `locus.settings` | Display name, theme, and alert-sound preference |
| `locus.blocked` | Browser-local blocklist |

The message history and settings are saved on the phone/browser, not as permanent message history in the ESP32 firmware. The ESP32's in-memory live-event ring buffer is cleared on reboot. Private-group secrets are stored as browser data so the app can rejoin those groups later.

Some captive-portal mini-browsers isolate their storage from the phone's regular browser. When this happens, saved theme settings or chat history might not be shared between the captive window and the regular browser even though both use the same IP address.

### UI dialogs and replies

The main web app and admin pages use custom HTML modal overlays for most confirmations and status messages instead of native JavaScript `alert()` and `confirm()` calls. Replies use a custom separator (` ⟡ `) to separate the quoted text from the new reply. The rendering code extracts that section into a quote box.

---

## 10. Captive portal behavior

The ESP32 runs `DNSServer` with a wildcard domain rule that directs DNS requests to `192.168.4.1`. The web server handles several common connectivity-check paths, including Android/Google, Apple, and Windows-style test routes, and serves the Locus home page for them. Unknown paths are redirected to the home page; HTTP `OPTIONS` requests receive a basic CORS preflight response.

This is an attempt to help phones detect the network as a captive portal, but **automatic pop-up behavior depends on the phone's operating system and browser**. It cannot be guaranteed on every device. If the page does not open automatically, connect to the `Locus_<MAC>` access point and visit:

`http://192.168.4.1/`

The access point has no internet connection by design. Captive-portal detection can show a “no internet” warning even while local Locus communication is functioning normally.

---

## 11. Admin console

Open the main app's Settings panel and select **Open Locus Admin Console**, or visit `http://192.168.4.1/admin` while connected to the node. The admin pages are protected by an application login and a session cookie.

### Admin login and sessions

- Username: `admin`
- Default password if no saved password exists: `58427319`
- Session cookie: `LocusSession`
- Session expires after 30 minutes without activity
- Cookie lifetime requested by the browser: 8 hours
- Up to 8 sessions are stored in RAM
- Logging out invalidates the current session
- Changing the administrator password invalidates all existing sessions
- No web-based password-reset feature is provided

The password is stored in ESP32 Preferences under the `locus` namespace as `adminPass`. It survives ordinary reboots and firmware updates that preserve NVS, but it may be lost if the device's stored settings or flash are erased. If there is no saved password, the firmware falls back to the compiled default.

**Security limitation:** The session cookie is marked `HttpOnly` and `SameSite=Strict`, but the network uses plain HTTP and an open Wi-Fi access point. HTTP does not encrypt the password, session cookie, or page traffic in transit. Anyone able to monitor the local wireless connection may be able to intercept them. The current login handler also does not implement a robust failed-login lockout. This admin design is suitable for a controlled prototype test, not an untrusted network.

### Admin dashboard features

The dashboard can display:

- Node MAC address and saved location label
- Uptime, chip model/revision, CPU frequency, flash size, firmware size, and free application space
- Arduino core and ESP-IDF versions and the reported reset reason
- Local AP address, Wi-Fi channel, connected station count, and a connected-client MAC/RSSI table
- Current/minimum free heap, RX queue depth, battery measurement, and firmware build label
- Packet receive/process/send counters, malformed packet count, queue-overflow count, last received packet length, and last RSSI value
- Known mesh-node list, hop counts, RSSI when available, and time since last packet
- Location-label editing
- A diagnostic mesh test packet
- Reboot control
- Administrator password change
- Link to the OTA firmware updater

The status values are snapshots of measurements and counters the firmware actually collects. They do not constitute a complete hardware health test. For example, a node being listed in the mesh table only means it was heard recently; it does not guarantee it can reliably deliver every message.

### OTA firmware update

OTA means **over-the-air** firmware update. After the node has been initially programmed over USB with an OTA-compatible partition layout, you can update it wirelessly:

1. Build the `.ino` for the correct classic ESP32 board in Arduino IDE.
2. Use **Sketch → Export Compiled Binary** to create the compiled `.bin` firmware file.
3. Connect a phone or computer to the Locus AP.
4. Sign in to the admin console.
5. Open the OTA updater, choose the `.bin`, and confirm the upload in the custom modal.
6. Keep the ESP32 powered and leave the connection alone while the binary is uploaded.
7. On success, the firmware updater restarts the ESP32.

The `.ino` is editable source code. The `.bin` is the compiled program the ESP32 installs. The ESP32 cannot directly install the `.ino` as firmware.

OTA depends on the flash partition scheme leaving space for an OTA application image. A firmware image must fit the available OTA slot and must be compiled for the correct chip. This current code does not provide signed firmware verification, a remote update rollback policy, or a multi-node coordinated update system. Each node must be updated individually.

---

## 12. Hardware pins and battery measurement

The current sketch configures the following pins:

| Function | GPIO | Behavior |
|---|---:|---|
| Physical emergency button | GPIO 4 | `INPUT_PULLUP`; button press is read when the pin is LOW |
| Buzzer control | GPIO 16 | Set HIGH during the buzzer alert, then LOW |
| Battery ADC input | GPIO 34 | Reads the resistor-divider voltage on a classic ESP32 |

### Physical SOS button

The button is configured with the internal pull-up resistor. In the usual wiring, the button connects GPIO 4 to GND when pressed. A press broadcasts an emergency-button message, triggers the local buzzer for 5 seconds, and observes a 30-second local cooldown.

GPIO 16 is a logic output, not a high-current power supply. Use a suitable transistor or driver circuit if the buzzer draws more current than the GPIO can safely supply. Verify polarity and current requirements for the actual buzzer module.

### Battery sensing

Battery sensing is enabled in the source. The configured values are:

- ADC pin: GPIO 34
- Top resistor: 100 kΩ
- Bottom resistor: 200 kΩ
- Calibration factor: 1.0
- 24 ADC millivolt samples per reading
- Samples sorted; the 4 lowest and 4 highest are excluded
- The middle 16 samples are averaged
- A smoothed voltage estimate is mapped through an 11-point table
- A 3% hysteresis threshold helps prevent the displayed 10% steps from flickering

The displayed percentage is an estimate, not a fuel gauge. Battery chemistry, load, temperature, wiring, resistor tolerance, ADC calibration, and the voltage curve all affect accuracy. The configured table must be calibrated for the battery you actually use. Check the divider wiring and confirm that the ADC pin never exceeds its permitted voltage before connecting a battery.

---

## 13. HTTP API endpoints

The firmware registers these main application endpoints.

| Route | Method | Purpose | Authentication |
|---|---|---|---|
| `/` | GET | Main chat application | No |
| `/events` | GET | Poll messages, presence, and ACK information | No |
| `/send` | POST | Submit a public/private message or an SOS | No; rate limited |
| `/join` | POST | Register/refresh a private-group listener | No |
| `/nodes` | GET | Return battery, local signal, and mesh-node data | No |
| `/admin/login` | GET | Serve administrator sign-in page | No |
| `/admin/login` | POST | Check username/password and create a session | No; no strong login lockout in this version |
| `/admin/logout` | POST | Invalidate the current admin session | Session cookie used |
| `/admin` | GET | Administrator dashboard | Yes |
| `/admin/status` | GET | Device, client, packet, battery, and mesh diagnostics | Yes |
| `/admin/config` | POST | Save the location label | Yes |
| `/admin/password` | POST | Change and save the administrator password | Yes |
| `/admin/test` | POST | Broadcast a diagnostic mesh packet | Yes |
| `/admin/reboot` | POST | Restart this node | Yes |
| `/ota` | GET | Serve OTA upload form | Yes |
| `/update` | POST | Receive and install firmware binary | Yes |

The source also registers common captive-portal connectivity-check paths, such as `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`, `/success.txt`, and `/fwlink`.

The `/send` and `/join` endpoints are intentionally available to anyone connected to the open access point. Ordinary messages are rate-limited to approximately one request per client IP every 1.5 seconds. Emergency messages have their separate 30-second per-IP cooldown. These are basic anti-spam controls, not a strong identity or security boundary.

---

## 14. Runtime memory structures and limits

The main fixed-size arrays are:

| Structure | Capacity | Use |
|---|---:|---|
| `rxQueue` | 12 packet items | Buffers incoming ESP-NOW packets for the mesh task |
| `seenCache` | 96 entries | Tracks recent origin/sequence pairs for duplicate suppression |
| `liveEvents` | 100 events | Temporary message-event ring buffer for browser polling |
| `meshNodes` | 64 nodes | Tracks recently heard ESP32 nodes |
| `groupKeys` | 12 groups | Stores in-memory AES keys and group identifiers |
| `pendingAcks` | 8 entries | Holds ACKs scheduled for delayed transmission |
| `groupPeers` | 96 entries | Tracks remote nodes' private-group listener information |
| `listeners` | 24 entries | Tracks local browser listeners in groups |
| `clientLimits` | 8 entries | Tracks ordinary message rate limits by client IP |
| `emergencyRateLimits` | 16 entries | Tracks emergency cooldowns by client IP |
| `adminSessions` | 8 sessions | Holds active administrator sessions in RAM |

These limits are fixed in the firmware. If a table fills up, behavior depends on the specific table: some operations fail, while tracking caches may replace an older entry. The current code uses Arduino `String` in many places, including building JSON and parsing messages, so it is not a system with zero dynamic heap allocation. The dashboard's free-heap values help observe memory pressure during testing.

The browser stores up to 300 chat messages locally, while the ESP32 has a separate 100-event volatile buffer. These are different storage areas with different lifetimes.

---

## 15. Serial diagnostics

Open the Arduino IDE Serial Monitor at **115200 baud**. The firmware prints startup information and a diagnostic line about every five seconds, with fields such as:

```text
[STAT] rx=... badLen=... qFull=... processed=... txOk=... txFail=... lastLen=... rssi=... ch=... stations=...
```

| Field | Meaning |
|---|---|
| `rx` | Raw ESP-NOW receive callbacks counted |
| `badLen` | Received frames whose size does not match `sizeof(MeshPacket)` |
| `qFull` | Incoming frames that could not be queued |
| `processed` | Packets that passed the first checks and duplicate filter |
| `txOk` | ESP-NOW send callback reported success |
| `txFail` | ESP-NOW send callback reported failure |
| `lastLen` | Length of the most recent received frame |
| `rssi` | Most recent receive RSSI when available |
| `ch` | Current Wi-Fi channel reported by the ESP32 |
| `stations` | Number of phones connected to this node's access point |

A successful ESP-NOW send callback is not proof that every destination application displayed the message. Use these counters together with tests using multiple physical nodes and phones.

---

## 16. Build and first upload

1. Install Arduino IDE and the Espressif Arduino-ESP32 board support package.
2. Open this sketch in Arduino IDE.
3. Select **ESP32 Dev Module** or the matching classic ESP32 board. Do not select ESP32-C3 for this source.
4. Choose a flash partition scheme that supports OTA, with enough space for the compiled application.
5. Connect the ESP32 over USB and choose its correct serial port.
6. Compile/Verify before uploading.
7. Upload over USB for the initial installation.
8. Open Serial Monitor at 115200 baud and confirm that Wi-Fi, ESP-NOW, the web server, and the mesh task start without errors.
9. Connect a phone to the `Locus_<MAC>` Wi-Fi network and visit `http://192.168.4.1/` if the captive portal does not appear automatically.
10. Test public messaging using two nodes before relying on private groups, ACKs, battery estimates, emergency alerts, or OTA.

### Wireless update after initial USB flashing

After OTA has been verified once, edit the source, compile it again, export the compiled binary, and upload the new `.bin` through the admin OTA page. This updates only the node receiving the file. If the new binary is too large for the OTA slot or the selected partition layout is not OTA-compatible, installation can fail.

---

## 17. Limitations and engineering notes

Keep these points in mind when documenting, demonstrating, or extending the project.

1. **Classic ESP32 only:** The compile guard rejects ESP32-C3, and GPIO 34 is the configured battery ADC pin.
2. **Packet size:** `MeshPacket` is 182 bytes based on the field sizes in this file, not 177 bytes.
3. **No guaranteed emergency delivery:** The system broadcasts and relays packets. It does not guarantee every responder or phone received an alert.
4. **ACK meaning:** The current ACK system is a best-effort mesh acknowledgment, not an individual phone-delivery receipt.
5. **Private key derivation:** The code uses prefixed SHA-256 hashes, not formal HKDF or a password-hardening method. Strong secret phrases matter.
6. **Hop count integrity:** The `hops` field is not part of the 25-byte GCM AAD, so the GCM tag does not protect that field against modification.
7. **Plain HTTP and open AP:** Chat, admin credentials, session cookies, and configuration travel without transport encryption. The admin session mechanism is not a substitute for HTTPS or a trusted physical/network environment.
8. **No strong login brute-force protection:** The admin password endpoint should not be exposed on an untrusted network.
9. **Browser identity is limited:** The node tries to map the client's IP to a MAC through the local ARP table and otherwise uses a fallback based on the IP. This is not a cryptographically verified user identity, and the display name is user-provided message text.
10. **Battery percentage requires calibration:** The configured table is only an estimate and may not match your actual battery chemistry.
11. **Captive portal pop-ups vary:** Some phones may not automatically open the portal; the manual URL remains available.
12. **Browser notifications vary:** Audio, vibration, notifications, and storage depend on browser permissions and background behavior.
13. **OTA is manual per node:** There is no central multi-node updater, signed firmware verification, or coordinated rollback system in this source.
14. **No permanent ESP32 message archive:** The local event ring is in RAM and cleared at boot. The phone's local history is separate.
15. **Static limits matter:** Fixed tables can become full, and high traffic can cause packet drops. Free-heap and queue-overflow statistics should be watched during stress tests.
16. **Source-level verification is not hardware validation:** Compile this exact file with the intended Arduino-ESP32 version and test it on the actual boards, phones, battery circuit, SOS button, buzzer, and OTA partition layout.

---

## 18. Recommended test plan

Before presenting the system as working end to end, test each layer separately.

### Single-node checks

- Confirm the access point appears and remains available after a reboot.
- Open the main page directly at `http://192.168.4.1/`.
- Test Settings, theme changes, channel controls, custom modals, and message length validation.
- Test admin login, logout, location label saving, password change, wrong-password behavior, and session expiry.
- Verify the battery voltage reading with a multimeter and a known input voltage.
- Press the physical SOS button and verify the expected buzzer and Serial output.

### Two-node checks

- Join each node from a separate phone.
- Send public messages in both directions.
- Test private groups with the same secret and with a different secret.
- Confirm emergency messages appear in the public feed and trigger alert behavior where expected.
- Compare message status with the actual messages received; do not equate a checkmark with delivery to every phone.
- Observe RSSI, packet counters, node list, and queue-overflow count.

### Multi-hop checks

- Put nodes so the endpoints cannot reliably hear each other directly, while the middle node can hear both.
- Check whether the message appears at the far endpoint and whether the node counters change.
- Repeat with different positions, obstacles, and traffic levels.
- Watch for duplicate messages, missed messages, queue overflow, and high memory use.

### OTA checks

- First confirm that USB flashing succeeds.
- Confirm the selected partition scheme actually supports OTA.
- Export a binary from the same known-good sketch and upload it to one test node.
- Do not interrupt power while the image is uploading.
- After reboot, check the build label, admin login, chat, radio, and physical SOS path again.

---

## 19. Glossary

- **AP (Access Point):** The Wi-Fi network created by a Locus node for nearby phones.
- **ESP-NOW:** A direct wireless data protocol used by ESP32 devices without needing a Wi-Fi router.
- **Mesh node:** An ESP32 that exchanges and forwards Locus packets.
- **Hop:** One relay step from one node to another.
- **TTL / hop limit:** A limit that stops a packet from being forwarded indefinitely.
- **RSSI:** A received-signal-strength measurement, usually expressed in dBm.
- **AES-256-GCM:** Authenticated encryption used for private-group message payloads.
- **Nonce:** A unique/random value used by an encryption operation.
- **Authentication tag:** A value used by AES-GCM to detect invalid encrypted data or covered metadata changes.
- **Preferences / NVS:** Non-volatile storage on the ESP32 used for settings such as the node label and saved administrator password.
- **`localStorage`:** Browser storage used for local chat history, theme settings, display name, block list, and saved group secrets.
- **OTA:** Over-the-air firmware updating by uploading a compiled binary over the node's Wi-Fi connection.
- **`.ino`:** Arduino source code edited and compiled by the developer.
- **`.bin`:** The compiled firmware image that the ESP32 installs.

---

## 20. Project summary

Locus combines an ESP32-hosted local web application with ESP-NOW multi-hop packet forwarding. Phones connect directly to a node's access point, while the nodes exchange public messages, encrypted private-group messages, emergency alerts, presence updates, and acknowledgments over the mesh. The browser provides the chat UI and stores local history and preferences. The administrator console provides per-node diagnostics, configuration, password management, reboot, and OTA firmware upload.

The next engineering priority is systematic validation: compile the current source with the exact target toolchain, confirm the actual packet and hardware behavior, calibrate the battery measurement, test multi-hop delivery under realistic conditions, and improve transport security before using the system in an environment where hostile users may connect.
