#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "ff.h"

void audio_pipeline_init(void);
//void audio_pipeline_init(FIL *file);
void audio_pipeline_start(const char *filename);
void audio_pipeline_stop(void);
void skip_id3(FIL *fp);

size_t audio_pipeline_get_stream_bytes(void);
      
extern StreamBufferHandle_t pcm_stream;