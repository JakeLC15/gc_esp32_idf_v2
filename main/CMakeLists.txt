idf_component_register(
    SRCS 
        "main.c"
        "bluetooth.c"
        "decoder.c"
        "sdcard.c"
        "ringbuffer.c"
        "system_ctrl.c"
        "audio_pipeline.c"
        "i2c.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        bt
        fatfs
        vfs
        driver
        nvs_flash
        esp_timer
        esp_system
        sdmmc
        esp_event
        chmorgan__esp-libhelix-mp3
)