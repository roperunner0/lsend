#!/usr/bin/env python3
"""
lsend - a LocalSend (protocol v2.1) client for the M5Stack CardputerZero.

Pure Python standard library: no pip installs needed on Raspberry Pi OS.
Talks to the official LocalSend apps (Android, iOS, Windows, macOS, Linux).

  lsend                      interactive TUI (sized for the 320x170 screen)
  lsend receive [--yes]      headless receiver (good for a systemd service)
  lsend send FILE... --to X  send files (X = alias, IP or IP:port)
  lsend send --text "hi" --to X
  lsend scan                 list LocalSend devices on the network

Keys (TUI): arrows or Fn+; . , /   Enter=pick   Esc/Backspace=back
            t=send text  r=rescan  a=send whole folder  q=quit
"""
import argparse
import curses
import hashlib
import http.client
import http.server
import ipaddress
import json
import mimetypes
import os
import queue
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import parse_qs, urlencode, urlparse

MCAST_GRP = "224.0.0.167"
DEFAULT_PORT = 53317
API = "/api/localsend/v2"
PROTO_VERSION = "2.1"
CONF_DIR = os.path.expanduser("~/.config/lsend")
CONF_FILE = os.path.join(CONF_DIR, "config.json")
CHUNK = 64 * 1024


# --------------------------------------------------------------------------- utils

def human(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def load_conf():
    try:
        with open(CONF_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def ensure_cert():
    """Self-signed cert for HTTPS mode (LocalSend default). None,None if openssl missing."""
    os.makedirs(CONF_DIR, exist_ok=True)
    cert = os.path.join(CONF_DIR, "cert.pem")
    key = os.path.join(CONF_DIR, "key.pem")
    if os.path.exists(cert) and os.path.exists(key):
        return cert, key
    try:
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", key, "-out", cert, "-days", "3650",
             "-subj", "/CN=LocalSend User"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(key, 0o600)
        return cert, key
    except (OSError, subprocess.CalledProcessError):
        return None, None


def safe_relpath(name):
    """Keep sender-supplied names (may include folders) inside the download dir."""
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else "file"


def unique_path(path):
    if not os.path.exists(path):
        return path
    base, ext = os.path.splitext(path)
    i = 1
    while os.path.exists(f"{base} ({i}){ext}"):
        i += 1
    return f"{base} ({i}){ext}"


# --------------------------------------------------------------------------- identity

class Identity:
    def __init__(self, alias, port, https=True, device_type="desktop"):
        self.alias = alias
        self.port = port
        self.device_type = device_type
        self.cert = self.key = None
        self.https = False
        if https:
            self.cert, self.key = ensure_cert()
            self.https = self.cert is not None
        if self.https:
            with open(self.cert) as f:
                der = ssl.PEM_cert_to_DER_cert(f.read())
            self.fingerprint = hashlib.sha256(der).hexdigest()
        else:
            self.fingerprint = uuid.uuid4().hex

    @property
    def protocol(self):
        return "https" if self.https else "http"

    def info(self, announce=None, full=True):
        d = {
            "alias": self.alias,
            "version": PROTO_VERSION,
            "deviceModel": "CardputerZero",
            "deviceType": self.device_type,
            "fingerprint": self.fingerprint,
            "download": False,
        }
        if full:
            d["port"] = self.port
            d["protocol"] = self.protocol
        if announce is not None:
            d["announce"] = announce
            d["announcement"] = announce  # v1 compat
        return d


# --------------------------------------------------------------------------- core

class Core:
    def __init__(self, ident, download_dir, pin=None, auto_accept=False):
        self.ident = ident
        self.download_dir = os.path.abspath(os.path.expanduser(download_dir))
        self.pin = pin
        self.auto_accept = auto_accept
        self.devices = {}          # fingerprint -> device dict
        self.lock = threading.Lock()
        self.events = queue.Queue()
        self.session = None        # incoming transfer session
        self.running = True
        self.prompt_timeout = 60
        self.listener_count = None  # daemon mode: callable -> number of GUI clients
        self._req_seq = 0

    # ---- events
    def emit(self, kind, payload=None):
        self.events.put((kind, payload))

    def log(self, msg):
        self.emit("log", msg)

    # ---- devices
    def add_device(self, info, ip):
        fp = info.get("fingerprint")
        if not fp or fp == self.ident.fingerprint:
            return None
        dev = {
            "alias": info.get("alias") or ip,
            "ip": ip,
            "port": int(info.get("port") or DEFAULT_PORT),
            "protocol": info.get("protocol") or "https",
            "fingerprint": fp,
            "deviceModel": info.get("deviceModel"),
            "deviceType": info.get("deviceType"),
            "seen": time.time(),
        }
        with self.lock:
            new = fp not in self.devices
            self.devices[fp] = dev
        if new:
            self.emit("devices")
        return dev

    def device_list(self):
        with self.lock:
            return sorted(self.devices.values(), key=lambda d: d["alias"].lower())

    # ---- incoming session helpers
    def session_busy(self):
        s = self.session
        if s and time.time() - s["touched"] > 300:
            self.session = None
            return False
        return s is not None

    def ask(self, req):
        """Block the HTTP thread until the user accepts/rejects (or timeout)."""
        if self.auto_accept:
            self.log(f"Auto-accepted {len(req['files'])} file(s) from {req['alias']}")
            return True
        if self.listener_count is not None and self.listener_count() == 0:
            self.log(f"Rejected {req['alias']} (app closed; quick save off)")
            return False
        self._req_seq += 1
        req["id"] = self._req_seq
        req["evt"] = threading.Event()
        req["decision"] = False
        req["deadline"] = time.time() + self.prompt_timeout
        self.emit("incoming", req)
        req["evt"].wait(self.prompt_timeout)
        self.emit("incoming_done", req)
        return req["decision"]


# --------------------------------------------------------------------------- HTTP server

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "lsend"
    timeout = 60

    def log_message(self, *a):
        pass

    @property
    def core(self):
        return self.server.core

    def _send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        if obj is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _reject(self, code, msg):
        self.close_connection = True  # body may be unread
        self._send(code, {"message": msg})

    def _json_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw) if raw else {}
        except ValueError:
            return {}

    def _iter_body(self):
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            while True:
                line = self.rfile.readline().strip()
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    return
                left = size
                while left:
                    buf = self.rfile.read(min(CHUNK, left))
                    if not buf:
                        raise ConnectionError("eof")
                    left -= len(buf)
                    yield buf
                self.rfile.readline()
        else:
            left = int(self.headers.get("Content-Length") or 0)
            while left:
                buf = self.rfile.read(min(CHUNK, left))
                if not buf:
                    raise ConnectionError("eof")
                left -= len(buf)
                yield buf

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in (API + "/info", "/api/localsend/v1/info"):
            return self._send(200, self.core.ident.info(full=False))
        self._reject(404, "not found")

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        p = u.path
        ip = self.client_address[0]
        core = self.core

        if p in (API + "/register", "/api/localsend/v1/register"):
            info = self._json_body()
            core.add_device(info, ip)
            return self._send(200, core.ident.info(full=False))

        if p == API + "/prepare-upload":
            if core.pin and q.get("pin") != core.pin:
                return self._reject(401, "PIN required")
            if core.session_busy():
                return self._reject(409, "Blocked by another session")
            data = self._json_body()
            info = data.get("info") or {}
            files = data.get("files") or {}
            if info.get("port"):
                core.add_device(info, ip)
            alias = info.get("alias") or ip

            # Text message: single text/plain file with preview -> show it, 204
            if len(files) == 1:
                f = next(iter(files.values()))
                if (f.get("fileType") or "").startswith("text/") and f.get("preview") is not None:
                    core.emit("text", (alias, f["preview"]))
                    return self._send(204)

            req = {"alias": alias, "files": list(files.values())}
            if not core.ask(req):
                return self._reject(403, "Rejected")
            sid = uuid.uuid4().hex
            sess = {"id": sid, "ip": ip, "alias": alias, "files": {}, "touched": time.time()}
            tokens = {}
            for fid, meta in files.items():
                tok = uuid.uuid4().hex
                sess["files"][fid] = {"meta": meta, "token": tok, "done": False}
                tokens[fid] = tok
            core.session = sess
            core.log(f"Receiving {len(files)} file(s) from {alias}")
            return self._send(200, {"sessionId": sid, "files": tokens})

        if p == API + "/upload":
            s = core.session
            if not s or q.get("sessionId") != s["id"] or ip != s["ip"]:
                return self._reject(403, "Invalid session")
            f = s["files"].get(q.get("fileId"))
            if not f or q.get("token") != f["token"]:
                return self._reject(403, "Invalid token")
            meta = f["meta"]
            name = meta.get("fileName") or "file"
            dest = os.path.realpath(os.path.join(core.download_dir, safe_relpath(name)))
            if not dest.startswith(core.download_dir + os.sep):
                return self._reject(403, "Bad file name")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            dest = unique_path(dest)
            total = int(meta.get("size") or self.headers.get("Content-Length") or 0)
            got = 0
            last = 0
            try:
                with open(dest, "wb") as out:
                    for buf in self._iter_body():
                        out.write(buf)
                        got += len(buf)
                        s["touched"] = time.time()
                        if time.time() - last > 0.25:
                            last = time.time()
                            core.emit("progress", ("Recv " + os.path.basename(name), got, total))
            except Exception:
                try:
                    os.remove(dest)
                except OSError:
                    pass
                core.emit("progress_end", (False, f"Receive failed: {name}"))
                self.close_connection = True
                return self._send(500, {"message": "write failed"})
            f["done"] = True
            core.emit("progress", ("Recv " + os.path.basename(name), got, total))
            if all(x["done"] for x in s["files"].values()):
                core.session = None
                core.emit("progress_end", (True, f"Saved {len(s['files'])} file(s) to {core.download_dir}"))
            return self._send(200)

        if p == API + "/cancel":
            s = core.session
            if s and q.get("sessionId") == s["id"]:
                core.session = None
                core.emit("progress_end", (False, f"{s['alias']} cancelled"))
            return self._send(200)

        self._reject(404, "not found")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def finish_request(self, request, client_address):
        if isinstance(request, ssl.SSLSocket):
            request.settimeout(30)
            request.do_handshake()  # in the worker thread, not the accept loop
        super().finish_request(request, client_address)

    def handle_error(self, request, client_address):
        pass  # never spray tracebacks over the curses screen


def start_server(core):
    srv = Server(("0.0.0.0", core.ident.port), Handler)
    srv.core = core
    if core.ident.https:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(core.ident.cert, core.ident.key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True,
                                     do_handshake_on_connect=False)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# --------------------------------------------------------------------------- HTTP client

def _conn(dev, timeout=10):
    if dev["protocol"] == "https":
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # LocalSend uses self-signed certs
        return http.client.HTTPSConnection(dev["ip"], dev["port"], timeout=timeout, context=ctx)
    return http.client.HTTPConnection(dev["ip"], dev["port"], timeout=timeout)


def request_json(dev, method, path, obj=None, timeout=10):
    c = _conn(dev, timeout)
    try:
        body = json.dumps(obj).encode() if obj is not None else None
        hdr = {"Content-Type": "application/json"} if body is not None else {}
        c.request(method, path, body=body, headers=hdr)
        r = c.getresponse()
        raw = r.read()
        try:
            data = json.loads(raw) if raw else None
        except ValueError:
            data = None
        return r.status, data
    finally:
        c.close()


def probe(core, ip, port=DEFAULT_PORT, timeout=1.0):
    """Register with a host directly (HTTP discovery). Tries https then http."""
    for proto in ("https", "http"):
        dev = {"ip": ip, "port": port, "protocol": proto}
        try:
            st, data = request_json(dev, "POST", API + "/register", core.ident.info(), timeout)
        except Exception:
            continue
        if st == 200 and isinstance(data, dict):
            data.setdefault("port", port)
            data.setdefault("protocol", proto)
            data["port"], data["protocol"] = port, proto
            return core.add_device(data, ip)
    return None


# --------------------------------------------------------------------------- multicast

def announce(core):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        s.sendto(json.dumps(core.ident.info(announce=True)).encode(), (MCAST_GRP, DEFAULT_PORT))
    except OSError as e:
        core.log(f"Announce failed: {e}")
    finally:
        s.close()


def _answer_announce(core, info, ip):
    dev = core.add_device(info, ip)
    if not dev:
        return
    try:
        st, data = request_json(dev, "POST", API + "/register", core.ident.info(), timeout=3)
        if st == 200:
            return
    except Exception:
        pass
    # fallback: answer over multicast
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        s.sendto(json.dumps(core.ident.info(announce=False)).encode(), (MCAST_GRP, DEFAULT_PORT))
    except OSError:
        pass
    finally:
        s.close()


def multicast_listener(core):
    while core.running:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                try:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                except OSError:
                    pass
            s.bind(("", DEFAULT_PORT))
            mreq = struct.pack("4s4s", socket.inet_aton(MCAST_GRP), socket.inet_aton("0.0.0.0"))
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            s.settimeout(1.0)
        except OSError:
            time.sleep(3)  # Wi-Fi not up yet; retry
            continue
        while core.running:
            try:
                data, addr = s.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                info = json.loads(data)
            except ValueError:
                continue
            if info.get("fingerprint") == core.ident.fingerprint:
                continue
            if info.get("announce") or info.get("announcement"):
                threading.Thread(target=_answer_announce, args=(core, info, addr[0]),
                                 daemon=True).start()
            else:
                core.add_device(info, addr[0])
        s.close()


def http_scan(core):
    ip = local_ip()
    if ip.startswith("127."):
        core.log("No network")
        return
    net = ipaddress.ip_network(ip + "/24", strict=False)
    hosts = [str(h) for h in net.hosts() if str(h) != ip]
    with ThreadPoolExecutor(max_workers=48) as ex:
        list(ex.map(lambda h: probe(core, h, timeout=0.7), hosts))


def discover(core, scan=True):
    announce(core)
    if scan:
        threading.Thread(target=lambda: (http_scan(core), core.log("Scan done")),
                         daemon=True).start()


# --------------------------------------------------------------------------- sending

def build_file_list(paths):
    """Return [(fid, abs_path, rel_name, size)], walking folders."""
    out = []
    for p in paths:
        p = os.path.abspath(os.path.expanduser(p))
        if os.path.isdir(p):
            base = os.path.dirname(p.rstrip("/"))
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for fn in files:
                    full = os.path.join(root, fn)
                    out.append((uuid.uuid4().hex, full, os.path.relpath(full, base), os.path.getsize(full)))
        elif os.path.isfile(p):
            out.append((uuid.uuid4().hex, p, os.path.basename(p), os.path.getsize(p)))
    return out


def send(core, dev, paths=None, text=None, pin=None):
    """Returns (ok, message). Emits progress events."""
    files = {}
    plan = []
    if text is not None:
        fid = uuid.uuid4().hex
        files[fid] = {"id": fid, "fileName": fid + ".txt", "size": len(text.encode()),
                      "fileType": "text/plain", "preview": text}
    else:
        plan = build_file_list(paths or [])
        if not plan:
            return False, "Nothing to send"
        for fid, full, rel, size in plan:
            files[fid] = {"id": fid, "fileName": rel, "size": size,
                          "fileType": mimetypes.guess_type(full)[0] or "application/octet-stream"}

    path = API + "/prepare-upload" + ("?" + urlencode({"pin": pin}) if pin else "")
    core.emit("progress", (f"Waiting for {dev['alias']}...", 0, 0))
    try:
        st, resp = request_json(dev, "POST", path, {"info": core.ident.info(), "files": files},
                                timeout=120)
    except Exception as e:
        return False, f"Connect failed: {e}"
    if st == 204:
        return True, "Delivered" if text is not None else "Nothing needed"
    if st == 401:
        return False, "PIN required/wrong"
    if st == 403:
        return False, "Rejected"
    if st == 409:
        return False, "Receiver busy"
    if st != 200 or not isinstance(resp, dict):
        return False, f"Error {st}"

    sid = resp.get("sessionId")
    tokens = resp.get("files") or {}
    by_id = {fid: (full, rel, size) for fid, full, rel, size in plan}
    total = sum(by_id[f][2] for f in tokens if f in by_id)
    sent_all = 0
    t0 = time.time()
    try:
        for fid, tok in tokens.items():
            if fid not in by_id:
                continue
            full, rel, size = by_id[fid]
            c = _conn(dev, timeout=60)
            q = urlencode({"sessionId": sid, "fileId": fid, "token": tok})
            c.putrequest("POST", f"{API}/upload?{q}")
            c.putheader("Content-Type", "application/octet-stream")
            c.putheader("Content-Length", str(size))
            c.endheaders()
            last = 0
            with open(full, "rb") as fh:
                while True:
                    buf = fh.read(CHUNK)
                    if not buf:
                        break
                    c.send(buf)
                    sent_all += len(buf)
                    if time.time() - last > 0.25:
                        last = time.time()
                        core.emit("progress", ("Send " + os.path.basename(rel), sent_all, total))
            r = c.getresponse()
            r.read()
            c.close()
            if r.status != 200:
                raise RuntimeError(f"upload {r.status}")
    except Exception as e:
        try:
            request_json(dev, "POST", f"{API}/cancel?" + urlencode({"sessionId": sid}), timeout=3)
        except Exception:
            pass
        return False, f"Transfer failed: {e}"
    dt = max(time.time() - t0, 0.001)
    return True, f"Sent {len(tokens)} file(s), {human(total)} @ {human(total / dt)}/s"


# --------------------------------------------------------------------------- TUI

UP = {curses.KEY_UP, ord(";"), ord("k")}
DOWN = {curses.KEY_DOWN, ord("."), ord("j")}
BACK = {curses.KEY_LEFT, ord(","), curses.KEY_BACKSPACE, 127, 8, 27}
ENTER = {curses.KEY_RIGHT, ord("/"), 10, 13, curses.KEY_ENTER}


class UI:
    def __init__(self, scr, core):
        self.scr = scr
        self.core = core
        self.mode = "devices"
        self.sel = 0
        self.status = "Scanning..."
        self.prog = None
        self.incoming = None
        self.message = None
        self.target = None
        self.cwd = os.path.expanduser("~")
        self.fsel = 0
        self.show_hidden = False
        self.text = ""
        self.busy = False
        self.ip = local_ip()

    # ---- drawing helpers
    def put(self, y, x, s, attr=0):
        h, w = self.scr.getmaxyx()
        if y < 0 or y >= h or x >= w:
            return
        s = s[: max(0, w - x - (1 if y == h - 1 else 0))]
        try:
            self.scr.addstr(y, x, s, attr)
        except curses.error:
            pass

    def bar(self, y, s):
        _, w = self.scr.getmaxyx()
        self.put(y, 0, s.ljust(w), curses.A_REVERSE)

    def listing(self, items, sel, top_y, height):
        start = max(0, min(sel - height // 2, len(items) - height))
        for i, label in enumerate(items[start:start + height]):
            idx = start + i
            attr = curses.A_REVERSE if idx == sel else 0
            self.put(top_y + i, 0, ("> " if idx == sel else "  ") + label, attr)

    def status_line(self, y, w):
        if self.prog:
            label, done, total = self.prog
            if total:
                pct = done / total
                bw = max(4, w - 6)
                fill = int(bw * pct)
                self.put(y - 1, 0, label[:w])
                self.put(y, 0, "[" + "#" * fill + "-" * (bw - fill) + "]" + f"{int(pct * 100):3d}%")
                return
            self.put(y, 0, label[:w])
            return
        self.put(y, 0, self.status[:w])

    def draw(self):
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        ident = self.core.ident
        body_h = max(1, h - 4)

        if self.mode == "devices":
            self.bar(0, f" LocalSend {ident.alias} {self.ip}")
            devs = self.core.device_list()
            self.sel = min(self.sel, max(0, len(devs) - 1))
            if devs:
                self.listing([f"{d['alias'][:w - 18]:<{max(1, w - 18)}}{d['ip']}" for d in devs],
                             self.sel, 1, body_h)
            else:
                self.put(2, 1, "No devices yet.")
                self.put(3, 1, "Open LocalSend on your phone,")
                self.put(4, 1, "then press r to rescan.")
            self.status_line(h - 2, w)
            self.bar(h - 1, " Ent:file t:text r:scan q:quit")

        elif self.mode == "files":
            self.bar(0, f" To {self.target['alias']}: {self.cwd}"[-w:])
            entries = self.dir_entries()
            self.fsel = min(self.fsel, max(0, len(entries) - 1))
            labels = []
            for name, is_dir, size in entries:
                if is_dir:
                    labels.append(name + "/")
                else:
                    sz = human(size)
                    labels.append(f"{name[:max(1, w - 11)]:<{max(1, w - 11)}}{sz:>8}")
            if labels:
                self.listing(labels, self.fsel, 1, body_h)
            else:
                self.put(2, 1, "(empty)")
            self.status_line(h - 2, w)
            self.bar(h - 1, " Ent:send/open a:folder Esc:back")

        elif self.mode == "text":
            self.bar(0, f" Text to {self.target['alias']}")
            lines = [self.text[i:i + w - 1] for i in range(0, len(self.text), w - 1)] or [""]
            for i, ln in enumerate(lines[-body_h:]):
                self.put(1 + i, 0, ln)
            self.put(1 + min(len(lines), body_h) - 1, len(lines[-1]), "_")
            self.status_line(h - 2, w)
            self.bar(h - 1, " Enter:send Esc:cancel")

        if self.incoming:
            req = self.incoming
            files = req["files"]
            total = sum(int(f.get("size") or 0) for f in files)
            left = max(0, int(req["deadline"] - time.time()))
            self.scr.erase()
            self.bar(0, f" Incoming from {req['alias']}")
            self.put(1, 0, f"{len(files)} file(s), {human(total)}")
            for i, f in enumerate(files[: max(0, h - 4)]):
                self.put(2 + i, 1, (f.get("fileName") or "?")[: w - 2])
            self.bar(h - 1, f" y:accept n:reject ({left}s)")

        elif self.message:
            alias, text = self.message
            self.scr.erase()
            self.bar(0, f" Message from {alias}")
            lines = []
            for para in text.splitlines() or [""]:
                lines += [para[i:i + w] for i in range(0, max(1, len(para)), w)]
            for i, ln in enumerate(lines[: h - 2]):
                self.put(1 + i, 0, ln)
            self.bar(h - 1, " any key: close")

        self.scr.refresh()

    # ---- file browser
    def dir_entries(self):
        try:
            names = os.listdir(self.cwd)
        except OSError:
            return []
        out = []
        for n in names:
            if n.startswith(".") and not self.show_hidden:
                continue
            full = os.path.join(self.cwd, n)
            try:
                is_dir = os.path.isdir(full)
                size = 0 if is_dir else os.path.getsize(full)
            except OSError:
                continue
            out.append((n, is_dir, size))
        out.sort(key=lambda e: (not e[1], e[0].lower()))
        return out

    # ---- actions
    def start_send(self, paths=None, text=None):
        if self.busy:
            self.status = "Busy, wait..."
            return
        self.busy = True
        dev = self.target

        def work():
            ok, msg = send(self.core, dev, paths=paths, text=text, pin=self.core.send_pin)
            self.core.emit("progress_end", (ok, msg))
            self.busy = False

        threading.Thread(target=work, daemon=True).start()
        self.mode = "devices"

    def handle_events(self):
        while True:
            try:
                kind, p = self.core.events.get_nowait()
            except queue.Empty:
                return
            if kind == "log":
                self.status = p
            elif kind == "devices":
                if not self.prog:
                    self.status = f"{len(self.core.device_list())} device(s)"
            elif kind == "incoming":
                self.incoming = p
                curses.beep()
            elif kind == "incoming_done":
                if self.incoming is p:
                    self.incoming = None
            elif kind == "text":
                self.message = p
                curses.beep()
            elif kind == "progress":
                self.prog = p
            elif kind == "progress_end":
                self.prog = None
                self.status = p[1] if p[0] else "X " + p[1]

    def key(self, k):
        if self.incoming:
            if k in (ord("y"), ord("Y")):
                self.incoming["decision"] = True
                self.incoming["evt"].set()
                self.incoming = None
            elif k in (ord("n"), ord("N"), 27):
                self.incoming["decision"] = False
                self.incoming["evt"].set()
                self.incoming = None
            return True
        if self.message:
            self.message = None
            return True

        if self.mode == "devices":
            devs = self.core.device_list()
            if k in (ord("q"), ord("Q")):
                return False
            if k in UP:
                self.sel = max(0, self.sel - 1)
            elif k in DOWN:
                self.sel = min(len(devs) - 1, self.sel + 1) if devs else 0
            elif k in (ord("r"), ord("R")):
                self.status = "Scanning..."
                self.ip = local_ip()
                discover(self.core)
            elif devs and k in ENTER:
                self.target = devs[self.sel]
                self.mode, self.fsel = "files", 0
            elif devs and k in (ord("t"), ord("T")):
                self.target = devs[self.sel]
                self.mode, self.text = "text", ""

        elif self.mode == "files":
            entries = self.dir_entries()
            if k in UP:
                self.fsel = max(0, self.fsel - 1)
            elif k in DOWN:
                self.fsel = min(len(entries) - 1, self.fsel + 1) if entries else 0
            elif k == 27:
                self.mode = "devices"
            elif k in BACK:
                parent = os.path.dirname(self.cwd.rstrip("/")) or "/"
                child = os.path.basename(self.cwd.rstrip("/"))
                self.cwd = parent
                names = [e[0] for e in self.dir_entries()]
                self.fsel = names.index(child) if child in names else 0
            elif k in ENTER and entries:
                name, is_dir, _ = entries[self.fsel]
                full = os.path.join(self.cwd, name)
                if is_dir:
                    self.cwd, self.fsel = full, 0
                else:
                    self.start_send(paths=[full])
            elif k in (ord("a"), ord("A")):
                self.start_send(paths=[self.cwd])
            elif k == ord("H"):
                self.show_hidden = not self.show_hidden

        elif self.mode == "text":
            if k == 27:
                self.mode = "devices"
            elif k in (10, 13, curses.KEY_ENTER):
                if self.text:
                    self.start_send(text=self.text)
            elif k in (curses.KEY_BACKSPACE, 127, 8):
                self.text = self.text[:-1]
            elif 32 <= k < 127:
                self.text += chr(k)
        return True

    def run(self):
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        curses.set_escdelay(25) if hasattr(curses, "set_escdelay") else None
        self.scr.keypad(True)
        self.scr.timeout(250)
        while True:
            self.handle_events()
            if self.incoming and time.time() > self.incoming["deadline"]:
                self.incoming = None
            self.draw()
            k = self.scr.getch()
            if k == -1 or k == curses.KEY_RESIZE:
                continue
            if not self.key(k):
                break


# --------------------------------------------------------------------------- CLI

def make_core(args, auto_accept=False):
    conf = load_conf()
    alias = args.alias or conf.get("alias") or f"CardputerZero-{socket.gethostname()}"
    ident = Identity(alias, args.port, https=not args.http,
                     device_type=conf.get("deviceType", "desktop"))
    dl = args.dir or conf.get("downloadDir") or "~/Downloads/LocalSend"
    core = Core(ident, dl, pin=args.pin or conf.get("pin"), auto_accept=auto_accept)
    core.send_pin = getattr(args, "send_pin", None)
    os.makedirs(core.download_dir, exist_ok=True)
    start_server(core)
    threading.Thread(target=multicast_listener, args=(core,), daemon=True).start()
    return core


def find_target(core, to, wait=4.0):
    host, _, port = to.partition(":")
    try:
        ipaddress.ip_address(host)
        dev = probe(core, host, int(port or DEFAULT_PORT), timeout=3)
        return dev
    except ValueError:
        pass
    announce(core)
    end = time.time() + wait
    while time.time() < end:
        for d in core.device_list():
            if to.lower() in d["alias"].lower():
                return d
        time.sleep(0.2)
    http_scan(core)
    for d in core.device_list():
        if to.lower() in d["alias"].lower():
            return d
    return None


def cli_progress(core, stop):
    while not stop.is_set():
        try:
            kind, p = core.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if kind == "progress":
            label, done, total = p
            line = f"{label} {done * 100 // total}% {human(done)}/{human(total)}" if total else label
            print("\r" + line.ljust(60), end="", flush=True)
        elif kind == "progress_end":
            print("\r" + p[1].ljust(60), flush=True)
        elif kind in ("log",):
            print(p, flush=True)


def cmd_receive(args):
    core = make_core(args, auto_accept=args.yes)
    print(f"{core.ident.alias} listening on {local_ip()}:{core.ident.port} ({core.ident.protocol})")
    print(f"Saving to {core.download_dir}" + (" [auto-accept]" if args.yes else ""))
    announce(core)

    def reprint():
        while True:
            kind, p = core.events.get()
            if kind == "incoming":
                total = sum(int(f.get("size") or 0) for f in p["files"])
                ans = input(f"\n{p['alias']} wants to send {len(p['files'])} file(s), "
                            f"{human(total)}. Accept? [y/N] ")
                p["decision"] = ans.strip().lower().startswith("y")
                p["evt"].set()
            elif kind == "text":
                print(f"\n[{p[0]}] {p[1]}", flush=True)
            elif kind == "progress":
                label, done, total = p
                if total:
                    print("\r" + f"{label} {done * 100 // total}%".ljust(60), end="", flush=True)
            elif kind == "progress_end":
                print("\r" + p[1].ljust(60), flush=True)
            elif kind == "log":
                print("\r" + str(p).ljust(60), flush=True)

    threading.Thread(target=reprint, daemon=True).start()
    try:
        while True:
            time.sleep(300)
            announce(core)  # stay visible
    except KeyboardInterrupt:
        pass


def cmd_send(args):
    if not args.files and args.text is None:
        sys.exit("Give files or --text")
    core = make_core(args)
    dev = find_target(core, args.to)
    if not dev:
        sys.exit(f"Device '{args.to}' not found")
    print(f"-> {dev['alias']} ({dev['ip']})")
    stop = threading.Event()
    t = threading.Thread(target=cli_progress, args=(core, stop), daemon=True)
    t.start()
    ok, msg = send(core, dev, paths=args.files, text=args.text, pin=args.send_pin)
    time.sleep(0.3)
    stop.set()
    print(("\nOK: " if ok else "\nFailed: ") + msg)
    sys.exit(0 if ok else 1)


def cmd_scan(args):
    core = make_core(args)
    announce(core)
    time.sleep(2)
    http_scan(core)
    for d in core.device_list():
        print(f"{d['alias']:<28} {d['ip']:<15} {d['protocol']}:{d['port']} {d.get('deviceModel') or ''}")
    if not core.device_list():
        print("No devices found")


# --------------------------------------------------------------------------- daemon / GUI control
#
# `lsend daemon` runs in the background and exposes a Unix socket that the
# CardputerZero GUI app talks to. Protocol: one JSON object per line.
#
#   GUI -> daemon: {"cmd":"state"} {"cmd":"scan"} {"cmd":"answer","id":N,"accept":true}
#                  {"cmd":"send","to":"<fingerprint>","paths":["/abs/file"]}
#                  {"cmd":"send","to":"<fingerprint>","text":"hello"}
#                  {"cmd":"set","auto_accept":true} {"cmd":"shutdown"}
#   daemon -> GUI: {"type":"state",...} {"type":"devices","devices":[...]}
#                  {"type":"incoming","id":N,"alias":..,"files":[{"name","size"}],"timeout":S}
#                  {"type":"incoming_done","id":N} {"type":"text","alias":..,"text":..}
#                  {"type":"progress","label":..,"done":N,"total":N}
#                  {"type":"progress_end","ok":bool,"msg":..} {"type":"log","msg":..}

def default_socket():
    return os.path.join(CONF_DIR, "ctl.sock")


def public_dev(d):
    return {k: d.get(k) for k in ("alias", "ip", "port", "protocol", "fingerprint",
                                  "deviceModel", "deviceType")}


class Control:
    def __init__(self, core, path):
        self.core = core
        self.path = path
        self.clients = []
        self.lock = threading.Lock()
        self.pending = {}
        self.sending = False

    def state(self):
        c = self.core
        return {"type": "state", "alias": c.ident.alias, "ip": local_ip(),
                "port": c.ident.port, "protocol": c.ident.protocol,
                "download_dir": c.download_dir, "auto_accept": c.auto_accept,
                "busy": self.sending,
                "devices": [public_dev(d) for d in c.device_list()]}

    def start(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        if os.path.exists(self.path):
            probe_s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                probe_s.connect(self.path)
                probe_s.close()
                print("lsend daemon already running")
                sys.exit(0)
            except OSError:
                os.unlink(self.path)  # stale socket
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        old = os.umask(0o077)
        srv.bind(self.path)
        os.umask(old)
        srv.listen(4)
        self.core.listener_count = lambda: len(self.clients)
        threading.Thread(target=self._accept, args=(srv,), daemon=True).start()
        threading.Thread(target=self._dispatch, daemon=True).start()

    def _send(self, conn, obj):
        conn.sendall((json.dumps(obj) + "\n").encode())

    def broadcast(self, obj):
        with self.lock:
            for conn in list(self.clients):
                try:
                    self._send(conn, obj)
                except OSError:
                    self.clients.remove(conn)

    def _accept(self, srv):
        while True:
            conn, _ = srv.accept()
            with self.lock:
                self.clients.append(conn)
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        try:
            with self.lock:
                self._send(conn, self.state())
            buf = b""
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        msg = json.loads(line)
                    except ValueError:
                        continue
                    self._handle(conn, msg)
        except OSError:
            pass
        finally:
            with self.lock:
                if conn in self.clients:
                    self.clients.remove(conn)
            conn.close()

    def _handle(self, conn, msg):
        cmd = msg.get("cmd")
        core = self.core
        if cmd == "state":
            with self.lock:
                self._send(conn, self.state())
        elif cmd == "scan":
            discover(core)
            self.broadcast({"type": "log", "msg": "Scanning..."})
        elif cmd == "answer":
            req = self.pending.pop(msg.get("id"), None)
            if req:
                req["decision"] = bool(msg.get("accept"))
                req["evt"].set()
        elif cmd == "set":
            if "auto_accept" in msg:
                core.auto_accept = bool(msg["auto_accept"])
            self.broadcast(self.state())
        elif cmd == "send":
            dev = next((d for d in core.device_list() if d["fingerprint"] == msg.get("to")), None)
            if not dev:
                self.broadcast({"type": "progress_end", "ok": False, "msg": "Device gone, rescan"})
            elif self.sending:
                self.broadcast({"type": "progress_end", "ok": False, "msg": "Already sending"})
            else:
                self.sending = True

                def work():
                    try:
                        ok, m = send(core, dev, paths=msg.get("paths"), text=msg.get("text"),
                                     pin=msg.get("pin"))
                    except Exception as e:
                        ok, m = False, str(e)
                    self.sending = False
                    core.emit("progress_end", (ok, m))

                threading.Thread(target=work, daemon=True).start()
        elif cmd == "shutdown":
            os._exit(0)

    def _dispatch(self):
        core = self.core
        while True:
            kind, p = core.events.get()
            if kind == "log":
                self.broadcast({"type": "log", "msg": p})
            elif kind == "devices":
                self.broadcast({"type": "devices",
                                "devices": [public_dev(d) for d in core.device_list()]})
            elif kind == "incoming":
                self.pending[p["id"]] = p
                self.broadcast({"type": "incoming", "id": p["id"], "alias": p["alias"],
                                "files": [{"name": f.get("fileName") or "?",
                                           "size": int(f.get("size") or 0)} for f in p["files"]],
                                "timeout": int(p["deadline"] - time.time())})
            elif kind == "incoming_done":
                self.pending.pop(p.get("id"), None)
                self.broadcast({"type": "incoming_done", "id": p.get("id")})
            elif kind == "text":
                self.broadcast({"type": "text", "alias": p[0], "text": p[1]})
            elif kind == "progress":
                self.broadcast({"type": "progress", "label": p[0], "done": p[1], "total": p[2]})
            elif kind == "progress_end":
                self.broadcast({"type": "progress_end", "ok": bool(p[0]), "msg": p[1]})


def cmd_daemon(args):
    path = args.socket or default_socket()
    core = make_core(args, auto_accept=args.yes)
    ctl = Control(core, path)
    ctl.start()
    print(f"lsend daemon: {core.ident.alias} {local_ip()}:{core.ident.port} "
          f"({core.ident.protocol}) control={path}", flush=True)
    discover(core)
    while True:
        time.sleep(120)
        announce(core)  # stay visible


def cmd_tui(args):
    core = make_core(args)
    discover(core)
    curses.wrapper(lambda scr: UI(scr, core).run())
    core.running = False


def main():
    ap = argparse.ArgumentParser(prog="lsend", description="LocalSend for CardputerZero")
    ap.add_argument("--alias", help="name shown to other devices")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--http", action="store_true", help="plain HTTP instead of HTTPS")
    ap.add_argument("--pin", help="require this PIN for incoming transfers")
    ap.add_argument("--send-pin", help="PIN to use when the receiver requires one")
    ap.add_argument("--dir", help="download folder (default ~/Downloads/LocalSend)")
    sub = ap.add_subparsers(dest="cmd")
    r = sub.add_parser("receive", help="headless receiver")
    r.add_argument("-y", "--yes", action="store_true", help="auto-accept everything")
    s = sub.add_parser("send", help="send files or text")
    s.add_argument("files", nargs="*")
    s.add_argument("--to", required=True, help="alias (substring), IP or IP:port")
    s.add_argument("--text")
    sub.add_parser("scan", help="list devices")
    d = sub.add_parser("daemon", help="background service for the GUI app")
    d.add_argument("-y", "--yes", action="store_true", help="quick save: auto-accept")
    d.add_argument("--socket", help=f"control socket (default {default_socket()})")
    args = ap.parse_args()
    {"receive": cmd_receive, "send": cmd_send, "scan": cmd_scan,
     "daemon": cmd_daemon}.get(args.cmd, cmd_tui)(args)


if __name__ == "__main__":
    main()
