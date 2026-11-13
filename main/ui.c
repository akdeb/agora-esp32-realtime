#include <stdatomic.h>
#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_int state=UI_IDLE;
void ui_set(ui_state_t next) { if(atomic_load(&state)!=UI_SLEEP) atomic_store(&state,next); }
static void task(void *arg) {
    for(;;) {
        int s=atomic_load(&state);
        if(s!=UI_SLEEP && wifi_provisioning()) s=UI_SOFT_AP;
        bool r=false,g=false,b=false;
        switch(s) {
            case UI_IDLE: r=g=b=true; break;
            case UI_SOFT_AP: r=b=true; break;
            case UI_LISTENING: r=g=true; break;
            case UI_PROCESSING: g=true; break;
            case UI_SPEAKING: b=true; break;
            case UI_SLEEP: break;
        }
        gpio_set_level(LED_R,!r); gpio_set_level(LED_G,!g); gpio_set_level(LED_B,!b);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void ui_init(void) {
    gpio_config_t cfg={.pin_bit_mask=(1ULL<<LED_R)|(1ULL<<LED_G)|(1ULL<<LED_B),.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_hold_dis(LED_R); gpio_hold_dis(LED_G); gpio_hold_dis(LED_B);
    xTaskCreate(task,"led",2048,NULL,3,NULL);
}
