#pragma once

#include "esp_lcd_panel_rgb.h"

const esp_lcd_rgb_panel_event_callbacks_t *demo_get_isr_callbacks(void);
void demo_prepare(void);
void demo_start(void);
