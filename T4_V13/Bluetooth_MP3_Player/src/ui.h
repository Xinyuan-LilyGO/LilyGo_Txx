#pragma once
#include <Arduino.h>
#include "Button2.h"

class Playlist;

// Button2 handlers (loop task). Registered in uiInit().
void onPrev(Button2 &b);
void onNext(Button2 &b);
void onOkClick(Button2 &b);
void onOkLongPress(Button2 &b);

// Boot button: force a full (deck + card) rescan.
void onBootButton(Button2 &b);

void uiInit(Playlist *pl);
void uiTick(); // call every loop iteration; dispatches Button2 + LVGL timer

// Opens the Bluetooth device picker screen (also used to boot straight
// into it: scanning runs automatically, no button press needed).
void openPicker();
