#include "board.h"
#include "demo.h"
#include "display.h"
#include "touch.h"

#include "esp_log.h"

static const char *TAG = "bootstrap";

void app_main(void)
{
    ESP_LOGI(TAG, "Bootstrapping Guition ESP32-S3-4848S040  mode=%s",
             LCD_BUF_MODE_NAME);

    demo_prepare();
    ESP_ERROR_CHECK(display_init(demo_get_isr_callbacks()));
    ESP_ERROR_CHECK(touch_init());
    demo_start();
}
