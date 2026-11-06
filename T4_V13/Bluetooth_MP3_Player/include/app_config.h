#pragma once
#include <stdint.h>

// ================= LilyGO T4 V1.3 pin map =================
// 2.4" ILI9341 LCD + microSD + 3 buttons (see official LilyGo_Txx repo,
// LilyGo_Txx/T4_V13.h, and the ESP32_NAS reference project).
//
// Display drive config for TFT_eSPI lives in platformio.ini
// (backlight pin 4, active HIGH, HSPI).

// microSD on its own VSPI bus
#define PIN_SD_MISO            2
#define PIN_SD_MOSI            15
#define PIN_SD_SCLK            14
#define PIN_SD_CS              13
#define SD_SPI_FREQ            10000000UL   // 10 MHz: fast and stable on T4 V1.3

// Three on-board buttons, all active LOW with INPUT_PULLUP.
// GPIO numbers matched to the 3D-printed case as observed on the device:
// pressing the physical LEFT button reads GPIO 38, the MIDDLE one GPIO 37,
// the RIGHT one GPIO 39. (The LilyGo silkscreen order 37/38/39 does not
// match the case orientation.)
#define PIN_BTN_PREV            38   // physical left  -> cursor up / previous
#define PIN_BTN_OK              37   // physical middle -> OK / play-pause
#define PIN_BTN_NEXT            39   // physical right -> cursor down / next
#define PIN_BTN_BOOT            0    // BOOT button (start it from boot; used to restore the device)

// ================= Bluetooth =================
#define BT_DEVICE_NAME          "T4-MusicPlayer"
#define BT_MAX_DEVICES          12          // entries kept for the picker UI

// ================= Playback / libs =================
// Decoded-PCM result queue inside the audio-tools decoder. libhelix decodes
// EVERY complete frame contained in one input chunk in a single burst
// (frames are 4608 bytes), so this queue must absorb available(<1536) +
// a multi-frame burst (up to ~3 frames = 13824) -> 16 KB with headroom.
#define PCM_QUEUE_SIZE          (16 * 1024)
#define MP3_SCAN_WINDOW         (64 * 1024) // bytes scanned to find first frame
                                            // (large embedded album art needs
                                            // more than a small window)

// ================= UI theme =================
// Single-bit theme switch: 1 = light theme (default), 0 = dark theme.
// Change the value and re-flash; the compiler keeps only the selected
// palette, so there is no runtime cost either way.
#define UI_THEME_LIGHT 1

// ================= Behaviour =================
#define LCD_AUTO_SLEEP_MS       180000UL    // backlight off after 3 min idle

#define APP_NAME                "T4 MP3"
#define APP_VERSION             "1.0.0"
