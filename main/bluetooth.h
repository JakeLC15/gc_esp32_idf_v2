#pragma once

#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void bluetooth_init(void);

bool bluetooth_connected(void);

void bluetooth_disconnect(void);

void bluetooth_play(void);

void bluetooth_pause(void);

void bluetooth_next(void);

void bluetooth_previous(void);

#ifdef __cplusplus
}
#endif