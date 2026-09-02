#pragma once

#include <stdio.h>
#include "esp_err.h"

esp_err_t sdcard_init(void);

FILE *sdcard_open(char *filename);