#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
 
// modules
#include "ringbuffer.h"
#include "bluetooth.h"
#include "system_ctrl.h"
#include "audio_pipeline.h"
#include "sdcard.h"
#include "i2c.c"

#define TAG "MAIN"

#define MAX_TRACKS 256
#define MAX_NAME_LEN 64
static volatile bool random_playback = true;
//#define RANDOM_PLAYBACK  1       // 0 = sequential, 1 = random
#define INITIAL_PRIME     (8 * 1024)

////I2C Commands
#define CMD_PREV            0x01
#define CMD_PLAYPAUSE       0x02
#define CMD_NEXT            0x03
#define CMD_SHUFFLE         0x04
#define STATUS_PLAYING      0x20
#define STATUS_PAUSED       0x21
#define STATUS_STOPPED      0x22
#define STATUS_SEQUENTIAL   0x23
#define STATUS_RANDOM       0x24
#define STATUS_TRACK        0x30

static volatile uint8_t pending_command = 0;
/*
0x01  PREV
0x02  PLAY/PAUSE
0x03  NEXT
0x10  STATUS REQUEST
0x20  PLAYING
0x21  PAUSED
0x22  STOPPED
0x23 SEQUENTIAL
0x24 RANDOM
0x30  TRACK INFO
*/

static char (*tracks)[MAX_NAME_LEN];
//static FIL current_file_handle; 
//static bool current_file_is_open = false;
static int track_count = 0;
static int current_track = -1; // Was 0

#define MOUNT_POINT "/sdcard"

static void app_task(void *arg);
static bool start_track(int index);
static int select_next_track(void);

void app_command(uint8_t command);

void app_command(uint8_t command)
{
    pending_command = command;
}

int app_get_current_track(void)
{
    return current_track;
}

int app_get_track_count(void)
{
    return track_count;
}

const char *app_get_current_track_name(void)
{
    if (current_track < 0 ||
        current_track >= track_count ||
        tracks == NULL)
    {
        return NULL;
    }

    return tracks[current_track];
}

bool app_get_random_playback(void)
{
    return random_playback;
}

uint8_t app_get_playback_status(void)
{
    switch (system_ctrl_get_state())
    {
        case SYS_PLAYING:
            return 0x01;

        case SYS_PAUSED:
            return 0x02;

        default:
            return 0x00;
    }
}
void app_get_status_packet(uint8_t *packet, size_t len)
{
    if (packet == NULL || len < 32)
        return;

    memset(packet, 0, 32);

    // Byte 0: packet type
    packet[0] = 0x30;

    // Byte 1: playback state
    switch (system_ctrl_get_state())
    {
        case SYS_PLAYING:
            packet[1] = 1;
            break;

        case SYS_BOOT:
        case SYS_BT_INIT:
        case SYS_BT_SCANNING:
        case SYS_BT_CONNECTED:
        case SYS_STREAM_READY:
        case SYS_DECODER_RUNNING:
        case SYS_TRACK_FINISHED:
        case SYS_ERROR:
        default:
            packet[1] = 0;
            break;
    }

    // Byte 2: playback mode
    packet[2] = random_playback ? 1 : 0;

    // Byte 3-4: current track number
    uint16_t track = 0;

    if (current_track >= 0)
        track = (uint16_t)(current_track + 1);

    packet[3] = track & 0xFF;
    packet[4] = (track >> 8) & 0xFF;

    // Byte 5-6: total tracks
    uint16_t total = (uint16_t)track_count;

    packet[5] = total & 0xFF;
    packet[6] = (total >> 8) & 0xFF;

    // Byte 7-31: filename
    if (current_track >= 0 &&
        current_track < track_count &&
        tracks != NULL)
    {
        strncpy((char *)&packet[7],
                tracks[current_track],
                25);

        packet[31] = '\0';
    }
}
// ============================================================================
// FATFS SD CARD DIRECTORY SCANNER
// ============================================================================
static void scan_sd_tracks(void)
{
    DIR *dir = opendir(MOUNT_POINT);

    if (!dir)
    {
        ESP_LOGE(TAG, "Cannot open %s", MOUNT_POINT);
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;
        int len = strlen(name);

        if (len > 4 &&
            strcasecmp(name + len - 4, ".mp3") == 0)
        {
            strncpy(
                tracks[track_count],
                name,
                MAX_NAME_LEN - 1
            );

            tracks[track_count][MAX_NAME_LEN - 1] = '\0';

            ESP_LOGI(
                TAG,
                "Track %d: %s",
                track_count,
                tracks[track_count]
            );

            track_count++;

            if (track_count >= MAX_TRACKS)
                break;
        }
    }

    closedir(dir);

    ESP_LOGI(
        TAG,
        "Found %d MP3 files",
        track_count
    );
}
static int select_next_track(void)
{
    if (track_count <= 0)
        return -1;

    if (random_playback)
    {
        // Pick a random track, but never the same track that just finished.
        if (track_count == 1)
        {
            current_track = 0;
        }
        else
        {
            int next_track;

            do
            {
                next_track = esp_random() % track_count;
            }
            while (next_track == current_track);

            current_track = next_track;
        }
    }
    else
    {
   
        // Sequential playback: 0 -> 1 -> 2 -> ... -> last -> 0
        current_track++;

        if (current_track >= track_count)
            current_track = 0;
    }

    ESP_LOGI(TAG,
             "Selected %s track %d/%d: %s",
             random_playback ? "random" : "sequential",
             current_track + 1,
             track_count,
             tracks[current_track]);

    return current_track;
}

static bool start_track(int index)
{
    if (index < 0 || index >= track_count)
        return false;

    ESP_LOGI(TAG, "Starting track %d/%d: %s",
             index + 1,
             track_count,
             tracks[index]);

    //audio_pipeline_init();

    audio_pipeline_start(tracks[index]);

    bluetooth_play();

    system_ctrl_set_state(SYS_PLAYING);

    return true;
}

static void play_next_track(void)
{
    audio_pipeline_stop();

    current_track = select_next_track();

    if (current_track >= 0)
    {
        start_track(current_track);
    }
}
static void play_previous_track(void)
{
    audio_pipeline_stop();

    if (track_count <= 0)
        return;

    current_track--;

    if (current_track < 0)
        current_track = track_count - 1;

    ESP_LOGI(TAG,
             "Previous track %d/%d: %s",
             current_track + 1,
             track_count,
             tracks[current_track]);

    start_track(current_track);
}

static void toggle_play_pause(void)
{
    system_state_t state = system_ctrl_get_state();

    if (state == SYS_PLAYING)
    {
        ESP_LOGI(TAG, "PAUSE command");

        bluetooth_pause();

        system_ctrl_set_state(SYS_PAUSED);
    }
    else if (state == SYS_PAUSED)
    {
        ESP_LOGI(TAG, "PLAY command");

        bluetooth_play();

        system_ctrl_set_state(SYS_PLAYING);
    }
    else
    {
        ESP_LOGI(TAG,
                 "PLAY command while state=%d",
                 state);

        bluetooth_play();

        system_ctrl_set_state(SYS_PLAYING);
    }
}
// ============================================================================
// PART 5: CLEAN TRACK MANAGEMENT ENGINE (app_task)
// ============================================================================
static void app_task(void *arg)
{
    ESP_LOGI(TAG, "APP TASK STARTED");
    while (1)
    {
        if (track_count == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (system_ctrl_get_state() == SYS_TRACK_FINISHED)
        {
            ESP_LOGI(TAG, "Track finished");

            audio_pipeline_stop();

            current_track = select_next_track();

            if (current_track < 0)
            {
                ESP_LOGE(TAG, "Failed to select next track");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            ESP_LOGI(TAG,
                     "Starting %s track %d/%d: %s",
                     random_playback ? "random" : "next",
                     current_track + 1,
                     track_count,
                     tracks[current_track]);

            system_ctrl_set_state(SYS_BT_CONNECTED);

            if (!start_track(current_track))
            {
                ESP_LOGE(TAG,
                         "Failed to start track %d: %s",
                         current_track,
                         tracks[current_track]);

                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        if (pending_command != 0)
        {
            uint8_t cmd = pending_command;
            pending_command = 0;

            switch (cmd)
            {
                case CMD_PREV:
                    ESP_LOGI(TAG, "APP: PREVIOUS");
                    play_previous_track();
                    break;

                case CMD_PLAYPAUSE:
                    ESP_LOGI(TAG, "APP: PLAY/PAUSE");
                    toggle_play_pause();
                    break;

                case CMD_NEXT:
                    ESP_LOGI(TAG, "APP: NEXT");
                    play_next_track();
                    break;

                case CMD_SHUFFLE:
                    random_playback = !random_playback;
                    ESP_LOGI(TAG, "Playback mode: %s", random_playback ? "RANDOM" : "SEQUENTIAL");
                    break;

                default:
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// PART 6: BOOT MANAGEMENT ENGINE (app_main)
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Booting Offline BT Player");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    system_ctrl_init();
    i2c_slave_init();
    // Core initial execution setup
    audio_pipeline_init();
    //audio_pipeline_init(NULL);
    bluetooth_init();

    ESP_LOGI(TAG, "Waiting for BT...");
    while (system_ctrl_get_state() != SYS_BT_CONNECTED)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "BT audio ready");

    sdcard_init();

    tracks = heap_caps_malloc(MAX_TRACKS * MAX_NAME_LEN, MALLOC_CAP_SPIRAM);
    if (!tracks)
    {
        ESP_LOGE(TAG, "Failed to allocate playlist");
        return;
    }
    memset(tracks, 0, MAX_TRACKS * MAX_NAME_LEN);

    track_count = 0;
    current_track = -1;

    scan_sd_tracks();

    if (track_count == 0)
    {
        ESP_LOGE(TAG, "No MP3 files found");
        return;
    }

    ESP_LOGI(TAG,
            "Playlist contains %d tracks",
            track_count);
    ESP_LOGI(TAG,
            "Before app_task: free=%u internal=%u dma=%u",
            (unsigned)esp_get_free_heap_size(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));

    TaskHandle_t app_handle = NULL;

    BaseType_t result = xTaskCreatePinnedToCore(
        app_task, "app_task", 4096, NULL, 3, &app_handle, 0);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "FAILED TO CREATE app_task! result=%d", result);
    }
    else
    {
        ESP_LOGI(TAG, "app_task CREATED successfully");
    }

    current_track = select_next_track();

    ESP_LOGI(TAG,
            "Initial %s track %d/%d: %s",
            random_playback ? "random" : "first",
            current_track + 1,
            track_count,
            tracks[current_track]);

    if (!start_track(current_track))
    {
        ESP_LOGE(TAG, "Failed to start first track");
        return;
    }

    ESP_LOGI(TAG, "System fully started");
}

