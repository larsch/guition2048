#pragma once

#include "sdkconfig.h"

/* ================================================================
 * Board: Guition ESP32-S3-4848S040
 * Pin definitions, display constants, and buffer-mode selection
 * ================================================================ */

/* --- Display dimensions --- */
#define LCD_H_RES             480
#define LCD_V_RES             480
#define LCD_BPP               16
#define LCD_BYTES_PER_PIXEL   2
#define LCD_FRAME_SIZE        (LCD_H_RES * LCD_V_RES * LCD_BYTES_PER_PIXEL)

/* --- RGB interface pins --- */
#define LCD_PIN_PCLK   21
#define LCD_PIN_DE     18
#define LCD_PIN_VSYNC  17
#define LCD_PIN_HSYNC  16

/* --- 3-wire SPI (ST7701 command interface) --- */
#define LCD_PIN_SPI_CS  39
#define LCD_PIN_SPI_SCL 48
#define LCD_PIN_SPI_SDA 47

/* --- Backlight --- */
#define LCD_PIN_BL      38

/* --- Touch (GT911 via I²C) --- */
#define TOUCH_PIN_SDA       19
#define TOUCH_PIN_SCL       45
#define TOUCH_I2C_SPEED_HZ  100000
#define TOUCH_POLL_MS       10
#define TOUCH_BOX_HALF      30

/* --- Timing --- */
#define LCD_PCLK_HZ       (12 * 1000 * 1000)
#define LCD_BOUNCE_LINES  10
#define LCD_BOUNCE_PX      (LCD_H_RES * LCD_BOUNCE_LINES)

/* --- RGB data pins (16-bit: R4G5B5) --- */
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
 * LCD buffer mode selection (from Kconfig)
 * ================================================================ */

#if CONFIG_LCD_BUF_MODE_INTERNAL_FB
 #error "Mode 1 (Internal FB) not available on ESP32-S3. Use mode 2 or higher."
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
