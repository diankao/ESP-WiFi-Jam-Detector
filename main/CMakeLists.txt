# SPDX-License-Identifier: GPL-3.0-or-later
set(MAIN_SRCS
    "app_main.c"
    "audio.c"
    "bandplan.c"
    "detect.c"
    "detector.c"
    "dsp.c"
    "link.c"
    "link_proto.c"
    "ota_upgrade.c"
    "params.c"
    "settings.c"
    "settings_values.c"
    "sweep.c"
)

set(MAIN_PRIV_REQS
    app_update
    board
    esp_app_format
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_usb_serial_jtag
    esp_hw_support
    espsdr_rx
    esp_timer
    heap
    nvs_flash
    spi_flash
)

if(CONFIG_JD_BOARD_C5_MINI_KIT)
    list(APPEND MAIN_SRCS "status_led.c")
    if(CONFIG_JD_CHARACTERIZE)
        list(APPEND MAIN_SRCS "characterize.c")
    endif()
else()
    list(APPEND MAIN_SRCS
        "characterize.c"
        "eventlog.c"
        "lvgl_port.c"
        "plot.c"
        "ui.c"
        "ui_settings.c"
        "ui_spectrum.c"
        "ui_status.c"
    )
    list(APPEND MAIN_PRIV_REQS esp_driver_i2s esp_driver_sdspi fatfs sdmmc)
endif()

idf_component_register(SRCS ${MAIN_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${MAIN_PRIV_REQS})
