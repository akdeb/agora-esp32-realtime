import os
import time
import unittest
from unittest.mock import patch

from server import Control, AgoraHTTPError, BOARD_UID, AGENT_UID
from vendor.AccessToken2 import AccessToken, ServiceRtc, ServiceConvoAI


class ControlTest(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ, {
            "AGORA_APP_ID": "a" * 32, "AGORA_APP_CERTIFICATE": "b" * 32,
            "OPENAI_API_KEY": "test-provider-secret", "DEVICE_KEY": "test-device-secret",
            "AGENT_MODE": "pipeline"})
        self.env.start()
        self.c = Control()

    def tearDown(self):
        self.env.stop()

    def test_board_token_signature_channel_uid_and_no_master_secrets(self):
        session = self.c.create()
        token = AccessToken()
        self.assertTrue(token.from_string(session["token"]))
        self.assertTrue(token.verify_signature(self.c.certificate))
        rtc = token.get_services(ServiceRtc.kServiceType)[0]
        self.assertEqual(rtc._ServiceRtc__channel_name, session["channel"].encode())
        self.assertEqual(rtc._ServiceRtc__uid, str(BOARD_UID).encode())
        self.assertNotIn("test-provider-secret", str(session))
        self.assertNotIn(self.c.certificate, str(session))

    def test_agent_token_has_convoai_and_correct_rtc_identity(self):
        token = AccessToken()
        self.assertTrue(token.from_string(self.c.agent_token("test-channel")))
        self.assertTrue(token.verify_signature(self.c.certificate))
        self.assertIsNotNone(token.get_services(ServiceConvoAI.kServiceType))
        rtc = token.get_services(ServiceRtc.kServiceType)[0]
        self.assertEqual((rtc._ServiceRtc__channel_name, rtc._ServiceRtc__uid), (b"test-channel", str(AGENT_UID).encode()))

    def test_start_is_idempotent_and_stop_targets_only_owned_agent(self):
        session = self.c.create()
        sid = session["session_id"]
        with patch.object(self.c, "request", return_value={"agent_id": "our-agent"}) as request:
            self.c.action("start", sid)
            self.c.action("start", sid)
            self.assertEqual(request.call_count, 1)
            self.c.action("stop", sid)
            self.assertEqual(request.call_args.args[0], "/agents/our-agent/leave")
        self.assertFalse(self.c.sessions)

    def test_heartbeat_expiration_stops_agent(self):
        sid = self.c.create()["session_id"]
        self.c.sessions[sid]["last_seen"] = time.monotonic() - 100
        self.c.reap()
        self.assertFalse(self.c.sessions)

    def test_new_session_replaces_old_and_stops_its_agent(self):
        first = self.c.create()["session_id"]
        self.c.sessions[first]["agent_id"] = "old-agent"
        with patch.object(self.c, "request", return_value={}) as request:
            second = self.c.create()["session_id"]
            self.assertEqual(request.call_args.args[0], "/agents/old-agent/leave")
        self.assertEqual(list(self.c.sessions), [second])

    def test_server_vad_and_half_duplex_config(self):
        p = self.c.payload("test", "token")["properties"]
        self.assertFalse(p["interruption"]["enable"])
        self.assertEqual(p["turn_detection"]["config"]["end_of_speech"]["mode"], "vad")
        self.assertEqual(p["llm"]["api_key"], "test-provider-secret")
        self.assertEqual(p["tts"]["params"]["api_key"], "test-provider-secret")

    def test_failed_stop_is_retained_for_retry(self):
        sid = self.c.create()["session_id"]
        self.c.sessions[sid]["agent_id"] = "our-agent"
        with patch.object(self.c, "request", side_effect=RuntimeError("offline")):
            with self.assertRaises(RuntimeError):
                self.c.action("stop", sid)
        self.assertIn(sid, self.c.sessions)

    def test_stop_of_already_ended_agent_clears_session(self):
        sid = self.c.create()["session_id"]
        self.c.sessions[sid]["agent_id"] = "our-agent"
        with patch.object(self.c, "request", side_effect=AgoraHTTPError(404, "gone")):
            self.c.action("stop", sid)
        self.assertFalse(self.c.sessions)

    def test_gemini_mode_uses_mllm_with_server_vad(self):
        with patch.dict(os.environ, {"AGENT_MODE": "gemini", "GEMINI_API_KEY": "test-gemini-secret"}):
            p = Control().payload("test", "token")["properties"]
        self.assertEqual(p["mllm"]["vendor"], "gemini")
        self.assertEqual(p["mllm"]["turn_detection"]["mode"], "server_vad")
        self.assertNotIn("llm", p)


if __name__ == "__main__":
    unittest.main()
