#include <stdatomic.h>
#include "app.h"
#include "board.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG="elato";
static atomic_bool sleep_requested, interrupt_requested;
static void enter_sleep(session_t *session) {
    ui_set(UI_SLEEP);
    audio_set_session(false);
    rtc_leave();
    if(session && session->session_id[0]) control_action("stop",session);
    esp_wifi_stop();
    // Wait for release, then hold LEDs and amp off across deep sleep.
    while(!gpio_get_level(BUTTON_GPIO)) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(AMP_ENABLE,0);
    gpio_set_level(LED_R,1); gpio_set_level(LED_G,1); gpio_set_level(LED_B,1);
    gpio_hold_en(AMP_ENABLE); gpio_hold_en(LED_R); gpio_hold_en(LED_G); gpio_hold_en(LED_B);
    gpio_deep_sleep_hold_en();
    ESP_ERROR_CHECK(rtc_gpio_init(BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(BUTTON_GPIO,RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BUTTON_GPIO));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_GPIO,0));
    esp_deep_sleep_start();
}
static void buttons(void *arg) {
    gpio_config_t cfg={.pin_bit_mask=1ULL<<BUTTON_GPIO,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    unsigned held=0;
    int64_t last_click=0;
    // A wake press must be released before registering new gestures.
    while(!gpio_get_level(BUTTON_GPIO)) vTaskDelay(pdMS_TO_TICKS(20));
    for(;;) {
        if(!gpio_get_level(BUTTON_GPIO)) {
            held++;
        } else if(held) {
            int64_t now=esp_timer_get_time();
            if(held>=75) {                                   // hold >= 1.5 s, then release
                atomic_store(&sleep_requested,true);
            } else if(held>=2) {                             // debounced short click
                if(last_click && now-last_click<DOUBLE_CLICK_MS*1000LL) {
                    atomic_store(&sleep_requested,true);     // double click
                } else if(audio_interrupt()) {
                    // Tap while the agent talks: speaker is already cut and the mic
                    // re-opened; the main loop tells the agent to stop generating.
                    atomic_store(&interrupt_requested,true);
                }
                last_click=now;
            }
            held=0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void app_main(void) {
    esp_err_t err=nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err=nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    gpio_deep_sleep_hold_dis();
    rtc_gpio_deinit(BUTTON_GPIO);
    atomic_init(&sleep_requested,false);
    session_rtc_init(); ui_init(); audio_init();
    xTaskCreate(buttons,"button",3072,NULL,3,NULL);
    wifi_setup_init();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_init();
    for(;;) {
        static session_t session;
        if(atomic_load(&sleep_requested)) enter_sleep(NULL);
        if(!wifi_wait(1000)) continue;
        ui_set(UI_PROCESSING);
        if(control_create(&session)!=ESP_OK) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        if(rtc_join(&session)!=ESP_OK) { control_action("stop",&session); vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        int waited=0;
        while(!rtc_connected() && wifi_connected() && waited++<150 && !atomic_load(&sleep_requested)) vTaskDelay(pdMS_TO_TICKS(100));
        if(rtc_connected()) {
            // Be ready for greeting audio before asking the server to start it.
            audio_set_session(true);
            if(control_action("start",&session)==ESP_OK) {
                int64_t start=esp_timer_get_time(),heartbeat=start;
                while(wifi_connected() && rtc_connected() && !atomic_load(&sleep_requested) && esp_timer_get_time()-start<50LL*60*1000000) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if(atomic_exchange(&interrupt_requested,false)) control_action("interrupt",&session);
                    if(esp_timer_get_time()-heartbeat>20LL*1000000) {
                        if(control_action("heartbeat",&session)!=ESP_OK) break;
                        heartbeat=esp_timer_get_time();
                    }
                }
            }
        }
        if(atomic_load(&sleep_requested)) enter_sleep(&session);
        audio_set_session(false); // Serializes against uplink before RTC teardown.
        rtc_leave();
        control_action("stop",&session);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
