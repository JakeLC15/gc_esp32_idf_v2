#include "sdcard.h"

#include "esp_log.h"

#include "esp_vfs_fat.h"

#include "driver/spi_common.h"

#include "driver/sdspi_host.h"

#include "sdmmc_cmd.h"

#define MOUNT_POINT "/sdcard"

#define PIN_NUM_MISO 2
#define PIN_NUM_MOSI 15
#define PIN_NUM_CLK 14
#define PIN_NUM_CS 13

//static const char *TAG="SD";

esp_err_t sdcard_init()
{
    spi_bus_config_t buscfg={
        .mosi_io_num=PIN_NUM_MOSI,
        .miso_io_num=PIN_NUM_MISO,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=96 * 1024,
        //.max_transfer_sz=4096,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI2_HOST,
            &buscfg,
            SPI_DMA_CH_AUTO));

    sdmmc_host_t host=SDSPI_HOST_DEFAULT();

    host.slot=SPI2_HOST;
    host.max_freq_khz = 20000; //started with 20k

    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();

    slot.host_id=SPI2_HOST;

    slot.gpio_cs=PIN_NUM_CS;

    esp_vfs_fat_sdmmc_mount_config_t mount={
        .format_if_mount_failed=false,
        .max_files=2,
        .allocation_unit_size=16384
        
    };

    sdmmc_card_t *card;

    esp_err_t ret=
        esp_vfs_fat_sdspi_mount(
            MOUNT_POINT,
            &host,
            &slot,
            &mount,
            &card);

    if(ret==ESP_OK)
        sdmmc_card_print_info(stdout,card);

    return ret;
}

FILE *sdcard_open(char *filename)
{
    static char path[256];

    sprintf(path,"%s/%s",MOUNT_POINT,filename);

    return fopen(path,"rb");
}
