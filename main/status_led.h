/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless status indicator (onboard WS2812B RGB LED + alarm beep trigger)
 * for the Waveshare ESP32-C5-MINI-KIT-U.
 */
#pragma once

#include "esp_err.h"

/* Start the status LED & alarm-beep monitoring task. Call after sweep_start()
 * and detector_start(). */
esp_err_t status_led_start(void);
