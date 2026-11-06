/*
 * main.cpp — Bluetooth MP3 player for the LilyGO T4 V1.3 (2.4" ILI9341).
 *
 * Flow:
 *   1. Init SD (VSPI) and scan the card for .mp3 files into a playlist.
 *   2. Init LVGL UI + three buttons (left/prev, middle/OK, right/next).
 *   3. Start the Bluetooth A2DP source (classic Bluetooth):
 *        - if the NVS still holds the last used sink, try to reconnect;
 *        - otherwise keep scanning nearby audio sinks for the picker.
 *   4. Loop: LVGL timer handler; when playing, decoded MP3 PCM streams from
 *      the SD card into the A2DP data callback at 44.1 kHz stereo.
 *
 * Threading: everything on the loop task except the A2DP data callback +
 * device-scan callback, which run on the Bluetooth task (see engine.h).
 */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <lvgl.h>
#include "app_config.h"
#include "playlist.h"
#include "ui.h"
#include "engine.h"

static Playlist playlist;

static SPIClass sdSPI(VSPI);
static SPIClass *sdSPIPtr = nullptr;

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.printf("[%s %s] boot\n", APP_NAME, APP_VERSION);

    // Backlight on early so the user sees the boot screen (pin 4, active HIGH)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // ---- SD card (VSPI: MISO=2 MOSI=15 SCK=14 CS=13) ----
    sdSPIPtr = &sdSPI;
    sdSPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    bool sdOk = SD.begin(PIN_SD_CS, sdSPI, SD_SPI_FREQ);
    if (!sdOk)
    {
        Serial.println("SD init FAILED - continuing without SD");
    }
    else
    {
        uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);
        Serial.printf("SD ok: %llu MB\n", cardSize);
    }

    // ---- UI + buttons first (shows boot/home screen) ----
    uiInit(&playlist);

    // ---- Playlist scan (may take a moment on big cards) ----
    if (sdOk)
    {
        size_t n = playlist.load();
        Serial.printf("Playlist: %d track(s)\n", (int)n);
    }

    // ---- Audio engine + Bluetooth (starts scanning / reconnecting) ----
    // The classic BT stack claims a large chunk of internal heap while it
    // starts; log it so OOM problems at bluedroid init are visible.
    Serial.printf("Free heap before BT: %u, PSRAM: %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());
    AudioEngine::instance().init(&playlist);
    Serial.println("A2DP source started");

    // Boot straight into the Bluetooth device list: scanning starts on its
    // own and the screen fills up as devices are found — no button press
    // needed. If the last device reconnects in the background, the UI jumps
    // to the player screen automatically.
    if (AudioEngine::instance().sinkState() != AudioEngine::SINK_CONNECTED)
    {
        openPicker();
    }
}

void loop()
{
    // pumps LVGL + button polling via the LVGL timer
    uiTick();

    // engine queue work: end-of-track handling and autoplay
    AudioEngine::instance().work();
}
