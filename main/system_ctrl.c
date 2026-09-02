#include "system_ctrl.h"
#include "esp_log.h"

#define TAG "SYS"

static system_state_t state = SYS_BOOT;
static SemaphoreHandle_t state_lock = NULL;

void system_ctrl_init(void)
{
    state_lock = xSemaphoreCreateMutex();
    state = SYS_BT_INIT;

    ESP_LOGI(TAG, "System state machine initialized");
}

system_state_t system_ctrl_get_state(void)
{
    return state;
}

bool system_ctrl_set_state(system_state_t new_state)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);

    state = new_state;

    ESP_LOGI(TAG,"STATE -> %d",state);

    xSemaphoreGive(state_lock);
    return true;
}

bool system_ctrl_can_decode(void)
{
    system_state_t s = system_ctrl_get_state();

    return (s == SYS_STREAM_READY ||
            s == SYS_DECODER_RUNNING);
}

void system_ctrl_notify_bt_connected(void)
{
    system_ctrl_set_state(SYS_BT_CONNECTED);
}

void system_ctrl_notify_stream_ready(void)
{
    system_ctrl_set_state(SYS_STREAM_READY);
}