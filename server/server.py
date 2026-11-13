"""Local control plane. Audio flows directly between the ESP32 and Agora.

Only Python's standard library is needed. Never return provider credentials or
the App Certificate from this service.
"""
import argparse
import atexit
import hmac
import json
import os
from pathlib import Path
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from vendor.RtcTokenBuilder2 import RtcTokenBuilder, Role_Publisher
from vendor.convoai_token_builder import ConvoAITokenBuilder

ROOT = Path(__file__).resolve().parent
TOKEN_SECONDS = 3600
BOARD_UID, AGENT_UID = 1002, 1001


def load_env(path):
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                os.environ.setdefault(key.strip(), value.strip().strip("\"'"))


class AgoraHTTPError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class Control:
    def __init__(self):
        self.app_id = os.environ["AGORA_APP_ID"]
        self.certificate = os.environ["AGORA_APP_CERTIFICATE"]
        self.openai_key = os.environ["OPENAI_API_KEY"]
        self.device_key = os.environ["DEVICE_KEY"]
        self.gemini_key = os.getenv("GEMINI_API_KEY", "")
        # "gemini": one realtime speech-to-speech model. "pipeline": ARES ASR + OpenAI LLM + OpenAI TTS.
        self.mode = os.getenv("AGENT_MODE", "gemini" if self.gemini_key else "pipeline")
        self.sessions = {}
        self.lock = threading.RLock()
        self.base = f"https://api.agora.io/api/conversational-ai-agent/v2/projects/{self.app_id}"

    def agent_token(self, channel):
        return ConvoAITokenBuilder.build_token(
            self.app_id, self.certificate, channel, str(AGENT_UID), TOKEN_SECONDS, TOKEN_SECONDS)

    def request(self, path, token, body=None):
        req = Request(self.base + path, method="POST",
                      data=json.dumps(body or {}).encode(),
                      headers={"Authorization": f'agora token="{token}"',
                               "Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=25) as response:
                return json.loads(response.read() or b"{}")
        except HTTPError as error:
            detail = error.read(400).decode("utf-8", "replace")
            for secret in filter(None, (self.openai_key, self.gemini_key, self.certificate, self.device_key)):
                detail = detail.replace(secret, "[redacted]")
            raise AgoraHTTPError(error.code, f"Agora returned HTTP {error.code}: {detail}") from None

    def create(self):
        with self.lock:
            # This control plane serves one physical board. If it asks for a new
            # session (reboot, reset, crash), the old one is dead: replace it so the
            # board is never locked out, and never leave two billable agents running.
            for old in list(self.sessions):
                try:
                    self.action("stop", old)
                except Exception as error:
                    print(f"Could not stop replaced agent: {error}", flush=True)
                    self.sessions.pop(old, None)
            sid = secrets.token_hex(16)
            channel = "elato_" + sid
            token = RtcTokenBuilder.build_token_with_uid(
                self.app_id, self.certificate, channel, BOARD_UID,
                Role_Publisher, TOKEN_SECONDS, TOKEN_SECONDS)
            self.sessions[sid] = {"channel": channel, "agent_id": None,
                                  "last_seen": time.monotonic(), "created": time.monotonic()}
            return {"session_id": sid, "channel": channel, "app_id": self.app_id,
                    "uid": BOARD_UID, "agent_uid": AGENT_UID, "token": token}

    PROMPT = ("You are Elato, a friendly voice assistant. Keep replies brief, usually one or two sentences. "
              "Speak naturally. Avoid markdown and lists unless requested.")
    GREETING = "Hi, I'm Elato. What would you like to talk about?"

    def payload(self, channel, token):
        if self.mode == "gemini":
            return {"name": channel, "properties": {
                "channel": channel, "token": token, "agent_rtc_uid": str(AGENT_UID),
                "remote_rtc_uids": [str(BOARD_UID)], "enable_string_uid": False,
                "idle_timeout": 30,
                "mllm": {"enable": True, "vendor": "gemini", "api_key": self.gemini_key,
                         "url": os.getenv("GEMINI_LIVE_URL", "wss://generativelanguage.googleapis.com/ws/"
                                          "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"),
                         "params": {"model": os.getenv("GEMINI_MODEL", "gemini-3.8-live"),
                                    "instructions": self.PROMPT,
                                    "voice": os.getenv("GEMINI_VOICE", "Aoede"),
                                    "transcribe_agent": True, "transcribe_user": True},
                         # Gemini's own server-side VAD decides the turns.
                         "turn_detection": {"mode": "server_vad", "server_vad_config": {
                             "prefix_padding_ms": 200, "silence_duration_ms": 300,
                             "end_of_speech_sensitivity": "END_SENSITIVITY_HIGH"}},
                         "input_modalities": ["audio"], "output_modalities": ["audio"],
                         "greeting_message": self.GREETING,
                         "failure_message": "Could you say that again?"},
                "parameters": {"data_channel": "datastream"}}}
        return {"name": channel, "properties": {
            "channel": channel, "token": token, "agent_rtc_uid": str(AGENT_UID),
            "remote_rtc_uids": [str(BOARD_UID)], "enable_string_uid": False,
            "idle_timeout": 30,
            "asr": {"vendor": "ares", "language": "en-US"},
            "llm": {"vendor": "openai", "style": "openai", "credential_mode": "byok",
                    "url": "https://api.openai.com/v1/chat/completions", "api_key": self.openai_key,
                    "params": {"model": os.getenv("OPENAI_MODEL", "gpt-4.1-mini")},
                    "system_messages": [{"role": "system", "content":
                        "You are Elato, a friendly voice assistant. Keep replies brief, usually one or two sentences. "
                        "Speak naturally. Avoid markdown and lists unless requested."}],
                    "greeting_message": "Hi, I'm Elato. What would you like to talk about?",
                    "failure_message": "Could you say that again?", "max_history": 20},
            "tts": {"vendor": "openai", "credential_mode": "byok", "params": {
                "url": "https://api.openai.com/v1/audio/speech", "api_key": self.openai_key,
                "model": "gpt-4o-mini-tts", "voice": os.getenv("OPENAI_VOICE", "coral")}},
            "turn_detection": {"mode": "default", "config": {
                "speech_threshold": 0.5,
                "start_of_speech": {"mode": "vad", "vad_config": {"prefix_padding_ms": 320}},
                "end_of_speech": {"mode": "vad", "vad_config": {"silence_duration_ms": 480}}}},
            "interruption": {"enable": False, "disabled_config": {"strategy": "ignore"}},
            # G.722 is the only codec this embedded SDK build can encode/decode
            # (Opus fails with an acm_encoder error), so request it explicitly.
            "parameters": {"data_channel": "datastream"}
        }}

    def action(self, action, sid):
        with self.lock:
            if sid not in self.sessions:
                raise KeyError("Unknown session")
            s = self.sessions[sid]
            if action == "start":
                if not s["agent_id"]:
                    token = self.agent_token(s["channel"])
                    result = self.request("/join", token, self.payload(s["channel"], token))
                    if not isinstance(result.get("agent_id"), str) or not result["agent_id"]:
                        raise RuntimeError("Agora did not return an agent ID")
                    s["agent_id"] = result["agent_id"]
                s["last_seen"] = time.monotonic()
            elif action == "heartbeat":
                s["last_seen"] = time.monotonic()
            elif action == "interrupt":
                # Button tap on the board: stop the agent's current reply.
                s["last_seen"] = time.monotonic()
                if s["agent_id"]:
                    self.request(f'/agents/{s["agent_id"]}/interrupt', self.agent_token(s["channel"]))
            elif action == "stop":
                if s["agent_id"]:
                    try:
                        self.request(f'/agents/{s["agent_id"]}/leave', self.agent_token(s["channel"]))
                    except AgoraHTTPError as error:
                        # 404 means the agent already ended (idle timeout or board left);
                        # any other failure keeps the session so the stop is retried.
                        if error.code != 404:
                            raise
                del self.sessions[sid]
            else:
                raise ValueError("Unknown action")
            return {"ok": True}

    def reap(self):
        with self.lock:
            stale = [sid for sid, s in self.sessions.items()
                     if time.monotonic() - s["last_seen"] > 90
                     or time.monotonic() - s["created"] > 55 * 60]
        for sid in stale:
            try:
                self.action("stop", sid)
            except Exception as error:
                print(f"Could not stop expired agent; will retry: {error}", flush=True)

    def close(self):
        for sid in list(self.sessions):
            try:
                self.action("stop", sid)
            except Exception:
                pass


def handler(control):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            # Never log headers, bodies or RTC tokens.
            print(f"{self.command} {self.path}")

        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self.reply(200 if self.path == "/health" else 404,
                       {"ok": self.path == "/health"})

        def do_POST(self):
            if not hmac.compare_digest(self.headers.get("Authorization", ""),
                                       "Bearer " + control.device_key):
                return self.reply(401, {"error": "Unauthorized"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 512:
                    return self.reply(400, {"error": "Invalid body length"})
                body = json.loads(self.rfile.read(length))
                action = self.path.lstrip("/")
                if action == "session":
                    result = control.create()
                elif action in ("start", "stop", "heartbeat", "interrupt"):
                    sid = body.get("session_id")
                    if not isinstance(sid, str) or len(sid) != 32:
                        raise ValueError("Invalid session ID")
                    result = control.action(action, sid)
                else:
                    return self.reply(404, {"error": "Not found"})
                self.reply(200, result)
            except KeyError:
                self.reply(404, {"error": "Session not found"})
            except (ValueError, TypeError, AttributeError):
                self.reply(400, {"error": "Invalid request or session already active"})
            except Exception as error:
                print(f"{self.path}: {type(error).__name__}: {error}", flush=True)
                self.reply(502, {"error": "Agent request failed; check Agora project, credits and OpenAI model access"})
    return Handler


def main():
    load_env(ROOT / ".env")
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    control = Control()
    atexit.register(control.close)
    stop = threading.Event()

    def cleaner():
        while not stop.wait(10):
            control.reap()

    threading.Thread(target=cleaner, daemon=True).start()
    server = ThreadingHTTPServer((args.host, args.port), handler(control))
    print(f"Elato control backend listening on {args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
        control.close()


if __name__ == "__main__":
    main()
