#pragma once
#include <Arduino.h>

// Result of inspecting an MP3 file without decoding it
struct Mp3Info
{
    bool ok = false;
    uint32_t sampleRate = 44100;   // Hz of the first frame
    uint32_t bitrateKbps = 128;    // kbps of the first frame
    uint32_t durationMs = 0;       // best-effort total duration
};

// Parses the first MP3 frame header (skipping a leading ID3v2 tag), reads
// bitrate/sample-rate, and estimates duration from the Xing/Info header when
// the file is VBR, otherwise from file size and bitrate. Sound data untouched.
bool analyzeMp3(const char *path, Mp3Info &out);
