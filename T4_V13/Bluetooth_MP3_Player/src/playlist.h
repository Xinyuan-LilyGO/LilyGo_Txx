#pragma once
#include <Arduino.h>

// Fixed capacity playlist: enough full paths and display titles for a card
// that is organised into a few album folders. Scanning the whole card would
// otherwise cost too much RAM. Paths must be long enough for real-world song
// names ("Artist - Song Title (Official Video).mp3" easily exceeds 60 chars);
// a truncated path would no longer open on the card.
//
// The arrays are allocated on demand in load(): in PSRAM when the board has
// it, internal heap otherwise. They must NOT be plain static members — the
// Bluetooth stack needs all the internal RAM it can get during init, and
// 128 x (192+64) bytes of it would crash bluedroid with an OOM panic.
#ifndef PLAYLIST_MAX
#define PLAYLIST_MAX 128
#endif
#ifndef PLAYLIST_PATH_LEN
#define PLAYLIST_PATH_LEN 192
#endif
#ifndef PLAYLIST_TITLE_LEN
#define PLAYLIST_TITLE_LEN 64
#endif

class Playlist
{
public:
    // Recursively scans the (mounted) SD card for .mp3 files.
    // Returns the number of tracks found.
    size_t load();

    size_t count() const { return count_; }
    // Full path on the SD card ("nullable" when empty list).
    const char *pathAt(size_t i) const { return paths_ && i < count_ ? paths_[i] : nullptr; }
    // Display title (file name without path/extension, truncated).
    const char *titleAt(size_t i) const { return titles_ && i < count_ ? titles_[i] : ""; }

    void clear();

private:
    bool ensureStorage();
    void scanDir(const String &dirPath, int depth);
    void add(const String &fullPath);

    char (*paths_)[PLAYLIST_PATH_LEN] = nullptr;   // PSRAM when available
    char (*titles_)[PLAYLIST_TITLE_LEN] = nullptr; // PSRAM when available
    size_t count_ = 0;
};
