/*#include "decoder.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ringbuffer.h"
#include "system_ctrl.h"
#include "mp3dec.h"
#include "audio_pipeline.h"

#define TAG "DECODER"

static bool running = false;

static HMP3Decoder hMP3Decoder;

FILE *mp3_file;

bool decoder_running(void)
{
    ESP_LOGI(TAG,
             "decoder_running=%d",
             running);

    return running;
}

void decoder_init(void)
{
    hMP3Decoder = MP3InitDecoder();

    if (!hMP3Decoder) {
        ESP_LOGE(TAG, "MP3 decoder init failed");
        return;
    }

    ESP_LOGI(TAG, "MP3 decoder initialized");
}
*/