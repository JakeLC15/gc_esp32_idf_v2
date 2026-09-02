#pragma once

#include <stdio.h>
#include <stdbool.h>

void decoder_init(void);
void decoder_start(FILE *file);
void decoder_stop(void);
bool decoder_running(void);
