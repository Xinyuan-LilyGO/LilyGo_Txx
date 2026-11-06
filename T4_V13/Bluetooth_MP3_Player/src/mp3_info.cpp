#include "mp3_info.h"
#include "SD.h"
#include "app_config.h"

// ---- MPEG audio constants ----
static const uint16_t V1_L3_BITRATES[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const uint16_t V2_L3_BITRATES[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const uint16_t V1_RATES[4] = {44100, 48000, 32000, 0};
static const uint16_t V2_RATES[4] = {22050, 24000, 16000, 0};
static const uint16_t V25_RATES[4] = {11025, 12000, 8000, 0};

bool analyzeMp3(const char *path, Mp3Info &out)
{
    File f = SD.open(path, FILE_READ);
    if (!f)
        return false;

    uint32_t fileSize = f.size();
    if (fileSize < 128)
    {
        f.close();
        return false;
    }

    size_t pos = 0;
    uint8_t h[4];
    // -------- skip an ID3v2 tag if present --------
    if (f.read(h, 3) == 3 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
    {
        // After "ID3": version, revision, flags, then a 4-byte tag size.
        uint8_t b[7];
        if (f.read(b, 7) == 7)
        {
            uint8_t ver = b[0];
            uint32_t szSync = ((uint32_t)(b[3] & 0x7F) << 21) | ((uint32_t)(b[4] & 0x7F) << 14) |
                              ((uint32_t)(b[5] & 0x7F) << 7) | (uint32_t)(b[6] & 0x7F);
            uint32_t szPlain = ((uint32_t)b[3] << 24) | ((uint32_t)b[4] << 16) |
                               ((uint32_t)b[5] << 8) | (uint32_t)b[6];
            // v2.4 stores the size syncsafe, v2.3 as plain 32-bit; some v2.3
            // writers use syncsafe anyway — pick whichever lands inside the
            // file, preferring the version-correct one.
            uint32_t sz = (ver >= 4) ? szSync : szPlain;
            if (10 + sz > fileSize)
                sz = (10 + szSync <= fileSize) ? szSync : 0;
            pos = 10 + sz;
        }
    }

    // -------- find the first MP3 frame sync (0xFF Ex/Ex) --------
    bool found = false;
    uint32_t frameStart = 0;
    uint32_t frameHeader = 0;
    {
        uint8_t buf[512];
        size_t window = 0;
        while (window < MP3_SCAN_WINDOW && !found)
        {
            f.seek(pos + window);
            size_t got = f.read(buf, sizeof(buf));
            if (got < 4)
                break;
            for (size_t i = 0; i + 3 < got; i++)
            {
                if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0)
                {
                    uint8_t versionBits = (buf[i + 1] >> 3) & 3; // 3=MPEG1 2=MPEG2 0=MPEG2.5
                    uint8_t layerBits = (buf[i + 1] >> 1) & 3;   // 1=Layer III
                    uint8_t bitIdx = (buf[i + 2] >> 4) & 0xF;
                    uint8_t sIdx = (buf[i + 2] >> 2) & 3;

                    if (layerBits != 1 || versionBits == 1 || bitIdx == 0 || bitIdx == 15 || sIdx == 3)
                        continue;

                    uint32_t sr = (versionBits == 3) ? V1_RATES[sIdx]
                                  : (versionBits == 2) ? V2_RATES[sIdx]
                                                       : V25_RATES[sIdx];
                    uint32_t br = (versionBits == 3) ? V1_L3_BITRATES[bitIdx] : V2_L3_BITRATES[bitIdx];
                    if (sr == 0 || br == 0)
                        continue;

                    frameStart = pos + window + i;
                    frameHeader = ((uint32_t)buf[i] << 24) | ((uint32_t)buf[i + 1] << 16) |
                                  ((uint32_t)buf[i + 2] << 8) | (uint32_t)buf[i + 3];
                    out.ok = true;
                    out.sampleRate = sr;
                    out.bitrateKbps = br;
                    found = true;
                    break;
                }
            }
            if (!found)
                window += (got > 3) ? (got - 3) : 1;
        }
    }

    if (!found)
    {
        f.close();
        return false;
    }

    // -------- duration --------
    {
        uint8_t b1 = (frameHeader >> 16) & 0xFF;
        uint8_t b2 = (frameHeader >> 8) & 0xFF;
        uint8_t b3 = frameHeader & 0xFF;
        uint8_t versionBits = (b1 >> 3) & 3;
        uint8_t channelMode = (b3 >> 6) & 3; // 3 = mono
        uint8_t pad = (b2 >> 1) & 1;

        uint32_t samplesPerFrame = (versionBits == 3) ? 1152 : 576;
        uint32_t frameLen = (versionBits == 3)
                                ? (144UL * out.bitrateKbps * 1000UL) / out.sampleRate + pad
                                : (72UL * out.bitrateKbps * 1000UL) / out.sampleRate + pad;

        // The Xing/Info header (VBR) lives inside the first frame
        size_t sideInfo = (versionBits == 3) ? ((channelMode == 3) ? 17 : 32)
                                             : ((channelMode == 3) ? 9 : 17);
        uint8_t x[8];
        if (frameLen > sideInfo + 4 + 8 &&
            f.seek(frameStart + 4 + sideInfo) && f.read(x, 8) == 8)
        {
            bool isXing = (x[0] == 'X' && x[1] == 'i' && x[2] == 'n' && x[3] == 'g');
            bool isInfo = (x[0] == 'I' && x[1] == 'n' && x[2] == 'f' && x[3] == 'o');
            if ((isXing || isInfo) && (x[7] & 0x01))
            {
                uint8_t fr[4];
                if (f.read(fr, 4) == 4)
                {
                    uint32_t frames = ((uint32_t)fr[0] << 24) | ((uint32_t)fr[1] << 16) |
                                      ((uint32_t)fr[2] << 8) | (uint32_t)fr[3];
                    // 64-bit: frames*1152*1000 overflows 32 bits for tracks
                    // longer than ~75 minutes
                    out.durationMs = (uint32_t)((uint64_t)frames * samplesPerFrame * 1000ULL / out.sampleRate);
                }
            }
        }

        if (out.durationMs == 0)
        {
            // CBR estimate: bytes*8 bits / (kbps*1000 bits per second).
            // 64-bit: fileSize*8000 overflows 32 bits above ~500 KB.
            out.durationMs = (uint32_t)((uint64_t)fileSize * 8000ULL / (out.bitrateKbps * 1000ULL));
        }
    }

    f.close();
    return out.durationMs > 0;
}
