#pragma once
/*
 * AudioEngine — SD-card MP3 playback streamed over Bluetooth A2DP (source).
 *
 * Responsibilities:
 *  - Runs a BluetoothA2DPSource (classic Bluetooth). It connects to the last
 *    used sink on boot (NVS) or keeps scanning nearby audio sinks.
 *  - Maintains a list of discovered A2DP sinks for the on-screen picker
 *    (synced from the library's ssid callback, BT-task side).
 *  - Pulls PCM from an MP3 file on the SD card inside the A2DP data callback:
 *    MP3 frames are decoded with libhelix (via arduino-audio-tools'
 *    EncodedAudioStream). Files that are not 44.1 kHz are linearly
 *    resampled to 44.1 kHz stereo 16-bit, which is what A2DP/SBC requires.
 *
 * Threading model:
 *  - The A2DP stack calls AudioEngine::fillAudio from its Bluetooth task
 *    roughly every 23 ms and must always return the full chunk (silence
 *    padding is used when paused or between tracks).
 *  - The Arduino loop task owns track loading/unloading and queue handling:
 *    both sides are serialized with audioMutex_; the BT task only ever
 *    tries to take it briefly and falls back to silence.
 */
#pragma once
#include <Arduino.h>
#include "BluetoothA2DPSource.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "SPI.h"
#include "SD.h"
#include "app_config.h"
#include "mp3_info.h"

class Playlist;

struct BtDeviceRec
{
    char name[33];
    uint8_t address[6];
    int8_t rssi;
};

class AudioEngine
{
public:
    static AudioEngine &instance();

    enum SinkState : uint8_t
    {
        SINK_NONE = 0,      // scanning / nothing paired
        SINK_CONNECTING,    // connect attempt under way
        SINK_CONNECTED      // streaming link established
    };

    // SD must already be mounted; pl (may be empty) provides the tracks.
    void init(Playlist *pl);
    // Called from the Arduino loop task.
    void work();

    // ---- SD bus arbitration ----
    // Call around direct SD access from the loop task (e.g. playlist rescan)
    // so it cannot interleave with the BT task's SD reads. Pair strictly.
    void suspendAudio();
    void resumeAudio();

    // ---- transport ----
    void togglePlay();
    void next();
    void prev();
    // Volume: sent to the connected speaker as AVRCP absolute volume
    // (steps of 10 %). Holds on the left/right buttons call these.
    void volumeUp();
    void volumeDown();
    uint8_t volumePct() const { return volumePct_; }
    uint32_t volumeGen() const { return volumeGen_; }
    // Drop the Bluetooth link on user request (OK on the connected device
    // in the picker). The picker stays open; scanning restarts automatically.
    void disconnectSink();
    // true when rec is the currently connected device
    bool isConnectedDevice(const BtDeviceRec &rec) const;

    // ---- playback state (read from loop task) ----
    bool isPlaying() const { return playing_; }
    size_t trackIndex() const { return trackIdx_; }
    const char *trackTitle() const { return trackTitle_; }
    uint32_t elapsedMs() const { return elapsedMs_; }
    uint32_t totalMs() const { return totalMs_; }
    size_t trackTotal() const { return playlistCount(); }

    // ---- bluetooth state ----
    SinkState sinkState() const { return sinkState_; }
    const char *sinkName() const { return sinkName_; }

    // ---- device picker ----
    size_t deviceCount();
    bool deviceAt(size_t i, BtDeviceRec &out);
    void selectDevice(size_t i);
    // abort a pending selection ("don't connect anywhere")
    void cancelPendingSelection();
    // monotonically increasing whenever the discovered device list changed
    uint32_t deviceListGeneration() const { return deviceGen_; }

private:
    AudioEngine() = default;

    // BT task side
    static int32_t audioDataTrampoline(uint8_t *data, int32_t len);
    static bool sinkFoundTrampoline(const char *name, esp_bd_addr_t address, int rssi);
    static void connStateTrampoline(esp_a2d_connection_state_t state, void *ctx);
    bool onSinkFound(const char *name, esp_bd_addr_t address, int rssi);
    int32_t fillAudio(uint8_t *data, int32_t len);

    // Decode task: pulls decoded/resampled PCM into the ring buffer so the
    // Bluetooth callback never touches the SD card or the MP3 decoder (that
    // starved the BT stack's watchdog when playback resumed).
    static void decodeLoopEntry(void *arg);
    void decodeLoop();
    int pumpStraightRing();     // audioMutex_ held; -1 = end of track
    int pumpResampledRing();    // audioMutex_ held; -1 = end of track
    bool refillConvChunk(float &pos);
    void updateElapsed();
    void postEos();

    // loop task side
    void switchTo(size_t index);
    void switchToRelative(int delta);
    void handleEos();
    bool openPipelineLocked(size_t index);
    void closePipelineLocked();
    size_t playlistCount() const;

    // ---- objects ----
    Playlist *playlist_ = nullptr;
    BluetoothA2DPSource a2dpSource_;

    SemaphoreHandle_t audioMutex_ = nullptr;   // guards decoder_ / file_ / conv state
    QueueHandle_t eventQueue_ = nullptr;       // CMD_EOS posted from the BT task

    File file_;
    // Both are per-track allocations, created and destroyed together: the
    // EncodedAudioStream registers itself in the codec's audio-info notify
    // list and nothing deregisters it — reusing one codec object across
    // tracks leaves dangling pointers that crash on the first decoded frame
    // of the next track (observed as LoadProhibited in notifyAudioChange).
    MP3DecoderHelix *helix_ = nullptr;
    EncodedAudioStream *decoder_ = nullptr;

    // ---- playback state ----
    volatile bool playing_ = false;
    size_t trackIdx_ = 0;
    char trackTitle_[64] = "";
    uint32_t totalMs_ = 0;
    uint64_t elapsedSamples48k_ = 0;   // output samples at 44100 Hz
    uint32_t elapsedMs_ = 0;

    // ---- resampler state (decode task, guarded by audioMutex_) ----
    uint32_t sourceRate_ = 44100;
    bool resample_ = false;
    int16_t convChunk_[1152 * 2];   // one decoded MP3 frame of stereo samples
    uint8_t convHold_[4];           // partial frame bytes carried between reads
    size_t convHoldLen_ = 0;
    uint8_t readStaging_[384 * 4];  // raw PCM staging (straight path)
    size_t convFrames_ = 0;         // valid frames currently in convChunk_
    size_t convIdx_ = 0;            // integer part of the read position
    float convFrac_ = 0.f;          // fractional part (0..1) between two frames
    bool convEof_ = false;

    // ---- PCM ring buffer (internal RAM: both cores access internal SRAM
    // uncached, so a PSRAM ring would risk stale cross-core reads) ----
    // Producer: decode task (pcmTail_). Consumer: BT data callback
    // (pcmHead_). Free-running counters, never reset: single-producer/
    // single-consumer over 32-bit aligned indices is race-free, and not
    // resetting means a track switch simply plays the previous track's
    // residual frames (<=0.2 s) before the new one starts.
    static const uint32_t PCM_RING_FRAMES = 4096; // 2^12 frames = 16 KB ≈ 93 ms
#define PCM_RING_MASK (PCM_RING_FRAMES - 1)
    int16_t *pcmRing_ = nullptr;
    volatile uint32_t pcmHead_ = 0; // frames consumed
    volatile uint32_t pcmTail_ = 0; // frames produced
    size_t pcmAvail() const { return pcmTail_ - pcmHead_; }
    size_t pcmSpace() const { return PCM_RING_FRAMES - pcmAvail(); }
    TaskHandle_t decodeTask_ = nullptr;
    volatile bool decodeEof_ = false; // decode task parked at end of track
    bool producedLogged_ = false;     // one-shot "decode works" log per track
    bool starveLogged_ = false;       // one-shot "ring empty while playing" log
    uint32_t lastFullLogMs_ = 0;      // rate-limits the "ring full" log

    // ---- bluetooth device list (fixed array: written from the BT task
    //      inside a critical section, no heap allocation there) ----
    BtDeviceRec devices_[BT_MAX_DEVICES];
    size_t deviceCount_ = 0;
    portMUX_TYPE deviceMux_ = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t deviceGen_ = 0;

    // ---- sink state ----
    volatile SinkState sinkState_ = SINK_NONE;
    char sinkName_[33] = "";
    uint8_t connectedAddr_[6] = {0}; // address of the current selection
    volatile bool hasConnectedAddr_ = false;
    volatile bool rescanQueued_ = false; // link dropped -> work() rescans

    // ---- volume ----
    int8_t volumePct_ = 70;      // percent, sent as AVRCP absolute volume
    volatile uint32_t volumeGen_ = 0;  // bumped on change (UI feedback)
    volatile bool volumeApplyPending_ = false; // push volume once connected
    void setVolumePct(int8_t pct);

    // ---- diagnostics ----
    bool dataCbLogged_ = false;   // first A2DP data callback logged
    bool eosLogged_ = false;      // end-of-track logged once per pipeline
    uint32_t lastStatusMs_ = 0;   // periodic status line

    // selection handled inside the ssid callback (BT task) via this flag
    volatile bool selectionPending_ = false;   // user picked a device in the UI
    BtDeviceRec selection_ = {};
};

enum EngineEvent : uint8_t
{
    EV_EOS = 1,
    EV_NONE = 0
};
