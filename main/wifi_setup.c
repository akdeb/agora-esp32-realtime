#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "app.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#if __has_include("bootstrap.h")
#include "bootstrap.h"
#else
#define DEFAULT_BACKEND_URL ""
#define DEFAULT_DEVICE_KEY ""
#endif

settings_t settings;
static EventGroupHandle_t events;
static httpd_handle_t http;
static atomic_bool provisioning=false;
static int attempts;
static int64_t ap_started, next_retry;
void captive_dns_task(void *arg);
#define ONLINE BIT0
#define FAILED BIT1
static const char *TAG = "wifi";
static const char page[] =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Elato setup</title><style>body{font:17px system-ui;max-width:420px;margin:40px auto;padding:20px}"
"input,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0}</style>"
"<h1>Connect Elato</h1><p>Use a 2.4 GHz Wi-Fi network.</p><form id=f>"
"<input name=ssid placeholder='Wi-Fi name' maxlength=32 required>"
"<input name=password type=password placeholder='Wi-Fi password (blank for open)' maxlength=64>"
"<details><summary>Connection settings</summary>"
"<input name=backend type=url placeholder='Backend URL: http://192.168.x.x:8080' value='" DEFAULT_BACKEND_URL "' required>"
"<input name=device_key type=password placeholder='Device key from backend setup' value='" DEFAULT_DEVICE_KEY "' required maxlength=95></details>"
"<button>Save and connect</button></form><p id=status></p><script>"
"f.onsubmit=async e=>{e.preventDefault();try{let r=await fetch('/save',{method:'POST',headers:"
"{'Content-Type':'application/json'},body:JSON.stringify(Object.fromEntries(new FormData(f)))});"
"document.getElementById('status').textContent=await r.text()}catch(e){document.getElementById('status').textContent='Connection lost; reconnect to ELATO if needed.'}}"
"</script>";

static esp_err_t home(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, page, HTTPD_RESP_USE_STRLEN);
}
static void reboot(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}
static bool copy_field(cJSON *j, const char *name, char *dst, size_t cap, bool required) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap ||
        (required && !v->valuestring[0])) return false;
    strlcpy(dst, v->valuestring, cap);
    return true;
}
static esp_err_t save(httpd_req_t *r) {
    char body[768];
    if (r->content_len <= 0 || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid settings");
    int n = 0;
    while (n < r->content_len) {
        int got = httpd_req_recv(r, body+n, r->content_len-n);
        if (got <= 0) return ESP_FAIL;
        n += got;
    }
    body[n] = 0;
    settings_t s = {0};
    cJSON *j = cJSON_Parse(body);
    bool valid = j && copy_field(j,"ssid",s.ssid,sizeof(s.ssid),true) &&
        copy_field(j,"password",s.password,sizeof(s.password),false) &&
        copy_field(j,"backend",s.backend,sizeof(s.backend),true) &&
        copy_field(j,"device_key",s.device_key,sizeof(s.device_key),true) &&
        (!strncmp(s.backend,"http://",7) || !strncmp(s.backend,"https://",8));
    cJSON_Delete(j);
    if (!valid) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Check fields and lengths");
    size_t len = strlen(s.backend);
    while (len && s.backend[len-1]=='/') s.backend[--len]=0;
    nvs_handle_t h;
    esp_err_t err = nvs_open("elato",NVS_READWRITE,&h);
    if (err==ESP_OK) {
        err=nvs_set_blob(h,"settings",&s,sizeof(s));
        if (err==ESP_OK) err=nvs_commit(h);
        nvs_close(h);
    }
    if (err!=ESP_OK) return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Could not save");
    httpd_resp_sendstr(r,"Saved. Rebooting; reconnect your phone to normal Wi-Fi.");
    xTaskCreate(reboot,"reboot",2048,NULL,3,NULL);
    return ESP_OK;
}
static void start_ap(void) {
    if (provisioning) return;
    provisioning=true;
    wifi_config_t c={0};
    strlcpy((char *)c.ap.ssid,"ELATO",sizeof(c.ap.ssid));
    c.ap.authmode=WIFI_AUTH_OPEN;
    c.ap.max_connection=2;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&c));
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.stack_size=6144;
    cfg.uri_match_fn=httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&http,&cfg));
    httpd_uri_t root={.uri="/*",.method=HTTP_GET,.handler=home};
    httpd_uri_t post={.uri="/save",.method=HTTP_POST,.handler=save};
    ESP_ERROR_CHECK(httpd_register_uri_handler(http,&root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http,&post));
    ap_started=esp_timer_get_time();
    ESP_LOGI(TAG,"Setup AP: ELATO (open). Open http://192.168.4.1/wifi");
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_START && settings.ssid[0]) esp_wifi_connect();
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events,ONLINE);
        xEventGroupSetBits(events,FAILED);
    }
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(events,FAILED);
        xEventGroupSetBits(events,ONLINE);
        ESP_LOGI(TAG,"Connected to Wi-Fi");
    }
}
static void manager(void *arg) {
    int64_t attempt_start=esp_timer_get_time();
    for(;;) {
        int64_t now=esp_timer_get_time();
        if(wifi_connected()) {
            attempts=0; next_retry=0; attempt_start=now;
            if(provisioning) {
                provisioning=false; esp_wifi_set_mode(WIFI_MODE_STA);
                if(http) { httpd_stop(http); http=NULL; }
            }
        } else if(provisioning) {
            wifi_sta_list_t clients={0};
            if(esp_wifi_ap_get_sta_list(&clients)==ESP_OK && clients.num) ap_started=now;
            if(now-ap_started>120LL*1000000) {
                provisioning=false;
                esp_wifi_set_mode(WIFI_MODE_STA);
                if(http) { httpd_stop(http); http=NULL; }
                attempts=0; next_retry=now+1500000;
                if(!settings.ssid[0]) start_ap();
            }
        } else if(!wifi_connected()) {
            if(xEventGroupGetBits(events)&FAILED) {
                xEventGroupClearBits(events,FAILED);
                if(++attempts>=3) start_ap();
                next_retry=now+1500000;
            }
            if(!settings.ssid[0]) start_ap();
            else if(!provisioning && next_retry && now>=next_retry) {
                next_retry=0; attempt_start=now; esp_wifi_connect();
            } else if(!provisioning && !next_retry && now-attempt_start>10000000) {
                // A connection attempt must not remain stuck indefinitely.
                esp_wifi_disconnect();
                // Disconnection event counts the failed attempt.
                next_retry=now+1500000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void wifi_setup_init(void) {
    nvs_handle_t h;
    if (nvs_open("elato",NVS_READONLY,&h)==ESP_OK) {
        size_t len=sizeof(settings);
        if (nvs_get_blob(h,"settings",&settings,&len)!=ESP_OK || len!=sizeof(settings)) memset(&settings,0,sizeof(settings));
        nvs_close(h);
    }
    if(!settings.backend[0]) strlcpy(settings.backend,DEFAULT_BACKEND_URL,sizeof(settings.backend));
    if(!settings.device_key[0]) strlcpy(settings.device_key,DEFAULT_DEVICE_KEY,sizeof(settings.device_key));
    // Preserve the production Arduino firmware's first saved network when NVS
    // is retained during flashing. Never copy its auth tokens or change it.
    if(!settings.ssid[0] && nvs_open("wifimanager",NVS_READONLY,&h)==ESP_OK) {
        size_t a=sizeof(settings.ssid),b=sizeof(settings.password);
        nvs_get_str(h,"apName0",settings.ssid,&a);
        nvs_get_str(h,"apPass0",settings.password,&b);
        nvs_close(h);
    }
    if(!settings.backend[0] || !settings.device_key[0]) settings.ssid[0]=0;
    events=xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (settings.ssid[0]) {
        wifi_config_t c={0};
        memcpy(c.sta.ssid,settings.ssid,strlen(settings.ssid));
        memcpy(c.sta.password,settings.password,strlen(settings.password));
        c.sta.pmf_cfg.capable=true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&c));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (!settings.ssid[0]) start_ap();
    xTaskCreate(captive_dns_task,"dns",3072,NULL,3,NULL);
    xTaskCreate(manager,"wifi_manager",4096,NULL,3,NULL);
}
bool wifi_wait(unsigned ms) { return (xEventGroupWaitBits(events,ONLINE,pdFALSE,pdFALSE,pdMS_TO_TICKS(ms))&ONLINE)!=0; }
bool wifi_connected(void) { return (xEventGroupGetBits(events)&ONLINE)!=0; }
bool wifi_provisioning(void) { return atomic_load(&provisioning); }
void wifi_forget(void) {
    nvs_handle_t h;
    if (nvs_open("elato",NVS_READWRITE,&h)==ESP_OK) { nvs_erase_key(h,"settings"); nvs_commit(h); nvs_close(h); }
    esp_restart();
}
