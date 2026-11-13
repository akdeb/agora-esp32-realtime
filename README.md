# Elato tactile board: Agora voice AI

ESP-IDF firmware and a small local Python control server for the
`elato-tactile-mic-front` ESP32-S3-WROOM-1-N16R8 board.

**Status:** source implementation, not yet compiled, flashed, or tested on hardware.
The current agent session and token tests pass. In the authoring session, USB
access was denied, public SDK downloads could not resolve DNS, and the installed
PlatformIO IDF Python environments were missing dependencies. Starting the local
backend was also blocked by a socket-bind permission error, so it is not running.
Do not assume this
is a working firmware binary until a complete build and the hardware checks below pass.

## Design

The ESP32 streams mono PCM to the Agora IoT/RTSA SDK, using G.722 at 16 kHz.
Agora transports the audio to its Conversational AI agent. Agora ARES performs
speech recognition and server-side VAD; OpenAI supplies the language model
(`gpt-4.1-mini`) and voice (`gpt-4o-mini-tts`, Coral). The OpenAI key is used by
the cloud agent, not the ESP32. This is Agora RTC transport, not an SDP/ICE
connection to a generic WebRTC endpoint.

The local server only creates tokens and starts/stops the cloud agent. Audio
does not pass through the computer. Keep the computer/server reachable while
using the board: it receives a heartbeat every 20 seconds. Lost heartbeats cause
agent cleanup after 90 seconds. Sessions reconnect before their one-hour RTC
tokens expire. Sleeping explicitly stops the current agent; only this server's
own agent IDs can be stopped.

## Board pins

Verified against the exported 2026-10-17 tactile schematic and GPIO netlist in
`Documents/EasyEDA-Pro/projects/flauschfabrik-files`.

| Function | GPIO |
| --- | --- |
| ICS-43434 WS / BCLK / SD | 1 / 4 / 14 |
| MAX98357A WS / BCLK / DIN / enable | 5 / 6 / 7 / 10 |
| Tactile button, active low | 2 |
| RGB red / green / blue, active low | 9 / 8 / 13 |

RGB channel names follow the existing production firmware (`Config.cpp`);
verify the actual colours on the physical board. GPIO35–37 are reserved for
octal PSRAM. The mic LR pin is grounded, selecting the left slot. Capture uses
32-bit stereo slots, extracts the left channel, and converts to PCM16. Speaker
output duplicates mono into both 16-bit slots.

## Production controls

Based on `elato-client-prod/esp32/src/{main.cpp,LEDHandler.cpp,WifiManager.cpp}`,
specifically the tactile branch (the current production source defines TOUCH_MODE).

| State or gesture | Behaviour |
| --- | --- |
| Idle | White |
| Wi-Fi setup | Magenta; open AP named `ELATO` |
| Listening | Yellow |
| Processing / connection setup | Green |
| Speaking | Blue |
| Sleep | LEDs off, amplifier disabled |
| Single click | No action |
| Double click | Deep sleep |
| Hold for at least 1.5 seconds, release | Deep sleep |
| Press while asleep | Wake and reconnect |

There is no hold-to-reset gesture. Configuration opens on first boot or after
three failed Wi-Fi attempts. The AP serves DNS and an HTTP captive setup page at
`http://192.168.4.1/wifi`; phone popup behavior still needs a physical check.
Unused APs retry after two minutes; a connected setup client extends the timer.
Saved settings survive reboot. If NVS is preserved, the first network saved by
production Arduino firmware (`wifimanager/apName0`, `apPass0`) can be reused.
Unlike production's Wi-Fi manager, this initial version saves one active network
and does not scan/rank four stored networks or cache BSSID/channel.

## Half duplex

Microphone DMA is continuously drained. Once non-silent decoded reply audio
arrives, the uplink sends zero PCM, and the speaker amplifier is enabled.
The mic stays gated while buffered audio plays and for a conservative silence
hangover. Agent interruptions are disabled server-side. No AEC is enabled.

**Current limitation:** playback completion uses decoded-audio energy plus queue
drain and a 1.6-second hangover, not a verified Agora end-of-turn event. Long TTS
pauses or packet gaps can reopen listening between audio segments. Each new
segment mutes the uplink before being queued, so it cannot intentionally play
and transmit live mic audio simultaneously. Validate speaker tail/room echo
and adjust `PLAYBACK_HANGOVER_MS` on the board. Add authoritative turn events
before treating this as production-grade turn handling. Green indicates
connection/setup processing; cloud thinking events are not yet wired to the LED.

## Local setup

Python 3.10+ is sufficient for the server; its token builders are vendored from
Agora's official Tools repository. No pip packages are needed for this server.

The local `server/.env` has already been populated with the supplied Agora
credentials and the OpenAI key from `elato-deno/.env`. It is ignored and has
permissions 0600. For a fresh checkout, copy `server/.env.example` to
`server/.env` and fill the values. Generate DEVICE_KEY with
`python3 -c 'import secrets; print(secrets.token_urlsafe(32))'`.

```sh
python3 scripts/configure.py
python3 server/server.py
```

`configure.py` detects the computer's en0 LAN address; use
`--backend-url http://YOUR_COMPUTER_LAN_IP:8080` to override. It generates ignored
`main/bootstrap.h` containing only the local backend address and device key.
Provider keys and the App Certificate are never compiled into firmware.
Regenerate and reflash if the computer's IP changes, or edit Connection settings
in the setup page. HTTP is intended for this trusted LAN demo; use an HTTPS
origin with a publicly trusted certificate when hosting remotely.

The Agora project must have Conversational AI enabled and sufficient credits;
the OpenAI project must allow the configured models. These live account settings
have not been checked. User authorization permits use of the existing OpenAI
key for this project; it remains local except when sent to Agora's agent API
as the configured provider credential.

## Build and flash

Use ESP-IDF **5.2.3**, matching the official Agora ESP32 sample and its published
SDK. This code uses the IDF 5 standard I2S driver and does not require ESP-ADF.
Binary compatibility with any newer IDF must be verified before upgrading.

With a working ESP-IDF shell:

```sh
bash scripts/fetch_sdk.sh
python3 scripts/configure.py
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-10 flash monitor
```

`fetch_sdk.sh` obtains Agora's proprietary ESP32 SDK from the URL published in
its ESP32 sample. The extracted folder must provide the `agora_iot_sdk` CMake
component, its headers, and its ESP32-S3 static libraries. The SDK is not bundled
here. Its audio callback must deliver decoded 16 kHz PCM; the implementation
checks PCM type and drops other formats. This assumption and the G.722 agent
parameter need confirmation against the downloaded SDK/live service.

Do not erase flash merely to connect Wi-Fi: doing so would delete the existing
board's saved credentials. The provided partition table keeps the conventional
Arduino NVS region at 0x9000 with size 0x5000, but verify the board's current
partition table before flashing if retaining its NVS matters.

If a build environment is missing, install IDF 5.2.3 using Espressif's normal
installer or clone into `.tools/esp-idf`, run `install.sh esp32s3`, and source
`export.sh`. Do not use a broken PlatformIO Python environment for `idf.py`.

## Checks

```sh
python3 -m unittest discover -s server -v
```

These tests verify signed RTC tokens and identity binding, ConvoAI token
privileges, session ownership, duplicate-start prevention, heartbeat cleanup,
and stop retries. They do not call billable services.

After compilation and flashing, verify: AP and phone portal; 2.4 GHz network
save/reconnect; mic slot and level; blue only during playback; no self-response
from speaker echo; repeated voice turns; long pauses and packet loss; double
click and long-press sleep; button wake; Wi-Fi outage recovery; backend outage
and agent cleanup. No physical verification has been performed yet.

## References

- [Agora official ESP32 client](https://github.com/AgoraIO-Conversational-AI/esp32-client/tree/main/esp32-respeaker)
- [Agora agent API](https://docs.agora.io/en/api-reference/api-ref/conversational-ai/join)
- [Agora token authentication](https://docs.agora.io/en/api-reference/api-ref/conversational-ai/authentication)
- [OpenAI TTS through Agora](https://docs.agora.io/en/ai/models/tts/openai)
