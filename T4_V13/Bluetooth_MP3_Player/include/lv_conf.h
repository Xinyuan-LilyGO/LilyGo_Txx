#pragma once
#ifndef LV_CONF_H
#define LV_CONF_H

/* Minimal LVGL 8.3 configuration — everything not defined here falls back
 * to the library default (lv_conf_internal.h), which keeps the build simple.
 * This file only trims what we must control. */

/* ---- Colour / display ---- */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0           /* we swap bytes in the flush callback */
#define LV_DISP_DEF_REFR_PERIOD 33
#define LV_INDEV_DEF_READ_PERIOD 40

/* ---- Memory ---- */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (32U * 1024U)

/* ---- Tick (derive LVGL's tick from Arduino millis(); no timer hack) ---- */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* ---- Fonts (only the ones the UI uses; defaults excluded) ---- */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ---- Widgets the UI does not need (saves ~300 KB flash) ---- */
#define LV_USE_BTNMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CALENDAR_HEADER_ARROW 0
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#define LV_USE_CANVAS 0
#define LV_USE_MSGBOX 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_IMG 1
#define LV_USE_LED 0
#define LV_USE_DROPDOWN 1
#define LV_USE_METER 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LINE 1
#define LV_USE_ARC 1

/* ---- Theme ---- */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_MONO 0
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

/* ---- Extras ---- */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

#endif /* LV_CONF_H */
