#include "touch.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "touch";

static touch_state_t s_touch;

const touch_state_t *touch_get_state(void)
{
    return &s_touch;
}

/* ================================================================
 * GT911 touch task (polling via esp_lcd_touch component)
 * ================================================================ */

static void touch_task(void *arg)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)arg;
    while (1) {
        esp_lcd_touch_read_data(tp);
        uint8_t n = 0;
        esp_lcd_touch_point_data_t points[TOUCH_MAX_CONTACTS];
        esp_lcd_touch_get_data(tp, points, &n, TOUCH_MAX_CONTACTS);

        if (n > 0) {
            for (uint8_t i = 0; i < n; i++) {
                s_touch.x[i] = (points[i].x < LCD_H_RES) ? points[i].x : (LCD_H_RES - 1);
                s_touch.y[i] = (points[i].y < LCD_V_RES) ? points[i].y : (LCD_V_RES - 1);
            }
            s_touch.points = n;
        } else {
            s_touch.points = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

/* ================================================================
 * Initialization
 * ================================================================ */

esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "Initializing GT911 touch on I²C (SDA=%d, SCL=%d)",
             TOUCH_PIN_SDA, TOUCH_PIN_SCL);

    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port     = -1,
        .sda_io_num   = TOUCH_PIN_SDA,
        .scl_io_num   = TOUCH_PIN_SCL,
        .clk_source   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_cfg, &bus_handle),
                        TAG, "i2c_new_master_bus failed");

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = TOUCH_I2C_SPEED_HZ;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus_handle, &io_config, &io_handle),
                        TAG, "esp_lcd_new_panel_io_i2c failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io_handle, &tp_cfg, &tp),
                        TAG, "esp_lcd_touch_new_i2c_gt911 failed");

    xTaskCreate(touch_task, "touch", 4096, tp, 5, NULL);
    ESP_LOGI(TAG, "Touch active");
    return ESP_OK;
}
