/*
 * ui.cpp — LVGL 8 UI for the LilyGO T4 V1.3 (ILI9341 240x320 portrait).
 *
 * Look: light theme by default (UI_THEME_LIGHT=1 in include/app_config.h;
 * set it to 0 for the dark theme) — soft gray-blue background, white
 * panels, blue accent, rounded panels, and a status bar on every screen
 * that mirrors the Bluetooth link state live.
 *
 * Screens:
 *   1. Bluetooth picker (boot default): discovered devices; left = cursor
 *      up, right = cursor down, middle = connect. Pressing OK on the
 *      currently connected device ejects it. Long-press middle = back.
 *      The picker re-opens by itself whenever the link is lost.
 *   2. Now playing: title, play state, progress, connected device.
 *      left = previous, right = next (hold = fast repeat),
 *      middle = play/pause, long-press middle = device picker.
 *   3. Home dashboard: SD card + Bluetooth overview (shown when leaving
 *      the picker without a connection).
 *
 * Buttons: three active-LOW buttons with INPUT_PULLUP, polled through the
 * vendored Button2 library inside an LVGL timer; prev/next auto-repeat
 * while held.
 */
#include "ui.h"
#include "engine.h"
#include "playlist.h"
#include "app_config.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// TFT / LVGL integration
// ---------------------------------------------------------------------------
static TFT_eSPI tft;

#define DRAW_BUF_ROWS 10
#define DRAW_BUF_PX  (320 * DRAW_BUF_ROWS)
static lv_disp_draw_buf_t drawBuf;
static lv_color_t drawBufMem[DRAW_BUF_PX];
static lv_disp_drv_t dispDrv;

static Playlist *uiPlaylist = nullptr;

void invalidateNowPlaying();

// ---------------------------------------------------------------------------
// Theme — switch with UI_THEME_LIGHT in include/app_config.h (1 = light,
// 0 = dark). Both palettes live here; the compiler keeps only the selected
// one, so flipping the bit costs nothing at runtime.
// ---------------------------------------------------------------------------
#if UI_THEME_LIGHT

#define COL_BG       0xE9EEF5
#define COL_PANEL    0xFFFFFF
#define COL_PANEL2   0xCBD8E6
#define COL_ACCENT   0x0A6CFF
#define COL_ACCENT2  0x0B9E58
#define COL_WARN     0xE68A00
#define COL_TEXT     0x1A2430
#define COL_MUTED    0x66738A
#define COL_ROW      0xFFFFFF
#define COL_ROW_SEL  0xD7E8FF
#define COL_CHIP_TXT 0xFFFFFF
#define COL_RAY_DIM  0xD3DDE9
#define COL_SCREEN_INIT TFT_WHITE

#else

#define COL_BG       0x0A0E14
#define COL_PANEL    0x111823
#define COL_PANEL2   0x182230
#define COL_ACCENT   0x00D0FF
#define COL_ACCENT2  0x00E68C
#define COL_WARN     0xFFC94D
#define COL_TEXT     0xE6EDF5
#define COL_MUTED    0x7C8AA0
#define COL_ROW      0x141D29
#define COL_ROW_SEL  0x0C3B54
#define COL_CHIP_TXT 0x052330
#define COL_RAY_DIM  0x24313F
#define COL_SCREEN_INIT TFT_BLACK

#endif

static void stylePanel(lv_obj_t *obj, uint32_t bg)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 10, 0);
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, uint8_t font,
                           uint32_t color)
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, text);
    const lv_font_t *f;
    switch (font)
    {
    case 20:
        f = &lv_font_montserrat_20;
        break;
    case 16:
        f = &lv_font_montserrat_16;
        break;
    case 10:
        f = &lv_font_montserrat_10;
        break;
    default:
        f = &lv_font_montserrat_14;
        break;
    }
    lv_obj_set_style_text_font(lab, f, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    return lab;
}

// ---------------------------------------------------------------------------
// Screen management (declared early: the button handlers use these)
// ---------------------------------------------------------------------------
enum Screen
{
    SCR_HOME,
    SCR_NOW_PLAYING,
    SCR_PICKER
};
static Screen currentScreen = SCR_HOME;
static lv_obj_t *homeScr = nullptr;
static lv_obj_t *nowScr = nullptr;
static lv_obj_t *pickerScr = nullptr;

static void switchScreen(Screen s);
void openPicker();
static void pickerMoveCursor(int dy);
static void pickerConfirm();
static void pickerCancel();

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area,
                          lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // lv_color_t is RGB565 (LV_COLOR_16_SWAP = 0); TFT_eSPI pushColors
    // with swap=true converts to the wire format for the ILI9341.
    tft.pushColors((uint16_t *)color_p, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// Button wiring (Button2)
// ---------------------------------------------------------------------------
static Button2 btnPrev, btnOk, btnNext, btnBoot;

// hold-to-change-volume bookkeeping (left = down, right = up)
static uint32_t prevDownMs = 0, nextDownMs = 0, lastVolStepMs = 0;
static bool prevWasDown = false, nextWasDown = false;
static const uint32_t VOL_DELAY_MS = 450; // hold before volume changes start
static const uint32_t VOL_STEP_MS = 180;  // volume repeat interval

void onPrev(Button2 &b)
{
    // fired on release: act only on a quick tap — a long hold was already
    // handled as volume-down while the button was down
    if (b.wasPressedFor() >= LONGCLICK_MS)
        return;
    if (currentScreen == SCR_PICKER)
    {
        pickerMoveCursor(-1);
    }
    else
    {
        AudioEngine::instance().prev();
        invalidateNowPlaying();
    }
}

void onNext(Button2 &b)
{
    if (b.wasPressedFor() >= LONGCLICK_MS)
        return;
    if (currentScreen == SCR_PICKER)
    {
        pickerMoveCursor(+1);
    }
    else
    {
        AudioEngine::instance().next();
        invalidateNowPlaying();
    }
}

void onOkClick(Button2 &)
{
    switch (currentScreen)
    {
    case SCR_HOME:
        if (AudioEngine::instance().sinkState() == AudioEngine::SINK_CONNECTED)
            switchScreen(SCR_NOW_PLAYING);
        else
            openPicker();
        break;
    case SCR_NOW_PLAYING:
        AudioEngine::instance().togglePlay();
        invalidateNowPlaying();
        break;
    case SCR_PICKER:
        pickerConfirm();
        break;
    default:
        break;
    }
}

void onOkLongPress(Button2 &)
{
    if (currentScreen == SCR_NOW_PLAYING)
        openPicker(); // pick a different device while playing
    else if (currentScreen == SCR_PICKER)
        pickerCancel();
}

void onBootButton(Button2 &)
{
    // full rescan: reload the playlist from the card, then refresh UI.
    // suspendAudio() keeps the BT task from reading the card mid-rescan.
    if (uiPlaylist)
    {
        AudioEngine::instance().suspendAudio();
        uiPlaylist->load();
        AudioEngine::instance().resumeAudio();
        lv_obj_invalidate(lv_scr_act());
        invalidateNowPlaying();
    }
}

static void setupButtons()
{
    // This vendored Button2 (older variant) has no begin(); the constructor
    // sets the pin, mode (INPUT_PULLUP default) and debounce.
    btnPrev = Button2(PIN_BTN_PREV);
    btnNext = Button2(PIN_BTN_NEXT);
    btnOk = Button2(PIN_BTN_OK);
    btnBoot = Button2(PIN_BTN_BOOT);

    // prev/next act when released after a quick tap; holding changes volume
    btnPrev.setReleasedHandler(onPrev);
    btnNext.setReleasedHandler(onNext);
    btnOk.setClickHandler(onOkClick);
    btnOk.setLongClickHandler(onOkLongPress);
    btnBoot.setClickHandler(onBootButton);
}

void buttonsTick()
{
    btnPrev.loop();
    btnNext.loop();
    btnOk.loop();
    btnBoot.loop();
}

// ---------------------------------------------------------------------------
// Screen management
// ---------------------------------------------------------------------------
static void switchScreen(Screen s)
{
    if (currentScreen == s)
        return;
    currentScreen = s;
    lv_obj_t *scr = nullptr;
    switch (s)
    {
    case SCR_HOME:
        scr = homeScr;
        break;
    case SCR_NOW_PLAYING:
        scr = nowScr;
        break;
    case SCR_PICKER:
        scr = pickerScr;
        break;
    }
    if (!scr)
        return;
    lv_scr_load(scr);
    invalidateNowPlaying();
}

void invalidateNowPlaying()
{
    if (nowScr)
        lv_obj_invalidate(nowScr);
}

// ---------------------------------------------------------------------------
// Status bar (shared look on every screen)
// ---------------------------------------------------------------------------
static lv_obj_t *homeState = nullptr;
static lv_obj_t *nowState = nullptr;
static lv_obj_t *pickerState = nullptr;
static char statusText[40] = "";

// Returns the state label of the status bar; tick() keeps it in sync.
static lv_obj_t *makeStatusBar(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 240, 26);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    stylePanel(bar, COL_PANEL);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = makeLabel(bar, title, 14, COL_ACCENT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *s = makeLabel(bar, "", 10, COL_MUTED);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, 110); // leaves room for the title on the left
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, -8, 0);

    // accent underline
    static lv_obj_t *line = nullptr; // one style object is enough per screen;
    line = lv_obj_create(parent);    // keep last reference alive
    lv_obj_set_size(line, 240, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    return s;
}

static void updateStatusLabel(lv_obj_t *lab)
{
    if (!lab)
        return;
    AudioEngine &eng = AudioEngine::instance();
    char buf[40];
    uint32_t col;
    switch (eng.sinkState())
    {
    case AudioEngine::SINK_CONNECTED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s",
                 eng.sinkName()[0] ? eng.sinkName() : "LINK");
        col = COL_ACCENT2;
        break;
    case AudioEngine::SINK_CONNECTING:
        snprintf(buf, sizeof(buf), "CONNECTING");
        col = COL_WARN;
        break;
    default:
        snprintf(buf, sizeof(buf), "SCANNING");
        col = COL_MUTED;
        break;
    }
    if (strcmp(buf, statusText) != 0)
    {
        strlcpy(statusText, buf, sizeof(statusText));
        lv_label_set_text(lab, buf);
        lv_obj_set_style_text_color(lab, lv_color_hex(col), 0);
    }
    else
    {
        lv_obj_set_style_text_color(lab, lv_color_hex(col), 0);
    }
}

// ---------------------------------------------------------------------------
// Home screen
// ---------------------------------------------------------------------------
static lv_obj_t *homeTracks = nullptr;
static lv_obj_t *homeBT = nullptr;
static lv_obj_t *homeHint = nullptr;

static void buildHomeScreen()
{
    homeScr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(homeScr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(homeScr, LV_OPA_COVER, 0);

    homeState = makeStatusBar(homeScr, "T4 MP3");

    lv_obj_t *card = lv_obj_create(homeScr);
    lv_obj_set_size(card, 216, 96);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 48);
    stylePanel(card, COL_PANEL);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = makeLabel(card, "SD CARD", 10, COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 12, 8);
    homeTracks = makeLabel(card, "no card", 14, COL_TEXT);
    lv_obj_align(homeTracks, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    lv_obj_t *card2 = lv_obj_create(homeScr);
    lv_obj_set_size(card2, 216, 96);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 156);
    stylePanel(card2, COL_PANEL);
    lv_obj_set_style_pad_all(card2, 0, 0);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap2 = makeLabel(card2, "BLUETOOTH", 10, COL_MUTED);
    lv_obj_align(cap2, LV_ALIGN_TOP_LEFT, 12, 8);
    homeBT = makeLabel(card2, "SCANNING", 14, COL_TEXT);
    lv_obj_align(homeBT, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    homeHint = makeLabel(homeScr, "M = devices   BOOT = rescan SD", 10, COL_MUTED);
    lv_obj_align(homeHint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void updateHomeContent()
{
    if (!homeTracks || !uiPlaylist)
        return;

    char buf[96];
    AudioEngine &eng = AudioEngine::instance();

    snprintf(buf, sizeof(buf), "%d track%s ready",
             (int)uiPlaylist->count(), uiPlaylist->count() == 1 ? "" : "s");
    if (strcmp(lv_label_get_text(homeTracks), buf) != 0)
        lv_label_set_text(homeTracks, buf);

    const char *bt;
    switch (eng.sinkState())
    {
    case AudioEngine::SINK_CONNECTED:
        bt = eng.sinkName();
        break;
    case AudioEngine::SINK_CONNECTING:
        bt = "connecting...";
        break;
    default:
        bt = "scanning for devices";
        break;
    }
    snprintf(buf, sizeof(buf), "%s", bt);
    if (strcmp(lv_label_get_text(homeBT), buf) != 0)
        lv_label_set_text(homeBT, buf);
}

// ---------------------------------------------------------------------------
// Now playing screen (reference design: sunburst emblem, 2-line title,
// big circular transport buttons)
// ---------------------------------------------------------------------------
static lv_obj_t *nowTitle = nullptr;
static lv_obj_t *nowTrack = nullptr;
static lv_obj_t *nowPlayBtn = nullptr;
static lv_obj_t *nowPlaySym = nullptr;
static lv_obj_t *nowTime = nullptr;
static lv_obj_t *nowTotal = nullptr;
static lv_obj_t *nowBar = nullptr;
static bool chipWasPlaying = false;
static bool uiWasConnected = false;

// Radiating "light" rays around the music-note disc; the point arrays must
// outlive the line objects, hence static.
static void drawSunburst(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy)
{
    static lv_point_t pts[24][2];
    for (int i = 0; i < 24; i++)
    {
        float a = i * 15.0f * 3.14159265f / 180.0f;
        float r1 = 30.0f;
        float r2 = (i % 2 == 0) ? 56.0f : 46.0f;
        pts[i][0].x = cx + (lv_coord_t)(r1 * cosf(a));
        pts[i][0].y = cy + (lv_coord_t)(r1 * sinf(a));
        pts[i][1].x = cx + (lv_coord_t)(r2 * cosf(a));
        pts[i][1].y = cy + (lv_coord_t)(r2 * sinf(a));

        lv_obj_t *ln = lv_line_create(parent);
        lv_line_set_points(ln, pts[i], 2);
        lv_obj_set_style_line_color(ln, lv_color_hex(i % 2 == 0 ? COL_ACCENT : COL_RAY_DIM), 0);
        lv_obj_set_style_line_width(ln, 2, 0);
    }
}

// A big circular transport "button" visual; returns the circle, its symbol
// label is the only child.
static lv_obj_t *makeTransportCircle(lv_obj_t *parent, lv_coord_t size,
                                     lv_coord_t cx, lv_coord_t cy,
                                     const char *sym)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_pos(c, cx - size / 2, cy - size / 2);
    stylePanel(c, COL_PANEL2);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lab = makeLabel(c, sym, 20, COL_TEXT);
    lv_obj_center(lab);
    return c;
}

static void buildNowPlayingScreen()
{
    nowScr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(nowScr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(nowScr, LV_OPA_COVER, 0);

    nowState = makeStatusBar(nowScr, "NOW PLAYING");

    // sunburst emblem with a music-note core (stands in for album art)
    drawSunburst(nowScr, 120, 86);
    lv_obj_t *disc = lv_obj_create(nowScr);
    lv_obj_set_size(disc, 46, 46);
    lv_obj_set_pos(disc, 120 - 23, 86 - 23);
    stylePanel(disc, COL_PANEL);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(disc, 1, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_pad_all(disc, 0, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *note = makeLabel(disc, LV_SYMBOL_AUDIO, 20, COL_ACCENT);
    lv_obj_center(note);

    // track name over two lines so long titles stay readable
    nowTitle = makeLabel(nowScr, "-", 16, COL_TEXT);
    lv_label_set_long_mode(nowTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nowTitle, 224);
    lv_obj_set_height(nowTitle, 42);
    lv_obj_set_style_text_align(nowTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nowTitle, LV_ALIGN_TOP_MID, 0, 118);

    nowTrack = makeLabel(nowScr, "TRACK 00/00", 10, COL_MUTED);
    lv_obj_align(nowTrack, LV_ALIGN_TOP_MID, 0, 164);

    nowBar = lv_bar_create(nowScr);
    lv_obj_set_size(nowBar, 204, 8);
    lv_obj_align(nowBar, LV_ALIGN_TOP_MID, 0, 182);
    lv_bar_set_range(nowBar, 0, 1000);
    lv_bar_set_value(nowBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(nowBar, lv_color_hex(COL_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nowBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(nowBar, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(nowBar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(nowBar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(nowBar, 4, LV_PART_INDICATOR);

    nowTime = makeLabel(nowScr, "0:00", 14, COL_TEXT);
    lv_obj_align(nowTime, LV_ALIGN_TOP_MID, -84, 196);
    nowTotal = makeLabel(nowScr, "0:00", 14, COL_MUTED);
    lv_obj_align(nowTotal, LV_ALIGN_TOP_MID, 84, 196);

    // big circular transport controls: left / middle / right physical keys
    makeTransportCircle(nowScr, 46, 48, 258, LV_SYMBOL_PREV);
    nowPlayBtn = makeTransportCircle(nowScr, 60, 120, 258, LV_SYMBOL_PLAY);
    nowPlaySym = (lv_obj_t *)lv_obj_get_child(nowPlayBtn, 0);
    makeTransportCircle(nowScr, 46, 192, 258, LV_SYMBOL_NEXT);

    // letter of the physical button under each circle
    lv_obj_t *lLab = makeLabel(nowScr, "L", 10, COL_MUTED);
    lv_obj_align(lLab, LV_ALIGN_TOP_MID, -72, 290);
    lv_obj_t *mLab = makeLabel(nowScr, "M", 10, COL_MUTED);
    lv_obj_align(mLab, LV_ALIGN_TOP_MID, 0, 294);
    lv_obj_t *rLab = makeLabel(nowScr, "R", 10, COL_MUTED);
    lv_obj_align(rLab, LV_ALIGN_TOP_MID, 72, 290);

    lv_obj_t *hint = makeLabel(nowScr, "hold M = device list", 10, COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

// format ms as m:ss
static void fmtTime(char *buf, size_t n, uint32_t ms)
{
    uint32_t total = ms / 1000;
    snprintf(buf, n, "%u:%02u", (unsigned)(total / 60), (unsigned)(total % 60));
}

static void updateNowPlaying()
{
    if (!nowTitle)
        return;
    AudioEngine &eng = AudioEngine::instance();
    char buf[96];

    // if the link died while listening, go back to the device list on its own
    bool connected = (eng.sinkState() == AudioEngine::SINK_CONNECTED);
    if (uiWasConnected && !connected)
    {
        uiWasConnected = false;
        openPicker();
        return;
    }
    uiWasConnected = connected;

    const char *title = eng.trackTitle();
    if (!title || !title[0])
        title = "-";
    if (strcmp(lv_label_get_text(nowTitle), title) != 0)
        lv_label_set_text(nowTitle, title);

    static uint32_t lastVolGen = 0;
    static uint32_t volShowUntil = 0;
    if (eng.volumeGen() != lastVolGen)
    {
        lastVolGen = eng.volumeGen();
        volShowUntil = millis() + 1500; // flash the volume readout briefly
    }

    if (millis() < volShowUntil)
        snprintf(buf, sizeof(buf), "VOLUME %u%%", (unsigned)eng.volumePct());
    else
        snprintf(buf, sizeof(buf), "TRACK %02u/%02u",
                 (unsigned)(eng.trackIndex() + 1), (unsigned)eng.trackTotal());
    if (strcmp(lv_label_get_text(nowTrack), buf) != 0)
        lv_label_set_text(nowTrack, buf);

    bool playing = eng.isPlaying();
    if (playing != chipWasPlaying)
    {
        chipWasPlaying = playing;
        lv_label_set_text(nowPlaySym, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(nowPlayBtn,
                                  lv_color_hex(playing ? COL_ACCENT : COL_PANEL2), 0);
        lv_obj_set_style_text_color(nowPlaySym,
                                    lv_color_hex(playing ? COL_CHIP_TXT : COL_TEXT), 0);
    }

    char t1[12];
    char t2[12];
    fmtTime(t1, sizeof(t1), eng.elapsedMs());
    fmtTime(t2, sizeof(t2), eng.totalMs());
    if (strcmp(lv_label_get_text(nowTime), t1) != 0)
        lv_label_set_text(nowTime, t1);
    if (strcmp(lv_label_get_text(nowTotal), t2) != 0)
        lv_label_set_text(nowTotal, t2);

    int16_t pct;
    if (eng.totalMs() > 0)
    {
        pct = (int16_t)((int64_t)eng.elapsedMs() * 1000LL / (int64_t)eng.totalMs());
        if (pct < 0)
            pct = 0;
        if (pct > 1000)
            pct = 1000;
    }
    else
    {
        pct = 0;
    }
    if (lv_bar_get_value(nowBar) != pct)
        lv_bar_set_value(nowBar, pct, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// Bluetooth picker screen
// ---------------------------------------------------------------------------
#define PICKER_ROW_H   38
#define PICKER_VISIBLE 5

static lv_obj_t *pickerList = nullptr;
static lv_obj_t *pickerRows[PICKER_VISIBLE];
static lv_obj_t *pickerCount = nullptr;
static int pickerSelIdx = 0;  // absolute list index under the cursor
static uint32_t pickerDevGen = 0;
static bool pickerAutoAdvance = false; // boot/auto-open: jump to player once connected

// forward declared here; definitions sit below with the screen builders
static void pickerRebuild();
static void pickerMoveCursor(int dy);
static void pickerConfirm();
static void pickerCancel();
void openPicker();

static void pickerClearRows()
{
    if (pickerList)
    {
        lv_obj_clean(pickerList);
        for (int i = 0; i < PICKER_VISIBLE; i++)
            pickerRows[i] = nullptr;
    }
}

static void pickerRebuild()
{
    if (!pickerList)
        return;
    AudioEngine &eng = AudioEngine::instance();
    size_t n = eng.deviceCount();

    char cntBuf[24];
    snprintf(cntBuf, sizeof(cntBuf), "found %u", (unsigned)n);
    if (strcmp(lv_label_get_text(pickerCount), cntBuf) != 0)
        lv_label_set_text(pickerCount, cntBuf);

    pickerClearRows();
    pickerDevGen = eng.deviceListGeneration();

    if (n == 0)
    {
        lv_obj_t *empty = makeLabel(pickerList,
                                    "Searching...\n"
                                    "Put your device in PAIRING\n"
                                    "mode (LED flashing) and keep\n"
                                    "it clear of your phone/PC.\n"
                                    "First scan can take ~30s.",
                                    14, COL_MUTED);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    if (pickerSelIdx >= (int)n)
        pickerSelIdx = (int)n - 1;
    if (pickerSelIdx < 0)
        pickerSelIdx = 0;
    int top = pickerSelIdx - pickerSelIdx % PICKER_VISIBLE;
    int visible = (int)n - top;
    if (visible > PICKER_VISIBLE)
        visible = PICKER_VISIBLE;

    for (int i = 0; i < visible; i++)
    {
        BtDeviceRec rec;
        if (!eng.deviceAt((size_t)(top + i), rec))
            break;
        bool sel = (top + i == pickerSelIdx);
        bool cur = eng.isConnectedDevice(rec);

        lv_obj_t *row = lv_btn_create(pickerList);
        lv_obj_set_size(row, 226, PICKER_ROW_H - 6);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * PICKER_ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? COL_ROW_SEL : COL_ROW), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, sel ? 1 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_radius(row, 8, 0);

        lv_obj_t *sym = makeLabel(row, LV_SYMBOL_AUDIO, 14,
                                  sel ? COL_ACCENT : COL_MUTED);
        lv_obj_align(sym, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *lab = makeLabel(row, rec.name[0] ? rec.name : "(unknown)", 14, COL_TEXT);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lab, 150);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 36, 0);

        // green check only on the connected device (OK ejects it); the
        // cursor is conveyed by the highlighted row alone
        lv_obj_t *mark = makeLabel(row, cur ? LV_SYMBOL_OK : "", 14, COL_ACCENT2);
        lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -10, 0);

        pickerRows[i] = row;
    }
}

static void pickerMoveCursor(int dy)
{
    AudioEngine &eng = AudioEngine::instance();
    int n = (int)eng.deviceCount();
    if (n == 0)
        return;
    pickerSelIdx += dy;
    if (pickerSelIdx < 0)
        pickerSelIdx = 0;
    if (pickerSelIdx >= n)
        pickerSelIdx = n - 1;
    pickerRebuild();
}

void openPicker()
{
    pickerSelIdx = 0;
    // advance to the player automatically only when the picker opened by
    // itself (boot / connection lost); a manual open stays until the user
    // picks or cancels
    pickerAutoAdvance =
        (AudioEngine::instance().sinkState() != AudioEngine::SINK_CONNECTED);
    AudioEngine::instance().cancelPendingSelection();
    pickerRebuild();
    switchScreen(SCR_PICKER);
}

static void pickerConfirm()
{
    AudioEngine &eng = AudioEngine::instance();
    if (eng.deviceCount() == 0)
        return;
    BtDeviceRec rec;
    if (!eng.deviceAt((size_t)pickerSelIdx, rec))
        return;
    Serial.printf("[UI] OK pressed on list item %d of %u\n",
                  (int)pickerSelIdx, (unsigned)eng.deviceCount());

    if (eng.sinkState() == AudioEngine::SINK_CONNECTED && eng.isConnectedDevice(rec))
    {
        eng.disconnectSink(); // eject: stay in the picker, it rescans
        pickerRebuild();      // refresh the connected marker
        return;
    }

    eng.selectDevice((size_t)pickerSelIdx);
    switchScreen(SCR_NOW_PLAYING);
}

static void pickerCancel()
{
    AudioEngine::instance().cancelPendingSelection();
    switchScreen(AudioEngine::instance().sinkState() == AudioEngine::SINK_CONNECTED
                     ? SCR_NOW_PLAYING
                     : SCR_HOME);
}

static void buildPickerScreen()
{
    pickerScr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pickerScr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(pickerScr, LV_OPA_COVER, 0);

    pickerState = makeStatusBar(pickerScr, "BT DEVICES");

    pickerCount = makeLabel(pickerScr, "found 0", 10, COL_MUTED);
    lv_obj_align(pickerCount, LV_ALIGN_TOP_MID, 0, 32);

    pickerList = lv_obj_create(pickerScr);
    lv_obj_set_size(pickerList, 240, PICKER_ROW_H * PICKER_VISIBLE);
    lv_obj_align(pickerList, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_clear_flag(pickerList, LV_OBJ_FLAG_SCROLLABLE);
    stylePanel(pickerList, COL_BG);
    lv_obj_set_style_pad_all(pickerList, 0, 0);

    lv_obj_t *hint = makeLabel(pickerScr,
                               "L=up  R=down  M=connect\n"
                               "hold M=back  OK again=eject",
                               14, COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void pickerTick()
{
    AudioEngine &eng = AudioEngine::instance();
    // auto-opened picker: a connection came up (fresh connect or silent
    // auto-reconnect) -> continue to the player screen without a button
    if (pickerAutoAdvance && eng.sinkState() == AudioEngine::SINK_CONNECTED)
    {
        switchScreen(SCR_NOW_PLAYING);
        return;
    }
    if (pickerDevGen != eng.deviceListGeneration())
        pickerRebuild();
}

// ---------------------------------------------------------------------------
// Backlight auto-sleep (LCD_AUTO_SLEEP_MS): any button press wakes it up
// ---------------------------------------------------------------------------
static uint32_t lastUiActivity = 0;
static bool backlightOff = false;

static void noteActivity()
{
    lastUiActivity = millis();
    if (backlightOff)
    {
        digitalWrite(TFT_BL, HIGH);
        backlightOff = false;
    }
}

static void backlightTick()
{
    if (btnPrev.isPressed() || btnNext.isPressed() ||
        btnOk.isPressed() || btnBoot.isPressed())
    {
        noteActivity();
        return;
    }
    if (!backlightOff && lastUiActivity != 0 &&
        millis() - lastUiActivity > LCD_AUTO_SLEEP_MS)
    {
        digitalWrite(TFT_BL, LOW);
        backlightOff = true;
    }
}

// ---------------------------------------------------------------------------
// LVGL timer
// ---------------------------------------------------------------------------
static const uint32_t UI_PERIOD_MS = 30;

static void lv_timer_cb(lv_timer_t *)
{
    buttonsTick(); // Button2 polling (debounced handlers fire here)
    backlightTick();

    // ---- hold left/right = volume down/up (repeats while held) ----
    uint32_t now = millis();
    bool prevDown = btnPrev.isPressed();
    bool nextDown = btnNext.isPressed();
    if (currentScreen != SCR_PICKER)
    {
        if (prevDown && !prevWasDown)
            prevDownMs = now;
        if (nextDown && !nextWasDown)
            nextDownMs = now;
        if (prevDown && now - prevDownMs >= VOL_DELAY_MS &&
            now - lastVolStepMs >= VOL_STEP_MS)
        {
            lastVolStepMs = now;
            AudioEngine::instance().volumeDown();
        }
        if (nextDown && now - nextDownMs >= VOL_DELAY_MS &&
            now - lastVolStepMs >= VOL_STEP_MS)
        {
            lastVolStepMs = now;
            AudioEngine::instance().volumeUp();
        }
    }
    prevWasDown = prevDown;
    nextWasDown = nextDown;

    switch (currentScreen)
    {
    case SCR_HOME:
        updateStatusLabel(homeState);
        updateHomeContent();
        break;
    case SCR_NOW_PLAYING:
        updateStatusLabel(nowState);
        updateNowPlaying();
        break;
    case SCR_PICKER:
        updateStatusLabel(pickerState);
        pickerTick();
        break;
    }
}

void uiInit(Playlist *pl)
{
    uiPlaylist = pl;

    // Bring up the ILI9341 first: without this the panel stays blank white
    // (backlight on, controller never initialised).
    tft.init();
    tft.setRotation(0); // native portrait 240x320
    tft.fillScreen(COL_SCREEN_INIT);

    lv_init(); // tick comes from millis() via lv_conf.h LV_TICK_CUSTOM
    lv_disp_draw_buf_init(&drawBuf, drawBufMem, NULL, DRAW_BUF_PX);
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = 240;
    dispDrv.ver_res = 320;
    dispDrv.flush_cb = disp_flush_cb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    buildHomeScreen();
    buildNowPlayingScreen();
    buildPickerScreen();
    lv_scr_load(homeScr);

    setupButtons();
    lastUiActivity = millis(); // start the idle window at power-on

    lv_timer_create(lv_timer_cb, UI_PERIOD_MS, NULL);
}

void uiTick()
{
    lv_timer_handler(); // pumps LVGL, runs lv_timer_cb -> buttonsTick etc.
    delay(2);
}
