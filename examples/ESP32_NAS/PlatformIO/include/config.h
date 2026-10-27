#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// LILYGO TTGO T4 V1.3 pin definitions, based on T4_V13.h and its schematic.
// The 2.4-inch variant uses the same 320x240 ILI9341 interface.
// ============================================================

// --- microSD card (separate SPI bus from the TFT) ---
#define SD_MOSI     15
#define SD_MISO     2
#define SD_SCK      14
#define SD_CS       13

// --- TFT display (ILI9341, 320x240 landscape) ---
// TFT_eSPI pin assignments are set in platformio.ini.
#define TFT_BL      4
#define TFT_MISO_PIN 12
#define TFT_MOSI_PIN 23
#define TFT_SCLK_PIN 18
#define TFT_CS_PIN   27
#define TFT_DC_PIN   32
#define TFT_RST_PIN  5

// --- Other board pins (not used by NAS firmware) ---
#define SPEAKER_PWR 19
#define SPEAKER_OUT 25

// ============================================================
// WiFi Configuration
// ============================================================
// AP mode is open (no password) — user connects to set up home WiFi
#define AP_SSID               "NAS-Setup"
#define WIFI_HOSTNAME       "esp32-nas"
#define MAX_WIFI_FAIL       10   // non-blocking reconnect attempts before AP fallback

// Default WiFi credentials (optional — secrets.h is gitignored)
#if __has_include("secrets.h")
#include "secrets.h"
#define HAS_DEFAULT_CREDENTIALS 1
#else
#define DEFAULT_SSID ""
#define DEFAULT_PASS ""
#define HAS_DEFAULT_CREDENTIALS 0
#endif

// NVS Preferences keys
#define PREF_NAMESPACE  "nas_wifi"
#define PREF_SSID       "ssid"
#define PREF_PASS       "pass"

// ============================================================
// Server Configuration
// ============================================================
#define HTTP_PORT       80

// ============================================================
// Timing (milliseconds)
// ============================================================
#define SD_POLL_INTERVAL    5000
#define WIFI_RETRY_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 60000
#define SD_RETRY_ANIM_INTERVAL 500   // localized SD-fault animation tick (small area, no flicker)

#endif // CONFIG_H