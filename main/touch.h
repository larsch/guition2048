#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define TOUCH_MAX_CONTACTS  CONFIG_ESP_LCD_TOUCH_MAX_POINTS

typedef struct {
    volatile uint8_t  points;
    volatile uint16_t x[TOUCH_MAX_CONTACTS];
    volatile uint16_t y[TOUCH_MAX_CONTACTS];
} touch_state_t;

esp_err_t touch_init(void);
const touch_state_t *touch_get_state(void);
