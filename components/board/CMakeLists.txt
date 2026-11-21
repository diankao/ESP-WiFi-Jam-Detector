# SPDX-License-Identifier: GPL-3.0-or-later
if(CONFIG_JD_BOARD_C5_MINI_KIT)
    set(BOARD_SRCS "board_c5_mini.c")
    set(BOARD_PRIV_REQS "")
else()
    set(BOARD_SRCS "board.c" "st7796.c")
    set(BOARD_PRIV_REQS "esp_driver_spi")
endif()

idf_component_register(SRCS ${BOARD_SRCS}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd
                       PRIV_REQUIRES ${BOARD_PRIV_REQS})
