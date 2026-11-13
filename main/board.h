#pragma once
// Board: elato-voice-cam dev kit (ESP32-S3-WROOM-1-N16R8), pin map per the
// EasyEDA design table (Waveshare ESP32-S3-AUDIO-Board compatible).
//   ES7210 ADC  (I2C 0x40): MIC1 + MIC2 analog ZTS6117 mics, MIC3 = speaker loopback (AEC reference)
//   ES8311 DAC  (I2C 0x18) -> NS4150B amp, enabled by TCA9555 (I2C 0x20) pin P10
//   One I2S bus shared by ADC and DAC, so both run at the same sample rate.
#define I2C_PORT 0
#define I2C_SCL 10
#define I2C_SDA 11
#define I2S_MCLK 12
#define I2S_BCLK 13
#define I2S_WS 14
#define I2S_DIN 15   /* ES7210 SDOUT -> ESP32 */
#define I2S_DOUT 16  /* ESP32 -> ES8311 DSDIN */
#define ES8311_I2C_ADDR 0x30 /* 8-bit form of 0x18, as esp_codec_dev expects */
#define ES7210_I2C_ADDR 0x80 /* 8-bit form of 0x40 */
#define TCA9555_I2C_ADDR 0x20
#define TCA9555_PA_EN_BIT 0  /* P10 = port 1, bit 0 */
#define BUTTON_GPIO 6
// Common-anode RGB LED, active low. Colour order assumed from the tactile
// board's equivalent pins; not checked against the LED datasheet.
#define LED_R 38
#define LED_G 39
#define LED_B 47
#define MIC_GAIN_DB 30.0f
#define SPEAKER_VOLUME 90 /* 0-100, ES8311 DAC volume */
// Uplink: 16 kHz mono Opus is plenty for ASR. Downlink: the Agora agent sends
// fullband Opus, decoded on the board and played at 48 kHz.
#define MIC_RATE 16000
#define MIC_SAMPLES (MIC_RATE/50) /* 20 ms */
#define MIC_DECIMATE (SPK_RATE/MIC_RATE) /* ADC runs at SPK_RATE on the shared bus */
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
