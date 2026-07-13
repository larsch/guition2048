# 2048 for Guition ESP32-S3-4848S040

A playable 2048 tile-matching game for the Guition ESP32-S3-4848S040
development board (480×480 RGB LCD with ST7701 controller + GT911 touch).

## Gameplay

- Swipe to move tiles. Equal tiles merge.
- Reach 2048 to win, keep playing for higher scores.
- Game over when no moves remain — tap to restart.

## Build & flash

```bash
. /opt/esp-idf/export.sh
export PORT=/dev/ttyUSB0

idf.py build
idf.py -p $PORT flash monitor
```

## Project structure

| File | Purpose |
|------|---------|
| `main/board.h` | Pin definitions, display constants |
| `main/display.h/c` | ST7701 panel init, backlight |
| `main/touch.h/c` | GT911 touch via I²C |
| `main/game_2048.h/c` | Game logic, rendering, animation |
| `main/main.c` | Entry point (~10 lines) |

Replace `game_2048.c` and modify `main.c` to build your own app —
the board support files are reusable as-is.
