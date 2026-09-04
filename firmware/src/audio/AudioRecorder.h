// firmware/src/audio/AudioRecorder.h
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/**
 * AudioRecorder — records audio from the ES7210 codec ADC (microphone input,
 * a separate I2C device from the ES8311 — see BoardConfig::ES7210_ADDRESS)
 * via I2S and writes WAV files to SD card.
 *
 * Also handles playback of WAV files through the ES8311 DAC.
 *
 * Recording format: 16-bit PCM, 16 kHz, mono WAV.
 * Max recording duration: 30 minutes.
 */

static constexpr uint32_t kDefaultSampleRate = 16000;

class AudioRecorder {
 public:
    // WAV header structure (public for helper functions)
    struct WavHeader {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t fileSize = 0;      // filled on stop
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt[4] = {'f', 'm', 't', ' '};
        uint32_t fmtSize = 16;
        uint16_t audioFormat = 1;   // PCM
        uint16_t numChannels = 1;
        uint32_t sampleRate = kDefaultSampleRate;
        uint32_t byteRate = kDefaultSampleRate * 1 * 2;
        uint16_t blockAlign = 2;
        uint16_t bitsPerSample = 16;
        char data[4] = {'d', 'a', 't', 'a'};
        uint32_t dataSize = 0;      // filled on stop
    } __attribute__((packed));

    bool begin();

    // Recording
    bool startRecording(const char* absolutePath);
    bool stopRecording();
    bool isRecording() const;
    uint32_t recordingElapsedMs() const;

    // Peak input level of the most recently captured buffer, 0-100 (percent
    // of int16 full scale). For on-screen diagnosis of "is the mic actually
    // picking anything up" without a serial cable — stays at 0 the whole
    // recording if the ADC path is silent.
    uint8_t recordingPeakLevel() const;

    // Playback
    bool startPlayback(const char* absolutePath);
    bool stopPlayback();
    bool isPlaying() const;
    uint32_t playbackElapsedMs() const;
    uint32_t playbackTotalMs() const;

    // Pause holds the playback task idle (no file reads, no I2S writes)
    // without tearing down the codec/I2S session, so resume is instant and
    // glitch-free. playbackElapsedMs() freezes while paused.
    void pausePlayback();
    void resumePlayback();
    bool isPaused() const;

    // Jumps playback by deltaMs (negative = backward), clamped to
    // [0, playbackTotalMs()]. Takes effect within one read buffer — no
    // task restart, so no re-init pop.
    void seekPlaybackBy(int32_t deltaMs);

    // Re-pushes AudioVolume::dacRegisterValue() to the codec while playback
    // is active, so a volume change takes effect immediately.
    void applyVolume();

 private:
    static constexpr uint32_t kSampleRate = 16000;
    static constexpr uint32_t kBitsPerSample = 16;
    static constexpr uint32_t kChannels = 1;  // mono recording
    static constexpr uint32_t kMaxRecordingMs = 30UL * 60UL * 1000UL;  // 30 min
    static constexpr size_t kRecordBufferSize = 1024;  // bytes per DMA read
    static constexpr size_t kPlaybackBufferSize = 1024;
    static constexpr uint32_t kRecordTaskStackSize = 8192;
    static constexpr uint32_t kPlaybackTaskStackSize = 8192;

    bool configureCodecForRecording();
    bool configureCodecForPlayback();
    bool configureI2sForRecording();
    bool configureI2sForPlayback();
    void deinitI2s();
    bool enableAudioRail();

    bool readCodecRegister(uint8_t reg, uint8_t& value);
    bool writeCodecRegister(uint8_t reg, uint8_t value);

    // Recording talks to a second, separate I2C codec — see the comment on
    // BoardConfig::ES7210_ADDRESS for why. Kept as distinct helpers rather
    // than an address parameter on the ES8311 ones above so a misplaced call
    // can't silently address the wrong chip.
    bool readEs7210Register(uint8_t reg, uint8_t& value);
    bool writeEs7210Register(uint8_t reg, uint8_t value);
    bool updateEs7210RegisterBits(uint8_t reg, uint8_t mask, uint8_t data);
    bool selectEs7210Mics();

    static void recordTaskEntry(void* param);
    void recordTaskLoop();
    static void playbackTaskEntry(void* param);
    void playbackTaskLoop();

    // State
    volatile bool recording_ = false;
    volatile bool playing_ = false;
    volatile bool stopRequested_ = false;
    volatile uint32_t recordStartMs_ = 0;
    volatile uint32_t playbackStartMs_ = 0;
    volatile uint32_t playbackTotalMs_ = 0;
    volatile uint8_t recordingPeakLevel_ = 0;

    volatile bool paused_ = false;
    volatile uint32_t pauseBeganMs_ = 0;
    // Bytes of PCM data per millisecond of audio for the file currently
    // playing — set from its WAV header in startPlayback(), used to turn a
    // seek target (ms) into a file byte offset.
    volatile uint32_t playbackBytesPerMs_ = 0;
    // >=0 = pending seek target in ms, consumed by playbackTaskLoop(); -1 =
    // no pending seek.
    volatile int32_t seekTargetMs_ = -1;

    TaskHandle_t recordTask_ = nullptr;
    TaskHandle_t playbackTask_ = nullptr;

    String currentFilePath_;
    bool initialized_ = false;
};
