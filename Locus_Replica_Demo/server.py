#!/usr/bin/env python3
"""Locus classroom demo host: stdlib-only LAN server for simulated phones."""
from __future__ import annotations
import json
import socket
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parent
HTML_FILE = ROOT / "Locus_Exact_UI_Replica.html"
HOST = "0.0.0.0"
PORT = 8765
BOOT_ID = 2601025
PEER_TTL_SECONDS = 12
MAX_EVENTS = 500
MAX_TEXT_BYTES = 127
MAX_SECRET_BYTES = 31
LOCK = threading.RLock()

SEED_EVENTS = [
    {"id": 1, "text": "Campus Safety: Network check — mesh node is online.", "type": "public", "group": "", "sender": "node-campus-safety"},
    {"id": 2, "text": "Engineering Desk: Please keep the east stairwell clear.", "type": "public", "group": "", "sender": "node-engineering-desk"},
    {"id": 3, "text": "Rescue Team: Copy. Team members are checking the second floor.", "type": "public", "group": "", "sender": "node-rescue-team"},
    {"id": 4, "text": "Campus Safety: Status update: main corridor is passable.", "type": "public", "group": "", "sender": "node-campus-safety"},
]
EVENTS = list(SEED_EVENTS)
NEXT_ID = 5
PEERS: dict[str, dict] = {}

NODE_ROWS = [
    {"n": "D4:8A:FC:21:7B:10", "r": -48, "h": 1, "a": 2},
    {"n": "D4:8A:FC:21:7B:22", "r": -61, "h": 1, "a": 7},
    {"n": "D4:8A:FC:21:7B:35", "r": None, "h": 2, "a": 12},
]
NODE_MAC = "D4:8A:FC:21:7B:01"
LOCATION_LABEL = "Engineering Building · Floor 2"


def byte_len(value: str) -> int:
    return len(value.encode("utf-8", errors="replace"))


def client_id_from(handler: BaseHTTPRequestHandler) -> str:
    value = handler.headers.get("X-Locus-Client", "").strip()
    # Accept the app-generated phone-xxxx id, with a conservative safe charset.
    if not value or len(value) > 80 or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." for c in value):
        return "phone-unknown"
    return value


def active_peers(now: float | None = None) -> dict[str, dict]:
    now = now or time.time()
    expired = [cid for cid, peer in PEERS.items() if now - peer.get("seenAt", 0) > PEER_TTL_SECONDS]
    for cid in expired:
        PEERS.pop(cid, None)
    return PEERS


def touch_peer(cid: str, name: str | None = None) -> dict:
    now = time.time()
    peers = active_peers(now)
    peer = peers.get(cid, {"id": cid, "name": cid, "channel": "public", "groups": []})
    if name:
        peer["name"] = str(name)[:24]
    peer["seenAt"] = now
    peer["id"] = cid
    peer.setdefault("groups", [])
    PEERS[cid] = peer
    return peer


class Handler(BaseHTTPRequestHandler):
    server_version = "LocusClassroomDemo/1.0"

    def log_message(self, fmt, *args):
        # Compact log: avoid dumping full message content into the terminal.
        print(f"[{self.log_date_time_string()}] {self.client_address[0]} {fmt % args}")

    def send_bytes(self, status: int, data: bytes, content_type: str = "application/json; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, status: int, payload):
        self.send_bytes(status, json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

    def send_text(self, status: int, text: str):
        self.send_bytes(status, text.encode("utf-8"), "text/plain; charset=utf-8")

    def read_body(self) -> bytes:
        try:
            length = min(int(self.headers.get("Content-Length", "0")), 32_000)
        except ValueError:
            length = 0
        return self.rfile.read(length) if length > 0 else b""

    def read_form(self) -> dict[str, str]:
        raw = self.read_body().decode("utf-8", errors="replace")
        parsed = parse_qs(raw, keep_blank_values=True)
        return {k: v[-1] for k, v in parsed.items()}

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        cid = client_id_from(self)
        if path in ("/", "/index.html", "/Locus_Exact_UI_Replica.html"):
            if not HTML_FILE.exists():
                return self.send_text(500, "Locus_Exact_UI_Replica.html is missing beside server.py")
            data = HTML_FILE.read_bytes()
            # Turn on cross-device API mode only in the copy served by this LAN host.
            data = data.replace(b"window.LOCUS_LAN_MODE = false;", b"window.LOCUS_LAN_MODE = true;", 1)
            return self.send_bytes(200, data, "text/html; charset=utf-8")
        if path == "/favicon.ico":
            return self.send_bytes(204, b"", "image/x-icon")
        if path == "/health":
            return self.send_json(200, {"ok": True, "mode": "LAN simulation", "port": PORT})
        if path == "/events":
            query = parse_qs(parsed.query)
            try:
                requested = max(0, int(query.get("cursor", ["0"])[0]))
            except ValueError:
                requested = 0
            with LOCK:
                peer = touch_peer(cid)
                all_after = [ev for ev in EVENTS if int(ev["id"]) > requested]
                scan = all_after[:40]
                visible = [ev for ev in scan if ev.get("type") == "public" or (ev.get("group") and ev["group"] in peer.get("groups", []))]
                newest = int(EVENTS[-1]["id"]) if EVENTS else 0
                reply_cursor = int(scan[-1]["id"]) if scan else newest
                peers = active_peers()
                phone_count = len(peers)
                group_listeners = [p for p in peers.values() if cid == p.get("id") or p.get("channel") == "public"]
                payload = {
                    "boot": BOOT_ID,
                    "me": cid,
                    "cursor": reply_cursor,
                    "mac": NODE_MAC,
                    "label": LOCATION_LABEL,
                    "nodes": 4,
                    "phones": phone_count,
                    "pub": phone_count,
                    "events": visible,
                    "av": 0,
                    "acks": [],
                }
            return self.send_json(200, payload)
        if path == "/nodes":
            query = parse_qs(parsed.query)
            try:
                limit = max(1, min(64, int(query.get("limit", ["20"])[0])))
            except ValueError:
                limit = 20
            rows = [dict(row) for row in NODE_ROWS[:limit]]
            return self.send_json(200, {
                "myRssi": -54,
                "total": len(NODE_ROWS),
                "direct": sum(1 for row in NODE_ROWS if row["h"] <= 1),
                "shown": len(rows),
                "full": False,
                "bat": {"s": "ok", "pct": 80, "v": 3.91},
                "nodes": rows,
            })
        # Captive-portal-style routes used by some browsers should reopen the main UI.
        if path in ("/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink", "/success.txt"):
            self.send_response(302)
            self.send_header("Location", "/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(302)
        self.send_header("Location", "/")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        global NEXT_ID
        parsed = urlparse(self.path)
        path = parsed.path
        cid = client_id_from(self)
        if path == "/presence":
            try:
                body = json.loads(self.read_body().decode("utf-8", errors="replace") or "{}")
            except json.JSONDecodeError:
                return self.send_text(400, "Invalid presence JSON.")
            with LOCK:
                peer = touch_peer(cid, str(body.get("name", cid)))
                peer["channel"] = str(body.get("channel", "public"))[:40]
                supplied_groups = body.get("groups", [])
                if not isinstance(supplied_groups, list):
                    supplied_groups = []
                peer["groups"] = list(dict.fromkeys(str(g)[:31] for g in supplied_groups if g and byte_len(str(g)) <= MAX_SECRET_BYTES))[:12]
                peer["seenAt"] = time.time()
                PEERS[cid] = peer
            return self.send_json(200, {"ok": True})
        if path == "/join":
            body = self.read_form()
            secret = body.get("secret", "").strip()
            if not secret or byte_len(secret) > MAX_SECRET_BYTES:
                return self.send_text(400, "Secret must be 1 to 31 bytes.")
            with LOCK:
                peer = touch_peer(cid)
                group_list = peer.get("groups", [])
                if secret not in group_list:
                    group_list.append(secret)
                peer["groups"] = group_list[:12]
                peer["seenAt"] = time.time()
                PEERS[cid] = peer
                count = sum(1 for p in active_peers().values() if secret in p.get("groups", []))
            return self.send_json(200, {"n": count})
        if path == "/send":
            body = self.read_form()
            message = body.get("message", "").strip()
            kind = body.get("type", "")
            secret = body.get("secret", "")
            if not message or byte_len(message) > MAX_TEXT_BYTES:
                return self.send_text(400, "Message must contain 1 to 127 bytes.")
            if kind not in ("public", "private"):
                return self.send_text(400, "Invalid channel.")
            if kind == "private" and (not secret or byte_len(secret) > MAX_SECRET_BYTES):
                return self.send_text(400, "Private group secret must be 1 to 31 bytes.")
            requested_emergency = body.get("emergency") == "1" or message.startswith("[SOS]") or message.startswith("[EMERGENCY BUTTON]")
            safe_status = "CLEAR / SAFE HERE" in message or "SAFE & FULLY ACCOUNTED" in message
            emergency = requested_emergency and not safe_status
            if emergency and kind != "public":
                return self.send_text(400, "Emergency alerts must be sent to the public emergency feed.")
            if kind == "private":
                with LOCK:
                    peer = touch_peer(cid)
                    if secret not in peer.get("groups", []):
                        peer["groups"].append(secret)
            with LOCK:
                touch_peer(cid)
                event_id = NEXT_ID
                NEXT_ID += 1
                ev = {
                    "id": event_id,
                    "text": message,
                    "type": "public" if emergency else kind,
                    "group": "" if emergency or kind == "public" else secret,
                    "sender": cid,
                }
                EVENTS.append(ev)
                if len(EVENTS) > MAX_EVENTS:
                    del EVENTS[:-MAX_EVENTS]
                boot = BOOT_ID
            return self.send_json(200, {"id": event_id, "boot": boot, "emergency": emergency})
        return self.send_text(404, "Unknown demo API endpoint.")


def local_ipv4_addresses() -> list[str]:
    result: set[str] = set()
    try:
        # UDP connect asks the OS which interface it would use; it sends no payload.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("192.0.2.1", 80))
        result.add(sock.getsockname()[0])
        sock.close()
    except OSError:
        pass
    try:
        for address in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not address.startswith("127."):
                result.add(address)
    except OSError:
        pass
    # Filter link-local / loopback and prioritize RFC1918 addresses.
    return sorted((ip for ip in result if not ip.startswith(("127.", "169.254."))), key=lambda ip: (not ip.startswith(("192.168.", "10.", "172.")), ip))


def main():
    global PORT
    if not HTML_FILE.exists():
        print("ERROR: Locus_Exact_UI_Replica.html must be in the same folder as server.py.")
        input("Press Enter to exit...")
        return
    httpd = None
    last_error = None
    # Try a small range so another local development server cannot block the demo.
    for candidate_port in range(8765, 8781):
        try:
            httpd = ThreadingHTTPServer((HOST, candidate_port), Handler)
            PORT = candidate_port
            break
        except OSError as exc:
            last_error = exc
    if httpd is None:
        print(f"Could not start the Locus demo server: {last_error}")
        print("Ports 8765–8780 are all busy. Close another local server and try again.")
        input("Press Enter to exit...")
        return
    ips = local_ipv4_addresses()
    print("=" * 62)
    print(" LOCUS CLASSROOM DEMO — SHARED LAN HOST")
    print("=" * 62)
    print(f"This computer: http://127.0.0.1:{PORT}/")
    if ips:
        print("Give classmates one of these addresses (same Wi-Fi/LAN):")
        for ip in ips:
            print(f"  http://{ip}:{PORT}/")
    else:
        print(f"Find this computer's LAN IPv4 address, then use http://<IP>:{PORT}/")
    print("\nKeep this window open while classmates are using the demo.")
    print("If Windows Firewall asks, allow Python on Private networks only.")
    print("This is simulated browser chat, not actual ESP-NOW traffic.")
    print("Press Ctrl+C to stop the shared demo.\n")
    try:
        import webbrowser
        webbrowser.open(f"http://127.0.0.1:{PORT}/")
    except Exception:
        pass
    try:
        httpd.serve_forever(poll_interval=0.4)
    except KeyboardInterrupt:
        print("\nStopping Locus classroom demo...")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
