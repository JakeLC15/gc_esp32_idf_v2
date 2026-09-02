#pragma once

#include <stdint.h>
#include <stddef.h>

void i2c_slave_init(void);

void i2c_send_status_request(void);

void app_get_status_packet(uint8_t *packet, size_t len);