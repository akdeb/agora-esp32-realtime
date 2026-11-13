#pragma once
// Mic WS/BCLK follow the working production firmware (WS=IO4, SCK=IO1); the schematic notes list them swapped.
// elato-tactile-mic-front schematic, 2026-10-17, and exported PCB netlist.
#define MIC_WS 4
#define MIC_BCLK 1
#define MIC_DATA 14
#define AMP_WS 5
#define AMP_BCLK 6
#define AMP_DATA 7
#define AMP_ENABLE 10
#define BUTTON_GPIO 2
#define LED_R 9
#define LED_G 8
#define LED_B 13
// Uplink: 16 kHz mono Opus is plenty for ASR. Downlink: the Agora agent sends
// fullband Opus, decoded on the board and played at 48 kHz.
#define MIC_RATE 16000
#define MIC_SAMPLES (MIC_RATE/50) /* 20 ms */
#define SPK_RATE 48000
#define SPK_SAMPLES (SPK_RATE/50) /* 20 ms */
#define MIC_OPUS_BITRATE 24000
// Conservative playback release; tune on the physical speaker.
#define PLAYBACK_HANGOVER_MS 400
#define PLAYBACK_VOICE_PEAK 32
#define PLAYBACK_PREBUFFER_MS 80
// The agent can deliver a long reply faster than real time; hold it all.
#define PLAYBACK_BUFFER_S 12
// Agent audio arrives well below full scale; boost it (saturating).
#define PLAYBACK_GAIN_PCT 300
#define DOUBLE_CLICK_MS 450
