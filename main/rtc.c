#include <string.h>
#include <stdatomic.h>
#include "app.h"
#include "board.h"
#include "agora_rtc_api.h"
#include "esp_log.h"

static connection_id_t connection;
static atomic_bool connected;
static bool initialized, created;
static uint32_t agent_uid;
static const char *TAG="rtc";
static void joined(connection_id_t id,uint32_t uid,int elapsed) {
    atomic_store(&connected,true);
    ESP_LOGI(TAG,"Joined channel as %lu",(unsigned long)uid);
}
static void lost(connection_id_t id) { atomic_store(&connected,false); }
static void error(connection_id_t id,int code,const char *msg) {
    ESP_LOGE(TAG,"SDK error %d: %s",code,msg?msg:"");
}
static void received(connection_id_t id,const uint32_t uid,uint16_t ts,
    const void *data,size_t len,const audio_frame_info_t *info) {
    if(uid!=agent_uid) return;
    // SDK codec is disabled: frames arrive as raw Opus packets.
    if(info->data_type!=AUDIO_DATA_TYPE_OPUS && info->data_type!=AUDIO_DATA_TYPE_OPUSFB) {
        static int warned;
        if(!warned++) ESP_LOGE(TAG,"Unsupported downlink audio type %d",info->data_type);
        return;
    }
    audio_receive_opus(data,len);
}
void session_rtc_init(void) { atomic_init(&connected,false); }
esp_err_t rtc_join(const session_t *s) {
    agent_uid=s->agent_uid;
    agora_rtc_event_handler_t events={0};
    events.on_join_channel_success=joined;
    events.on_rejoin_channel_success=joined;
    events.on_connection_lost=lost;
    events.on_error=error;
    events.on_audio_data=received;
    rtc_service_option_t service={0};
    service.area_code=AREA_CODE_GLOB;
    service.log_cfg.log_disable=false;
    service.log_cfg.log_level=RTC_LOG_WARNING;
    service.log_cfg.log_path="agora";
    int rc=agora_rtc_init(s->app_id,&events,&service);
    if(rc<0) return ESP_FAIL;
    initialized=true;
    rc=agora_rtc_create_connection(&connection);
    if(rc<0) { rtc_leave(); return ESP_FAIL; }
    created=true;
    rtc_channel_options_t options={0};
    options.auto_subscribe_audio=true;
    options.auto_subscribe_video=false;
    options.audio_codec_opt.audio_codec_type=AUDIO_CODEC_DISABLED;
// This SDK build has no Opus codec inside, so codecs run in audio.c.
    rc=agora_rtc_join_channel(connection,s->channel,s->uid,s->token,&options);
    if(rc<0) { ESP_LOGE(TAG,"Join failed: %s",agora_rtc_err_2_str(rc)); rtc_leave(); return ESP_FAIL; }
    return ESP_OK;
}
void rtc_leave(void) {
    atomic_store(&connected,false);
    if(created) { agora_rtc_leave_channel(connection); agora_rtc_destroy_connection(connection); created=false; }
    if(initialized) { agora_rtc_fini(); initialized=false; }
}
bool rtc_connected(void) { return atomic_load(&connected); }
int rtc_send_opus(const void *packet,size_t len) {
    audio_frame_info_t frame={0}; frame.data_type=AUDIO_DATA_TYPE_OPUS;
    return agora_rtc_send_audio_data(connection,packet,len,&frame);
}
