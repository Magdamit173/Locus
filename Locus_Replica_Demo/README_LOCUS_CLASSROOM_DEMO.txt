LOCUS CLASSROOM DEMO — SHARED COMPUTER / PHONE SIMULATION
=========================================================

FILES
-----
START_LOCUS_DEMO.bat
    Easy launcher. Pick Host or Join.
server.py
    Lightweight Python web server and shared demo message store.
Locus_Exact_UI_Replica.html
    The Locus firmware UI replica. It includes its CSS and JavaScript in one file.

HOW TO RUN IT
-------------
1. Extract ALL files into one folder on a Windows computer.
2. All computers/phones must be connected to the same Wi-Fi or LAN. Do not use a
   guest Wi-Fi that isolates clients.
3. On ONE computer, double-click START_LOCUS_DEMO.bat and choose 1 (Host).
4. If Windows Firewall asks, allow Python on PRIVATE networks only. Do not enable
   public-network access unless you understand and intend that exposure.
5. A browser opens on the host. The black console also prints one or more LAN URLs,
   for example http://192.168.1.23:8765/ . Keep the console open.
6. On every other laptop/phone, open START_LOCUS_DEMO.bat and choose 2 (Join), then
   paste the COMPLETE URL shown by the host (including :port and trailing slash). Or
   type that full URL directly into any browser.
7. Set a different Display Name on each browser, then send messages. Public messages
   should appear on all connected clients. Private groups work when the clients enter
   the same group secret. The online-phone count updates as clients connect/disconnect.
8. To stop the shared demo, press Ctrl+C in the host's black console (or close it).

REQUIREMENTS
------------
- Host computer: Python 3 installed. No pip packages are required; server.py only uses
  Python's built-in standard library.
- Joining computers/phones: a modern web browser only. They do not need Python.
- Windows host: one of ports 8765–8780 must be available and the Private-network firewall rule may
  need user approval.

IMPORTANT LIMITS
----------------
- This is a browser/LAN simulation, not actual ESP-NOW radio communication.
- It does not connect to or control real ESP32 hardware.
- Messages and presence are stored in the host server's RAM and reset when the host
  server stops. Do not use it for real emergency response or confidential messages.
- Anyone on the same LAN who can reach the host port (8765–8780) can use the demo. Use only on a
  trusted classroom network; stop the server after the presentation.
- The simulated private channel only filters which demo clients receive a group's
  messages. It is not a security boundary or real end-to-end encryption.
- Keep server.py and Locus_Exact_UI_Replica.html beside the batch file in the same
  extracted folder. Do not distribute the .bat alone.

TROUBLESHOOTING
---------------
- If classmates cannot connect, confirm they are on the same Wi-Fi, not guest Wi-Fi.
- Use the complete LAN URL printed by the host, not 127.0.0.1 on another computer.
  127.0.0.1 always means the current device itself.
- If a firewall popup appears, allow access on private networks.
- The server automatically tries ports 8765–8780. If all are occupied, close another local server and retry.
- If Python is missing on the host, install Python 3 from python.org. Joining devices
  do not need Python.
