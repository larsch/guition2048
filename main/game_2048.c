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
#define CELL_SIZE  120
#define CELL_GAP   0
#define BOARD_X    0
#define BOARD_Y    0
#define SCORE_Y    2

#define FONT_SCALE 4

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
    {0xCE16, 0x736C}, // 0 (empty: tan bg, dark fg)
    {0xEF3B, 0x736C}, // 2 (cream bg, dark fg)
    {0xEF19, 0x736C}, // 4 (cream bg, dark fg)
    {0xF58F, 0xFFFF}, // 8 (orange bg, white fg)
    {0xF4AC, 0xFFFF}, // 16
    {0xF3EB, 0xFFFF}, // 32
    {0xF2E7, 0xFFFF}, // 64
    {0xEE6E, 0xFFFF}, // 128
    {0xEE6C, 0xFFFF}, // 256
    {0xEE4A, 0xFFFF}, // 512
    {0xEE27, 0xFFFF}, // 1024
    {0xEE05, 0xFFFF}, // 2048
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
static uint16_t *s_fb[2];
static int       s_fb_idx;

/* animation state */
static uint32_t s_grid_prev[GRID_SIZE][GRID_SIZE];
static int      s_anim_src[GRID_SIZE][GRID_SIZE];   /* primary source row/col */
static int      s_anim_src2[GRID_SIZE][GRID_SIZE];  /* second source (merge) or -1 */
#define ANIM_FRAMES 8

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

/* Compute where each tile in s_grid came from in s_grid_prev */
static void compute_anims(dir_t dir)
{
    /* reset */
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            s_anim_src[r][c] = s_anim_src2[r][c] = -1;

    for (int line = 0; line < GRID_SIZE; line++) {
        /* collect non-zero tiles with their positions along the axis */
        int vals[GRID_SIZE], idxs[GRID_SIZE], cnt = 0;
        for (int i = 0; i < GRID_SIZE; i++) {
            int r = (dir == DIR_UP || dir == DIR_DOWN) ? i : line;
            int c = (dir == DIR_UP || dir == DIR_DOWN) ? line : i;
            if (dir == DIR_RIGHT || dir == DIR_DOWN) {
                r = (dir == DIR_DOWN) ? (GRID_SIZE - 1 - i) : line;
                c = (dir == DIR_UP || dir == DIR_DOWN) ? line : (GRID_SIZE - 1 - i);
            }
            if (s_grid_prev[r][c]) {
                vals[cnt] = s_grid_prev[r][c];
                idxs[cnt] = (dir == DIR_UP || dir == DIR_DOWN) ? r : c;
                cnt++;
            }
        }

        /* simulate merge to get result positions */
        int res_idx[GRID_SIZE], res_idx2[GRID_SIZE];
        int rcnt = 0;
        for (int i = 0; i < cnt; i++) {
            if (i + 1 < cnt && vals[i] == vals[i+1]) {
                res_idx[rcnt] = idxs[i];
                res_idx2[rcnt] = idxs[i+1];
                rcnt++; i++;
            } else {
                res_idx[rcnt] = idxs[i];
                res_idx2[rcnt] = -1;
                rcnt++;
            }
        }

        /* map results back to grid positions */
        for (int dst = 0; dst < rcnt; dst++) {
            int d = (dir == DIR_RIGHT || dir == DIR_DOWN) ? (GRID_SIZE - 1 - dst) : dst;
            int r = (dir == DIR_UP || dir == DIR_DOWN) ? d : line;
            int c = (dir == DIR_UP || dir == DIR_DOWN) ? line : d;
            s_anim_src[r][c] = res_idx[dst];
            s_anim_src2[r][c] = res_idx2[dst];
        }
    }
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
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    uint32_t color32 = ((uint32_t)color << 16) | color;
    for (int row = 0; row < h; row++) {
        uint16_t *line = &s_fb[s_fb_idx][(y + row) * LCD_H_RES + x];
        int col = 0;
        /* aligned 32-bit writes for speed */
        if (((uintptr_t)line & 3) && w > 0) { *line++ = color; col++; }
        uint32_t *line32 = (uint32_t *)line;
        int w32 = (w - col) / 2;
        for (int i = 0; i < w32; i++) line32[i] = color32;
        col += w32 * 2;
        line = (uint16_t *)(line32 + w32);
        for (; col < w; col++) *line++ = color;
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
                            s_fb[s_fb_idx][(y + sy) * LCD_H_RES + x + sx] = color;
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

static void render_anim(dir_t dir, int step, int total)
{
    /* grid background */
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, rgb565(187, 173, 160));

    bool horiz = (dir == DIR_LEFT || dir == DIR_RIGHT);

    /* draw empty cells first (static), then animated tiles on top */
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (s_grid[r][c] != 0) continue;
            tile_color_t etc = tile_color(0);
            fill_rect(c * CELL_SIZE + 1, r * CELL_SIZE + 1,
                      CELL_SIZE - 2, CELL_SIZE - 2, etc.bg);
        }
    }

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (s_grid[r][c] == 0) continue;
            int src = s_anim_src[r][c];
            int from_x = horiz ? src * CELL_SIZE : c * CELL_SIZE;
            int from_y = horiz ? r * CELL_SIZE : src * CELL_SIZE;
            int to_x   = c * CELL_SIZE;
            int to_y   = r * CELL_SIZE;
            int cur_x  = from_x + (to_x - from_x) * step / total;
            int cur_y  = from_y + (to_y - from_y) * step / total;
            int src2   = s_anim_src2[r][c];

            /* during animation, show pre-merge value so both tiles look identical */
            uint32_t v = (src2 >= 0 && step < total)
                ? s_grid_prev[horiz ? r : src][horiz ? src : c]
                : s_grid[r][c];
            tile_color_t tc = tile_color(v);
            fill_rect(cur_x + 1, cur_y + 1, CELL_SIZE - 2, CELL_SIZE - 2, tc.bg);

            int num_scale = FONT_SCALE;
            int digits = 0; uint32_t vv = v;
            do { digits++; vv /= 10; } while (vv > 0);
            int digit_w = 3 * num_scale + num_scale;
            int total_w = digits * digit_w - num_scale;
            while (total_w > CELL_SIZE - 14 && num_scale > 1) {
                num_scale--;
                digit_w = 3 * num_scale + num_scale;
                total_w = digits * digit_w - num_scale;
            }
            draw_number(cur_x + CELL_SIZE / 2,
                        cur_y + (CELL_SIZE - 5 * num_scale) / 2,
                        v, tc.fg, num_scale);

            /* second source tile sliding in for a merge */
            if (src2 >= 0) {
                int from2_x = horiz ? src2 * CELL_SIZE : c * CELL_SIZE;
                int from2_y = horiz ? r * CELL_SIZE : src2 * CELL_SIZE;
                int cur2_x  = from2_x + (to_x - from2_x) * step / total;
                int cur2_y  = from2_y + (to_y - from2_y) * step / total;
                tile_color_t tc2 = tile_color(s_grid_prev[horiz ? r : src2][horiz ? src2 : c]);
                fill_rect(cur2_x + 1, cur2_y + 1, CELL_SIZE - 2, CELL_SIZE - 2, tc2.bg);
                int num_scale2 = FONT_SCALE;
                uint32_t v2 = s_grid_prev[horiz ? r : src2][horiz ? src2 : c];
                int digits2 = 0; uint32_t vv2 = v2;
                do { digits2++; vv2 /= 10; } while (vv2 > 0);
                int digit_w2 = 3 * num_scale2 + num_scale2;
                int total_w2 = digits2 * digit_w2 - num_scale2;
                while (total_w2 > CELL_SIZE - 14 && num_scale2 > 1) {
                    num_scale2--;
                    digit_w2 = 3 * num_scale2 + num_scale2;
                    total_w2 = digits2 * digit_w2 - num_scale2;
                }
                draw_number(cur2_x + CELL_SIZE / 2,
                            cur2_y + (CELL_SIZE - 5 * num_scale2) / 2,
                            v2, tc2.fg, num_scale2);
            }
        }
    }

    /* score */
    {
        uint32_t v = s_score;
        int digits = 0; uint32_t vv = v;
        do { digits++; vv /= 10; } while (vv > 0);
        int scale = 2;
        int digit_w = 3 * scale + scale;
        int total_w = digits * digit_w - scale;
        draw_number(4 + total_w / 2, SCORE_Y + 10,
                    s_score, rgb565(119, 110, 101), scale);
    }

    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0,
                              LCD_H_RES, LCD_V_RES, s_fb[s_fb_idx]);
    s_fb_idx ^= 1;
}

static void render(void)
{
    /* grid background fills screen */
    fill_rect(0, 0, LCD_H_RES, LCD_V_RES, rgb565(187, 173, 160));

    /* tiles */
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int tx = c * CELL_SIZE;
            int ty = r * CELL_SIZE;
            tile_color_t tc = tile_color(s_grid[r][c]);
            /* 2px gap between tiles shows board background */
            fill_rect(tx + 1, ty + 1, CELL_SIZE - 2, CELL_SIZE - 2, tc.bg);

            if (s_grid[r][c] != 0) {
                int num_scale = FONT_SCALE;
                uint32_t v = s_grid[r][c];
                int digits = 0; uint32_t vv = v;
                do { digits++; vv /= 10; } while (vv > 0);
                int digit_w = 3 * num_scale + num_scale;
                int total_w = digits * digit_w - num_scale;
                while (total_w > CELL_SIZE - 14 && num_scale > 1) {
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

    /* score in top-left corner */
    {
        uint32_t v = s_score;
        int digits = 0; uint32_t vv = v;
        do { digits++; vv /= 10; } while (vv > 0);
        int scale = 2;
        int digit_w = 3 * scale + scale;
        int total_w = digits * digit_w - scale;
        /* left-aligned: draw_number centers at cx, so cx = left + total_w/2 */
        draw_number(4 + total_w / 2, SCORE_Y + 10,
                    s_score, rgb565(119, 110, 101), scale);
    }

    /* game-over dim + tap prompt */
    if (s_game_over) {
        fill_rect(0, 0, LCD_H_RES, LCD_V_RES, rgb565(238, 228, 218));
        // draw "GAME OVER" as score text replacement
    }

    /* push full frame */
    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0,
                              LCD_H_RES, LCD_V_RES, s_fb[s_fb_idx]);
    s_fb_idx ^= 1;
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

    s_fb[0] = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fb[1] = heap_caps_aligned_alloc(64, LCD_FRAME_SIZE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_fb[0] && s_fb[1] ? ESP_OK : ESP_ERR_NO_MEM);
    s_fb_idx = 0;

    reset_game();
    bool dirty = true;

    while (1) {
        dir_t dir;
        if (detect_swipe(&dir) && !s_game_over) {
            memcpy(s_grid_prev, s_grid, sizeof(s_grid));
            if (move_dir(dir)) {
                compute_anims(dir);
                if (!can_move()) s_game_over = true;
                if (s_score > s_best) s_best = s_score;

                /* animate slide */
                for (int step = 0; step <= ANIM_FRAMES; step++) {
                    render_anim(dir, step, ANIM_FRAMES);
                    vTaskDelay(pdMS_TO_TICKS(16));
                }

                spawn_tile();
                dirty = true;
            }
        }

        /* tap to restart when game over */
        if (s_game_over) {
            const touch_state_t *ts = touch_get_state();
            if (ts->points > 0) {
                reset_game();
                dirty = true;
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }

        if (dirty) {
            render();
            dirty = false;
        }
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
