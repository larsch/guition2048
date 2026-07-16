#pragma once

#include "esp_lcd_panel_rgb.h"

void game_2048_init(void);
void game_2048_start(void);
const esp_lcd_rgb_panel_event_callbacks_t *game_get_isr_callbacks(void);
