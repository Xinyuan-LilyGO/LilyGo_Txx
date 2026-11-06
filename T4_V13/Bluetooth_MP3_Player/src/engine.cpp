#include "engine.h"
#include "playlist.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static inline int16_t clip16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

AudioEngine &AudioEngine::instance()
{
    static AudioEngine eng;
    return eng;
}

// ---------------------------------------------------------------------------
// init / lifecycle (loop task)
// ---------------------------------------------------------------------------
void AudioEngine::init(Playlist *pl)
{
    playlist_ = pl;
    audioMutex_ = xSemaphoreCreateMutex();
    eventQueue_ = xQueueCreate(8, sizeof(uint8_t));

    // PCM ring in INTERNAL RAM (both cores access internal SRAM uncached —
    // a PSRAM ring would risk stale cross-core cache reads)
    pcmRing_ = static_cast<int16_t *>(calloc(PCM_RING_FRAMES, 2 * sizeof(int16_t)));
    if (!pcmRing_)
    {
        Serial.println("[DEC] FATAL: PCM ring allocation failed");
    }
    if (xTaskCreatePinnedToCore(decodeLoopEntry, "mp3dec", 8192, this, 3,
                                &decodeTask_, 1) != pdPASS)
    {
        Serial.println("[DEC] FATAL: decode task creation failed");
        decodeTask_ = nullptr;
    }

    a2dpSource_.set_local_name(BT_DEVICE_NAME);
    a2dpSource_.set_ssp_enabled(true);
    // Accept every Class-of-Device service class (0x7FF = all 11 bits): the
    // picker is meant to list ALL discoverable devices; filtering by
    // "audio-ish" COD can silently hide devices that advertise oddly.
    a2dpSource_.set_valid_cod_service(0x7FF);
    a2dpSource_.set_ssid_callback(sinkFoundTrampoline);
    a2dpSource_.set_on_connection_state_changed(connStateTrampoline);
    a2dpSource_.set_data_callback(audioDataTrampoline);
    // Deliberately NO set_auto_reconnect(): the device always boots into the
    // picker and the user picks a device fresh every time. After a link
    // drop the engine restarts discovery itself (see work()).

    // preload the first track (paused) so the UI can already show a title
    if (playlistCount() > 0)
    {
        switchTo(0);
    }

    // start Bluetooth: reconnect to the last sink (NVS) or scan nearby sinks
    a2dpSource_.start();
}

void AudioEngine::work()
{
    if (!eventQueue_)
        return;
    // Coalesce EOS events: at end of file the BT callback posts one EOS per
    // invocation until the track is switched, and handling each of them
    // would skip forward one extra track per queued event.
    uint8_t ev;
    bool eos = false;
    while (xQueueReceive(eventQueue_, &ev, 0) == pdTRUE)
    {
        if (ev == EV_EOS)
            eos = true;
    }
    if (eos)
        handleEos();

    // A link dropped (user eject or out of range): the library does not
    // rescan on its own without auto-reconnect, so kick discovery here.
    if (rescanQueued_)
    {
        rescanQueued_ = false;
        if (!a2dpSource_.is_connected() && !a2dpSource_.is_discovery_active())
        {
            Serial.println("[BT] restarting device discovery");
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
    }

    // push our preferred volume once a connection is up
    if (volumeApplyPending_)
    {
        volumeApplyPending_ = false;
        a2dpSource_.set_volume((uint8_t)((uint32_t)volumePct_ * 127u / 100u));
    }

    // periodic one-line status: makes play/track problems visible on serial
    uint32_t now = millis();
    if (now - lastStatusMs_ >= 5000)
    {
        lastStatusMs_ = now;
        Serial.printf("[ST] playing=%d bt=%s elapsed=%ums ring=%u heap=%u\n",
                      (int)playing_,
                      a2dpSource_.is_connected() ? "up" : "down",
                      (unsigned)elapsedMs_, (unsigned)pcmAvail(),
                      (unsigned)ESP.getFreeHeap());
    }
}

size_t AudioEngine::playlistCount() const
{
    return playlist_ ? playlist_->count() : 0;
}

// ---------------------------------------------------------------------------
// SD bus arbitration (loop task)
// ---------------------------------------------------------------------------
void AudioEngine::suspendAudio()
{
    // Hold the engine mutex so the BT task's data callback (which reads the
    // SD card through the decoder) cannot touch the card while the loop task
    // rescans it. The callback falls back to silence on timeout.
    if (audioMutex_)
        xSemaphoreTake(audioMutex_, portMAX_DELAY);
}

void AudioEngine::resumeAudio()
{
    if (audioMutex_)
        xSemaphoreGive(audioMutex_);
}

// ---------------------------------------------------------------------------
// track management (loop task only)
// ---------------------------------------------------------------------------
void AudioEngine::closePipelineLocked()
{
    if (decoder_)
    {
        decoder_->end();
        delete decoder_;
        decoder_ = nullptr;
    }
    if (helix_)
    {
        delete helix_;
        helix_ = nullptr;
    }
    if (file_)
    {
        file_.close();
    }
    convFrames_ = 0;
    convEof_ = false;
}

bool AudioEngine::openPipelineLocked(size_t index)
{
    if (!playlist_ || index >= playlist_->count())
        return false;

    const char *path = playlist_->pathAt(index);
    Mp3Info info;
    if (!analyzeMp3(path, info))
    {
        Serial.printf("[MP3] header parse failed: %s\n", path);
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f || info.sampleRate == 0)
    {
        Serial.printf("[MP3] open failed: %s\n", path);
        return false;
    }

    file_ = f; // decoder keeps a reference to this File handle

    helix_ = new MP3DecoderHelix();
    decoder_ = new EncodedAudioStream(&file_, helix_);
    if (!decoder_)
    {
        delete helix_;
        helix_ = nullptr;
        return false;
    }
    decoder_->transformationReader().resizeResultQueue(PCM_QUEUE_SIZE);
    if (!decoder_->begin())
    {
        Serial.printf("[MP3] decoder begin failed: %s\n", path);
        delete decoder_;
        decoder_ = nullptr;
        delete helix_;
        helix_ = nullptr;
        file_.close();
        return false;
    }

    trackIdx_ = index;
    strlcpy(trackTitle_, playlist_->titleAt(index), sizeof(trackTitle_));
    totalMs_ = info.durationMs;
    elapsedSamples48k_ = 0;
    elapsedMs_ = 0;

    sourceRate_ = info.sampleRate;
    resample_ = (sourceRate_ != 44100);
    convFrames_ = 0;
    convIdx_ = 0;
    convFrac_ = 0.f;
    convEof_ = false;
    decodeEof_ = false;
    producedLogged_ = false;
    starveLogged_ = false;
    eosLogged_ = false;
    Serial.printf("[MP3] track %u: %s (%u Hz, %u ms%s)\n", (unsigned)index,
                  path, (unsigned)info.sampleRate, (unsigned)info.durationMs,
                  resample_ ? ", resampled" : "");
    return true;
}

void AudioEngine::switchTo(size_t index)
{
    size_t n = playlistCount();
    if (n == 0 || !audioMutex_)
        return;

    xSemaphoreTake(audioMutex_, portMAX_DELAY);
    closePipelineLocked();
    bool ok = false;
    for (size_t tries = 0; tries < n; tries++)
    {
        size_t idx = (index + tries) % n;
        if (openPipelineLocked(idx))
        {
            ok = true;
            break;
        }
    }
    if (!ok)
    {
        closePipelineLocked();
        playing_ = false;
        trackTitle_[0] = '\0';
        totalMs_ = 0;
        elapsedMs_ = 0;
    }
    xSemaphoreGive(audioMutex_);
}

void AudioEngine::switchToRelative(int delta)
{
    size_t n = playlistCount();
    if (n == 0)
        return;
    size_t cur = trackIdx_ % n;
    switchTo((cur + n + delta) % n);
}

void AudioEngine::handleEos()
{
    if (playlistCount() == 0)
    {
        playing_ = false;
        return;
    }
    switchToRelative(1); // keeps current play/pause state
}

void AudioEngine::postEos()
{
    uint8_t ev = EV_EOS;
    xQueueSend(eventQueue_, &ev, 0);
}

// ---------------------------------------------------------------------------
// transport controls (loop task)
// ---------------------------------------------------------------------------
void AudioEngine::togglePlay()
{
    if (playlistCount() == 0)
        return;
    playing_ = !playing_;
    Serial.printf("[MP3] %s (bt %s)\n", playing_ ? "PLAY" : "PAUSE",
                  a2dpSource_.is_connected() ? "connected" : "NOT connected");
}

void AudioEngine::next() { switchToRelative(1); }
void AudioEngine::prev() { switchToRelative(-1); }

// ---------------------------------------------------------------------------
// BT task side: discovered devices + selection
// ---------------------------------------------------------------------------
bool AudioEngine::sinkFoundTrampoline(const char *name, esp_bd_addr_t address, int rssi)
{
    return instance().onSinkFound(name, address, rssi);
}

bool AudioEngine::onSinkFound(const char *name, esp_bd_addr_t address, int rssi)
{
    size_t slot = SIZE_MAX;
    portENTER_CRITICAL(&deviceMux_);
    for (size_t i = 0; i < deviceCount_; i++)
    {
        if (memcmp(devices_[i].address, address, 6) == 0)
        {
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX)
    {
        if (deviceCount_ >= BT_MAX_DEVICES)
        {
            // replace the weakest entry
            size_t weakest = 0;
            for (size_t i = 1; i < deviceCount_; i++)
                if (devices_[i].rssi < devices_[weakest].rssi)
                    weakest = i;
            slot = weakest;
            deviceGen_++; // replaced entry → picker list must rebuild
        }
        else
        {
            slot = deviceCount_++;
            deviceGen_++; // new entry → picker list must rebuild
        }
        memset(&devices_[slot], 0, sizeof(BtDeviceRec));
        memcpy(devices_[slot].address, address, 6);
    }
    strlcpy(devices_[slot].name, name ? name : "", sizeof(devices_[slot].name));
    devices_[slot].rssi = (int8_t)rssi;

    bool match = selectionPending_ && memcmp(selection_.address, address, 6) == 0;
    portEXIT_CRITICAL(&deviceMux_);

    if (match)
    {
        selectionPending_ = false;
        strlcpy(sinkName_, name ? name : "", sizeof(sinkName_));
        sinkState_ = SINK_CONNECTING;
        memcpy(connectedAddr_, address, 6);
        hasConnectedAddr_ = true;
        Serial.printf("[BT] scan matched selection '%s' " ESP_BD_ADDR_STR "\n",
                      name ? name : "", ESP_BD_ADDR_HEX(address));
    }
    return match;
}

size_t AudioEngine::deviceCount()
{
    portENTER_CRITICAL(&deviceMux_);
    size_t n = deviceCount_;
    portEXIT_CRITICAL(&deviceMux_);
    return n;
}

bool AudioEngine::deviceAt(size_t i, BtDeviceRec &out)
{
    portENTER_CRITICAL(&deviceMux_);
    bool ok = i < deviceCount_;
    if (ok)
        out = devices_[i];
    portEXIT_CRITICAL(&deviceMux_);
    return ok;
}

bool AudioEngine::isConnectedDevice(const BtDeviceRec &rec) const
{
    return hasConnectedAddr_ && memcmp(rec.address, connectedAddr_, 6) == 0;
}

void AudioEngine::disconnectSink()
{
    Serial.println("[BT] disconnect requested by user");
    sinkState_ = SINK_NONE;
    sinkName_[0] = '\0';
    hasConnectedAddr_ = false;
    if (a2dpSource_.is_connected())
        a2dpSource_.disconnect(); // DISCONNECTED event queues the rescan
    else
        rescanQueued_ = true;
}

void AudioEngine::selectDevice(size_t i)
{
    BtDeviceRec rec;
    if (!deviceAt(i, rec))
        return;
    Serial.printf("[BT] user selected '%s' (" ESP_BD_ADDR_STR ") discovery %s\n",
                  rec.name, ESP_BD_ADDR_HEX(rec.address),
                  a2dpSource_.is_discovery_active() ? "active" : "idle");

    // pressing OK on the connected device ejects it instead of reconnecting
    if (sinkState_ == SINK_CONNECTED && isConnectedDevice(rec))
    {
        disconnectSink();
        return;
    }

    portENTER_CRITICAL(&deviceMux_);
    selection_ = rec;
    selectionPending_ = true;
    portEXIT_CRITICAL(&deviceMux_);

    strlcpy(sinkName_, rec.name, sizeof(sinkName_));
    sinkState_ = SINK_CONNECTING;

    if (a2dpSource_.is_connected())
    {
        // drop the current link; the DISCONNECTED event restarts discovery,
        // which then matches the new selection and connects to it
        a2dpSource_.disconnect();
    }
    // NOTE: do NOT call a2dpSource_.cancel_discovery() here. It sets the
    // library's is_end flag, which permanently disables its automatic
    // scan-restart — if the current inquiry round ends before the chosen
    // device is re-reported, scanning would never resume and the selection
    // would never connect. The ongoing round re-reports the device within
    // seconds, which is exactly what the scan-match below waits for.
}

void AudioEngine::cancelPendingSelection()
{
    portENTER_CRITICAL(&deviceMux_);
    selectionPending_ = false;
    portEXIT_CRITICAL(&deviceMux_);
    sinkState_ = a2dpSource_.is_connected() ? SINK_CONNECTED : SINK_NONE;
}

void AudioEngine::connStateTrampoline(esp_a2d_connection_state_t state, void *)
{
    switch (state)
    {
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
        Serial.println("[BT] connected");
        instance().sinkState_ = SINK_CONNECTED;
        if (instance().sinkName_[0] == '\0')
            strlcpy(instance().sinkName_, "Bluetooth", sizeof(instance().sinkName_));
        break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
        Serial.println("[BT] disconnected");
        instance().sinkState_ = SINK_NONE; // engine restarts discovery
        instance().sinkName_[0] = '\0';    // stale name would fake "connected"
        instance().hasConnectedAddr_ = false;
        instance().rescanQueued_ = true;
        break;
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
    default:
        Serial.println("[BT] connecting...");
        instance().sinkState_ = SINK_CONNECTING;
        break;
    }
}

// ---------------------------------------------------------------------------
// BT task side: audio data
// ---------------------------------------------------------------------------
int32_t AudioEngine::audioDataTrampoline(uint8_t *data, int32_t len)
{
    return instance().fillAudio(data, len);
}

int32_t AudioEngine::fillAudio(uint8_t *data, int32_t len)
{
    if (len <= 0)
        return 0;

    if (!dataCbLogged_)
    {
        dataCbLogged_ = true; // proves the A2DP streaming loop is live
        Serial.printf("[BT] data callback active, chunk=%d bytes\n", (int)len);
        // streaming is confirmed running: the AVRCP link is up by now, so
        // it is safe to push our preferred volume to the speaker
        volumeApplyPending_ = true;
    }

    // The Bluetooth task only consumes pre-decoded frames from the ring.
    // No SD access and no MP3 decoding happen here — anything slow inside
    // this callback would starve the BT stack's tasks (watchdog abort).
    size_t want = (size_t)len / 4; // stereo frames
    size_t made = 0;
    if (playing_)
    {
        uint32_t avail = pcmTail_ - pcmHead_;
        if (avail == 0 && !starveLogged_)
        {
            starveLogged_ = true; // decode task not keeping up / stalled
            Serial.println("[DEC] ring starved while playing");
        }
        made = avail < want ? avail : want;
        int16_t *out = (int16_t *)data;
        for (size_t f = 0; f < made; f++)
        {
            uint32_t idx = ((pcmHead_ + (uint32_t)f) & PCM_RING_MASK) * 2;
            out[2 * f] = pcmRing_[idx];
            out[2 * f + 1] = pcmRing_[idx + 1];
        }
        pcmHead_ += made;
        if (made)
        {
            elapsedSamples48k_ += made;
            updateElapsed();
        }
    }
    if (made < want)
        memset(data + made * 4, 0, (want - made) * 4);
    return len;
}

// ---------------------------------------------------------------------------
// Decode task: decoder -> (optional resample) -> PCM ring buffer
// ---------------------------------------------------------------------------
void AudioEngine::decodeLoopEntry(void *arg)
{
    static_cast<AudioEngine *>(arg)->decodeLoop();
}

void AudioEngine::decodeLoop()
{
    for (;;)
    {
        if (!decoder_ || decodeEof_)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (pcmSpace() < 384)
        {
            if (millis() - lastFullLogMs_ > 5000)
            {
                lastFullLogMs_ = millis(); // rate-limited: this is normal when
                                           // the ring is kept topped up
                Serial.println("[DEC] ring full, waiting for playback");
            }
            vTaskDelay(pdMS_TO_TICKS(3)); // ring full; playback will drain it
            continue;
        }
        if (xSemaphoreTake(audioMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        int made = 0;
        if (decoder_ && !decodeEof_) // switchTo may have swapped while waiting
            made = resample_ ? pumpResampledRing() : pumpStraightRing();
        xSemaphoreGive(audioMutex_);
        if (made > 0 && !producedLogged_)
        {
            producedLogged_ = true; // confirms the pipeline actually decodes
            Serial.printf("[DEC] producing, first batch %d frames\n", made);
        }
        if (made < 0) // end of track reached
        {
            decodeEof_ = true;
            if (!eosLogged_)
            {
                eosLogged_ = true;
                Serial.printf("[MP3] end of track %u\n", (unsigned)trackIdx_);
            }
            postEos();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

int AudioEngine::pumpStraightRing()
{
    size_t space = pcmSpace();
    if (space == 0)
        return 0;
    // Read 1536 bytes per pump: small enough that the decoder's internal
    // result queue (16 KB, see PCM_QUEUE_SIZE) absorbs any multi-frame
    // decode burst between consumer drains, large enough to keep up.
    int32_t got = decoder_->readBytes(readStaging_, 384 * 4);
    if (got <= 0)
        return -1;
    if ((size_t)got % 4 != 0)
    {
        got -= got % 4; // keep whole stereo frames
        if (got <= 0)
            return -1;  // only a trailing partial frame remains
    }
    size_t frames = (size_t)got / 4;
    int16_t *src = (int16_t *)readStaging_;
    for (size_t f = 0; f < frames; f++)
    {
        uint32_t idx = ((pcmTail_ + (uint32_t)f) & PCM_RING_MASK) * 2;
        pcmRing_[idx] = src[2 * f];
        pcmRing_[idx + 1] = src[2 * f + 1];
    }
    pcmTail_ += frames;
    return (int)frames;
}

// Loads the next chunk of decoded PCM (one MP3 frame's worth when running).
// Returns false at end of file. pos is advanced past the consumed frames.
bool AudioEngine::refillConvChunk(float &pos)
{
    size_t oldFrames = convFrames_;
    uint8_t *buf = (uint8_t *)convChunk_;
    const size_t capacity = sizeof(convChunk_);

    memcpy(buf, convHold_, convHoldLen_);
    // same 1536-byte cap as pumpStraightRing: keeps the decoder's internal
    // result queue (16 KB) safely ahead of any multi-frame decode burst
    size_t room = capacity - convHoldLen_;
    if (room > 1536)
        room = 1536;
    int32_t got = decoder_->readBytes(buf + convHoldLen_, (int32_t)room);
    if (got > 0)
        convHoldLen_ += got;

    size_t usable = (convHoldLen_ / 4) * 4;
    convFrames_ = usable / 4;
    memcpy(convHold_, buf + usable, convHoldLen_ - usable);
    convHoldLen_ -= usable;

    if (convFrames_ == 0)
    {
        convFrames_ = 0;
        convHoldLen_ = 0;
        return false;
    }
    pos -= (float)oldFrames;
    if (pos < 0.f)
        pos = 0.f;
    return true;
}

int AudioEngine::pumpResampledRing()
{
    size_t space = pcmSpace();
    if (space == 0)
        return 0;
    size_t want = space < 1024 ? space : 1024;
    size_t made = 0;
    const float inStep = (float)sourceRate_ / 44100.0f;

    while (made < want)
    {
        if (convEof_)
            break;

        // consume frames from convChunk_, refilling when exhausted
        float pos = (float)convIdx_ + convFrac_;
        while (pos >= (float)convFrames_)
        {
            if (!refillConvChunk(pos))
            {
                convEof_ = true;
                break;
            }
        }
        if (convEof_)
            break;

        size_t i = convIdx_;
        float frac = pos - (float)i;
        int16_t l0 = convChunk_[2 * i], r0 = convChunk_[2 * i + 1];
        int16_t l1 = (i + 1 < convFrames_) ? convChunk_[2 * i + 2] : l0;
        int16_t r1 = (i + 1 < convFrames_) ? convChunk_[2 * i + 3] : r0;

        int32_t l = l0 + (int32_t)((float)(l1 - l0) * frac);
        int32_t r = r0 + (int32_t)((float)(r1 - r0) * frac);
        uint32_t idx = ((pcmTail_ + (uint32_t)made) & PCM_RING_MASK) * 2;
        pcmRing_[idx] = clip16(l);
        pcmRing_[idx + 1] = clip16(r);
        made++;

        pos += inStep;
        if (pos >= (float)convFrames_)
        {
            if (!refillConvChunk(pos))
                convEof_ = true;
        }
        convIdx_ = (size_t)pos;
        convFrac_ = pos - (float)convIdx_;
    }

    pcmTail_ += made;
    if (made == 0 && convEof_)
        return -1; // resampler drained: end of track
    return (int)made;
}

void AudioEngine::updateElapsed()
{
    elapsedMs_ = (uint32_t)(elapsedSamples48k_ * 1000ULL / 44100ULL);
}

// ---------------------------------------------------------------------------
// Volume (AVRCP absolute volume on the connected speaker)
// ---------------------------------------------------------------------------
void AudioEngine::volumeUp() { setVolumePct(volumePct_ + 10); }

void AudioEngine::volumeDown() { setVolumePct(volumePct_ - 10); }

void AudioEngine::setVolumePct(int8_t pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    if (pct == volumePct_)
        return;
    volumePct_ = pct;
    volumeGen_++;
    a2dpSource_.set_volume((uint8_t)((uint32_t)pct * 127u / 100u));
    Serial.printf("[BT] volume %d%%\n", pct);
}
