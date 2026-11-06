#include "playlist.h"
#include <SD.h>
#include <esp_heap_caps.h>

size_t Playlist::load()
{
    if (!ensureStorage())
        return 0;
    clear();
    scanDir("/", 0);
    return count_;
}

// Track storage is allocated on first use, preferring PSRAM: the classic
// Bluetooth stack claims most of the internal heap while it starts up.
bool Playlist::ensureStorage()
{
    if (paths_ && titles_)
        return true;

    paths_ = static_cast<char (*)[PLAYLIST_PATH_LEN]>(
        heap_caps_calloc(PLAYLIST_MAX, PLAYLIST_PATH_LEN, MALLOC_CAP_SPIRAM));
    titles_ = static_cast<char (*)[PLAYLIST_TITLE_LEN]>(
        heap_caps_calloc(PLAYLIST_MAX, PLAYLIST_TITLE_LEN, MALLOC_CAP_SPIRAM));
    if (!paths_ || !titles_)
    {
        // no usable PSRAM — fall back to internal heap
        free(paths_);
        free(titles_);
        paths_ = static_cast<char (*)[PLAYLIST_PATH_LEN]>(calloc(PLAYLIST_MAX, PLAYLIST_PATH_LEN));
        titles_ = static_cast<char (*)[PLAYLIST_TITLE_LEN]>(calloc(PLAYLIST_MAX, PLAYLIST_TITLE_LEN));
    }
    if (!paths_ || !titles_)
    {
        free(paths_);
        free(titles_);
        paths_ = nullptr;
        titles_ = nullptr;
        Serial.println("Playlist: out of memory for track storage");
        return false;
    }
    return true;
}

void Playlist::clear()
{
    count_ = 0;
    if (paths_)
        memset(paths_, 0, sizeof(paths_[0]) * PLAYLIST_MAX);
    if (titles_)
        memset(titles_, 0, sizeof(titles_[0]) * PLAYLIST_MAX);
}

void Playlist::scanDir(const String &dirPath, int depth)
{
    if (depth > 4 || count_ >= PLAYLIST_MAX)
        return;

    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory())
        return;

    while (count_ < PLAYLIST_MAX)
    {
        File entry = dir.openNextFile();
        if (!entry)
            break;
        const char *name = entry.name();
        String full = dirPath.endsWith("/") ? dirPath + name : dirPath + "/" + name;
        if (entry.isDirectory())
        {
            scanDir(full, depth + 1);
        }
        else
        {
            int len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".mp3") == 0)
            {
                add(full);
            }
        }
        entry.close();
    }
    dir.close();
}

void Playlist::add(const String &fullPath)
{
    if (count_ >= PLAYLIST_MAX)
        return;
    if (fullPath.length() >= sizeof(paths_[0]))
    {
        // A stored-but-truncated path would fail to open later, so drop it
        // and tell the user why the track is missing.
        Serial.printf("Playlist: path too long (%u), skipped: %s\n",
                      (unsigned)fullPath.length(), fullPath.c_str());
        return;
    }
    size_t i = count_++;
    strlcpy(paths_[i], fullPath.c_str(), sizeof(paths_[0]));

    // Title = plain file name (lowest path segment) without ".mp3"
    const char *p = paths_[i];
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    size_t n = strlen(base);
    if (n > 4 && strcasecmp(base + n - 4, ".mp3") == 0)
        n -= 4;
    if (n == 0)
        n = 1; // degenerate path; still show something
    if (n >= sizeof(titles_[0]))
        n = sizeof(titles_[0]) - 1;
    memcpy(titles_[i], base, n);
    titles_[i][n] = '\0';
}
