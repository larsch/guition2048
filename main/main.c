#include "board.h"
#include "display.h"
#include "game_2048.h"
#include "touch.h"

#include "esp_log.h"

static const char *TAG = "bootstrap";

void app_main(void)
{
    ESP_LOGI(TAG, "Guition ESP32-S3-4848S040  2048  mode=%s",
             LCD_BUF_MODE_NAME);

    game_2048_init();
    ESP_ERROR_CHECK(display_init(game_get_isr_callbacks()));
    ESP_ERROR_CHECK(touch_init());
    game_2048_start();
}
