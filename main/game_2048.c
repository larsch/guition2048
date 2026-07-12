#include "game_2048.h"
#include "board.h"
#include "display.h"
#include "touch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "2048";

#define GRID_SIZE  4
#define CELL_SIZE  100
#define CELL_GAP   10
#define BOARD_X    ((LCD_H_RES - (GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * CELL_GAP)) / 2)
#define BOARD_Y    55
#define SCORE_Y    15

#define FONT_SCALE 4
#define FONT_W     (3 * FONT_SCALE)
#define FONT_H     (5 * FONT_SCALE)

/* ================================================================
 * 3×5 pixel font for digits 0–9
 * ================================================================ */

static const uint8_t s_font[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

/* ================================================================
 * Tile colours (RGB565)
 * ================================================================ */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

typedef struct { uint16_t bg; uint16_t fg; } tile_color_t;

static const tile_color_t s_colors[] = {
    {0xCE59, 0x6B4D}, // 0 (empty)
    {0xEF5A, 0x6B4D}, // 2
    {0xEF38, 0x6B4D}, // 4
    {0xF595, 0xFE19}, // 8
    {0xF6A6, 0xFE19}, // 16
    {0xF73E, 0xFE19}, // 32
    {0xF72B, 0xFE19}, // 64
    {0xEE6E, 0xFE19}, // 128
    {0xEE4C, 0xFE19}, // 256
    {0xEE29, 0xFE19}, // 512
    {0xEE07, 0xFE19}, // 1024
    {0xEDE4, 0xFE19}, // 2048
};

static tile_color_t tile_color(uint32_t val)
{
    if (val == 0) return s_colors[0];
    int idx = 0;
    uint32_t v = val;
    while (v > 1) { idx++; v >>= 1; }
    if (idx >= (int)(sizeof(s_colors) / sizeof(s_colors[0])))
        idx = sizeof(s_colors) / sizeof(s_colors[0]) - 1;
    return s_colors[idx];
}

/* ================================================================
 * Game state
 * ================================================================ */

typedef enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } dir_t;

static uint32_t  s_grid[GRID_SIZE][GRID_SIZE];
static uint32_t  s_score;
static uint32_t  s_best;
static bool      s_game_over;
static bool      s_won;
static uint16_t *s_fb;

/* ================================================================
 * Logic
 * ================================================================ */

static void spawn_tile(void)
{
    int empty = 0;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (s_grid[r][c] == 0) empty++;
    if (empty == 0) return;

    int target = esp_random() % empty;
    int val = (esp_random() % 10) < 9 ? 2 : 4;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (s_grid[r][c] == 0) {
                if (target == 0) { s_grid[r][c] = val; return; }
                target--;
            }
        }
    }
}

static bool slide_row(uint32_t *row)
{
    bool moved = false;
    uint32_t tmp[GRID_SIZE] = {0};
    int pos = 0;

    /* compact */
    for (int i = 0; i < GRID_SIZE; i++)
        if (row[i] != 0) tmp[pos++] = row[i];

    /* merge */
    for (int i = 0; i < pos - 1; i++) {
        if (tmp[i] == tmp[i+1]) {
            tmp[i] *= 2;
            s_score += tmp[i];
            if (tmp[i] == 2048) s_won = true;
            tmp[i+1] = 0;
            moved = true;
        }
    }

    /* compact again */
    uint32_t final[GRID_SIZE] = {0};
    int fp = 0;
    for (int i = 0; i < pos; i++)
        if (tmp[i] != 0) final[fp++] = tmp[i];

    for (int i = 0; i < GRID_SIZE; i++) {
        if (row[i] != final[i]) moved = true;
        row[i] = final[i];
    }
    return moved;
}

static void reverse_row(uint32_t *row)
{
    for (int i = 0; i < GRID_SIZE / 2; i++) {
        uint32_t t = row[i];
        row[i] = row[GRID_SIZE - 1 - i];
        row[GRID_SIZE - 1 - i] = t;
    }
}

static bool transpose(void)
{
    bool moved = false;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = r + 1; c < GRID_SIZE; c++) {
            uint32_t t = s_grid[r][c];
            s_grid[r][c] = s_grid[c][r];
            s_grid[c][r] = t;
        }
    }
    return moved;
}

static bool move_dir(dir_t dir)
{
    bool moved = false;

    if (dir == DIR_UP || dir == DIR_DOWN) transpose();

    for (int r = 0; r < GRID_SIZE; r++) {
        if (dir == DIR_RIGHT || dir == DIR_DOWN) reverse_row(s_grid[r]);
        uint32_t save[GRID_SIZE];
        memcpy(save, s_grid[r], sizeof(save));
        if (slide_row(s_grid[r])) moved = true;
        if (dir == DIR_RIGHT || dir == DIR_DOWN) reverse_row(s_grid[r]);
    }

    if (dir == DIR_UP || dir == DIR_DOWN) transpose();

    return moved;
}

static bool can_move(void)
{
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (s_grid[r][c] == 0) return true;
            if (c < GRID_SIZE - 1 && s_grid[r][c] == s_grid[r][c+1]) return true;
            if (r < GRID_SIZE - 1 && s_grid[r][c] == s_grid[r+1][c]) return true;
        }
    }
    return false;
}

static void reset_game(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score = 0;
    s_game_over = false;
    s_won = false;
    spawn_tile();
    spawn_tile();
}

/* ================================================================
 * Rendering
 * ================================================================ */

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= LCD_V_RES) continue;
        for (int col = x; col < x + w; col++) {
            if (col >= 0 && col < LCD_H_RES)
                s_fb[row * LCD_H_RES + col] = color;
        }
    }
}

static void draw_char(int cx, int cy, char ch, uint16_t color, int scale)
{
    if (ch < '0' || ch > '9') return;
    const uint8_t *glyph = s_font[ch - '0'];
    int pw = 3 * scale;
    int ph = 5 * scale;

    for (int row = 0; row < 5; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 3; col++) {
            if (bits & (1 << (2 - col))) {
                int x = cx + col * scale;
                int y = cy + row * scale;
                for (int sy = 0; sy < scale; sy++) {
                    if ((y + sy) < 0 || (y + sy) >= LCD_V_RES) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        if ((x + sx) >= 0 && (x + sx) < LCD_H_RES)
                            s_fb[(y + sy) * LCD_H_RES + x + sx] = color;
                    }
                }
            }
        }
    }
}

static void draw_number(int cx, int cy, uint32_t val, uint16_t color, int scale)
{
    if (val == 0) return;
    char buf[16];
    int len = 0;
    uint32_t v = val;
    do { buf[len++] = '0' + (v % 10); v /= 10; } while (v > 0);

    /* reverse */
    for (int i = 0; i < len / 2; i++) {
        char t = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = t;
    }

    int digit_w = 3 * scale + scale; // +1 scale gap
    int total_w = len * digit_w - scale;
    int start_x = cx - total_w / 2;

    for (int i = 0; i < len; i++)
        draw_char(start_x + i * digit_w, cy, buf[i], color, scale);
}

static void render(void)
{
    /* background */
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, rgb565(250, 248, 239));

    /* score */
    char score_buf[32];
    snprintf(score_buf, sizeof(score_buf), "Score: %lu  Best: %lu",
             (unsigned long)s_score, (unsigned long)s_best);
    int score_w = strlen(score_buf) * (3 * 2 + 2);
    // ... simplified: draw score with small font
    draw_number(LCD_H_RES / 2, SCORE_Y, s_score, rgb565(119, 110, 101), 2);

    /* grid background */
    fill_rect(BOARD_X - 5, BOARD_Y - 5,
              GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * CELL_GAP + 10,
              GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * CELL_GAP + 10,
              rgb565(187, 173, 160));

    /* tiles */
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int tx = BOARD_X + c * (CELL_SIZE + CELL_GAP);
            int ty = BOARD_Y + r * (CELL_SIZE + CELL_GAP);
            tile_color_t tc = tile_color(s_grid[r][c]);
            fill_rect(tx, ty, CELL_SIZE, CELL_SIZE, tc.bg);

            if (s_grid[r][c] != 0) {
                int num_scale = FONT_SCALE;
                uint32_t v = s_grid[r][c];
                /* reduce scale for large numbers */
                int digits = 0; uint32_t vv = v;
                do { digits++; vv /= 10; } while (vv > 0);
                int digit_w = 3 * num_scale + num_scale;
                int total_w = digits * digit_w - num_scale;
                while (total_w > CELL_SIZE - 10 && num_scale > 1) {
                    num_scale--;
                    digit_w = 3 * num_scale + num_scale;
                    total_w = digits * digit_w - num_scale;
                }
                draw_number(tx + CELL_SIZE / 2,
                            ty + (CELL_SIZE - 5 * num_scale) / 2,
                            v, tc.fg, num_scale);
            }
        }
    }

    /* game over overlay */
    if (s_game_over) {
        uint16_t ov = rgb565(255, 255, 255);
        ov = (ov & 0xF7DE) >> 1; // darken
        // semi-transparent not easy in RGB565; just draw a dim overlay
        fill_rect(BOARD_X, BOARD_Y,
                  GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * CELL_GAP,
                  GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * CELL_GAP,
                  rgb565(238, 228, 218));
        // "GAME OVER" text — just draw a red rectangle as placeholder
        // TODO: proper text rendering for labels
    }

    /* push frame */
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0,
                              LCD_H_RES, LCD_V_RES, s_fb);
}

/* ================================================================
 * Input
 * ================================================================ */

#define SWIPE_THRESHOLD  40

static bool detect_swipe(dir_t *out_dir)
{
    static bool  active = false;
    static int   start_x, start_y;
    static int   last_x, last_y;

    const touch_state_t *ts = touch_get_state();

    if (ts->points > 0) {
        int cx = (int)ts->x[0], cy = (int)ts->y[0];
        if (!active) {
            active = true;
            start_x = last_x = cx;
            start_y = last_y = cy;
        } else {
            last_x = cx;
            last_y = cy;
        }
        return false;
    }

    if (!active) return false;
    active = false;

    int dx = last_x - start_x;
    int dy = last_y - start_y;
    int adx = dx > 0 ? dx : -dx;
    int ady = dy > 0 ? dy : -dy;

    if (adx < SWIPE_THRESHOLD && ady < SWIPE_THRESHOLD) return false;

    if (adx > ady)
        *out_dir = (dx > 0) ? DIR_RIGHT : DIR_LEFT;
    else
        *out_dir = (dy > 0) ? DIR_DOWN : DIR_UP;

    return true;
}

/* ================================================================
 * Game task
 * ================================================================ */

static void game_task(void *arg)
{
    (void)arg;

    s_fb = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_fb ? ESP_OK : ESP_ERR_NO_MEM);

    reset_game();

    while (1) {
        dir_t dir;
        if (detect_swipe(&dir) && !s_game_over) {
            if (move_dir(dir)) {
                spawn_tile();
                if (!can_move()) s_game_over = true;
            }
            if (s_score > s_best) s_best = s_score;
        }

        /* tap to restart when game over */
        if (s_game_over) {
            const touch_state_t *ts = touch_get_state();
            if (ts->points > 0) {
                reset_game();
                vTaskDelay(pdMS_TO_TICKS(300)); // debounce
            }
        }

        render();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ================================================================
 * Public
 * ================================================================ */

void game_2048_start(void)
{
    ESP_LOGI(TAG, "Starting 2048");
    xTaskCreate(game_task, "2048", 4096, NULL, 5, NULL);
}
