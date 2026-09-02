#include "audio_pipeline.h"
#include "ringbuffer.h"
#include "bluetooth.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "mp3dec.h"
#include "system_ctrl.h"

#include <string.h>
#include <assert.h>

#define TAG "AUDIO"

// ---------------- STREAM BUFFERS ----------------
#define MP3_STREAM_SIZE   (256 * 1024)
#define READ_CHUNK        (96 * 1024) //was 4 // Tried 32, works on some tracks
#define DECODE_BUF_SIZE   (32 * 1024)
#define MP3_PREBUFFER_SIZE  (128 * 1024)

static StreamBufferHandle_t mp3_stream = NULL;
static StaticStreamBuffer_t mp3_struct;
static uint8_t *mp3_storage = NULL;
static uint8_t decode_buf[DECODE_BUF_SIZE] __attribute__((aligned(4)));
static int decode_fill = 0;
static int16_t pcm_out[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP] __attribute__((aligned(4)));
static volatile bool decoder_finished = false;

// ---------------- FILE ----------------
static volatile bool running = false;
static FIL native_file;
static bool file_is_open = false;

// ---------------- TASKS ----------------
static TaskHandle_t reader_task = NULL;
static TaskHandle_t decoder_task = NULL;

// Forward declarations
void skip_id3(FIL *fp);

// =================================================
// SD READER → MP3 STREAM
// =================================================
static void sd_reader_task(void *arg)
{
    ESP_LOGI(TAG, "SD reader started natively");

    uint8_t *buf = heap_caps_malloc(
        READ_CHUNK,
        MALLOC_CAP_DMA |
        MALLOC_CAP_INTERNAL |
        MALLOC_CAP_8BIT);

    if (!buf)
    {
        ESP_LOGE(TAG, "Failed to allocate SD buffer");
        running = false;
        reader_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    //bool sd_paused = false;

    while (running)
    {
        UINT bytes_read = 0;

        FRESULT res = f_read(
            &native_file,
            buf,
            READ_CHUNK,
            &bytes_read);

        if (res != FR_OK)
        {
            ESP_LOGE(TAG, "f_read failed: %d", res);
            running = false;
            break;
        }

        if (bytes_read == 0)
        {
            decoder_finished = true;
            break;
        }

        size_t sent = 0;

        while (sent < bytes_read && running)
        {
            size_t remaining = bytes_read - sent;
            size_t chunk = remaining;

            if (chunk > (8 * 1024))
                chunk = (8 * 1024); // Limit chunk size to avoid blocking too long
          
            size_t n = xStreamBufferSend(
                mp3_stream,
                buf + sent,
                chunk,
                //bytes_read - sent,
                //portMAX_DELAY); //Don't use, blocks next track
                pdMS_TO_TICKS(1));

            if (n > 0)
            {
                sent += n;
            }
            else if (!running)
            {
                break;
            }
            else
            {
                vTaskDelay(2);
            }
        }
    }

    heap_caps_free(buf);

    ESP_LOGI(TAG, "SD reader task terminated");

    reader_task = NULL;
    vTaskDelete(NULL);
}

// =================================================
// MP3 DECODER → PCM STREAM
// =================================================
static void decoder_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Decoder started natively");
    HMP3Decoder dec = MP3InitDecoder();

    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        decoder_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (system_ctrl_get_state() < SYS_BT_CONNECTED)
    {
        if (!running) {
            MP3FreeDecoder(dec);
            decoder_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    decode_fill = 0;
    uint32_t decode_count = 0;
    uint32_t error_count = 0;

    //// Decoder timing statistics for debug////
    uint32_t decode_frames = 0;
    int64_t last_stats = esp_timer_get_time();

    ESP_LOGI(TAG, "Waiting for SD Card to pre-buffer raw MP3 data...");

    while (xStreamBufferBytesAvailable(mp3_stream) < MP3_PREBUFFER_SIZE)
    {
        if (!running) {
            MP3FreeDecoder(dec);
            decoder_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        if (decoder_finished)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    ESP_LOGI(TAG, "Pre-buffering complete! Starting decoder. stream=%u", 
            (unsigned)xStreamBufferBytesAvailable(mp3_stream));

    while (running)
    {
        if (decode_fill < 0 || decode_fill > DECODE_BUF_SIZE)
        {
            ESP_LOGE(TAG, "BAD decode_fill=%d resetting", decode_fill);
            decode_fill = 0;
        }

        if (decode_fill < 1024)
        {
            size_t got = xStreamBufferReceive(
                mp3_stream,
                decode_buf + decode_fill,
                DECODE_BUF_SIZE - decode_fill,
                pdMS_TO_TICKS(10));
                //portMAX_DELAY);

            if (got > 0)
            {
                decode_fill += got;
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(2));

            if (decoder_finished && xStreamBufferBytesAvailable(mp3_stream) == 0)
            {
                break;
            }

            continue;
        }

        unsigned char *read_ptr = decode_buf;
        int bytes_left = decode_fill;

        while (running && bytes_left > 512)
        {
            int sync = MP3FindSyncWord(read_ptr, bytes_left);

            if (sync < 0)
            {
                ESP_LOGW(TAG,"Skipping %d bytes looking for sync", sync);
                // Keep last few bytes in case they are the beginning of sync
                if (bytes_left > 256)
                {
                    read_ptr += bytes_left - 256;
                    bytes_left = 256;
                }
                break;
            }

            read_ptr += sync;
            bytes_left -= sync;

            unsigned char *frame_start = read_ptr;
            int frame_bytes = bytes_left;
            int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm_out, 0);

            decode_frames++;
//DEBUG
            decode_count++;

            if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW)
            {
                if (decode_count < 40)
                {
                    ESP_LOGI(TAG, "MP3 underflow err=%d remaining=%d", err, bytes_left);
                }

                read_ptr = frame_start;
                bytes_left = frame_bytes;
                break;
            }

            if (err != ERR_MP3_NONE)
            {
                error_count++;

                if (error_count < 20 || (error_count % 100) == 0)
                {
                    ESP_LOGW(TAG,
                            "MP3 decode err=%d remaining=%d",
                            err,
                            bytes_left);
                }

                read_ptr = frame_start + 1;
                bytes_left = frame_bytes - 1;

                continue;
            }

            MP3FrameInfo info;

            MP3GetLastFrameInfo( dec, &info);

            static int last_bitrate = 0;

            if (info.bitrate != last_bitrate)
            {
                last_bitrate = info.bitrate;

                ESP_LOGI(TAG,
                    "MP3 FORMAT: %d Hz %d ch %d kbps",
                    info.samprate,
                    info.nChans,
                    info.bitrate / 1000);
            }

            size_t pcm_bytes = info.outputSamps * sizeof(int16_t);
            size_t pcm_capacity = sizeof(pcm_out);

            if (pcm_bytes == 0 || pcm_bytes > pcm_capacity)
            {
                ESP_LOGE(TAG,
                    "INVALID PCM size=%u samples=%d channels=%d hz=%d",
                    (unsigned)pcm_bytes, info.outputSamps, info.nChans, info.samprate);
                continue;
            }

            while (running && ringbuffer_free() < pcm_bytes)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (!running)
                break;

            ringbuffer_write(pcm_out, pcm_bytes);
//DEBUG
            int64_t now = esp_timer_get_time();
            if (now - last_stats >= 1000000)
            {
                last_stats = now;
                ESP_LOGI(TAG,
                    "DEC: frames=%u MP3=%u PCM=%u",
                    decode_frames,
                    (unsigned)xStreamBufferBytesAvailable(mp3_stream),
                    (unsigned)ringbuffer_available());
                decode_frames = 0;
            }
//DEBUG
        }

        if (bytes_left > 0 && read_ptr != decode_buf)
        {
            memmove( decode_buf, read_ptr, bytes_left);
        }

        decode_fill = bytes_left;

        if (decoder_finished && xStreamBufferBytesAvailable(mp3_stream) == 0) // && decode_fill <= 0)
        {
            ESP_LOGI(TAG, "Decoder processed all stream bytes. Track complete.");
            vTaskDelay(pdMS_TO_TICKS(50));
            decode_fill = 0;
            system_ctrl_set_state(SYS_TRACK_FINISHED);
            break;
        }
    }
    ESP_LOGI( TAG, "Decoder stopped frames=%u errors=%u", (unsigned)decode_count, (unsigned)error_count);
    MP3FreeDecoder(dec);
    decoder_task = NULL;
    vTaskDelete(NULL);
}

// =================================================
// PIPELINE LIFECYCLE MANAGEMENT INTERFACES
// =================================================
void audio_pipeline_init(void)
{
    decoder_finished = false;

    if (!mp3_storage)
    {
        mp3_storage = heap_caps_malloc(
            MP3_STREAM_SIZE,
            MALLOC_CAP_SPIRAM
        );

        assert(mp3_storage);

        mp3_stream = xStreamBufferCreateStatic(
            MP3_STREAM_SIZE,
            1,
            mp3_storage,
            &mp3_struct
        );

        assert(mp3_stream);

        ringbuffer_init();
    }
}

void audio_pipeline_start(const char *filename)
{
    if (running) return;

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "0:%s", filename); 

    FRESULT res = f_open(&native_file, full_path, FA_READ);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to open track natively over FatFS: %d", res);
        running = false;
        file_is_open = false;
        return;
    }
    file_is_open = true;
    skip_id3(&native_file);
    FSIZE_t pos = f_tell(&native_file);
    ESP_LOGI(TAG,"File position after skip_id3=%lu",(unsigned long)pos);
    running = true;

    xTaskCreatePinnedToCore(sd_reader_task, "sd_reader", 8192, NULL, 8, &reader_task, 1); //was 14/0
    xTaskCreatePinnedToCore(decoder_task_fn, "decoder", 8192, NULL, 8, &decoder_task, 0); //was 10/1
}

void audio_pipeline_stop(void)
{
    if (!running)
        return;

    ESP_LOGI(TAG, "Stopping audio pipeline");

    running = false;

    // Wait for worker tasks to actually terminate.
    for (int i = 0; i < 200; i++)
    {
        if (reader_task == NULL && decoder_task == NULL)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (reader_task != NULL)
        ESP_LOGW(TAG, "Reader did not stop cleanly");

    if (decoder_task != NULL)
        ESP_LOGW(TAG, "Decoder did not stop cleanly");

    if (file_is_open)
    {
        f_close(&native_file);
        file_is_open = false;
    }

    // Now that the workers are gone, discard old data.  
    if (mp3_stream)
        xStreamBufferReset(mp3_stream);

    ringbuffer_reset();

    decode_fill = 0;
    decoder_finished = false;

    ESP_LOGI(TAG, "Audio pipeline stopped");
}

void skip_id3(FIL *fp)
{
    uint8_t buf[1024];
    UINT br;

    f_lseek(fp, 0);

    // Skip ID3
    uint8_t hdr[10];

    if (f_read(fp, hdr, 10, &br) != FR_OK || br != 10)
    {
        f_lseek(fp,0);
        return;
    }

    uint32_t start = 0;

    if (hdr[0]=='I' &&
        hdr[1]=='D' &&
        hdr[2]=='3')
    {
        uint32_t size =
            ((hdr[6]&0x7f)<<21) |
            ((hdr[7]&0x7f)<<14) |
            ((hdr[8]&0x7f)<<7) |
            (hdr[9]&0x7f);

        start = 10 + size;

        ESP_LOGI(TAG,
            "ID3 size=%lu start search=%lu",
            (unsigned long)size,
            (unsigned long)start);
    }

    f_lseek(fp,start);

    uint32_t pos=start;

    while(1)
    {
        if(f_read(fp,buf,sizeof(buf),&br)!=FR_OK || br < 4)
            break;

        for(int i=0;i<br-3;i++)
        {
            if(buf[i]==0xff &&
               (buf[i+1]&0xe0)==0xe0)
            {
                MP3FrameInfo info;

                HMP3Decoder test = MP3InitDecoder();

                if(test && MP3GetNextFrameInfo(
                       test,
                       &info,
                       &buf[i]) == ERR_MP3_NONE)
                {
                    MP3FreeDecoder(test);

                    uint32_t found = pos+i;

                    ESP_LOGI(TAG, "MP3 frame found at %lu", (unsigned long)found);

                    f_lseek(fp,found);
                    return;
                }

                if(test)
                    MP3FreeDecoder(test);
            }
        }

        pos += br;

        if(br < sizeof(buf))
            break;

        f_lseek(fp,pos);
    }

    ESP_LOGW(TAG,"No MP3 frame found");
    f_lseek(fp,start);
}

size_t audio_pipeline_get_stream_bytes(void) {
    if (mp3_stream) {
        return xStreamBufferBytesAvailable(mp3_stream);
    }
    return 0;
}

/*
#include "audio_pipeline.h"
#include "ringbuffer.h"
#include "bluetooth.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "mp3dec.h"
#include "system_ctrl.h"

#include <string.h>
#include <assert.h>

#define TAG "AUDIO"

// ---------------- STREAM BUFFERS ----------------
#define MP3_STREAM_SIZE   (64 * 1024)
#define READ_CHUNK        (4 * 1024) //was 4 // Tried 32
#define DECODE_BUF_SIZE   (16 * 1024) 
#define MP3_PREBUFFER_SIZE  (48 * 1024)

static StreamBufferHandle_t mp3_stream = NULL;
static StaticStreamBuffer_t mp3_struct;
static uint8_t *mp3_storage = NULL;
static uint8_t decode_buf[DECODE_BUF_SIZE] __attribute__((aligned(4)));
static int decode_fill = 0;
static int16_t pcm_out[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP] __attribute__((aligned(4)));
static volatile bool decoder_finished = false;

// ---------------- FILE ----------------
static volatile bool running = false;
static FIL native_file;
static bool file_is_open = false;

// ---------------- TASKS ----------------
static TaskHandle_t reader_task = NULL;
static TaskHandle_t decoder_task = NULL;

// Forward declarations
void skip_id3(FIL *fp);

// =================================================
// SD READER → MP3 STREAM
// =================================================
static void sd_reader_task(void *arg)
{
    ESP_LOGI(TAG, "SD reader started natively");

        // Replace heap_caps_aligned_alloc with explicit 4-byte aligned DMA-safe allocation
    uint8_t *buf = (uint8_t *)heap_caps_malloc(READ_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf) {
        // Explicitly clear memory to establish clean structure alignment registers
        memset(buf, 0, READ_CHUNK); 
    }

    //uint8_t *buf = heap_caps_aligned_alloc(32, READ_CHUNK, MALLOC_CAP_DMA | 
            //MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate SD read buffer");
        running = false;
        vTaskDelete(NULL);
        return;
    }

    //uint32_t chunk_counter = 0;

    while (running)
    {
        UINT bytes_read = 0;
        FRESULT res = f_read(&native_file, buf, READ_CHUNK, &bytes_read);
        if (res != FR_OK) {
            running = false;
            break;
        }

        if (bytes_read == 0) {
            decoder_finished = true;
            break;
        }

        size_t sent = 0;
        while (sent < (size_t)bytes_read && running) {
            size_t n = xStreamBufferSend(mp3_stream, buf + sent, (size_t)bytes_read - sent, portMAX_DELAY); 
            if (n > 0) {
                sent += n;
            } else {
                vTaskDelay(1); 
            }
        }

        //chunk_counter++;
        
        // CRITICAL: Only yield every 32 blocks (128KB processed).
        // At 26MHz, 128KB reads take less than 60ms. This prevents the 5000ms Watchdog
        // from tripping while removing 97% of your 10ms scheduler oversleeping penalties.
        //if (chunk_counter >= 72)
        //{
        //    chunk_counter = 0;
        //    vTaskDelay(pdMS_TO_TICKS(1)); // Sleep for 1 tick (10ms) to reset the Watchdog
        //}
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    heap_caps_free(buf); 
    ESP_LOGI(TAG, "SD reader task terminated safely");
    reader_task = NULL;
    vTaskDelete(NULL);
}

// =================================================
// MP3 DECODER → PCM STREAM
// =================================================
static void decoder_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Decoder started natively");
    HMP3Decoder dec = MP3InitDecoder();

    if (!dec) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        decoder_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (system_ctrl_get_state() < SYS_BT_CONNECTED)
    {
        if (!running) {
            MP3FreeDecoder(dec);
            decoder_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    decode_fill = 0;
    uint32_t decode_count = 0;
    uint32_t error_count = 0;
    ESP_LOGI(TAG, "Waiting for SD Card to pre-buffer raw MP3 data...");

    while (xStreamBufferBytesAvailable(mp3_stream) < MP3_PREBUFFER_SIZE)
    {
        if (!running) {
            MP3FreeDecoder(dec);
            decoder_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        if (decoder_finished)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "Pre-buffering complete! Starting decoder. stream=%u", 
            (unsigned)xStreamBufferBytesAvailable(mp3_stream));

    while (running)
    {
        if (decode_fill < 0 || decode_fill > DECODE_BUF_SIZE)
        {
            ESP_LOGE(TAG, "BAD decode_fill=%d resetting", decode_fill);
            decode_fill = 0;
        }

        if (decode_fill < 1024)
        {
            size_t got = xStreamBufferReceive(
                mp3_stream,
                decode_buf + decode_fill,
                DECODE_BUF_SIZE - decode_fill,
                portMAX_DELAY);

            if (got > 0)
            {
                decode_fill += got;
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(1));

            if (decoder_finished && xStreamBufferBytesAvailable(mp3_stream) == 0)
            {
                break;
            }

            continue;
        }

        unsigned char *read_ptr = decode_buf;
        int bytes_left = decode_fill;

        while (running && bytes_left > 512)
        {
            int sync = MP3FindSyncWord(read_ptr, bytes_left);

            if (sync < 0)
            {
                ESP_LOGW(TAG,"Skipping %d bytes looking for sync", sync);
                // Keep last few bytes in case they are the beginning of sync
                if (bytes_left > 256)
                {
                    read_ptr += bytes_left - 256;
                    bytes_left = 256;
                }
                break;
            }

            read_ptr += sync;
            bytes_left -= sync;

            unsigned char *frame_start = read_ptr;
            int frame_bytes = bytes_left;
            int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm_out, 0);

            decode_count++;

            if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW)
            {
                if (decode_count < 40)
                {
                    ESP_LOGI(TAG, "MP3 underflow err=%d remaining=%d", err, bytes_left);
                }

                read_ptr = frame_start;
                bytes_left = frame_bytes;
                break;
            }

            if (err != ERR_MP3_NONE)
            {
                error_count++;

                if (error_count < 20 || (error_count % 100) == 0)
                {
                    ESP_LOGW(TAG, "MP3 decode err=%d remaining=%d", err, bytes_left);
                }

                if (read_ptr <= frame_start)
                {
                    read_ptr = frame_start + 1;
                    bytes_left = frame_bytes - 1;
                }

                continue;
            }

            MP3FrameInfo info;

            MP3GetLastFrameInfo( dec, &info);

            size_t pcm_bytes = info.outputSamps * sizeof(int16_t);
            size_t pcm_capacity = sizeof(pcm_out);

            if (pcm_bytes == 0 || pcm_bytes > pcm_capacity)
            {
                ESP_LOGE(TAG,
                    "INVALID PCM size=%u samples=%d channels=%d hz=%d",
                    (unsigned)pcm_bytes, info.outputSamps, info.nChans, info.samprate);
                continue;
            }

            while (running && ringbuffer_free() < pcm_bytes)
            {
                vTaskDelay(pdMS_TO_TICKS(1));
                break;
            }

            if (!running)
                break;

            ringbuffer_write(pcm_out, pcm_bytes);
//DEBUG
            static int64_t last_log = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_log > 1000000)
            {
                last_log = now;
                ESP_LOGI(TAG, "PIPE: MP3=%u PCM=%u FREE=%u", 
                        (unsigned)xStreamBufferBytesAvailable(mp3_stream),
                        (unsigned)ringbuffer_available(),
                        (unsigned)ringbuffer_free());
            }
//DEBUG          
        }

        if (bytes_left > 0 && read_ptr != decode_buf)
        {
            memmove( decode_buf, read_ptr, bytes_left);
        }

        decode_fill = bytes_left;

        if (decoder_finished && xStreamBufferBytesAvailable(mp3_stream) == 0 && decode_fill <= 0)
        {
            ESP_LOGI(TAG, "Decoder processed all stream bytes. Track complete.");
            vTaskDelay(pdMS_TO_TICKS(250));
            system_ctrl_set_state(SYS_TRACK_FINISHED);
            break;
        }
    }
    ESP_LOGI( TAG, "Decoder stopped frames=%u errors=%u", (unsigned)decode_count, (unsigned)error_count);
    MP3FreeDecoder(dec);
    decoder_task = NULL;
    vTaskDelete(NULL);
}

// =================================================
// PIPELINE LIFECYCLE MANAGEMENT INTERFACES
// =================================================
void audio_pipeline_init(void)
{
    decoder_finished=false;

    if(!mp3_storage)
    {
        mp3_storage=heap_caps_malloc( MP3_STREAM_SIZE, MALLOC_CAP_SPIRAM);
        assert(mp3_storage);
        mp3_stream=xStreamBufferCreateStatic(
             MP3_STREAM_SIZE,
             1,
             mp3_storage,
             &mp3_struct);

        ringbuffer_init();
    }
}

void audio_pipeline_start(const char *filename)
{
    if (running) return;

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "0:%s", filename); 

    FRESULT res = f_open(&native_file, full_path, FA_READ);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to open track natively over FatFS: %d", res);
        running = false;
        file_is_open = false;
        return;
    }
    file_is_open = true;
    skip_id3(&native_file);
    FSIZE_t pos = f_tell(&native_file);
    ESP_LOGI(TAG,"File position after skip_id3=%lu",(unsigned long)pos);
    running = true;

    xTaskCreatePinnedToCore(sd_reader_task, "sd_reader", 8192, NULL, 5, &reader_task, 1);
    xTaskCreatePinnedToCore(decoder_task_fn, "decoder", 8192, NULL, 6, &decoder_task, 1);
}

void audio_pipeline_stop(void)
{
    if (!running) return; // Already stopped
    
    running = false; // Signal tasks to exit their while loops

    while (reader_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Wait for the Decoder task to finish and delete itself
    while (decoder_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Now it is 100% safe to close the hardware file handle
    if (file_is_open) {
        f_close(&native_file);
        file_is_open = false;
    }

    ESP_LOGW(TAG, "RESETTING STREAM AND BUFFER MARGINS");
    if (mp3_stream) {
        xStreamBufferReset(mp3_stream);
    }
    ringbuffer_reset();
    
    decode_fill = 0;
    decoder_finished = false;
}
void skip_id3(FIL *fp)
{
    uint8_t buf[1024];
    UINT br;

    f_lseek(fp, 0);

    // Skip ID3
    uint8_t hdr[10];

    if (f_read(fp, hdr, 10, &br) != FR_OK || br != 10)
    {
        f_lseek(fp,0);
        return;
    }

    uint32_t start = 0;

    if (hdr[0]=='I' &&
        hdr[1]=='D' &&
        hdr[2]=='3')
    {
        uint32_t size =
            ((hdr[6]&0x7f)<<21) |
            ((hdr[7]&0x7f)<<14) |
            ((hdr[8]&0x7f)<<7) |
            (hdr[9]&0x7f);

        start = 10 + size;

        ESP_LOGI(TAG,
            "ID3 size=%lu start search=%lu",
            (unsigned long)size,
            (unsigned long)start);
    }

    f_lseek(fp,start);

    uint32_t pos=start;

    while(1)
    {
        if(f_read(fp,buf,sizeof(buf),&br)!=FR_OK || br < 4)
            break;

        for(int i=0;i<br-3;i++)
        {
            if(buf[i]==0xff &&
               (buf[i+1]&0xe0)==0xe0)
            {
                MP3FrameInfo info;

                HMP3Decoder test = MP3InitDecoder();

                if(test &&
                   MP3GetNextFrameInfo(
                       test,
                       &info,
                       &buf[i]) == ERR_MP3_NONE)
                {
                    MP3FreeDecoder(test);

                    uint32_t found = pos+i;

                    ESP_LOGI(TAG,
                        "MP3 frame found at %lu",
                        (unsigned long)found);

                    f_lseek(fp,found);
                    return;
                }

                if(test)
                    MP3FreeDecoder(test);
            }
        }

        pos += br;

        if(br < sizeof(buf))
            break;

        f_lseek(fp,pos);
    }

    ESP_LOGW(TAG,"No MP3 frame found");
    f_lseek(fp,start);
}

size_t audio_pipeline_get_stream_bytes(void) {
    if (mp3_stream) {
        return xStreamBufferBytesAvailable(mp3_stream);
    }
    return 0;
}
*/
//Audio almost keeps up