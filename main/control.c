#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

typedef struct {char data[4096]; size_t len; bool overflow;} response_t;
static esp_err_t event(esp_http_client_event_t *e) {
    response_t *r=e->user_data;
    if(e->event_id==HTTP_EVENT_ON_DATA) {
        if(r->len+e->data_len>=sizeof(r->data)) { r->overflow=true; return ESP_FAIL; }
        memcpy(r->data+r->len,e->data,e->data_len); r->len+=e->data_len; r->data[r->len]=0;
    }
    return ESP_OK;
}
static esp_err_t post(const char *path,const char *body,response_t *r) {
    char url[256],auth[112];
    snprintf(url,sizeof(url),"%s/%s",settings.backend,path);
    snprintf(auth,sizeof(auth),"Bearer %s",settings.device_key);
    esp_http_client_config_t cfg={.url=url,.method=HTTP_METHOD_POST,.timeout_ms=30000,
        .crt_bundle_attach=esp_crt_bundle_attach,.event_handler=event,.user_data=r};
    esp_http_client_handle_t h=esp_http_client_init(&cfg);
    if(!h) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(h,"Content-Type","application/json");
    esp_http_client_set_header(h,"Authorization",auth);
    esp_http_client_set_post_field(h,body,strlen(body));
    esp_err_t err=esp_http_client_perform(h);
    int code=esp_http_client_get_status_code(h);
    if(err!=ESP_OK || code!=200 || r->overflow) {
        ESP_LOGE("control","%s failed: HTTP %d, %s",path,code,esp_err_to_name(err));
        err=ESP_FAIL;
    }
    esp_http_client_cleanup(h);
    return err;
}
static bool field(cJSON *j,const char *name,char *dst,size_t cap) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(j,name);
    if(!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring)>=cap) return false;
    strlcpy(dst,v->valuestring,cap); return true;
}
esp_err_t control_create(session_t *s) {
    response_t r={0};
    if(post("session","{}",&r)!=ESP_OK) return ESP_FAIL;
    cJSON *j=cJSON_Parse(r.data);
    if(!j) return ESP_FAIL;
    bool ok=field(j,"app_id",s->app_id,sizeof(s->app_id)) &&
        field(j,"channel",s->channel,sizeof(s->channel)) &&
        field(j,"token",s->token,sizeof(s->token)) &&
        field(j,"session_id",s->session_id,sizeof(s->session_id));
    cJSON *uid=cJSON_GetObjectItemCaseSensitive(j,"uid"), *agent=cJSON_GetObjectItemCaseSensitive(j,"agent_uid");
    ok=ok && cJSON_IsNumber(uid) && uid->valuedouble>0 && cJSON_IsNumber(agent) && agent->valuedouble>0;
    if(ok) { s->uid=(uint32_t)uid->valuedouble; s->agent_uid=(uint32_t)agent->valuedouble; }
    cJSON_Delete(j); return ok?ESP_OK:ESP_FAIL;
}
esp_err_t control_action(const char *action,const session_t *s) {
    char body[100]; snprintf(body,sizeof(body),"{\"session_id\":\"%s\"}",s->session_id);
    response_t r={0}; return post(action,body,&r);
}
