#include "game_2048.h"
#include "board.h"
#include "touch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "2048";

#if LCD_BUF_MODE != 6
#error "This game requires LCD_BUF_MODE 6 (BOUNCE_ONLY). Check sdkconfig."
#endif

#define GRID_SIZE  4
#define CELL_SIZE  120
#define SCORE_Y    2

#define ANIM_DURATION_US  225000

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

/* 3×5 letter font */
static const uint8_t s_font_letter[7][5] = {
    {0b011, 0b100, 0b101, 0b101, 0b011}, // G
    {0b010, 0b101, 0b111, 0b101, 0b101}, // A
    {0b101, 0b111, 0b101, 0b101, 0b101}, // M
    {0b111, 0b100, 0b111, 0b100, 0b111}, // E
    {0b010, 0b101, 0b101, 0b101, 0b010}, // O
    {0b101, 0b101, 0b101, 0b101, 0b010}, // V
    {0b110, 0b101, 0b110, 0b101, 0b101}, // R
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
    {0xCE16, 0x736C},
    {0xEF3B, 0x736C},
    {0xEF19, 0x736C},
    {0xF58F, 0xFFFF},
    {0xF4AC, 0xFFFF},
    {0xF3EB, 0xFFFF},
    {0xF2E7, 0xFFFF},
    {0xEE6E, 0xFFFF},
    {0xEE6C, 0xFFFF},
    {0xEE4A, 0xFFFF},
    {0xEE27, 0xFFFF},
    {0xEE05, 0xFFFF},
};

static inline int tile_idx_from_val(uint32_t val)
{
    if (val == 0) return 0;
    int idx = 31 - __builtin_clz(val);
    return (idx > 11) ? 11 : idx;
}

/* ================================================================
 * Pre-rendered tiles (PSRAM) — 118×118
 * ================================================================ */

#define TILE_PX  (CELL_SIZE - 2)

static uint16_t *s_tiles[12];

static void blit_char_buf(uint16_t *buf, int stride, int cx, int cy,
                           char ch, uint16_t color, int scale)
{
    if (ch < '0' || ch > '9') return;
    const uint8_t *glyph = s_font[ch - '0'];
    for (int row = 0; row < 5; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 3; col++) {
            if (bits & (1 << (2 - col))) {
                int x = cx + col * scale;
                int y = cy + row * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        buf[(y + sy) * stride + x + sx] = color;
            }
        }
    }
}

static int letter_idx(char ch)
{
    switch (ch) {
        case 'G': case 'g': return 0;
        case 'A': case 'a': return 1;
        case 'M': case 'm': return 2;
        case 'E': case 'e': return 3;
        case 'O': case 'o': return 4;
        case 'V': case 'v': return 5;
        case 'R': case 'r': return 6;
        default: return -1;
    }
}

static void blit_letter_buf(uint16_t *buf, int stride, int cx, int cy,
                              char ch, uint16_t color, int scale)
{
    int idx = letter_idx(ch);
    if (idx < 0) return;
    const uint8_t *glyph = s_font_letter[idx];
    for (int row = 0; row < 5; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 3; col++) {
            if (bits & (1 << (2 - col))) {
                int x = cx + col * scale;
                int y = cy + row * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        buf[(y + sy) * stride + x + sx] = color;
            }
        }
    }
}

static void prerender_tiles(void)
{
    ESP_LOGI(TAG, "Pre-rendering %d tiles (%lu KB PSRAM) ...",
             12, (unsigned long)(12 * TILE_PX * TILE_PX * 2 / 1024));

    for (int idx = 0; idx < 12; idx++) {
        s_tiles[idx] = heap_caps_aligned_alloc(64,
            TILE_PX * TILE_PX * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(s_tiles[idx] ? ESP_OK : ESP_ERR_NO_MEM);

        tile_color_t tc = s_colors[idx];
        uint32_t tc32 = ((uint32_t)tc.bg << 16) | tc.bg;

        for (int y = 0; y < TILE_PX; y++) {
            uint16_t *row = s_tiles[idx] + y * TILE_PX;
            int x = 0;
            if (((uintptr_t)row & 3) && TILE_PX > 0) { row[0] = tc.bg; x++; }
            for (; x + 1 < TILE_PX; x += 2)
                *(uint32_t *)(row + x) = tc32;
            if (x < TILE_PX) row[x] = tc.bg;
        }

        if (idx >= 1) {
            uint32_t val = 1U << idx;
            char buf[16]; int len = 0;
            uint32_t v = val;
            do { buf[len++] = '0' + (v % 10); v /= 10; } while (v > 0);
            for (int i = 0; i < len / 2; i++) {
                char t = buf[i]; buf[i] = buf[len-1-i]; buf[len-1-i] = t;
            }
            int scale = 4;
            int digit_w = 3 * scale + scale;
            int total_w = len * digit_w - scale;
            while (total_w > TILE_PX - 14 && scale > 1) {
                scale--; digit_w = 3 * scale + scale; total_w = len * digit_w - scale;
            }
            int sx = (TILE_PX - total_w) / 2;
            int cy = (TILE_PX - 5 * scale) / 2;
            for (int i = 0; i < len; i++)
                blit_char_buf(s_tiles[idx], TILE_PX, sx + i * digit_w, cy,
                              buf[i], tc.fg, scale);
        }
    }
    ESP_LOGI(TAG, "Tiles ready");
}

/* ================================================================
 * Direction enum
 * ================================================================ */

typedef enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } dir_t;

/* ================================================================
 * Double-buffered game state (ISR reads, game task writes)
 * ================================================================ */

#define SCORE_BUF_W  128
#define SCORE_BUF_H  12  /* 5*scale + 2px padding */

#define GAMEOVER_BUF_W  256
#define GAMEOVER_BUF_H  64

typedef struct {
    uint32_t grid[GRID_SIZE][GRID_SIZE];
    uint32_t score;
    bool     game_over;

    /* animation — valid when anim_active */
    bool     anim_active;
    dir_t    anim_dir;
    int64_t  anim_start_us;
    uint32_t grid_prev[GRID_SIZE][GRID_SIZE];
    int8_t   anim_src[GRID_SIZE][GRID_SIZE];
    int8_t   anim_src2[GRID_SIZE][GRID_SIZE];

    /* pre-rendered score bitmap (game task writes, ISR copies) */
    uint16_t score_buf[SCORE_BUF_H][SCORE_BUF_W];
    int      score_w;

    /* pre-rendered game-over overlay (darken is done per-pixel by ISR) */
    uint16_t gameover_buf[GAMEOVER_BUF_H][GAMEOVER_BUF_W];
    int      gameover_w;
    int      gameover_h;
} game_state_t;

static game_state_t s_state[2];
static volatile int s_state_write;  /* game task writes here */
static volatile int s_state_ready;  /* ready for ISR (-1 = none) */
static volatile int s_state_read;   /* ISR reads from here */

static uint32_t s_best;

/* ================================================================
 * Game logic (operates on current write-buffer state)
 * ================================================================ */

static game_state_t *gs(void)  { return &s_state[s_state_write]; }

static void spawn_tile(void)
{
    int empty = 0;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (gs()->grid[r][c] == 0) empty++;
    if (empty == 0) return;

    int target = esp_random() % empty;
    int val = (esp_random() % 10) < 9 ? 2 : 4;

    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (gs()->grid[r][c] == 0) {
                if (target == 0) { gs()->grid[r][c] = val; return; }
                target--;
            }
}

static bool slide_row(uint32_t *row, uint32_t *score)
{
    bool moved = false;
    uint32_t tmp[GRID_SIZE] = {0};
    int pos = 0;

    for (int i = 0; i < GRID_SIZE; i++)
        if (row[i] != 0) tmp[pos++] = row[i];

    for (int i = 0; i < pos - 1; i++)
        if (tmp[i] == tmp[i+1]) {
            tmp[i] *= 2;
            *score += tmp[i];
            tmp[i+1] = 0;
            moved = true;
        }

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

static void transpose_grid(uint32_t g[GRID_SIZE][GRID_SIZE])
{
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = r + 1; c < GRID_SIZE; c++) {
            uint32_t t = g[r][c];
            g[r][c] = g[c][r];
            g[c][r] = t;
        }
}

static bool move_dir(dir_t dir, uint32_t *score)
{
    bool moved = false;
    uint32_t (*g)[4] = gs()->grid;

    if (dir == DIR_UP || dir == DIR_DOWN) transpose_grid(g);

    for (int r = 0; r < GRID_SIZE; r++) {
        if (dir == DIR_RIGHT || dir == DIR_DOWN) reverse_row(g[r]);
        if (slide_row(g[r], score)) moved = true;
        if (dir == DIR_RIGHT || dir == DIR_DOWN) reverse_row(g[r]);
    }

    if (dir == DIR_UP || dir == DIR_DOWN) transpose_grid(g);

    return moved;
}

static void compute_anims(game_state_t *st, dir_t dir)
{
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            st->anim_src[r][c] = st->anim_src2[r][c] = -1;

    for (int line = 0; line < GRID_SIZE; line++) {
        int vals[GRID_SIZE], idxs[GRID_SIZE], cnt = 0;
        for (int i = 0; i < GRID_SIZE; i++) {
            int r = (dir == DIR_UP || dir == DIR_DOWN) ? i : line;
            int c = (dir == DIR_UP || dir == DIR_DOWN) ? line : i;
            if (dir == DIR_RIGHT || dir == DIR_DOWN) {
                r = (dir == DIR_DOWN) ? (GRID_SIZE - 1 - i) : line;
                c = (dir == DIR_UP || dir == DIR_DOWN) ? line : (GRID_SIZE - 1 - i);
            }
            if (st->grid_prev[r][c]) {
                vals[cnt] = st->grid_prev[r][c];
                idxs[cnt] = (dir == DIR_UP || dir == DIR_DOWN) ? r : c;
                cnt++;
            }
        }

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

        for (int dst = 0; dst < rcnt; dst++) {
            int d = (dir == DIR_RIGHT || dir == DIR_DOWN)
                  ? (GRID_SIZE - 1 - dst) : dst;
            int r = (dir == DIR_UP || dir == DIR_DOWN) ? d : line;
            int c = (dir == DIR_UP || dir == DIR_DOWN) ? line : d;
            st->anim_src[r][c] = (int8_t)res_idx[dst];
            st->anim_src2[r][c] = (int8_t)res_idx2[dst];
        }
    }
}

static bool can_move(const game_state_t *st)
{
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++) {
            if (st->grid[r][c] == 0) return true;
            if (c < GRID_SIZE - 1 && st->grid[r][c] == st->grid[r][c+1]) return true;
            if (r < GRID_SIZE - 1 && st->grid[r][c] == st->grid[r+1][c]) return true;
        }
    return false;
}

static void commit_state(void)
{
    s_state_ready = s_state_write;
    s_state_write ^= 1;
    /* copy live state into new write buffer */
    memcpy(&s_state[s_state_write], &s_state[s_state_write ^ 1], sizeof(game_state_t));
}

/* render score into gs()->score_buf — white bg, black text, 1px padding */
static void render_score(void)
{
    game_state_t *st = gs();
    uint16_t *buf = &st->score_buf[0][0];
    const uint16_t white = rgb565(255, 255, 255);
    const uint16_t black = 0x0000;

    /* fill with white */
    for (int i = 0; i < SCORE_BUF_H * SCORE_BUF_W; i++)
        buf[i] = white;

    char tmp[16];
    int len = 0;
    uint32_t v = st->score;
    if (v == 0) tmp[len++] = '0';
    else { do { tmp[len++] = '0' + (v % 10); v /= 10; } while (v > 0); }
    for (int i = 0; i < len / 2; i++) {
        char t = tmp[i]; tmp[i] = tmp[len - 1 - i]; tmp[len - 1 - i] = t;
    }

    int scale = 2;
    int digit_w = 3 * scale + scale;
    int text_w = len * digit_w - scale;
    st->score_w = text_w + 2;  /* +1px border each side */

    int px = 1, py = 1;  /* 1px padding */
    for (int i = 0; i < len; i++)
        blit_char_buf(buf, SCORE_BUF_W, px + i * digit_w, py,
                      tmp[i], black, scale);
}

/* render game-over overlay (large score + "GAME OVER" text) */
static void render_gameover(void)
{
    game_state_t *st = gs();
    uint16_t *buf = &st->gameover_buf[0][0];

    /* transparent background — ISR darkens screen first */
    memset(buf, 0, sizeof(st->gameover_buf));

    /* large score (scale 5, white) */
    {
        char tmp[16];
        int len = 0;
        uint32_t v = st->score;
        if (v == 0) tmp[len++] = '0';
        else { do { tmp[len++] = '0' + (v % 10); v /= 10; } while (v > 0); }
        for (int i = 0; i < len / 2; i++) {
            char t = tmp[i]; tmp[i] = tmp[len - 1 - i]; tmp[len - 1 - i] = t;
        }
        int scale = 5;
        int digit_w = 3 * scale + scale;
        int text_w = len * digit_w - scale;
        int sx = (GAMEOVER_BUF_W - text_w) / 2;
        for (int i = 0; i < len; i++)
            blit_char_buf(buf, GAMEOVER_BUF_W, sx + i * digit_w, 0,
                          tmp[i], 0xFFFF, scale);
    }

    /* "GAME OVER" text below score (scale 3, white) */
    {
        const char *msg = "GAME OVER";
        int len = strlen(msg);
        int scale = 3;
        int letter_w = 3 * scale + scale;
        int text_w = len * letter_w - scale;
        int sx = (GAMEOVER_BUF_W - text_w) / 2;
        int sy = 25 + 8;  /* below score */
        for (int i = 0; i < len; i++)
            blit_letter_buf(buf, GAMEOVER_BUF_W, sx + i * letter_w, sy,
                            msg[i], 0xFFFF, scale);
        st->gameover_h = sy + 5 * scale;  /* total height */
    }
    st->gameover_w = GAMEOVER_BUF_W;
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
        if (!active) { active = true; start_x = last_x = cx; start_y = last_y = cy; }
        else         { last_x = cx; last_y = cy; }
        return false;
    }
    if (!active) return false;
    active = false;

    int dx = last_x - start_x, dy = last_y - start_y;
    int adx = dx > 0 ? dx : -dx, ady = dy > 0 ? dy : -dy;
    if (adx < SWIPE_THRESHOLD && ady < SWIPE_THRESHOLD) return false;

    if (adx > 2 * ady)
        *out_dir = (dx > 0) ? DIR_RIGHT : DIR_LEFT;
    else if (ady > 2 * adx)
        *out_dir = (dy > 0) ? DIR_DOWN  : DIR_UP;
    else
        return false;  /* too diagonal */
    return true;
}

/* ================================================================
 * ISR helpers — all IRAM
 * ================================================================ */

static inline IRAM_ATTR void fill_row(uint16_t *row, uint16_t color)
{
    uint32_t c32 = ((uint32_t)color << 16) | color;
    int x = 0;
    if (((uintptr_t)row & 3)) { row[0] = color; x++; }
    for (; x + 1 < LCD_H_RES; x += 2)
        *(uint32_t *)(row + x) = c32;
    if (x < LCD_H_RES) row[x] = color;
}

/* copy one scanline of a tile into the bounce buffer, clipping to screen */
static inline IRAM_ATTR void isr_blit_tile_row(uint16_t *row, int tx,
                                                uint16_t *tile_src, int tile_row)
{
    int dx = tx + 1;  /* 1px gap */
    int src_x = 0;
    int copy_w = TILE_PX;

    if (dx < 0) { src_x = -dx; copy_w -= src_x; dx = 0; }
    if (dx + copy_w > LCD_H_RES) copy_w = LCD_H_RES - dx;
    if (copy_w <= 0) return;

    uint32_t *dst32 = (uint32_t *)(row + dx);
    const uint32_t *src32 = (const uint32_t *)(tile_src + tile_row * TILE_PX
                                                + src_x);
    int words = copy_w / 2;
    for (int i = 0; i < words; i++)
        dst32[i] = src32[i];

    if (copy_w & 1) {
        int tail = words * 2;
        row[dx + tail] = tile_src[tile_row * TILE_PX + src_x + tail];
    }
}

/* ================================================================
 * ISR callbacks
 * ================================================================ */

static SemaphoreHandle_t s_vsync_sem;
static DRAM_ATTR int64_t s_frame_elapsed;

static IRAM_ATTR bool on_vsync_isr(esp_lcd_panel_handle_t panel,
                                   const esp_lcd_rgb_panel_event_data_t *edata,
                                   void *ctx)
{
    (void)panel; (void)edata; (void)ctx;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (s_state_ready >= 0) {
        s_state_read = s_state_ready;
        s_state_ready = -1;
    }

    xSemaphoreGiveFromISR(s_vsync_sem, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
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

    const game_state_t *st = &s_state[s_state_read];
    const uint16_t board_bg = rgb565(187, 173, 160);

    /* compute elapsed once per frame */
    if (pos_px == 0) {
        if (st->anim_active) {
            int64_t now = esp_timer_get_time();
            s_frame_elapsed = now - st->anim_start_us;
            if (s_frame_elapsed > ANIM_DURATION_US)
                s_frame_elapsed = ANIM_DURATION_US;
        } else {
            s_frame_elapsed = 0;
        }
    }

    bool horiz = (st->anim_dir == DIR_LEFT || st->anim_dir == DIR_RIGHT);

    for (int y = 0; y < lines; y++) {
        int sy = start_ln + y;
        uint16_t *row = dst + y * LCD_H_RES;

        fill_row(row, board_bg);

        for (int r = 0; r < GRID_SIZE; r++) {
            for (int c = 0; c < GRID_SIZE; c++) {
                if (st->grid[r][c] == 0) continue;

                int tile_x, tile_y;
                if (st->anim_active && st->anim_src[r][c] >= 0) {
                    int from = st->anim_src[r][c] * CELL_SIZE;
                    int to   = horiz ? (c * CELL_SIZE) : (r * CELL_SIZE);
                    int cur  = from + (int)((to - from) * s_frame_elapsed
                                           / ANIM_DURATION_US);
                    if (horiz) { tile_x = cur;       tile_y = r * CELL_SIZE; }
                    else       { tile_x = c * CELL_SIZE; tile_y = cur; }
                } else {
                    tile_x = c * CELL_SIZE;
                    tile_y = r * CELL_SIZE;
                }

                /* primary tile */
                int ty0 = tile_y + 1;
                if (sy >= ty0 && sy < ty0 + TILE_PX) {
                    uint32_t val = st->grid[r][c];
                    /* during merge animation, show pre-merge value */
                    if (st->anim_active && st->anim_src2[r][c] >= 0 &&
                        s_frame_elapsed < ANIM_DURATION_US) {
                        int sr = horiz ? r : st->anim_src[r][c];
                        int sc = horiz ? st->anim_src[r][c] : c;
                        val = st->grid_prev[sr][sc];
                    }
                    int ti = tile_idx_from_val(val);
                    isr_blit_tile_row(row, tile_x, s_tiles[ti], sy - ty0);
                }

                /* second source tile (merge partner) */
                if (st->anim_active && st->anim_src2[r][c] >= 0) {
                    int src2 = st->anim_src2[r][c];
                    int from2 = src2 * CELL_SIZE;
                    int to2   = horiz ? (c * CELL_SIZE) : (r * CELL_SIZE);
                    int cur2  = from2 + (int)((to2 - from2) * s_frame_elapsed
                                             / ANIM_DURATION_US);
                    int tx2, ty2;
                    if (horiz) { tx2 = cur2;       ty2 = r * CELL_SIZE; }
                    else       { tx2 = c * CELL_SIZE; ty2 = cur2; }

                    int ty2_0 = ty2 + 1;
                    if (sy >= ty2_0 && sy < ty2_0 + TILE_PX) {
                        int sr = horiz ? r : src2;
                        int sc = horiz ? src2 : c;
                        uint32_t v2 = st->grid_prev[sr][sc];
                        int ti2 = tile_idx_from_val(v2);
                        isr_blit_tile_row(row, tx2, s_tiles[ti2], sy - ty2_0);
                    }
                }
            }
        }

        /* score on top of tiles (pre-rendered by game task) */
        if (sy >= SCORE_Y && sy < SCORE_Y + SCORE_BUF_H) {
            int score_row = sy - SCORE_Y;
            if (st->score_w > 0 && st->score_w <= SCORE_BUF_W) {
                memcpy(row + 2, st->score_buf[score_row], st->score_w * 2);
            }
        }

        /* game-over: 50% darken + overlay */
        if (st->game_over) {
            for (int x = 0; x < LCD_H_RES; x++)
                row[x] = (row[x] & 0xF7DE) >> 1;

            int go_y0 = (LCD_V_RES - st->gameover_h) / 2;
            if (sy >= go_y0 && sy < go_y0 + st->gameover_h) {
                int go_row = sy - go_y0;
                int go_x0 = (LCD_H_RES - st->gameover_w) / 2;
                if (go_x0 < 0) go_x0 = 0;
                int copy_w = st->gameover_w;
                if (go_x0 + copy_w > LCD_H_RES) copy_w = LCD_H_RES - go_x0;
                const uint16_t *src = st->gameover_buf[go_row];
                for (int x = 0; x < copy_w; x++) {
                    if (src[x] != 0)  /* 0 = transparent */
                        row[go_x0 + x] = src[x];
                }
            }
        }
    }

    return false;
}

static const esp_lcd_rgb_panel_event_callbacks_t s_isr_cbs = {
    .on_vsync        = on_vsync_isr,
    .on_bounce_empty = on_bounce_empty,
};

const esp_lcd_rgb_panel_event_callbacks_t *game_get_isr_callbacks(void)
{
    return &s_isr_cbs;
}

/* ================================================================
 * Game task — logic only, no rendering
 * ================================================================ */

static int64_t s_game_over_time;

static void game_task(void *arg)
{
    (void)arg;

    /* initial state */
    memset(gs(), 0, sizeof(game_state_t));
    gs()->grid[3][1] = 8;
    gs()->grid[3][2] = 8;
    gs()->grid[3][3] = 16;
    render_score();
    commit_state();

    while (1) {
        dir_t dir;
        if (detect_swipe(&dir) && !gs()->game_over) {
            /* snapshot pre-move grid */
            memcpy(gs()->grid_prev, gs()->grid, sizeof(gs()->grid));
            uint32_t score_add = 0;

            if (move_dir(dir, &score_add)) {
                gs()->score += score_add;
                if (gs()->score > s_best) s_best = gs()->score;
                compute_anims(gs(), dir);

                gs()->anim_active   = true;
                gs()->anim_dir      = dir;
                gs()->anim_start_us = esp_timer_get_time();
                render_score();
                commit_state();

                /* wait for animation to finish — ISR renders the slides */
                vTaskDelay(pdMS_TO_TICKS(ANIM_DURATION_US / 1000));

                /* spawn new tile and end animation */
                spawn_tile();
                gs()->anim_active = false;

                if (!can_move(gs())) {
                    gs()->game_over = true;
                    s_game_over_time = esp_timer_get_time();
                    render_gameover();
                }
                render_score();
                commit_state();
            }
        }

        /* tap to restart (2 s cooldown) */
        if (gs()->game_over) {
            const touch_state_t *ts = touch_get_state();
            if (ts->points > 0 &&
                (esp_timer_get_time() - s_game_over_time) > 2000000) {
                memset(gs(), 0, sizeof(game_state_t));
                spawn_tile();
                spawn_tile();
                render_score();
                commit_state();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

void game_2048_init(void)
{
    s_state_write = 0;
    s_state_ready = -1;
    s_state_read  = 0;

    s_vsync_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_vsync_sem ? ESP_OK : ESP_ERR_NO_MEM);

    prerender_tiles();
    ESP_LOGI(TAG, "Init done — %lu KB PSRAM (tiles only, no framebuffers)",
             (unsigned long)(12 * TILE_PX * TILE_PX * 2 / 1024));
}

void game_2048_start(void)
{
    ESP_LOGI(TAG, "Starting 2048 (mode 6, ISR renders tiles direct-to-bounce)");
    xTaskCreate(game_task, "2048", 4096, NULL, 5, NULL);
}
