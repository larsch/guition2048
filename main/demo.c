#include "demo.h"
#include "board.h"
#include "display.h"
#include "touch.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "demo";

#define SCRATCH_W 128
#define SCRATCH_H 128

/* --- persistent background (modes 2-5: pristine; mode 6: rendered by ISR) --- */
static uint16_t *s_clean_bg;

/* --- scratch buffer for partial touch updates (modes 2-5, non-animated) --- */
#if !LCD_BUF_IS_ISR_DRIVEN
static uint16_t *s_scratch;
#endif

/* ================================================================
 * Colour utility
 * ================================================================ */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

/* ================================================================
 * Background
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
#define ANIM_FRAMES 12
static uint16_t *s_anim_frames[ANIM_FRAMES];
static uint8_t   s_anim_phase;
static uint8_t   s_anim_hold;

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

static const uint16_t s_touch_colors[TOUCH_MAX_CONTACTS] = {
    0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F,
};

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
    const touch_state_t *ts = touch_get_state();
    uint8_t pts = ts->points;
    for (int y = 0; y < lines; y++) {
        int sy = start_ln + y;
        for (uint8_t p = 0; p < pts; p++) {
            int tx = (int)ts->x[p], ty = (int)ts->y[p];
            if (sy < ty - TOUCH_BOX_HALF || sy > ty + TOUCH_BOX_HALF) continue;
            int bx0 = tx - TOUCH_BOX_HALF; if (bx0 < 0) bx0 = 0;
            int bx1 = tx + TOUCH_BOX_HALF; if (bx1 >= LCD_H_RES) bx1 = LCD_H_RES - 1;
            uint16_t c = s_touch_colors[p % TOUCH_MAX_CONTACTS];
            uint16_t *row = dst + (y * LCD_H_RES);
            for (int px = bx0; px <= bx1; px++) row[px] = c;
        }
    }
    return false;
}

static const esp_lcd_rgb_panel_event_callbacks_t s_isr_cbs = {
    .on_vsync        = on_vsync_isr,
    .on_bounce_empty = on_bounce_empty,
};

#endif /* LCD_BUF_IS_ISR_DRIVEN */

const esp_lcd_rgb_panel_event_callbacks_t *demo_get_isr_callbacks(void)
{
#if LCD_BUF_IS_ISR_DRIVEN
    return &s_isr_cbs;
#else
    return NULL;
#endif
}

/* ================================================================
 * Render task (modes 2–5)
 * ================================================================ */

#if !LCD_BUF_IS_ISR_DRIVEN

static touch_state_t s_touch_prev;
static touch_state_t s_touch_held;
static uint8_t       s_touch_hold;

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
    esp_lcd_panel_draw_bitmap(display_get_panel(), x0, y0, x0 + w, y0 + h, s_scratch);
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

    int x0 = (obx0 < nbx0) ? obx0 : nbx0;
    int y0 = (oby0 < nby0) ? oby0 : nby0;
    int x1 = (obx1 > nbx1) ? obx1 : nbx1;
    int y1 = (oby1 > nby1) ? oby1 : nby1;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;

    for (int y = 0; y < h; y++) {
        const uint16_t *src = s_clean_bg + ((y0 + y) * LCD_H_RES) + x0;
        memcpy(s_scratch + (y * w), src, w * LCD_BYTES_PER_PIXEL);
    }

    for (int y = nby0; y <= nby1; y++) {
        uint16_t *row = s_scratch + ((y - y0) * w) + (nbx0 - x0);
        for (int x = 0; x <= nbx1 - nbx0; x++) row[x] = color;
    }

    esp_lcd_panel_draw_bitmap(display_get_panel(), x0, y0, x0 + w, y0 + h, s_scratch);
}

static void draw_touch_rect(int tx, int ty, uint16_t color)
{
    int bx0 = tx - TOUCH_BOX_HALF; if (bx0 < 0) bx0 = 0;
    int bx1 = tx + TOUCH_BOX_HALF; if (bx1 >= LCD_H_RES) bx1 = LCD_H_RES - 1;
    int by0 = ty - TOUCH_BOX_HALF; if (by0 < 0) by0 = 0;
    int by1 = ty + TOUCH_BOX_HALF; if (by1 >= LCD_V_RES) by1 = LCD_V_RES - 1;
    int w = bx1 - bx0 + 1, h = by1 - by0 + 1;

    for (int i = 0; i < w * h; i++) s_scratch[i] = color;
    esp_lcd_panel_draw_bitmap(display_get_panel(), bx0, by0, bx0 + w, by0 + h, s_scratch);
}

#endif /* !CONFIG_LCD_ANIMATION */

static void render_task(void *arg)
{
    (void)arg;

    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0, LCD_H_RES, LCD_V_RES,
                              s_clean_bg);
    s_touch_prev.points = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(16));

        /* debounce touch state */
        const touch_state_t *ts = touch_get_state();
        if (ts->points > 0) {
            s_touch_hold = 3;
            s_touch_held.points = ts->points;
            for (uint8_t i = 0; i < TOUCH_MAX_CONTACTS; i++) {
                s_touch_held.x[i] = ts->x[i];
                s_touch_held.y[i] = ts->y[i];
            }
        } else if (s_touch_hold > 0) {
            s_touch_hold--;
        }
        touch_state_t cur;
        cur.points = (s_touch_hold > 0) ? s_touch_held.points : 0;
        for (uint8_t i = 0; i < TOUCH_MAX_CONTACTS; i++) {
            cur.x[i] = s_touch_held.x[i];
            cur.y[i] = s_touch_held.y[i];
        }

#if CONFIG_LCD_ANIMATION
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
                uint16_t c = s_touch_colors[p % TOUCH_MAX_CONTACTS];
                for (int y = by0; y <= by1; y++) {
                    uint16_t *row = s_clean_bg + (y * LCD_H_RES);
                    for (int x = bx0; x <= bx1; x++) row[x] = c;
                }
            }
            esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0,
                                      LCD_H_RES, LCD_V_RES, s_clean_bg);
        }
#else
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
                uint16_t c = s_touch_colors[i % TOUCH_MAX_CONTACTS];
                move_touch_rect((int)s_touch_prev.x[i],
                                (int)s_touch_prev.y[i],
                                (int)cur.x[i], (int)cur.y[i], c);
            } else if (!was && is) {
                uint16_t c = s_touch_colors[i % TOUCH_MAX_CONTACTS];
                draw_touch_rect((int)cur.x[i], (int)cur.y[i], c);
            }
        }
#endif

        s_touch_prev = cur;
    }
}

#endif /* !LCD_BUF_IS_ISR_DRIVEN */

/* ================================================================
 * Public API
 * ================================================================ */

void demo_prepare(void)
{
    prepare_background();
#if CONFIG_LCD_ANIMATION
    prepare_animation_frames();
#endif
}

void demo_start(void)
{
#if LCD_BUF_IS_ISR_DRIVEN
    ESP_LOGI(TAG, "ISR-driven bounce-buffer mode active (no render task)");
#else
    s_scratch = heap_caps_aligned_alloc(64,
                                        SCRATCH_W * SCRATCH_H * LCD_BYTES_PER_PIXEL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_scratch ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(render_task, "render", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Render task started (incremental touch updates)");
#endif
}
