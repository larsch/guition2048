#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_rgb.h"

esp_err_t display_init(const esp_lcd_rgb_panel_event_callbacks_t *isr_cbs);
esp_lcd_panel_handle_t display_get_panel(void);
