#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    SYS_BOOT = 0,
    SYS_BT_INIT,
    SYS_BT_SCANNING,
    SYS_BT_CONNECTED,
    SYS_STREAM_READY,
    SYS_DECODER_RUNNING,
    SYS_PLAYING,
    SYS_PAUSED,
    SYS_TRACK_FINISHED,
    SYS_ERROR
} system_state_t;

void system_ctrl_init(void);

system_state_t system_ctrl_get_state(void);

bool system_ctrl_set_state(system_state_t new_state);

bool system_ctrl_can_decode(void);

// optional hooks
void system_ctrl_notify_bt_connected(void);
void system_ctrl_notify_stream_ready(void);