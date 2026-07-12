#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

/* ================================================================
 * Pin and timing defines
 * ================================================================ */

#define LCD_H_RES             480
#define LCD_V_RES             480
#define LCD_BPP               16
#define LCD_BYTES_PER_PIXEL   2
#define LCD_FRAME_SIZE        (LCD_H_RES * LCD_V_RES * LCD_BYTES_PER_PIXEL)

#define LCD_PIN_PCLK   21
#define LCD_PIN_DE     18
#define LCD_PIN_VSYNC  17
#define LCD_PIN_HSYNC  16

#define LCD_PIN_SPI_CS  39
#define LCD_PIN_SPI_SCL 48
#define LCD_PIN_SPI_SDA 47

#define LCD_PIN_BL      38

#define TOUCH_I2C_PORT      I2C_NUM_0
#define TOUCH_PIN_SDA       19
#define TOUCH_PIN_SCL       45
#define TOUCH_I2C_SPEED_HZ  100000
#define TOUCH_I2C_TIMEOUT_MS 50
#define TOUCH_POLL_MS       10
#define TOUCH_BOX_HALF      30

#define GT911_ADDR            0x5D
#define GT911_REG_PRODUCT_ID  0x8140
#define GT911_REG_CFG_VERSION 0x8047
#define GT911_REG_POINT_INFO  0x814E
#define GT911_REG_POINT_1     0x814F
#define GT911_MAX_CONTACTS    5

#define LCD_PCLK_HZ       (12 * 1000 * 1000)
#define LCD_BOUNCE_LINES  10
#define LCD_BOUNCE_PX      (LCD_H_RES * LCD_BOUNCE_LINES)

#define RGB_PIN_B0  4
#define RGB_PIN_B1  5
#define RGB_PIN_B2  6
#define RGB_PIN_B3  7
#define RGB_PIN_B4  15
#define RGB_PIN_G0  8
#define RGB_PIN_G1  20
#define RGB_PIN_G2  3
#define RGB_PIN_G3  46
#define RGB_PIN_G4  9
#define RGB_PIN_G5  10
#define RGB_PIN_R0  11
#define RGB_PIN_R1  12
#define RGB_PIN_R2  13
#define RGB_PIN_R3  14
#define RGB_PIN_R4  0

/* ================================================================
 * Mode selection (from Kconfig choice)
 * ================================================================ */

#if CONFIG_LCD_BUF_MODE_INTERNAL_FB
 #error "Mode 1 (Internal FB) requires ~460 KB SRAM — not available on ESP32-S3. Use mode 2 or higher."
#elif CONFIG_LCD_BUF_MODE_PSRAM_FB
 #define LCD_BUF_MODE  2
#elif CONFIG_LCD_BUF_MODE_DOUBLE_PSRAM_FB
 #define LCD_BUF_MODE  3
#elif CONFIG_LCD_BUF_MODE_USER_FB
 #define LCD_BUF_MODE  4
#elif CONFIG_LCD_BUF_MODE_BOUNCE_PSRAM_FB
 #define LCD_BUF_MODE  5
#elif CONFIG_LCD_BUF_MODE_BOUNCE_ONLY
 #define LCD_BUF_MODE  6
#else
 #define LCD_BUF_MODE  2
#endif

#define LCD_BUF_IS_ISR_DRIVEN  (LCD_BUF_MODE == 6)

#if LCD_BUF_MODE == 1
 #define LCD_BUF_MODE_NAME  "1:Internal FB"
#elif LCD_BUF_MODE == 2
 #define LCD_BUF_MODE_NAME  "2:PSRAM FB"
#elif LCD_BUF_MODE == 3
 #define LCD_BUF_MODE_NAME  "3:Double PSRAM FB"
#elif LCD_BUF_MODE == 4
 #define LCD_BUF_MODE_NAME  "4:User FB"
#elif LCD_BUF_MODE == 5
 #define LCD_BUF_MODE_NAME  "5:Bounce + PSRAM FB"
#elif LCD_BUF_MODE == 6
 #define LCD_BUF_MODE_NAME  "6:Bounce only"
#endif

/* ================================================================
 * Shared runtime state
 * ================================================================ */

typedef struct {
    volatile uint8_t  points;
    volatile uint16_t x[GT911_MAX_CONTACTS];
    volatile uint16_t y[GT911_MAX_CONTACTS];
} touch_state_t;

static const char *TAG = "bootstrap";

static touch_state_t           s_touch;
static esp_lcd_panel_handle_t  s_panel;
static uint16_t               *s_clean_bg;       /* pristine background */

#if !LCD_BUF_IS_ISR_DRIVEN
 #if LCD_BUF_MODE == 4
  static uint16_t       *s_framebuffer;
 #endif
 static touch_state_t   s_touch_prev;
 static touch_state_t   s_touch_held;
 static uint8_t         s_touch_hold;
 static uint16_t       *s_scratch;
 #if CONFIG_LCD_ANIMATION
  #define ANIM_FRAMES 12
  static uint16_t      *s_anim_frames[ANIM_FRAMES];
  static uint8_t        s_anim_phase;
  static uint8_t        s_anim_hold;
 #endif
#endif

/* ================================================================
 * ST7701 init table
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
 * Colour utilities
 * ================================================================ */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

/* ================================================================
 * Static background — computed once
 * ================================================================ */

static const uint8_t s_wave[12] = {0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};

static inline uint16_t anim_pixel(int x, int y, int phase)
{
    int wa  = s_wave[phase % 12];
    int wb  = s_wave[(phase + 4) % 12];
    int dx  = (x > y) ? (x - y) : (y - x);
    int da  = (x > (479 - y)) ? (x - (479 - y)) : ((479 - y) - x);
    int mx  = (x + wa * 3) % 480;
    int my  = (y + wb * 3) % 480;
    int vb  = (144 + wa * 12) % 480;
    int hb  = (144 + wb * 12) % 480;
    int cell = ((mx / 48) ^ (my / 48)) & 1;

    if (x < 6 || x > 473 || y < 6 || y > 473) return rgb565(255, 255, 255);
    if (dx <= 2 || da <= 2)                    return rgb565(255, 255, 255);
    if ((x >= 236 && x <= 244) || (y >= 236 && y <= 244)) return rgb565(255, 255, 255);
    if (x >= vb - 10 && x <= vb + 10)          return rgb565(255, 32, 220);
    if (y >= hb - 10 && y <= hb + 10)          return rgb565(32, 255, 255);

    if (y < 240)
        return (x < 240) ? (cell ? rgb565(255, 96, 96)  : rgb565(120, 24, 24))
                         : (cell ? rgb565(96, 255, 128) : rgb565(24, 120, 40));
    else
        return (x < 240) ? (cell ? rgb565(96, 160, 255) : rgb565(24, 48, 120))
                         : (cell ? rgb565(255, 224, 96) : rgb565(120, 96, 24));
}

static inline uint16_t bg_pixel(int x, int y) { return anim_pixel(x, y, 0); }

#if CONFIG_LCD_ANIMATION
static void prepare_animation_frames(void)
{
    ESP_LOGI(TAG, "Pre-computing %d animation frames (%.0f KB PSRAM) ...",
             ANIM_FRAMES, (float)(ANIM_FRAMES * LCD_FRAME_SIZE) / 1024.0f);
    for (int p = 0; p < ANIM_FRAMES; p++) {
        s_anim_frames[p] = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(s_anim_frames[p] ? ESP_OK : ESP_ERR_NO_MEM);
        for (int y = 0; y < LCD_V_RES; y++) {
            uint16_t *row = s_anim_frames[p] + (y * LCD_H_RES);
            for (int x = 0; x < LCD_H_RES; x++) {
                row[x] = anim_pixel(x, y, p);
            }
        }
    }
    ESP_LOGI(TAG, "Animation frames ready");
}
#endif

static void prepare_background(void)
{
    ESP_LOGI(TAG, "Pre-computing static background (450 KB) ...");
    s_clean_bg = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_clean_bg ? ESP_OK : ESP_ERR_NO_MEM);

    for (int y = 0; y < LCD_V_RES; y++) {
        uint16_t *row = s_clean_bg + (y * LCD_H_RES);
        for (int x = 0; x < LCD_H_RES; x++) {
            row[x] = bg_pixel(x, y);
        }
    }
    ESP_LOGI(TAG, "Background ready");
}

/* ================================================================
 * Touch overlay colours
 * ================================================================ */

static const uint16_t s_touch_colors[GT911_MAX_CONTACTS] = {
    0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F,
};

/* ================================================================
 * GT911 touch driver (I²C)
 * ================================================================ */

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t rb[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_write_read_device(TOUCH_I2C_PORT, GT911_ADDR,
                                        rb, sizeof(rb), data, len,
                                        pdMS_TO_TICKS(TOUCH_I2C_TIMEOUT_MS));
}

static esp_err_t gt911_write8(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return i2c_master_write_to_device(TOUCH_I2C_PORT, GT911_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(TOUCH_I2C_TIMEOUT_MS));
}

static bool gt911_probe(void)
{
    uint8_t pid[3] = {0}, cfg = 0;
    if (gt911_read(GT911_REG_PRODUCT_ID, pid, sizeof(pid)) != ESP_OK) return false;
    if (gt911_read(GT911_REG_CFG_VERSION, &cfg, 1) != ESP_OK)       return false;

    for (size_t i = 0; i < sizeof(pid); i++) {
        if (pid[i] != 0 && (pid[i] < '0' || pid[i] > '9')) return false;
    }
    if (pid[0] == 0 && pid[1] == 0 && pid[2] == 0) return false;

    ESP_LOGI(TAG, "GT911 prod=%c%c%c cfg=%u", pid[0], pid[1], pid[2], cfg);
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;
    while (1) {
        uint8_t info = 0;
        if (gt911_read(GT911_REG_POINT_INFO, &info, 1) == ESP_OK) {
            uint8_t n = info & 0x0FU;
            bool    ready = (info & 0x80U) && (n > 0);

            if (ready) {
                if (n > GT911_MAX_CONTACTS) n = GT911_MAX_CONTACTS;
                if (n > CONFIG_ESP_LCD_TOUCH_MAX_POINTS)
                    n = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;

                uint8_t buf[GT911_MAX_CONTACTS * 8];
                if (gt911_read(GT911_REG_POINT_1, buf, n * 8) == ESP_OK) {
                    for (uint8_t i = 0; i < n; i++) {
                        uint16_t x = buf[i * 8 + 1] | ((uint16_t)buf[i * 8 + 2] << 8);
                        uint16_t y = buf[i * 8 + 3] | ((uint16_t)buf[i * 8 + 4] << 8);
                        s_touch.x[i] = (x < LCD_H_RES) ? x : (LCD_H_RES - 1);
                        s_touch.y[i] = (y < LCD_V_RES) ? y : (LCD_V_RES - 1);
                    }
                    s_touch.points = n;
                } else {
                    s_touch.points = 0;
                }
            } else {
                s_touch.points = 0;
            }
            gt911_write8(GT911_REG_POINT_INFO, 0);
        } else {
            s_touch.points = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

/* ================================================================
 * Backlight
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
    cfg.flags.fb_in_psram = 1;  /* user buffer allocated in PSRAM */
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
 * ST7701 SPI init — sends commands only (mode 6)
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
#endif /* LCD_BUF_IS_ISR_DRIVEN */

/* ================================================================
 * ISR callbacks (mode 6 only)
 * ================================================================ */

#if LCD_BUF_IS_ISR_DRIVEN

static IRAM_ATTR bool on_vsync_isr(esp_lcd_panel_handle_t panel,
                                   const esp_lcd_rgb_panel_event_data_t *edata,
                                   void *ctx)
{
    (void)panel; (void)edata; (void)ctx;
    return false;
}

static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                       void *bounce_buf,
                                       int pos_px, int len_bytes,
                                       void *ctx)
{
    (void)panel; (void)ctx;

    int lines    = len_bytes / (LCD_BYTES_PER_PIXEL * LCD_H_RES);
    int start_ln = pos_px / LCD_H_RES;
    uint16_t *dst = (uint16_t *)bounce_buf;

    /* copy static background into bounce buffer */
    for (int y = 0; y < lines; y++) {
        int sy = start_ln + y;
        memcpy(dst + (y * LCD_H_RES), s_clean_bg + (sy * LCD_H_RES),
               LCD_H_RES * LCD_BYTES_PER_PIXEL);
    }

    /* overlay touch indicators */
    uint8_t pts = s_touch.points;
    for (int y = 0; y < lines; y++) {
        int sy = start_ln + y;
        for (uint8_t p = 0; p < pts; p++) {
            int tx = (int)s_touch.x[p], ty = (int)s_touch.y[p];
            if (sy < ty - TOUCH_BOX_HALF || sy > ty + TOUCH_BOX_HALF) continue;
            int bx0 = tx - TOUCH_BOX_HALF; if (bx0 < 0) bx0 = 0;
            int bx1 = tx + TOUCH_BOX_HALF; if (bx1 >= LCD_H_RES) bx1 = LCD_H_RES - 1;
            uint16_t c = s_touch_colors[p % GT911_MAX_CONTACTS];
            uint16_t *row = dst + (y * LCD_H_RES);
            for (int px = bx0; px <= bx1; px++) row[px] = c;
        }
    }
    return false;
}

#endif /* LCD_BUF_IS_ISR_DRIVEN */

/* ================================================================
 * Render task (modes 1–5): full-frame touch overlay
 * ================================================================ */

#if !LCD_BUF_IS_ISR_DRIVEN

#if !CONFIG_LCD_ANIMATION

static void restore_rect(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
    if (y1 >= LCD_V_RES) y1 = LCD_V_RES - 1;
    if (x0 > x1 || y0 > y1) return;
    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;
    for (int y = 0; y < h; y++) {
        const uint16_t *src = s_clean_bg + ((y0 + y) * LCD_H_RES) + x0;
        memcpy(s_scratch + (y * w), src, w * LCD_BYTES_PER_PIXEL);
    }
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + h, s_scratch);
}

static void move_touch_rect(int old_x, int old_y, int new_x, int new_y,
                            uint16_t color)
{
    int obx0 = old_x - TOUCH_BOX_HALF; if (obx0 < 0) obx0 = 0;
    int obx1 = old_x + TOUCH_BOX_HALF; if (obx1 >= LCD_H_RES) obx1 = LCD_H_RES - 1;
    int oby0 = old_y - TOUCH_BOX_HALF; if (oby0 < 0) oby0 = 0;
    int oby1 = old_y + TOUCH_BOX_HALF; if (oby1 >= LCD_V_RES) oby1 = LCD_V_RES - 1;

    int nbx0 = new_x - TOUCH_BOX_HALF; if (nbx0 < 0) nbx0 = 0;
    int nbx1 = new_x + TOUCH_BOX_HALF; if (nbx1 >= LCD_H_RES) nbx1 = LCD_H_RES - 1;
    int nby0 = new_y - TOUCH_BOX_HALF; if (nby0 < 0) nby0 = 0;
    int nby1 = new_y + TOUCH_BOX_HALF; if (nby1 >= LCD_V_RES) nby1 = LCD_V_RES - 1;

    /* combined bounding box */
    int x0 = (obx0 < nbx0) ? obx0 : nbx0;
    int y0 = (oby0 < nby0) ? oby0 : nby0;
    int x1 = (obx1 > nbx1) ? obx1 : nbx1;
    int y1 = (oby1 > nby1) ? oby1 : nby1;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;

    /* copy background into scratch for entire combined region */
    for (int y = 0; y < h; y++) {
        const uint16_t *src = s_clean_bg + ((y0 + y) * LCD_H_RES) + x0;
        memcpy(s_scratch + (y * w), src, w * LCD_BYTES_PER_PIXEL);
    }

    /* draw new touch box on top */
    for (int y = nby0; y <= nby1; y++) {
        uint16_t *row = s_scratch + ((y - y0) * w) + (nbx0 - x0);
        for (int x = 0; x <= nbx1 - nbx0; x++) row[x] = color;
    }

    /* single atomic push */
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + h, s_scratch);
}

static void draw_touch_rect(int tx, int ty, uint16_t color)
{
    int bx0 = tx - TOUCH_BOX_HALF; if (bx0 < 0) bx0 = 0;
    int bx1 = tx + TOUCH_BOX_HALF; if (bx1 >= LCD_H_RES) bx1 = LCD_H_RES - 1;
    int by0 = ty - TOUCH_BOX_HALF; if (by0 < 0) by0 = 0;
    int by1 = ty + TOUCH_BOX_HALF; if (by1 >= LCD_V_RES) by1 = LCD_V_RES - 1;
    int w = bx1 - bx0 + 1, h = by1 - by0 + 1;

    for (int i = 0; i < w * h; i++) s_scratch[i] = color;
    esp_lcd_panel_draw_bitmap(s_panel, bx0, by0, bx0 + w, by0 + h, s_scratch);
}

#endif /* !CONFIG_LCD_ANIMATION */

static void render_task(void *arg)
{
    (void)arg;

    /* push full background once */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES,
                              s_clean_bg);
    s_touch_prev.points = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(16));

        /* debounce touch state */
        if (s_touch.points > 0) {
            s_touch_hold = 3;
            s_touch_held.points = s_touch.points;
            for (uint8_t i = 0; i < GT911_MAX_CONTACTS; i++) {
                s_touch_held.x[i] = s_touch.x[i];
                s_touch_held.y[i] = s_touch.y[i];
            }
        } else if (s_touch_hold > 0) {
            s_touch_hold--;
        }
        touch_state_t cur;
        cur.points = (s_touch_hold > 0) ? s_touch_held.points : 0;
        for (uint8_t i = 0; i < GT911_MAX_CONTACTS; i++) {
            cur.x[i] = s_touch_held.x[i];
            cur.y[i] = s_touch_held.y[i];
        }

#if CONFIG_LCD_ANIMATION
        /* --- animated mode: bake touch into keyframe, push once --- */
        s_anim_hold++;
        if (s_anim_hold >= 2) {
            s_anim_hold = 0;
            s_anim_phase = (s_anim_phase + 1) % ANIM_FRAMES;
            memcpy(s_clean_bg, s_anim_frames[s_anim_phase], LCD_FRAME_SIZE);
            for (uint8_t p = 0; p < cur.points; p++) {
                int bx0 = (int)cur.x[p] - TOUCH_BOX_HALF;
                int bx1 = (int)cur.x[p] + TOUCH_BOX_HALF;
                int by0 = (int)cur.y[p] - TOUCH_BOX_HALF;
                int by1 = (int)cur.y[p] + TOUCH_BOX_HALF;
                if (bx0 < 0) bx0 = 0;
                if (bx1 >= LCD_H_RES) bx1 = LCD_H_RES - 1;
                if (by0 < 0) by0 = 0;
                if (by1 >= LCD_V_RES) by1 = LCD_V_RES - 1;
                uint16_t c = s_touch_colors[p % GT911_MAX_CONTACTS];
                for (int y = by0; y <= by1; y++) {
                    uint16_t *row = s_clean_bg + (y * LCD_H_RES);
                    for (int x = bx0; x <= bx1; x++) row[x] = c;
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES,
                                      s_clean_bg);
        }
#else
        /* --- static mode: partial touch updates --- */
        uint8_t max_pts = cur.points;
        if (s_touch_prev.points > max_pts) max_pts = s_touch_prev.points;

        for (uint8_t i = 0; i < max_pts; i++) {
            bool was = (i < s_touch_prev.points);
            bool is  = (i < cur.points);
            bool moved = was && is &&
                         (s_touch_prev.x[i] != cur.x[i] ||
                          s_touch_prev.y[i] != cur.y[i]);

            if (was && !is) {
                restore_rect((int)s_touch_prev.x[i] - TOUCH_BOX_HALF,
                             (int)s_touch_prev.y[i] - TOUCH_BOX_HALF,
                             (int)s_touch_prev.x[i] + TOUCH_BOX_HALF,
                             (int)s_touch_prev.y[i] + TOUCH_BOX_HALF);
            } else if (was && is && moved) {
                uint16_t c = s_touch_colors[i % GT911_MAX_CONTACTS];
                move_touch_rect((int)s_touch_prev.x[i],
                                (int)s_touch_prev.y[i],
                                (int)cur.x[i], (int)cur.y[i], c);
            } else if (!was && is) {
                uint16_t c = s_touch_colors[i % GT911_MAX_CONTACTS];
                draw_touch_rect((int)cur.x[i], (int)cur.y[i], c);
            }
        }
#endif

        s_touch_prev = cur;
    }
}

#endif /* !LCD_BUF_IS_ISR_DRIVEN */

/* ================================================================
 * Panel creation — unified, mode-dependent
 * ================================================================ */

static esp_lcd_panel_handle_t create_panel(void)
{
    spi_line_config_t line_cfg = {
        .cs_io_type  = IO_TYPE_GPIO, .cs_gpio_num  = LCD_PIN_SPI_CS,
        .scl_io_type = IO_TYPE_GPIO, .scl_gpio_num = LCD_PIN_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO, .sda_gpio_num = LCD_PIN_SPI_SDA,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_cfg =
        ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_cfg, 0);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io));

    esp_lcd_rgb_panel_config_t rgb_cfg = make_rgb_config();

#if LCD_BUF_MODE == 4
    s_framebuffer = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_framebuffer ? ESP_OK : ESP_ERR_NO_MEM);
    rgb_cfg.user_fbs[0] = s_framebuffer;
#endif

#if LCD_BUF_IS_ISR_DRIVEN
    /* mode 6: SPI init first, then bare RGB panel with ISR callbacks */
    st7701_send_init(io, &rgb_cfg);

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_cfg, &panel));

    const esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync        = on_vsync_isr,
        .on_bounce_empty = on_bounce_empty,
    };
    ESP_ERROR_CHECK(
        esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
#else
    /* modes 1–5: ST7701 driver handles RGB setup */
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

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &pcfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    return panel;
#endif
}

/* ================================================================
 * app_main
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "Bootstrapping Guition ESP32-S3-4848S040  mode=%s",
             LCD_BUF_MODE_NAME);

    backlight_init();
    prepare_background();
#if CONFIG_LCD_ANIMATION
    prepare_animation_frames();
#endif
    s_panel = create_panel();

#if !LCD_BUF_IS_ISR_DRIVEN
    /* allocate scratch buffer for partial touch updates */
 #define SCRATCH_W 128
 #define SCRATCH_H 128
    s_scratch = heap_caps_aligned_alloc(64,
                                        SCRATCH_W * SCRATCH_H * LCD_BYTES_PER_PIXEL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_scratch ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(render_task, "render", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Render task started (incremental touch updates)");
#else
    ESP_LOGI(TAG, "ISR-driven bounce-buffer mode active");
#endif

    /* --- touch --- */
    const i2c_config_t i2c_cfg = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = TOUCH_PIN_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num    = TOUCH_PIN_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master        = { .clk_speed = TOUCH_I2C_SPEED_HZ },
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, i2c_cfg.mode, 0, 0, 0));

    if (gt911_probe()) {
        xTaskCreate(touch_task, "touch", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "Touch active");
    } else {
        ESP_LOGW(TAG, "GT911 not found — touch disabled");
    }
}
