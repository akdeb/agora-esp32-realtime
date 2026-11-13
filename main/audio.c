#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_audio_dec.h"
#include "esp_audio_enc.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

static i2s_chan_handle_t mic, speaker;
static RingbufHandle_t playback, packets;
static void *opus_dec, *opus_enc;
static uint32_t decode_errors, packet_drops;
static int play_peak;
static bool discard; static int quiet_run; // after a tap: drop the rest of the reply
static uint32_t rx_quiet, rx_low, rx_loud, rx_queued, mic_lost, mic_heard;
static int64_t last_user_voice_us; static bool reply_logged;
static uint32_t rx_samples, tx_samples;
static int64_t enc_us, dec_us; static uint32_t dec_n;
// Codec stacks live in .bss so they stay in fast internal RAM, not PSRAM.
static StackType_t mic_stack[24576], dec_stack[24576];
static StaticTask_t mic_tcb, dec_tcb;
static SemaphoreHandle_t gate;
static bool session_enabled, user_muted, speaking;
static int64_t last_voice_us;
static uint32_t drops, underruns;
static bool primed;
static const char *TAG="audio";

static void status(void) { ui_set(!session_enabled?UI_IDLE:speaking?UI_SPEAKING:UI_LISTENING); }
void audio_set_session(bool enabled) {
    xSemaphoreTake(gate,portMAX_DELAY);
    session_enabled=enabled;
    if (!enabled) { speaking=false; discard=false; gpio_set_level(AMP_ENABLE,0); }
    status();
    xSemaphoreGive(gate);
}
void audio_set_user_mute(bool muted) {
    xSemaphoreTake(gate,portMAX_DELAY); user_muted=muted; status(); xSemaphoreGive(gate);
}
bool audio_user_muted(void) {
    xSemaphoreTake(gate,portMAX_DELAY); bool m=user_muted; xSemaphoreGive(gate); return m;
}
static void audio_receive(const void *data,size_t len) {
    if (!data || !len || (len&1)) return;
    const int16_t *p=data;
    int peak=0;
    for(size_t i=0;i<len/2;i++) { int a=abs((int)p[i]); if(a>peak) peak=a; }
    if (peak>play_peak) play_peak=peak;
    rx_samples+=len/2;
    if (peak<PLAYBACK_VOICE_PEAK) rx_quiet++; else if (peak<1500) rx_low++; else rx_loud++;
    // Digital boost with saturation; the gate above uses the pre-gain level.
    int16_t *w=(int16_t*)data;
    for(size_t i=0;i<len/2;i++) {
        int v=(int)w[i]*PLAYBACK_GAIN_PCT/100;
        w[i]=v>32767?32767:v<-32768?-32768:v;
    }
    xSemaphoreTake(gate,portMAX_DELAY);
    if (!session_enabled) { xSemaphoreGive(gate); return; }
    if (discard) {
        // Ignore the interrupted reply until the agent has been quiet for 300 ms.
        quiet_run=peak<PLAYBACK_VOICE_PEAK?quiet_run+1:0;
        if (quiet_run>=15) discard=false;
        xSemaphoreGive(gate); return;
    }
    if (peak>=PLAYBACK_VOICE_PEAK) {
        last_voice_us=esp_timer_get_time();
        if (peak>=1500 && !reply_logged && last_user_voice_us) {
            reply_logged=true;
            ESP_LOGI(TAG,"LATENCY reply audio %lld ms after user speech ended",(esp_timer_get_time()-last_user_voice_us)/1000);
        }
        if (!speaking) {
            speaking=true; // Gate uplink BEFORE the first speaker frame is queued.
            gpio_set_level(AMP_ENABLE,1); status();
            ESP_LOGI(TAG,"Speaking: microphone uplink muted");
        }
    }
    // After the hangover, stop queueing digital silence so continuous RTC
    // silence packets cannot prevent the queue from draining forever.
    if (speaking && esp_timer_get_time()-last_voice_us<=PLAYBACK_HANGOVER_MS*1000LL) {
        if (xRingbufferSend(playback,data,len,0)!=pdTRUE) drops++; else rx_queued++;
    }
    xSemaphoreGive(gate);
}
bool audio_interrupt(void) {
    xSemaphoreTake(gate,portMAX_DELAY);
    bool was=speaking;
    if (was) {
        discard=true; quiet_run=0;
        size_t len; void *item;
        while ((item=xRingbufferReceive(playback,&len,0))) vRingbufferReturnItem(playback,item);
        primed=false; speaking=false;
        gpio_set_level(AMP_ENABLE,0); status();
        ESP_LOGI(TAG,"Interrupted by button: listening");
    }
    xSemaphoreGive(gate);
    return was;
}
void audio_receive_opus(const void *data,size_t len) {
    // Called on an Agora SDK thread: just queue; decode_task owns the decoder.
    if (data && len && xRingbufferSend(packets,data,len,0)!=pdTRUE) packet_drops++;
}
static void decode_task(void *arg) {
    static int16_t pcm[SPK_RATE/1000*120]; // room for the longest Opus frame
    for (;;) {
        size_t len=0;
        uint8_t *packet=xRingbufferReceive(packets,&len,portMAX_DELAY);
        if (!packet) continue;
        esp_audio_dec_in_raw_t in={.buffer=packet,.len=len};
        esp_audio_dec_out_frame_t out={.buffer=(uint8_t*)pcm,.len=sizeof(pcm)};
        esp_audio_dec_info_t info={0};
        int64_t t0=esp_timer_get_time();
        esp_audio_err_t err=esp_opus_dec_decode(opus_dec,&in,&out,&info);
        dec_us+=esp_timer_get_time()-t0; dec_n++;
        vRingbufferReturnItem(packets,packet);
        if (err!=ESP_AUDIO_ERR_OK) { decode_errors++; continue; }
        audio_receive(pcm,out.decoded_size);
    }
}
static void capture_task(void *arg) {
    // ICS-43434: 24-bit signed data, left-aligned in a 32-bit left slot.
    static int32_t raw[MIC_SAMPLES*2];
    static int16_t pcm[MIC_SAMPLES], silence[MIC_SAMPLES];
    static uint8_t packet[400];
    int peak=0; unsigned sent=0, failed=0, muted=0, frames=0;
    for (;;) {
        size_t got=0;
        esp_err_t err=i2s_channel_read(mic,raw,sizeof(raw),&got,1000);
        if (err!=ESP_OK || got!=sizeof(raw)) continue;
        int fpeak=0;
        for (int i=0;i<MIC_SAMPLES;i++) {
            pcm[i]=(int16_t)(raw[i*2]>>16);
            int a=abs((int)pcm[i]); if(a>fpeak) fpeak=a;
        }
        if (fpeak>peak) peak=fpeak;
        // Always drain mic DMA. Never send old speaker echo when listening resumes.
        xSemaphoreTake(gate,portMAX_DELAY);
        bool active=session_enabled, mute=speaking||user_muted;
        xSemaphoreGive(gate);
        if (active && rtc_connected()) {
            // Encode outside the lock (it is the expensive part). Encode silence
            // while muted so the encoder and RTC clock stay continuous and server
            // VAD never hears the board's own speaker.
            esp_audio_enc_in_frame_t in={.buffer=(uint8_t*)(mute?silence:pcm),.len=sizeof(pcm)};
            esp_audio_enc_out_frame_t out={.buffer=packet,.len=sizeof(packet)};
            int rc=-1;
            int64_t t0=esp_timer_get_time();
            if (esp_opus_enc_process(opus_enc,&in,&out)==ESP_AUDIO_ERR_OK && out.encoded_bytes)
                rc=rtc_send_opus(packet,out.encoded_bytes);
            enc_us+=esp_timer_get_time()-t0;
            if (rc<0) failed++; else if (mute) muted++; else sent++;
            if (fpeak>=700) { if (mute) mic_lost++; else mic_heard++; }
            if (!mute && fpeak>=700) { last_user_voice_us=esp_timer_get_time(); reply_logged=false; }
        }
        if (++frames>=50) { // ~1 s of 20 ms frames
            ESP_LOGI(TAG,"mic peak=%d sent=%u muted=%u failed=%u speaking=%d",peak,sent,muted,failed,(int)speaking);
            ESP_LOGI(TAG,"playback underruns=%lu drops=%lu decode_errors=%lu packet_drops=%lu",(unsigned long)underruns,(unsigned long)drops,(unsigned long)decode_errors,(unsigned long)packet_drops);
            ESP_LOGI(TAG,"codec enc_ms/frame=%.1f dec_ms/frame=%.1f agent_peak=%d",enc_us/50000.0,dec_n?dec_us/1000.0/dec_n:0.0,play_peak); play_peak=0;
            UBaseType_t backlog=0; vRingbufferGetInfo(playback,NULL,NULL,NULL,NULL,&backlog);
            ESP_LOGI(TAG,"flow rx_ms=%lu tx_ms=%lu backlog_ms=%lu",(unsigned long)rx_samples/(SPK_RATE/1000),(unsigned long)tx_samples/(SPK_RATE/1000),(unsigned long)backlog*20);
            ESP_LOGI(TAG,"diag agent_frames quiet=%lu low=%lu loud=%lu queued=%lu | user_speech_frames heard=%lu lost_to_mute=%lu",(unsigned long)rx_quiet,(unsigned long)rx_low,(unsigned long)rx_loud,(unsigned long)rx_queued,(unsigned long)mic_heard,(unsigned long)mic_lost);
            rx_quiet=rx_low=rx_loud=rx_queued=mic_lost=mic_heard=0;
            rx_samples=tx_samples=0;
            peak=0; sent=muted=failed=frames=0; enc_us=0; dec_us=0; dec_n=0;
        }
    }
}
static void playback_task(void *arg) {
    static int16_t stereo[SPK_SAMPLES*2];
    for (;;) {
        size_t len=0;
        if (!primed) {
            UBaseType_t queued=0;
            vRingbufferGetInfo(playback,NULL,NULL,NULL,NULL,&queued);
            // Also flush a short tail once the cloud stops sending, so the last
            // fragment of a reply can't sit below the prebuffer threshold forever.
            // "queued" counts ring items; each item is one 20 ms decoded frame.
            if (queued >= PLAYBACK_PREBUFFER_MS/20 ||
                (queued>0 && esp_timer_get_time()-last_voice_us>250000)) primed=true;
        }
        int16_t *item=primed?xRingbufferReceive(playback,&len,pdMS_TO_TICKS(20)):NULL;
        if (item) {
            size_t count=len/2;
            for (size_t start=0;start<count;start+=SPK_SAMPLES) {
                size_t n=count-start;
                if(n>SPK_SAMPLES) n=SPK_SAMPLES;
                // Duplicate mono into both slots: MAX98357A SD_MODE high selects
                // a slot; both channels carry the same signal.
                for(size_t i=0;i<n;i++) stereo[2*i]=stereo[2*i+1]=item[start+i];
                size_t written=0;
                esp_err_t err=i2s_channel_write(speaker,stereo,n*4,&written,1000);
                if(err!=ESP_OK || written!=n*4) ESP_LOGW(TAG,"Speaker DMA write incomplete");
                tx_samples+=n;
            }
            vRingbufferReturnItem(playback,item);
        } else {
            if (primed) { primed=false; underruns++; } // ran dry: rebuffer before resuming
            // Clear previous DMA contents so an underrun cannot repeat old speech.
            memset(stereo,0,sizeof(stereo));
            size_t written=0;
            i2s_channel_write(speaker,stereo,sizeof(stereo),&written,100);
        }
        xSemaphoreTake(gate,portMAX_DELAY);
        if (speaking && esp_timer_get_time()-last_voice_us>PLAYBACK_HANGOVER_MS*1000LL) {
            // Drain queued audio before opening the mic. In particular, don't
            // reopen just because cloud packets stopped arriving.
            UBaseType_t waiting=0;
            vRingbufferGetInfo(playback,NULL,NULL,NULL,NULL,&waiting);
            if (waiting==0) {
                gpio_set_level(AMP_ENABLE,0);
                speaking=false; status();
                ESP_LOGI(TAG,"Listening");
            }
        }
        xSemaphoreGive(gate);
    }
}
void audio_init(void) {
    gpio_config_t io={.pin_bit_mask=1ULL<<AMP_ENABLE,.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_hold_dis(AMP_ENABLE);
    gpio_set_level(AMP_ENABLE,0);
    gate=xSemaphoreCreateMutex();
    playback=xRingbufferCreate(SPK_RATE*2*PLAYBACK_BUFFER_S,RINGBUF_TYPE_NOSPLIT); // in PSRAM
    packets=xRingbufferCreate(32768,RINGBUF_TYPE_NOSPLIT);
    assert(gate && playback && packets);
    esp_opus_dec_cfg_t dec=ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec.sample_rate=SPK_RATE; dec.channel=1;
    assert(esp_opus_dec_open(&dec,sizeof(dec),&opus_dec)==ESP_AUDIO_ERR_OK);
    esp_opus_enc_config_t enc=ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate=MIC_RATE; enc.channel=1; enc.bitrate=MIC_OPUS_BITRATE;
    enc.frame_duration=ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc.application_mode=ESP_OPUS_ENC_APPLICATION_VOIP; enc.complexity=1;
    assert(esp_opus_enc_open(&enc,sizeof(enc),&opus_enc)==ESP_AUDIO_ERR_OK);
    i2s_chan_config_t rx=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    rx.dma_desc_num=6; rx.dma_frame_num=MIC_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&rx,NULL,&mic));
    i2s_std_config_t in={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=MIC_BCLK,.ws=MIC_WS,.dout=I2S_GPIO_UNUSED,.din=MIC_DATA}
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic,&in));
    i2s_chan_config_t tx=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    tx.dma_desc_num=8; tx.dma_frame_num=SPK_SAMPLES/2;
    tx.auto_clear=true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx,&speaker,NULL));
    i2s_std_config_t out={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=AMP_BCLK,.ws=AMP_WS,.dout=AMP_DATA,.din=I2S_GPIO_UNUSED}
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(speaker,&out));
    ESP_ERROR_CHECK(i2s_channel_enable(mic));
    ESP_ERROR_CHECK(i2s_channel_enable(speaker));
    assert(xTaskCreateStaticPinnedToCore(capture_task,"mic",sizeof(mic_stack),NULL,6,mic_stack,&mic_tcb,0));
    assert(xTaskCreateStaticPinnedToCore(decode_task,"opusdec",sizeof(dec_stack),NULL,7,dec_stack,&dec_tcb,1));
    assert(xTaskCreate(playback_task,"speaker",8192,NULL,7,NULL)==pdPASS);
}
