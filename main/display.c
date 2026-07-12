#include "display.h"
#include "board.h"

#include "driver/ledc.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;

#if LCD_BUF_MODE == 4
static uint16_t *s_framebuffer;
#endif

/* ================================================================
 * ST7701 init table (Type 9)
 * ================================================================ */

static const st7701_lcd_init_cmd_t s_st7701_type9_init_ops[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                       0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                       0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x32}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                       0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                       0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                       0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                       0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE5, (uint8_t[]){0xE4}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x60}, 1, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

/* ================================================================
 * Backlight (LEDC PWM)
 * ================================================================ */

static void backlight_init(void)
{
    const ledc_timer_config_t tmr = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 150,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tmr));

    const ledc_channel_config_t ch = {
        .gpio_num   = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 1023,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

/* ================================================================
 * Panel timing
 * ================================================================ */

static esp_lcd_rgb_timing_t make_timing(void)
{
    return (esp_lcd_rgb_timing_t){
        .pclk_hz           = LCD_PCLK_HZ,
        .h_res             = LCD_H_RES,
        .v_res             = LCD_V_RES,
        .hsync_pulse_width = 8,
        .hsync_back_porch  = 50,
        .hsync_front_porch = 10,
        .vsync_pulse_width = 8,
        .vsync_back_porch  = 20,
        .vsync_front_porch = 10,
        .flags = { .pclk_active_neg = 0 },
    };
}

/* ================================================================
 * RGB config builder — compile-time driven by LCD_BUF_MODE
 * ================================================================ */

static esp_lcd_rgb_panel_config_t make_rgb_config(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src         = LCD_CLK_SRC_DEFAULT,
        .timings         = make_timing(),
        .data_width      = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format= LCD_COLOR_FMT_RGB565,
        .dma_burst_size  = 64,
        .hsync_gpio_num  = LCD_PIN_HSYNC,
        .vsync_gpio_num  = LCD_PIN_VSYNC,
        .de_gpio_num     = LCD_PIN_DE,
        .pclk_gpio_num   = LCD_PIN_PCLK,
        .disp_gpio_num   = -1,
        .data_gpio_nums  = {
            RGB_PIN_B0, RGB_PIN_B1, RGB_PIN_B2, RGB_PIN_B3, RGB_PIN_B4,
            RGB_PIN_G0, RGB_PIN_G1, RGB_PIN_G2, RGB_PIN_G3, RGB_PIN_G4, RGB_PIN_G5,
            RGB_PIN_R0, RGB_PIN_R1, RGB_PIN_R2, RGB_PIN_R3, RGB_PIN_R4,
        },
    };

#if LCD_BUF_MODE == 2
    cfg.num_fbs = 1;
    cfg.flags.fb_in_psram = 1;
#elif LCD_BUF_MODE == 3
    cfg.num_fbs = 2;
    cfg.flags.fb_in_psram = 1;
#elif LCD_BUF_MODE == 4
    cfg.num_fbs = 1;
    cfg.flags.fb_in_psram = 1;
#elif LCD_BUF_MODE == 5
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = LCD_BOUNCE_PX;
    cfg.flags.fb_in_psram = 1;
#elif LCD_BUF_MODE == 6
    cfg.num_fbs = 0;
    cfg.bounce_buffer_size_px = LCD_BOUNCE_PX;
    cfg.flags.no_fb = 1;
#endif

    return cfg;
}

/* ================================================================
 * ST7701 SPI init (mode 6 only: sends commands, no RGB setup)
 * ================================================================ */

#if LCD_BUF_IS_ISR_DRIVEN
static void st7701_send_init(esp_lcd_panel_io_handle_t io,
                             const esp_lcd_rgb_panel_config_t *rgb_cfg)
{
    st7701_vendor_config_t vcfg = {
        .rgb_config     = rgb_cfg,
        .init_cmds      = s_st7701_type9_init_ops,
        .init_cmds_size = sizeof(s_st7701_type9_init_ops) /
                          sizeof(s_st7701_type9_init_ops[0]),
        .flags          = { .enable_io_multiplex = 1 },
    };
    const esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config  = &vcfg,
    };

    esp_lcd_panel_handle_t tmp = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &pcfg, &tmp));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(tmp));
    ESP_ERROR_CHECK(esp_lcd_panel_init(tmp));
    ESP_ERROR_CHECK(esp_lcd_panel_del(tmp));
}
#endif

/* ================================================================
 * Panel creation — unified, mode-dependent
 * ================================================================ */

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}

esp_err_t display_init(const esp_lcd_rgb_panel_event_callbacks_t *isr_cbs)
{
    ESP_LOGI(TAG, "Initializing ST7701 display  mode=%s", LCD_BUF_MODE_NAME);

    backlight_init();

    /* 3-wire SPI for ST7701 commands */
    spi_line_config_t line_cfg = {
        .cs_io_type  = IO_TYPE_GPIO, .cs_gpio_num  = LCD_PIN_SPI_CS,
        .scl_io_type = IO_TYPE_GPIO, .scl_gpio_num = LCD_PIN_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO, .sda_gpio_num = LCD_PIN_SPI_SDA,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_cfg =
        ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_cfg, 0);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io),
                        TAG, "3-wire SPI IO failed");

    esp_lcd_rgb_panel_config_t rgb_cfg = make_rgb_config();

#if LCD_BUF_MODE == 4
    s_framebuffer = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_ERROR(s_framebuffer ? ESP_OK : ESP_ERR_NO_MEM,
                        TAG, "user framebuffer alloc failed");
    rgb_cfg.user_fbs[0] = s_framebuffer;
#endif

#if LCD_BUF_IS_ISR_DRIVEN
    /* Mode 6: SPI init first, then bare RGB panel with ISR callbacks */
    st7701_send_init(io, &rgb_cfg);

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel),
                        TAG, "esp_lcd_new_rgb_panel failed");

    if (isr_cbs) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_rgb_panel_register_event_callbacks(s_panel, isr_cbs, NULL),
            TAG, "register event callbacks failed");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
#else
    /* Modes 2–5: ST7701 driver handles RGB setup */
    st7701_vendor_config_t vcfg = {
        .rgb_config     = &rgb_cfg,
        .init_cmds      = s_st7701_type9_init_ops,
        .init_cmds_size = sizeof(s_st7701_type9_init_ops) /
                          sizeof(s_st7701_type9_init_ops[0]),
        .flags          = { .mirror_by_cmd = 1 },
    };
    const esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config  = &vcfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &pcfg, &s_panel),
                        TAG, "esp_lcd_new_panel_st7701 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "disp_on_off failed");
#endif

    ESP_LOGI(TAG, "Display ready");
    return ESP_OK;
}
