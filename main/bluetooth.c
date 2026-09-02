#include "bluetooth.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_log.h"

#include "system_ctrl.h"
#include "audio_pipeline.h"
#include "ringbuffer.h"

#define TAG "BT"
#define TARGET_NAME "BT5.0-Audio-PRO" //Wuhzi-audio
#define DISCOVERY_LENGTH 10
#define RETRY_DELAY_MS   1500

/* ------------------ State ------------------ */

static bool connected = false;
static bool connecting = false;
static bool discovery_running = false;
static bool audio_started = false;
static bool trying_saved_address = false;
static esp_bd_addr_t fallback_addr = {0};
static bool fallback_found = false;
static esp_bd_addr_t peer_addr = {0};
static bool have_saved_address = false;
static bool connection_task_pending = false;

/* ------------------ Forward declarations ------------------ */
static void gap_callback(
    esp_bt_gap_cb_event_t event,
    esp_bt_gap_cb_param_t *param);

static void a2dp_callback(
    esp_a2d_cb_event_t event,
    esp_a2d_cb_param_t *param);

static void connect_task(void *arg);

static int32_t bt_audio_cb(
    uint8_t *data,
    int32_t len);

static void start_discovery(void);

static void start_connection_task(void);

/* ------------------ NVS ------------------ */
static void load_saved_address(void)
{
    nvs_handle_t nvs;

    if (nvs_open("player", NVS_READONLY, &nvs) != ESP_OK)
        return;

    size_t size = sizeof(esp_bd_addr_t);

    if (nvs_get_blob(
            nvs,
            "speaker",
            peer_addr,
            &size) == ESP_OK)
    {
        if (size == sizeof(esp_bd_addr_t))
            have_saved_address = true;
    }

    nvs_close(nvs);
}

static void save_address(void)
{
    nvs_handle_t nvs;

    if (nvs_open("player", NVS_READWRITE, &nvs) != ESP_OK)
        return;

    esp_err_t err = nvs_set_blob(
        nvs,
        "speaker",
        peer_addr,
        sizeof(esp_bd_addr_t));

    if (err == ESP_OK)
        nvs_commit(nvs);

    nvs_close(nvs);
}

/* ------------------ Helpers ------------------ */
static void log_address(const char *prefix, const esp_bd_addr_t addr)
{
    ESP_LOGI(TAG,
             "%s%02X:%02X:%02X:%02X:%02X:%02X",
             prefix,
             addr[0],
             addr[1],
             addr[2],
             addr[3],
             addr[4],
             addr[5]);
}

static bool get_device_name(
    esp_bt_gap_cb_param_t *param,
    char *name,
    size_t name_size)
{
    if (!name || name_size == 0)
        return false;

    name[0] = '\0';

    for (int i = 0; i < param->disc_res.num_prop; i++)
    {
        esp_bt_gap_dev_prop_t *p =
            &param->disc_res.prop[i];

        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME)
        {
            size_t len = p->len;

            if (len >= name_size)
                len = name_size - 1;

            memcpy(name, p->val, len);
            name[len] = '\0';

            return true;
        }
    }

    return false;
}

/* ------------------ Discovery ------------------ */
static void start_discovery(void)
{
    if (connected)
        return;

    if (connecting)
        return;

    if (discovery_running)
        return;

    fallback_found = false;
    memset(fallback_addr, 0, sizeof(fallback_addr));

    discovery_running = true;

    ESP_LOGI(TAG, "Starting Bluetooth discovery...");

    esp_err_t err = esp_bt_gap_start_discovery(
        ESP_BT_INQ_MODE_GENERAL_INQUIRY,
        DISCOVERY_LENGTH,
        0);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Discovery failed: %s",
                 esp_err_to_name(err));

        discovery_running = false;

        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));

        if (!connected && !connecting)
            start_discovery();
    }
}

/* ------------------ Connection task ------------------ */
static void start_connection_task(void)
{
    if (connected)
        return;

    if (connecting)
        return;

    if (connection_task_pending)
        return;

    connection_task_pending = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        connect_task,
        "bt_connect",
        4096,
        NULL,
        5,
        NULL,
        0);

    if (result != pdPASS)
    {
        connection_task_pending = false;

        ESP_LOGE(TAG,
                 "Failed to create Bluetooth connection task");
    }
}

/* ------------------ API ------------------ */
bool bluetooth_connected(void)
{
    return connected;
}

void bluetooth_disconnect(void)
{
    if (connected)
        esp_a2d_source_disconnect(peer_addr);
}

void bluetooth_play(void)
{
    esp_a2d_media_ctrl(
        ESP_A2D_MEDIA_CTRL_START);
}

void bluetooth_pause(void)
{
    esp_a2d_media_ctrl(
        ESP_A2D_MEDIA_CTRL_SUSPEND);
}

void bluetooth_next(void)
{
    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_FORWARD,
        ESP_AVRC_PT_CMD_STATE_PRESSED);

    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_FORWARD,
        ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bluetooth_previous(void)
{
    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_BACKWARD,
        ESP_AVRC_PT_CMD_STATE_PRESSED);

    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_BACKWARD,
        ESP_AVRC_PT_CMD_STATE_RELEASED);
}

/* ------------------ INIT ------------------ */
void bluetooth_init(void)
{
    ESP_ERROR_CHECK(
        esp_bt_controller_mem_release(
            ESP_BT_MODE_BLE));

    esp_bt_controller_config_t cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_bt_controller_init(&cfg));

    ESP_ERROR_CHECK(
        esp_bt_controller_enable(
            ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(
        esp_bluedroid_init());

    ESP_ERROR_CHECK(
        esp_bluedroid_enable());

    ESP_ERROR_CHECK(
        esp_bt_gap_register_callback(
            gap_callback));

    ESP_ERROR_CHECK(
        esp_avrc_ct_init());

    ESP_ERROR_CHECK(
        esp_a2d_register_callback(
            a2dp_callback));

    ESP_ERROR_CHECK(
        esp_a2d_source_register_data_callback(
            bt_audio_cb));

    ESP_ERROR_CHECK(
        esp_a2d_source_init());

    esp_bt_gap_set_device_name(
        "ESP32-A2DP-SOURCE");

    load_saved_address();

    vTaskDelay(pdMS_TO_TICKS(1000));

    if (have_saved_address)
    {
        log_address(
            "Trying saved speaker ",
            peer_addr);

        trying_saved_address = true;

        start_connection_task();
    }
    else
    {
        ESP_LOGI(TAG,
                 "No saved speaker address");

        start_discovery();
    }
}

/* ------------------ GAP ------------------ */
static void gap_callback(
    esp_bt_gap_cb_event_t event,
    esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_DISC_RES_EVT:
    {
        if (connected || connecting)
            break;

        char name[
            ESP_BT_GAP_MAX_BDNAME_LEN + 1
        ] = {0};

        bool have_name =
            get_device_name(
                param,
                name,
                sizeof(name));

        log_address(
            "Found device ",
            param->disc_res.bda);

        if (have_name)
        {
            ESP_LOGI(TAG, "  Name: \"%s\"", name);
        }
        else
        {
            ESP_LOGI(TAG, "  Name: <unknown>");
        }

        if (!fallback_found)
        {
            memcpy(
                fallback_addr,
                param->disc_res.bda,
                sizeof(esp_bd_addr_t));

            fallback_found = true;

            ESP_LOGI(TAG, "Saved first device as fallback");
        }

        if (have_name &&
            strcmp(name, TARGET_NAME) == 0)
        {
            ESP_LOGI(TAG, "TARGET FOUND: %s", TARGET_NAME);

            memcpy(
                peer_addr,
                param->disc_res.bda,
                sizeof(esp_bd_addr_t));

            fallback_found = false;

            discovery_running = false;
            have_saved_address = true;
            save_address();

            esp_err_t err =
                esp_bt_gap_cancel_discovery();

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG,
                         "Cancel discovery: %s",
                         esp_err_to_name(err));
            }

            start_connection_task();
        }

        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
    {
        if (param->disc_st_chg.state ==
            ESP_BT_GAP_DISCOVERY_STARTED)
        {
            ESP_LOGI(TAG,
                     "Bluetooth discovery started");
        }
        else if (param->disc_st_chg.state ==
                 ESP_BT_GAP_DISCOVERY_STOPPED)
        {
            discovery_running = false;

            ESP_LOGI(TAG, "Bluetooth discovery stopped");

            if (connected || connecting)
                break;

            if (fallback_found)
            {
                memcpy(
                    peer_addr,
                    fallback_addr,
                    sizeof(esp_bd_addr_t));

                log_address(
                    "No target found. Using fallback ",
                    peer_addr);

                fallback_found = false;

                have_saved_address = true;
                save_address();

                start_connection_task();
            }
            else
            {
                ESP_LOGW(TAG, "No Bluetooth devices found");

                vTaskDelay(
                    pdMS_TO_TICKS(RETRY_DELAY_MS));

                if (!connected && !connecting)
                    start_discovery();
            }
        }

        break;
    }

    default:
        break;
    }
}

/* ------------------ A2DP ------------------ */
static void a2dp_callback(
    esp_a2d_cb_event_t event,
    esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        ESP_LOGI(TAG,
                 "A2DP state=%d reason=%d",
                 param->conn_stat.state,
                 param->conn_stat.disc_rsn);

        if (param->conn_stat.state ==
            ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            connected = true;
            connecting = false;
            connection_task_pending = false;
            trying_saved_address = false;
            have_saved_address = true;
            save_address();

            ESP_LOGI(TAG, "CONNECTED");

            log_address(
                "Connected device: ",
                peer_addr);

            system_ctrl_set_state(
                SYS_BT_CONNECTED);
        }

        else if (param->conn_stat.state ==
                 ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            connected = false;
            connecting = false;
            connection_task_pending = false;
            audio_started = false;

            ESP_LOGW(TAG,
                     "Disconnected, reason=%d",
                     param->conn_stat.disc_rsn);

            if (trying_saved_address)
            {
                trying_saved_address = false;

                ESP_LOGI(TAG, "Saved address failed");

                start_discovery();
            }
            else
            {
                ESP_LOGI(TAG, "Scanning for next available device");

                start_discovery();
            }
        }

        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:

        ESP_LOGI(TAG,
                 "Audio state=%d",
                 param->audio_stat.state);

        audio_started =
            (param->audio_stat.state ==
             ESP_A2D_AUDIO_STATE_STARTED);

        if (audio_started)
        {
            system_ctrl_set_state(
                SYS_STREAM_READY);
        }

        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:

        ESP_LOGI(TAG,
                 "MEDIA ACK cmd=%d status=%d",
                 param->media_ctrl_stat.cmd,
                 param->media_ctrl_stat.status);

        break;

    default:
        break;
    }
}

/* ------------------ A2DP audio callback ------------------ */
static int32_t bt_audio_cb(
    uint8_t *data,
    int32_t len)
{
    size_t got =
        ringbuffer_read(data, len);

    if (got < len)
    {
        memset(
            data + got,
            0,
            len - got);
    }

    return len;
}

/* ------------------ Connection task ------------------ */
static void connect_task(void *arg)
{
    connection_task_pending = false;

    vTaskDelay(
        pdMS_TO_TICKS(RETRY_DELAY_MS));

    if (connected)
    {
        connecting = false;
        vTaskDelete(NULL);
        return;
    }

    connecting = true;

    log_address(
        "Connecting to ",
        peer_addr);

    esp_err_t err =
        esp_a2d_source_connect(peer_addr);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Connect request failed: %s",
                 esp_err_to_name(err));

        connecting = false;

        if (!connected)
        {
            ESP_LOGI(TAG,
                     "Connection failed - starting discovery");

            start_discovery();
        }
    }

    vTaskDelete(NULL);
}

//Original workin BT
/*#include "bluetooth.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "system_ctrl.h"
#include "audio_pipeline.h"
#include "ringbuffer.h"

#define TAG "BT"

#define TARGET_NAME "BT5.0-Audio-PRO"
static bool connecting = false;
static bool discovery_running = false;

#define RETRY_DELAY_MS    1500
static bool ever_connected = false;
static bool connected = false;
//static bool waiting_for_pcm = false;
static bool audio_started = false;

static esp_bd_addr_t peer_addr = {0};
static bool have_saved_address = false;
static bool trying_saved_address = false;
static bool device_selected = false;
//static int64_t last_log_us = 0;

// ------------------ Forward ------------------
static void gap_callback(
        esp_bt_gap_cb_event_t event, 
        esp_bt_gap_cb_param_t *param);
static void a2dp_callback(
        esp_a2d_cb_event_t event, 
        esp_a2d_cb_param_t *param);
static void connect_task(void *arg);
static int32_t bt_audio_cb(
        uint8_t *data,
        int32_t len);

// ------------------ NVS ------------------

static void load_saved_address(void)
{
    nvs_handle_t nvs;
    if (nvs_open("player", NVS_READONLY, &nvs) != ESP_OK)
        return;

    size_t size = 6;
    if (nvs_get_blob(nvs, "speaker", peer_addr, &size) == ESP_OK)
        have_saved_address = true;

    nvs_close(nvs);
}

static void save_address(void)
{
    nvs_handle_t nvs;
    if (nvs_open("player", NVS_READWRITE, &nvs) != ESP_OK)
        return;

    nvs_set_blob(nvs, "speaker", peer_addr, 6);
    nvs_commit(nvs);
    nvs_close(nvs);
}
static void start_discovery(void)
{
    if (connected || connecting || discovery_running)
        return;

    device_selected = false;
    discovery_running = true;

    ESP_LOGI(TAG, "Starting Bluetooth discovery...");

    esp_err_t err = esp_bt_gap_start_discovery(
        ESP_BT_INQ_MODE_GENERAL_INQUIRY,
        10,
        0
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Discovery failed: %s",
                 esp_err_to_name(err));

        discovery_running = false;
    }
}
// ------------------ API ------------------

bool bluetooth_connected(void)
{
    return connected;
}

void bluetooth_disconnect(void)
{
    if (connected)
        esp_a2d_source_disconnect(peer_addr);
}

void bluetooth_play(void)
{
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
}

void bluetooth_pause(void)
{
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
}

void bluetooth_next()
{
    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_FORWARD,
        ESP_AVRC_PT_CMD_STATE_PRESSED);

    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_FORWARD,
        ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bluetooth_previous()
{
    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_BACKWARD,
        ESP_AVRC_PT_CMD_STATE_PRESSED);

    esp_avrc_ct_send_passthrough_cmd(
        0,
        ESP_AVRC_PT_CMD_BACKWARD,
        ESP_AVRC_PT_CMD_STATE_RELEASED);
}

//bluetooth init//
// ------------------ INIT ------------------ 

void bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(gap_callback);
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_callback));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_audio_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    //ESP_ERROR_CHECK(esp_avrc_ct_init());
    //ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_callback));   // optional if using AVRCP

    esp_bt_gap_set_device_name("ESP32-A2DP-SOURCE");

    load_saved_address();

    vTaskDelay(pdMS_TO_TICKS(1000));

    if (have_saved_address)
    {
        ESP_LOGI(TAG,
                "Trying saved speaker "
                "%02X:%02X:%02X:%02X:%02X:%02X",
                peer_addr[0],
                peer_addr[1],
                peer_addr[2],
                peer_addr[3],
                peer_addr[4],
                peer_addr[5]);

        trying_saved_address = true;

        xTaskCreatePinnedToCore(
            connect_task,
            "bt_connect",
            4096,
            NULL,
            5,
            NULL,
            1
        );
    }
    else
    {
        ESP_LOGI(TAG, "No saved speaker address");
        start_discovery();
    }
}

//Gap Callback//
// ------------------ GAP ------------------ //

static void gap_callback(esp_bt_gap_cb_event_t event,
                         esp_bt_gap_cb_param_t *param)
{
    ESP_LOGI(TAG, "GAP started");
    switch (event)
    {
    case ESP_BT_GAP_DISC_RES_EVT:
    {
        if (device_selected || connected || connecting)
            break;

        ESP_LOGI(TAG,
                "Found device [%02X:%02X:%02X:%02X:%02X:%02X]",
                param->disc_res.bda[0],
                param->disc_res.bda[1],
                param->disc_res.bda[2],
                param->disc_res.bda[3],
                param->disc_res.bda[4],
                param->disc_res.bda[5]);

        trying_saved_address = false;

        memcpy(peer_addr,
            param->disc_res.bda,
            sizeof(esp_bd_addr_t));

        device_selected = true;
        discovery_running = false;

        have_saved_address = true;
        save_address();

        esp_bt_gap_cancel_discovery();

        xTaskCreatePinnedToCore(
            connect_task,
            "bt_connect",
            4096,
            NULL,
            5,
            NULL,
            1
        );

        break;
    }

    default:
        break;
    }
}

//A2dp Callback//
// ------------------ A2DP ------------------ //
static void a2dp_callback(esp_a2d_cb_event_t event,
                          esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        ESP_LOGI(TAG,
                 "A2DP state=%d reason=%d",
                 param->conn_stat.state,
                 param->conn_stat.disc_rsn);

        if (param->conn_stat.state ==
            ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            connected = true;
            connecting = false;
            trying_saved_address = false;
            ever_connected = true;

            ESP_LOGI(TAG, "CONNECTED");

            system_ctrl_set_state(SYS_BT_CONNECTED);
        }
        else if (param->conn_stat.state ==
                 ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            connected = false;
            connecting = false;
            audio_started = false;

            ESP_LOGW(TAG,
                     "Disconnected, reason=%d",
                     param->conn_stat.disc_rsn);

            if (trying_saved_address)
            {
                trying_saved_address = false;

                ESP_LOGI(TAG,
                         "Saved address failed - starting discovery");

                start_discovery();
            }
            else
            {

                device_selected = false;

                ESP_LOGI(TAG,
                         "Starting discovery for next available device");

                start_discovery();
            }
        }

        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:

        ESP_LOGI(TAG,
                 "Audio state=%d",
                 param->audio_stat.state);

        audio_started =
            (param->audio_stat.state ==
             ESP_A2D_AUDIO_STATE_STARTED);

        if (audio_started)
            system_ctrl_set_state(SYS_STREAM_READY);

        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:

        ESP_LOGI(TAG,
                 "MEDIA ACK cmd=%d status=%d",
                 param->media_ctrl_stat.cmd,
                 param->media_ctrl_stat.status);

        break;

    default:
        break;
    }
}

static int32_t bt_audio_cb(uint8_t *data, int32_t len)
{
    size_t got = ringbuffer_read(data, len);

    if (got < len)
        memset(data + got,0,len-got);
    return len;
}

static void connect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));

    if (connected)
    {
        connecting = false;
        vTaskDelete(NULL);
        return;
    }

    connecting = true;

    ESP_LOGI(TAG,
             "Connecting to %02X:%02X:%02X:%02X:%02X:%02X...",
             peer_addr[0],
             peer_addr[1],
             peer_addr[2],
             peer_addr[3],
             peer_addr[4],
             peer_addr[5]);

    esp_err_t err = esp_a2d_source_connect(peer_addr);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Connect request failed: %s",
                 esp_err_to_name(err));

        connecting = false;

        if (have_saved_address)
        {
            trying_saved_address = false; //have_saved_address = false;
            start_discovery();
        }
    }

    vTaskDelete(NULL);
}
*/