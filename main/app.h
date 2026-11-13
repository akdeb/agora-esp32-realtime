#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char ssid[33], password[65], backend[192], device_key[96];
} settings_t;
typedef struct {
    char app_id[33], channel[64], token[2048], session_id[65];
    uint32_t uid, agent_uid;
} session_t;
extern settings_t settings;
typedef enum { UI_IDLE, UI_SOFT_AP, UI_LISTENING, UI_PROCESSING, UI_SPEAKING, UI_SLEEP } ui_state_t;
void ui_set(ui_state_t next);
void ui_init(void);
bool wifi_provisioning(void);
void wifi_setup_init(void);
bool wifi_wait(unsigned timeout_ms);
bool wifi_connected(void);
void wifi_forget(void);
void audio_init(void);
void audio_receive_opus(const void *data, size_t len);
void audio_set_session(bool enabled);
void audio_set_user_mute(bool muted);
bool audio_user_muted(void);
bool audio_interrupt(void);
void session_rtc_init(void);
esp_err_t rtc_join(const session_t *s);
void rtc_leave(void);
bool rtc_connected(void);
int rtc_send_opus(const void *packet, size_t len);
esp_err_t control_create(session_t *s);
esp_err_t control_action(const char *action, const session_t *s);
