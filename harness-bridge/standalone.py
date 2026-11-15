#!/usr/bin/env python3
"""Cardputer <-> DeepSeek Harness bridge, standalone ACP child process.

No DSH plugin is installed.  The bridge owns:
  * a `dsh --profile cardputer` ACP child on stdio (NDJSON JSON-RPC);
  * a LAN HTTP API for the Cardputer;
  * optional Vosk speech-to-text for 16 kHz mono WAV;
  * read-only history from the persisted session JSONL (zstd) for the UI.
"""
from __future__ import annotations

import atexit
import json
import os
import queue
import subprocess
import sys
import threading
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

try:
    import zstandard as zstd
except Exception:
    zstd = None

ROOT = Path(r"Y:\2026\cardputer")
DSH = Path(r"C:\Users\dloach\dsh-standalone")
NODE = Path(r"C:\nvm4w\nodejs\node.exe")
DSH_BIN = DSH / r"node_modules\@deepseek-ai\dsh\lib\bin.js"
TOKEN = os.environ.get("CARDPUTER_TOKEN", "cardputer-dev")
WORKSPACE = os.environ.get(
    "CARDPUTER_WORKSPACE",
    r"\\192.168.0.222\project\2026\cardputer",
)
PORT = int(os.environ.get("CARDPUTER_PORT", "9100"))
STT_PY = Path(r"Y:\2026\cardputer\harness-bridge\.venv\Scripts\python.exe")
STT_SCRIPT = ROOT / "harness-bridge" / "plugin" / "stt.py"
STT_MODEL = ROOT / "harness-bridge" / "models" / "vosk-model-small-cn-0.22"
DSH_HOME = Path(os.environ.get("DSH_HOME", r"C:\Users\dloach\.dsh"))
WORKSPACE_STORE = DSH_HOME / "storages" / "workspace.json"


class Acp:
    def __init__(self):
        self.proc = subprocess.Popen(
            [str(NODE), str(DSH_BIN), "--profile", "cardputer"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self.next_id = 1
        self.pending: dict[int, queue.Queue] = {}
        self.lock = threading.Lock()
        self.session_cache: dict[str, list[dict]] = {}
        self.active: set[str] = set()
        self.session_cwd: dict[str, str] = {}
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()
        atexit.register(self.proc.terminate)
        self.request("initialize", {
            "protocolVersion": 1,
            "clientCapabilities": {},
        }, timeout=10)

    def _reader(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except Exception:
                continue
            if "id" in msg and "method" not in msg:
                q = self.pending.pop(msg["id"], None)
                if q:
                    q.put(msg)
            elif msg.get("method") == "session/update":
                params = msg.get("params", {})
                sid = params.get("sessionId")
                update = params.get("update", {})
                if sid:
                    self.session_cache.setdefault(sid, [])
                    self._apply_update(sid, update)
            elif msg.get("method") == "session/request_permission":
                self._permission(msg)

    def _apply_update(self, sid: str, update: dict):
        kind = update.get("sessionUpdate")
        text = ""
        if kind in ("agent_message_chunk", "user_message_chunk"):
            content = update.get("content", {})
            if isinstance(content, dict):
                text = content.get("text", "")
            elif isinstance(content, list):
                text = "".join(x.get("text", "") for x in content if isinstance(x, dict))
            if text:
                role = "assistant" if kind == "agent_message_chunk" else "user"
                self.session_cache[sid].append({"role": role, "text": text})

    def _permission(self, msg):
        params = msg.get("params", {})
        options = params.get("options", [])
        choice = None
        for opt in options:
            if "allow" in str(opt.get("kind", "")).lower():
                choice = opt.get("optionId") or opt.get("id")
                break
        if choice is None and options:
            choice = options[0].get("optionId") or options[0].get("id")
        self.send({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "result": {"outcome": {"outcome": "selected", "optionId": choice}},
        })

    def send(self, obj):
        with self.lock:
            self.proc.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()

    def request(self, method, params=None, timeout=30):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            q = queue.Queue()
            self.pending[rid] = q
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
        msg = q.get(timeout=timeout)
        if "error" in msg:
            raise RuntimeError(msg["error"])
        return msg.get("result", {})

    def list_sessions(self):
        sessions = self.request("session/list", {}, timeout=30).get("sessions", [])
        for s in sessions:
            sid, cwd = s.get("sessionId"), s.get("cwd")
            if sid and cwd:
                self.session_cwd[sid] = cwd
        return sessions

    def new_session(self):
        result = self.request("session/new", {"cwd": WORKSPACE, "mcpServers": []}, timeout=60)
        sid = result.get("sessionId", "")
        if sid:
            self.active.add(sid)
            self.session_cwd[sid] = WORKSPACE
            register_session(sid)
        return result

    def resume(self, sid):
        cwd = self.session_cwd.get(sid, WORKSPACE)
        result = self.request("session/resume", {"sessionId": sid, "cwd": cwd}, timeout=60)
        self.active.add(sid)
        return result

    def close(self, sid):
        self.request("session/close", {"sessionId": sid}, timeout=60)
        self.active.discard(sid)

    def prompt(self, sid, text):
        # Resume is synchronous so a desktop-held Session surfaces as an HTTP
        # error instead of a silently accepted prompt with no output.
        if sid not in self.active:
            self.resume(sid)
        # Do not hold the HTTP request until the whole agent turn finishes.
        def run():
            try:
                self.request("session/prompt", {
                    "sessionId": sid,
                    "prompt": [{"type": "text", "text": text}],
                }, timeout=600)
            except Exception as e:
                sys.stderr.write(f"prompt failed: {e}\n")
            finally:
                # Release the Session after the turn so the desktop Web UI can
                # open the same persisted Session and continue the conversation.
                try:
                    self.close(sid)
                except Exception as e:
                    sys.stderr.write(f"close failed: {e}\n")
        threading.Thread(target=run, daemon=True).start()


ACP: Acp | None = None
ACP_LOCK = threading.Lock()


def acp() -> Acp:
    global ACP
    with ACP_LOCK:
        if ACP is None or ACP.proc.poll() is not None:
            ACP = Acp()
        return ACP


def find_session_log(session_id: str) -> Path | None:
    root = DSH_HOME / "sessions"
    for p in root.rglob("*.zstd"):
        if session_id in str(p.parent):
            return p
    return None


def register_session(session_id: str):
    """Make an ACP-created Session visible in the desktop cardputer workspace."""
    if not session_id or not WORKSPACE_STORE.exists():
        return
    try:
        with open(WORKSPACE_STORE, "r", encoding="utf-8") as f:
            data = json.load(f)
        target = None
        for ws in data.get("tables", {}).get("workspaces", {}).values():
            if str(ws.get("path", "")).rstrip("\\/").lower() == WORKSPACE.rstrip("\\/").lower():
                target = ws
                break
        if target is None:
            return
        ids = target.setdefault("sessionIds", [])
        if session_id not in ids:
            ids.append(session_id)
            target["updatedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())
            backup = WORKSPACE_STORE.with_suffix(".json.harness-bak")
            if not backup.exists():
                backup.write_bytes(WORKSPACE_STORE.read_bytes())
            tmp = WORKSPACE_STORE.with_suffix(".json.harness-tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            os.replace(tmp, WORKSPACE_STORE)
    except Exception as e:
        sys.stderr.write(f"register session failed: {e}\n")


def history(session_id: str) -> list[dict]:
    if zstd is None:
        return []
    path = find_session_log(session_id)
    if path is None:
        return []
    try:
        with open(path, "rb") as f:
            data = zstd.ZstdDecompressor().stream_reader(f).read()
        out = []
        for line in data.decode("utf-8", "replace").splitlines():
            try:
                ev = json.loads(line)
            except Exception:
                continue
            typ = ev.get("type")
            if typ not in ("user/message", "assistant/message"):
                continue
            data_block = ev.get("data", {})
            content = data_block.get("content")
            if content is None:
                content = data_block.get("message", {}).get("content", [])
            text = "".join(
                x.get("text", "") for x in content
                if isinstance(x, dict) and x.get("type") == "text"
            )
            if text:
                if typ == "user/message" and text.lstrip().startswith("<system-reminder>"):
                    continue
                out.append({"role": "user" if typ == "user/message" else "assistant",
                            "text": text, "seq": ev.get("seq", 0)})
        return out
    except Exception:
        return []


def one_line(text: str, limit: int) -> str:
    text = " ".join(str(text).replace("\r", " ").replace("\n", " ").split())
    return text[:limit]


def session_summary(item: dict) -> dict:
    sid = item.get("sessionId", "")
    msgs = history(sid)
    first_user = next((m["text"] for m in msgs if m["role"] == "user"), "")
    last = msgs[-1]["text"] if msgs else ""
    path = find_session_log(sid)
    updated = int(path.stat().st_mtime * 1000) if path else 0
    return {
        "sessionId": sid,
        "cwd": item.get("cwd", ""),
        "title": one_line(first_user, 36) or "New session",
        "preview": one_line(last, 46) or "No messages yet",
        "updatedAt": updated,
    }


def stt(wav_bytes: bytes) -> str:
    if not STT_PY.exists() or not STT_MODEL.exists():
        raise RuntimeError("STT not installed")
    tmp = Path(os.environ.get("TEMP", r"C:\Windows\Temp")) / f"cardputer-{uuid.uuid4().hex}.wav"
    tmp.write_bytes(wav_bytes)
    try:
        p = subprocess.run([str(STT_PY), str(STT_SCRIPT), str(STT_MODEL), str(tmp)],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=60)
        if p.returncode != 0:
            raise RuntimeError(p.stderr[-300:])
        return p.stdout.strip()
    finally:
        try: tmp.unlink()
        except Exception: pass


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        return

    def _json(self, code, value):
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _auth(self):
        if self.headers.get("X-Cardputer-Token") != TOKEN:
            self._json(401, {"error": "bad token"})
            return False
        return True

    def _body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        try:
            u = urlparse(self.path)
            if u.path == "/cardputer/health":
                return self._json(200, {"ok": True, "name": "dsh-cardputer-standalone"})
            if not self._auth():
                return
            if u.path == "/cardputer/sessions":
                items = [session_summary(s) for s in acp().list_sessions()]
                items.sort(key=lambda s: s.get("updatedAt", 0), reverse=True)
                return self._json(200, {"sessions": items})
            if u.path == "/cardputer/messages":
                sid = parse_qs(u.query).get("session", [""])[0]
                if not sid:
                    return self._json(400, {"error": "session required"})
                msgs = history(sid)
                if not msgs:
                    msgs = acp().session_cache.get(sid, [])
                return self._json(200, {"sessionId": sid, "messages": msgs, "running": False})
            return self._json(404, {"error": "not found"})
        except Exception as e:
            return self._json(500, {"error": str(e)})

    def do_POST(self):
        try:
            u = urlparse(self.path)
            if not self._auth():
                return
            if u.path == "/cardputer/new":
                return self._json(200, {"sessionId": acp().new_session().get("sessionId", "")})
            if u.path == "/cardputer/prompt":
                body = json.loads(self._body().decode() or "{}")
                sid, text = body.get("session", ""), body.get("text", "")
                if not sid or not str(text).strip():
                    return self._json(400, {"error": "session and text required"})
                acp().prompt(sid, text)
                return self._json(200, {"accepted": True})
            if u.path == "/cardputer/cancel":
                body = json.loads(self._body().decode() or "{}")
                sid = body.get("session", "")
                if sid:
                    acp().request("session/cancel", {"sessionId": sid}, timeout=20)
                return self._json(200, {"accepted": True})
            if u.path == "/cardputer/close":
                body = json.loads(self._body().decode() or "{}")
                sid = body.get("session", "")
                if sid:
                    acp().close(sid)
                return self._json(200, {"closed": True})
            if u.path == "/cardputer/stt":
                return self._json(200, {"text": stt(self._body())})
            return self._json(404, {"error": "not found"})
        except Exception as e:
            return self._json(500, {"error": str(e)})


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"cardputer bridge on http://0.0.0.0:{PORT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
