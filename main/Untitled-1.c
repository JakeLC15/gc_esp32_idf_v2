#include "audio_pipeline.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "mp3dec.h"
#include "system_ctrl.h"

#include <string.h>
#include <assert.h>

#define TAG "AUDIO"

// ---------------- STREAM BUFFERS ----------------
#define MP3_STREAM_SIZE   (8 * 1024)
#define PCM_STREAM_SIZE   (256 * 1024)
#define READ_CHUNK 4096

static StreamBufferHandle_t mp3_stream;
StreamBufferHandle_t pcm_stream = NULL;

static uint8_t decode_buf[8*1024];
static int decode_fill = 0;
static int64_t last_log_us = 0;

static StaticStreamBuffer_t pcm_struct;
static StaticStreamBuffer_t mp3_struct;

static uint8_t *mp3_storage = NULL;
static uint8_t *pcm_storage = NULL;

// ---------------- FILE ----------------
static FILE *mp3_file = NULL;
static volatile bool running = false;

// ---------------- TASKS ----------------
static TaskHandle_t reader_task;
static TaskHandle_t decoder_task;

// ---------------- PCM BUFFER ----------------
static short pcm_out[1152 * 2];

// =================================================
// SD READER → MP3 STREAM
// =================================================
static void sd_reader_task(void *arg)
{
    ESP_LOGI(TAG, "SD reader started");

    //uint8_t buf[2048];
    uint8_t *buf =
        heap_caps_malloc(
            READ_CHUNK,
            MALLOC_CAP_SPIRAM);

    while (running)
    {
        //size_t r = fread(buf, 1, sizeof(buf), mp3_file);
        size_t r =
            fread(
                buf,
                1,
                READ_CHUNK,
                mp3_file);

        if (r == 0)
        {
            ESP_LOGI(TAG,"Track finished");

            running = false;
            system_ctrl_set_state(SYS_TRACK_FINISHED);

            break;
        }
        size_t offset = 0;

        while(offset < r && running)
        {
            offset += xStreamBufferSend(
                            mp3_stream,
                            buf + offset,
                            r - offset,
                            portMAX_DELAY);
        }
    }

    ESP_LOGI(TAG, "SD reader stopped");


    vTaskDelete(NULL);
}

// =================================================
// MP3 DECODER → PCM STREAM
// =================================================
static void decoder_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Decoder started");

    HMP3Decoder dec = MP3InitDecoder();

    while(system_ctrl_get_state() < SYS_BT_CONNECTED)

    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    while (running)
    {
        if (decode_fill >= sizeof(decode_buf))
        {
            ESP_LOGE(TAG,
                    "decode buffer overflow fill=%d",
                    decode_fill);

            decode_fill = 0;
        }

        size_t got =
            xStreamBufferReceive(
                mp3_stream,
                decode_buf + decode_fill,
                sizeof(decode_buf)-decode_fill,
                portMAX_DELAY); //pdMS_TO_TICKS(1));

        decode_fill += got;
        if (decode_fill < 2048)
            continue;

        unsigned char *ptr = decode_buf;
        int bytes_left = decode_fill;

        int frames = 0;

        int decoded = 0;

        while (bytes_left > 128 && decoded < 4) // 8 or more crashed core 1
        {
            int sync = MP3FindSyncWord(ptr, bytes_left);

            if (sync < 0)
            {
                if (bytes_left > 512)
                {
                    memmove(
                        decode_buf,
                        ptr + bytes_left - 512,
                        512);

                    bytes_left = 512;
                }
                break;
            }

            ptr += sync;
            bytes_left -= sync;

            if (bytes_left <= 0)
                break;

            if (bytes_left > sizeof(decode_buf))
            {
                ESP_LOGE(TAG, "corrupt stream overflow");
                bytes_left = 0;
                break;
            }

            int err =
                MP3Decode(
                    dec,
                    &ptr,
                    &bytes_left,
                    pcm_out,
                    0);

            //ESP_LOGI(TAG,
              //      "got=%d fill=%d left=%d decoded=%d",
                //    got,
                  //  decode_fill,
                    //bytes_left,
                    //decoded);

            if (err != ERR_MP3_NONE)
            {
                if (err == ERR_MP3_INDATA_UNDERFLOW)
                {
                    break;   // WAIT FOR MORE DATA
                }
                //ESP_LOGE(TAG,
                //        "MP3Decode error=%d",
                //        err);
                continue;
            }

            MP3FrameInfo info;
            MP3GetLastFrameInfo(dec, &info);

            //ESP_LOGI(TAG,
            //        "channels=%d samplerate=%d bits=%d",
            //        info.nChans,
            //        info.samprate,
            //        info.bitsPerSample);

            size_t pcm_bytes =
                info.outputSamps * sizeof(short);

            //int64_t now = esp_timer_get_time();

            //if (now - last_log_us >= 1000000) 
            //{   // 1 second
            //    last_log_us = now;
                //ESP_LOGI(TAG, "Produced %d bytes", produced);
             //       ESP_LOGI(TAG,
            //                "PCM fill=%u free=%u"();
                //            ringbuffer_available(),
                //            ringbuffer_free());
            //}
            //ESP_LOGI(TAG,
            //        "pcm avail=%u free=%u",
            //        xStreamBufferBytesAvailable(pcm_stream),
            //        PCM_STREAM_SIZE -
            //        xStreamBufferBytesAvailable(pcm_stream));

            size_t sent =
                xStreamBufferSend(
                    pcm_stream,
                    pcm_out,
                    pcm_bytes,
                    portMAX_DELAY); //pdMS_TO_TICKS(1))

            decoded++;

            if (bytes_left < 0 || bytes_left > sizeof(decode_buf))
            {
                ESP_LOGE(TAG, "decoder corruption bytes_left=%d", bytes_left);
                bytes_left = 0;
                break;
            }

            if (sent != pcm_bytes)
            {
                vTaskDelay(1);
                break;
            }

            if (++frames >= 8)
            {
                frames = 0;
                taskYIELD();
            }

            static uint32_t produced = 0;

            produced += pcm_bytes;

            if (produced >= (44100 * 4))
            {
                ESP_LOGI(TAG, "Produced %u bytes", produced);
                produced = 0;
            }
            //ESP_LOGI(TAG,
            //        "pcm free=%u",
            //        PCM_STREAM_SIZE -
            //        xStreamBufferBytesAvailable(pcm_stream));
        }

        if (bytes_left > 0)
        {
            if (ptr != decode_buf)
            {
                memmove(decode_buf, ptr, bytes_left);
            }
        }
        else
        {
            bytes_left = 0;
        }

        decode_fill = bytes_left;
        //vTaskDelay(1);
    }
    MP3FreeDecoder(dec);

    ESP_LOGI(TAG,"Decoder stopped");

    vTaskDelete(NULL);
}

void audio_pipeline_start(void)
{
    if (running) return;
    running = true;

    xTaskCreatePinnedToCore(sd_reader_task, "sd_reader", 8192, NULL, 3, &reader_task, 0); //tryin core 1
    xTaskCreatePinnedToCore(decoder_task_fn, "decoder", 8192, NULL, 4, &decoder_task, 1);
}

void audio_pipeline_stop(void)
{
    running = false;

    if (mp3_file)
    {
        fclose(mp3_file);
        mp3_file = NULL;
    }

    ESP_LOGW(TAG, "RESETTING PCM STREAM");
    xStreamBufferReset(mp3_stream);
    xStreamBufferReset(pcm_stream);

    decode_fill = 0;
}

void audio_pipeline_init(FILE *file)
{
    mp3_file = file;

    if (mp3_file)
        skip_id3(mp3_file);

    if (!mp3_storage)
    {

        mp3_storage =
            heap_caps_malloc(
                MP3_STREAM_SIZE,
            //    MALLOC_CAP_INTERNAL |
            //    MALLOC_CAP_8BIT);
                MALLOC_CAP_SPIRAM);

        pcm_storage = heap_caps_malloc(
            PCM_STREAM_SIZE,
            //MALLOC_CAP_INTERNAL |
            //MALLOC_CAP_8BIT);
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_32BIT);

        assert(mp3_storage);
        assert(pcm_storage);

        mp3_stream =
            xStreamBufferCreateStatic(
                MP3_STREAM_SIZE,
                1,
                mp3_storage,
                &mp3_struct);

        pcm_stream =
            xStreamBufferCreateStatic(
                PCM_STREAM_SIZE,
                1,
                pcm_storage,
                &pcm_struct);

        assert(mp3_stream);
        assert(pcm_stream);
    }
    else
    {
        ESP_LOGW(TAG, "RESETTING PCM STREAM");
        xStreamBufferReset(mp3_stream);
        xStreamBufferReset(pcm_stream);
    }

    decode_fill = 0;

    ESP_LOGI(TAG,
             "mp3=%p pcm=%p",
             mp3_stream,
             pcm_stream);
}

void skip_id3(FILE *fp)
{
    uint8_t hdr[10];

    if (fread(hdr, 1, 10, fp) != 10)
    {
        fseek(fp, 0, SEEK_SET);
        return;
    }

    if (memcmp(hdr, "ID3", 3) == 0)
    {
        uint32_t size =
            ((hdr[6] & 0x7f) << 21) |
            ((hdr[7] & 0x7f) << 14) |
            ((hdr[8] & 0x7f) <<  7) |
            ((hdr[9] & 0x7f));

        ESP_LOGI(TAG,
                 "Skipping ID3v2 tag (%lu bytes)",
                 (unsigned long)(size + 10));

        fseek(fp, size + 10, SEEK_SET);
    }
    else
    {
        fseek(fp, 0, SEEK_SET);
    }
}