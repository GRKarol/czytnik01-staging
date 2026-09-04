// firmware/src/audio/AudioRecorder.cpp
#include "audio/AudioRecorder.h"

#include <SD_MMC.h>
#include <Wire.h>
#include <algorithm>
#include <driver/i2s.h>
#include <esp_log.h>

#include "audio/AudioVolume.h"
#include "board/BoardConfig.h"

static const char* TAG = "AudioRecorder";

namespace {
constexpr i2s_port_t kI2sPort = I2S_NUM_0;

// ES8311 register addresses
constexpr uint8_t kEs8311ResetReg = 0x00;
constexpr uint8_t kEs8311ClkManagerReg01 = 0x01;
constexpr uint8_t kEs8311ClkManagerReg02 = 0x02;
constexpr uint8_t kEs8311ClkManagerReg03 = 0x03;
constexpr uint8_t kEs8311ClkManagerReg04 = 0x04;
constexpr uint8_t kEs8311ClkManagerReg05 = 0x05;
constexpr uint8_t kEs8311ClkManagerReg06 = 0x06;
constexpr uint8_t kEs8311ClkManagerReg07 = 0x07;
constexpr uint8_t kEs8311ClkManagerReg08 = 0x08;
constexpr uint8_t kEs8311SdPinReg09 = 0x09;
constexpr uint8_t kEs8311SdPoutReg0A = 0x0A;
constexpr uint8_t kEs8311SystemReg0B = 0x0B;
constexpr uint8_t kEs8311SystemReg0C = 0x0C;
constexpr uint8_t kEs8311SystemReg0D = 0x0D;
constexpr uint8_t kEs8311SystemReg0E = 0x0E;
constexpr uint8_t kEs8311SystemReg10 = 0x10;
constexpr uint8_t kEs8311SystemReg11 = 0x11;
constexpr uint8_t kEs8311SystemReg12 = 0x12;
constexpr uint8_t kEs8311SystemReg13 = 0x13;
constexpr uint8_t kEs8311SystemReg14 = 0x14;
constexpr uint8_t kEs8311AdcReg15 = 0x15;
constexpr uint8_t kEs8311AdcReg16 = 0x16;
constexpr uint8_t kEs8311AdcReg17 = 0x17;
constexpr uint8_t kEs8311AdcReg1B = 0x1B;
constexpr uint8_t kEs8311AdcReg1C = 0x1C;
constexpr uint8_t kEs8311DacReg31 = 0x31;
constexpr uint8_t kEs8311DacReg32 = 0x32;
constexpr uint8_t kEs8311DacReg37 = 0x37;
constexpr uint8_t kEs8311GpioReg44 = 0x44;
constexpr uint8_t kEs8311GpReg45 = 0x45;

constexpr uint8_t kIoConfigRegister = 0x03;
constexpr uint8_t kIoOutputRegister = 0x01;

}  // namespace

bool AudioRecorder::begin() {
    if (initialized_) return true;
    initialized_ = true;
    return true;
}

// ─── Recording ──────────────────────────────────────────────────────────────

bool AudioRecorder::startRecording(const char* absolutePath) {
    if (recording_ || playing_) {
        ESP_LOGW(TAG, "Cannot start recording — already busy");
        return false;
    }

    currentFilePath_ = absolutePath;
    stopRequested_ = false;
    recording_ = true;
    recordStartMs_ = millis();
    recordingPeakLevel_ = 0;

    BaseType_t result = xTaskCreatePinnedToCore(
        recordTaskEntry, "rec", kRecordTaskStackSize, this, 1, &recordTask_, 0);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create record task");
        recording_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Recording started: %s", absolutePath);
    return true;
}

bool AudioRecorder::stopRecording() {
    if (!recording_) return false;

    stopRequested_ = true;

    // Wait for task to finish (max 2s)
    uint32_t waitStart = millis();
    while (recording_ && (millis() - waitStart) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (recording_) {
        // Force kill
        if (recordTask_) {
            vTaskDelete(recordTask_);
            recordTask_ = nullptr;
        }
        deinitI2s();
        recording_ = false;
    }

    ESP_LOGI(TAG, "Recording stopped");
    return true;
}

bool AudioRecorder::isRecording() const {
    return recording_;
}

uint32_t AudioRecorder::recordingElapsedMs() const {
    if (!recording_) return 0;
    return millis() - recordStartMs_;
}

uint8_t AudioRecorder::recordingPeakLevel() const {
    return recordingPeakLevel_;
}

// ─── Playback ───────────────────────────────────────────────────────────────

bool AudioRecorder::startPlayback(const char* absolutePath) {
    if (recording_ || playing_) {
        ESP_LOGW(TAG, "Cannot start playback — already busy");
        return false;
    }

    // Quick check: verify file exists before starting task
    File checkFile = SD_MMC.open(absolutePath, FILE_READ);
    if (!checkFile) {
        ESP_LOGE(TAG, "Playback file not found: %s", absolutePath);
        return false;
    }

    WavHeader hdr;
    if (checkFile.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr)) {
        checkFile.close();
        ESP_LOGE(TAG, "Failed to read WAV header");
        return false;
    }

    // Validate WAV
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
        checkFile.close();
        ESP_LOGE(TAG, "Invalid WAV file");
        return false;
    }

    uint32_t totalSamples = hdr.dataSize / (hdr.bitsPerSample / 8) / hdr.numChannels;
    playbackTotalMs_ = (totalSamples * 1000UL) / hdr.sampleRate;
    playbackBytesPerMs_ = (hdr.sampleRate * hdr.numChannels * (hdr.bitsPerSample / 8)) / 1000UL;

    checkFile.close();

    currentFilePath_ = absolutePath;
    stopRequested_ = false;
    paused_ = false;
    seekTargetMs_ = -1;
    playing_ = true;
    playbackStartMs_ = millis();

    BaseType_t result = xTaskCreatePinnedToCore(
        playbackTaskEntry, "play", kPlaybackTaskStackSize, this, 1, &playbackTask_, 0);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        playing_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Playback started: %s", absolutePath);
    return true;
}

bool AudioRecorder::stopPlayback() {
    if (!playing_) return false;

    stopRequested_ = true;

    uint32_t waitStart = millis();
    while (playing_ && (millis() - waitStart) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (playing_) {
        if (playbackTask_) {
            vTaskDelete(playbackTask_);
            playbackTask_ = nullptr;
        }
        deinitI2s();
        playing_ = false;
    }

    ESP_LOGI(TAG, "Playback stopped");
    return true;
}

bool AudioRecorder::isPlaying() const {
    return playing_;
}

uint32_t AudioRecorder::playbackElapsedMs() const {
    if (!playing_) return 0;
    // Frozen at the instant pause began — real time keeps moving while
    // paused, but the playback position must not appear to.
    if (paused_) return pauseBeganMs_ - playbackStartMs_;
    return millis() - playbackStartMs_;
}

uint32_t AudioRecorder::playbackTotalMs() const {
    return playbackTotalMs_;
}

void AudioRecorder::pausePlayback() {
    if (!playing_ || paused_) return;
    paused_ = true;
    pauseBeganMs_ = millis();
}

void AudioRecorder::resumePlayback() {
    if (!playing_ || !paused_) return;
    // Shift the elapsed-time epoch forward by however long we were paused,
    // so playbackElapsedMs() picks up exactly where it left off instead of
    // jumping ahead by the pause duration.
    playbackStartMs_ += millis() - pauseBeganMs_;
    paused_ = false;
}

bool AudioRecorder::isPaused() const {
    return paused_;
}

void AudioRecorder::seekPlaybackBy(int32_t deltaMs) {
    if (!playing_) return;

    int32_t current = static_cast<int32_t>(playbackElapsedMs());
    int32_t total = static_cast<int32_t>(playbackTotalMs_);
    int32_t target = current + deltaMs;
    if (target < 0) target = 0;
    if (target > total) target = total;

    // Same epoch-shift trick as pause/resume: move playbackStartMs_ so
    // playbackElapsedMs() reports the new position immediately, without
    // waiting for the playback task to catch up on the actual file seek.
    playbackStartMs_ = millis() - static_cast<uint32_t>(target);
    if (paused_) {
        // Keep the frozen-elapsed formula (pauseBeganMs_ - playbackStartMs_)
        // consistent with the new epoch, otherwise it'd read back the time
        // elapsed since the *original* pause instead of the seek target.
        pauseBeganMs_ = millis();
    }
    seekTargetMs_ = target;
}

// ─── I2S Configuration ──────────────────────────────────────────────────────

bool AudioRecorder::configureI2sForRecording() {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 4;
    config.dma_buf_len = 256;
    config.use_apll = false;
    config.tx_desc_auto_clear = false;
    config.fixed_mclk = 0;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_driver_install(kI2sPort, &config, 0, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install (RX) failed: %s", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = BoardConfig::PIN_AUDIO_MCLK;
    pins.bck_io_num = BoardConfig::PIN_AUDIO_BCLK;
    pins.ws_io_num = BoardConfig::PIN_AUDIO_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = BoardConfig::PIN_AUDIO_DIN;

    err = i2s_set_pin(kI2sPort, &pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pins (RX) failed: %s", esp_err_to_name(err));
        i2s_driver_uninstall(kI2sPort);
        return false;
    }

    i2s_set_clk(kI2sPort, kSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    return true;
}

bool AudioRecorder::configureI2sForPlayback() {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 4;
    config.dma_buf_len = 256;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 0;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_driver_install(kI2sPort, &config, 0, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install (TX) failed: %s", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = BoardConfig::PIN_AUDIO_MCLK;
    pins.bck_io_num = BoardConfig::PIN_AUDIO_BCLK;
    pins.ws_io_num = BoardConfig::PIN_AUDIO_WS;
    pins.data_out_num = BoardConfig::PIN_AUDIO_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(kI2sPort, &pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pins (TX) failed: %s", esp_err_to_name(err));
        i2s_driver_uninstall(kI2sPort);
        return false;
    }

    i2s_set_clk(kI2sPort, kSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(kI2sPort);
    return true;
}

void AudioRecorder::deinitI2s() {
    i2s_driver_uninstall(kI2sPort);
}

// ─── Codec Configuration ────────────────────────────────────────────────────

bool AudioRecorder::configureCodecForRecording() {
    uint8_t reg = 0;

    // Noise-immunity warm-up, same as configureCodecForPlayback(). Espressif's
    // own ES8311 driver writes this reg44 value twice before touching
    // anything else and comments why: "occasional failures during the first
    // I2C write with the ES8311 chip". This function resets the codec next,
    // which is exactly the kind of fresh-after-reset state that write is meant
    // to stabilize — recording was the one path skipping it, unlike playback.
    if (!writeCodecRegister(kEs8311GpioReg44, 0x08)) return false;
    if (!writeCodecRegister(kEs8311GpioReg44, 0x08)) return false;

    // Reset codec
    if (!writeCodecRegister(kEs8311ResetReg, 0x80)) return false;
    delay(5);
    if (!readCodecRegister(kEs8311ResetReg, reg)) return false;
    if (!writeCodecRegister(kEs8311ResetReg, static_cast<uint8_t>(reg & 0xBF))) return false;

    // Clock setup — MCLK from I2S master, 256x oversampling
    if (!writeCodecRegister(kEs8311ClkManagerReg01, 0x3F)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg02, 0x00)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg03, 0x10)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg04, 0x10)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg05, 0x00)) return false;
    if (!readCodecRegister(kEs8311ClkManagerReg06, reg)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg06, static_cast<uint8_t>((reg & 0xE0) | 0x03))) return false;
    if (!readCodecRegister(kEs8311ClkManagerReg07, reg)) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg07, static_cast<uint8_t>(reg & 0xC0))) return false;
    if (!writeCodecRegister(kEs8311ClkManagerReg08, 0xFF)) return false;

    // ADC serial data format: 16-bit I2S
    if (!readCodecRegister(kEs8311SdPoutReg0A, reg)) return false;
    reg = static_cast<uint8_t>((reg & 0xE0) | 0x0C);  // 16-bit
    // Bit 6 of this register is a mute for the ADC's SDOUT line, not an
    // enable — configureCodecForPlayback() sets it to 1 to deliberately
    // silence the mic path during playback-only sessions. Recording needs
    // the opposite: clear it so SDOUT actually carries ADC samples instead
    // of the codec's own internal mute pattern (which is what the mic was
    // capturing — a constant, near-zero signal, hence peak level stuck at 0%
    // regardless of what reached the microphone).
    reg &= static_cast<uint8_t>(~(1U << 6));
    if (!writeCodecRegister(kEs8311SdPoutReg0A, reg)) return false;

    // System configuration — power up ADC
    if (!writeCodecRegister(kEs8311SystemReg0B, 0x00)) return false;
    if (!writeCodecRegister(kEs8311SystemReg0C, 0x00)) return false;
    if (!writeCodecRegister(kEs8311SystemReg10, 0x1F)) return false;
    if (!writeCodecRegister(kEs8311SystemReg11, 0x7F)) return false;
    if (!writeCodecRegister(kEs8311SystemReg0D, 0x01)) return false;
    if (!writeCodecRegister(kEs8311SystemReg0E, 0x02)) return false;
    if (!writeCodecRegister(kEs8311SystemReg12, 0x00)) return false;
    if (!writeCodecRegister(kEs8311SystemReg13, 0x10)) return false;
    if (!writeCodecRegister(kEs8311SystemReg14, 0x1A)) return false;

    // ADC configuration — microphone input with PGA gain
    if (!writeCodecRegister(kEs8311AdcReg15, 0x40)) return false;  // ADC power on
    if (!writeCodecRegister(kEs8311AdcReg16, 0x24)) return false;  // MIC input select
    if (!writeCodecRegister(kEs8311AdcReg17, 0xBF)) return false;  // ADC enable, HPF on
    if (!writeCodecRegister(kEs8311AdcReg1B, 0x0A)) return false;  // MIC PGA gain
    if (!writeCodecRegister(kEs8311AdcReg1C, 0x6A)) return false;  // ADC volume

    // GPIO configuration
    if (!writeCodecRegister(kEs8311GpioReg44, 0x58)) return false;
    if (!writeCodecRegister(kEs8311GpReg45, 0x00)) return false;

    ESP_LOGI(TAG, "Codec configured for recording");
    return true;
}

bool AudioRecorder::configureCodecForPlayback() {
    // This mirrors AudioManager's ES8311 init byte-for-byte (the only other
    // DAC-output path in the firmware) instead of the previous paraphrased
    // sequence here, which transmitted I2S data fine — recordings play back
    // for their exact recorded length — but never actually powered on the
    // codec's analog output driver, so the speaker stayed silent.
    uint8_t reg = 0;

    const bool opened =
        writeCodecRegister(kEs8311GpioReg44, 0x08) &&
        writeCodecRegister(kEs8311GpioReg44, 0x08) &&
        writeCodecRegister(kEs8311ClkManagerReg01, 0x30) &&
        writeCodecRegister(kEs8311ClkManagerReg02, 0x00) &&
        writeCodecRegister(kEs8311ClkManagerReg03, 0x10) &&
        writeCodecRegister(kEs8311AdcReg16, 0x24) &&
        writeCodecRegister(kEs8311ClkManagerReg04, 0x10) &&
        writeCodecRegister(kEs8311ClkManagerReg05, 0x00) &&
        writeCodecRegister(kEs8311SystemReg0B, 0x00) &&
        writeCodecRegister(kEs8311SystemReg0C, 0x00) &&
        writeCodecRegister(kEs8311SystemReg10, 0x1F) &&
        writeCodecRegister(kEs8311SystemReg11, 0x7F) &&
        writeCodecRegister(kEs8311ResetReg, 0x80) &&
        readCodecRegister(kEs8311ResetReg, reg) &&
        writeCodecRegister(kEs8311ResetReg, static_cast<uint8_t>(reg & 0xBF)) &&
        writeCodecRegister(kEs8311ClkManagerReg01, 0x3F) &&
        readCodecRegister(kEs8311ClkManagerReg06, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg06, static_cast<uint8_t>(reg & ~0x20U)) &&
        writeCodecRegister(kEs8311SystemReg13, 0x10) &&
        writeCodecRegister(kEs8311AdcReg1B, 0x0A) &&
        writeCodecRegister(kEs8311AdcReg1C, 0x6A) &&
        writeCodecRegister(kEs8311GpioReg44, 0x58);

    if (!opened) {
        ESP_LOGE(TAG, "Codec power-up sequence failed (playback)");
        return false;
    }

    // Sample format — 16-bit I2S on both interfaces.
    uint8_t dacIface = 0;
    uint8_t adcIface = 0;
    if (!readCodecRegister(kEs8311SdPinReg09, dacIface) ||
        !readCodecRegister(kEs8311SdPoutReg0A, adcIface)) {
        return false;
    }
    dacIface = static_cast<uint8_t>((dacIface & 0xE0U) | 0x0CU);
    adcIface = static_cast<uint8_t>((adcIface & 0xE0U) | 0x0CU);

    const bool formatOk =
        writeCodecRegister(kEs8311SdPinReg09, dacIface) &&
        writeCodecRegister(kEs8311SdPoutReg0A, adcIface) &&
        readCodecRegister(kEs8311ClkManagerReg02, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg02, static_cast<uint8_t>(reg & 0x07U)) &&
        writeCodecRegister(kEs8311ClkManagerReg05, 0x00) &&
        readCodecRegister(kEs8311ClkManagerReg03, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg03, static_cast<uint8_t>((reg & 0x80U) | 0x10U)) &&
        readCodecRegister(kEs8311ClkManagerReg04, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg04, static_cast<uint8_t>((reg & 0x80U) | 0x10U)) &&
        readCodecRegister(kEs8311ClkManagerReg07, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg07, static_cast<uint8_t>(reg & 0xC0U)) &&
        writeCodecRegister(kEs8311ClkManagerReg08, 0xFF) &&
        readCodecRegister(kEs8311ClkManagerReg06, reg) &&
        writeCodecRegister(kEs8311ClkManagerReg06, static_cast<uint8_t>((reg & 0xE0U) | 0x03U));

    if (!formatOk) {
        ESP_LOGE(TAG, "Codec sample-format setup failed (playback)");
        return false;
    }

    // Start codec — DAC path active, ADC output disabled (playback-only).
    if (!writeCodecRegister(kEs8311ResetReg, 0x80) ||
        !writeCodecRegister(kEs8311ClkManagerReg01, 0x3F) ||
        !readCodecRegister(kEs8311SdPinReg09, dacIface) ||
        !readCodecRegister(kEs8311SdPoutReg0A, adcIface)) {
        return false;
    }
    dacIface &= static_cast<uint8_t>(~(1U << 6));   // DAC input enabled on SDIN
    adcIface |= static_cast<uint8_t>(1U << 6);       // ADC output disabled on SDOUT

    const bool started =
        writeCodecRegister(kEs8311SdPinReg09, dacIface) &&
        writeCodecRegister(kEs8311SdPoutReg0A, adcIface) &&
        writeCodecRegister(kEs8311AdcReg17, 0xBF) &&
        writeCodecRegister(kEs8311SystemReg0E, 0x02) &&
        writeCodecRegister(kEs8311SystemReg12, 0x00) &&
        writeCodecRegister(kEs8311SystemReg14, 0x1A) &&
        writeCodecRegister(kEs8311SystemReg0D, 0x01) &&
        writeCodecRegister(kEs8311AdcReg15, 0x40) &&
        writeCodecRegister(kEs8311DacReg37, 0x08) &&
        writeCodecRegister(kEs8311GpReg45, 0x00);

    if (!started) {
        ESP_LOGE(TAG, "Codec start sequence failed (playback)");
        return false;
    }

    uint8_t dacMute = 0;
    if (!readCodecRegister(kEs8311DacReg31, dacMute)) return false;
    dacMute &= 0x9F;  // unmute DAC
    if (!writeCodecRegister(kEs8311DacReg31, dacMute)) return false;
    if (!writeCodecRegister(kEs8311DacReg32, AudioVolume::dacRegisterValue())) return false;

    ESP_LOGI(TAG, "Codec configured for playback");
    return true;
}

// ─── Volume ─────────────────────────────────────────────────────────────────

void AudioRecorder::applyVolume() {
    // Only meaningful while the DAC path is actually active; writing the
    // register otherwise still succeeds but has nothing to affect until the
    // next configureCodecForPlayback() call, which already reads the current
    // AudioVolume value on its own.
    if (!playing_) return;
    writeCodecRegister(kEs8311DacReg32, AudioVolume::dacRegisterValue());
}

// ─── Audio Rail ─────────────────────────────────────────────────────────────

bool AudioRecorder::enableAudioRail() {
    BoardConfig::I2cBusLock lock;
    uint8_t direction = 0xFF;
    uint8_t output = 0xFF;

    Wire1.beginTransmission(BoardConfig::TCA9554_ADDRESS);
    Wire1.write(kIoConfigRegister);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(static_cast<int>(BoardConfig::TCA9554_ADDRESS), 1, 1) != 1) return false;
    direction = Wire1.read();

    Wire1.beginTransmission(BoardConfig::TCA9554_ADDRESS);
    Wire1.write(kIoOutputRegister);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(static_cast<int>(BoardConfig::TCA9554_ADDRESS), 1, 1) != 1) return false;
    output = Wire1.read();

    const uint8_t mask = static_cast<uint8_t>(1U << BoardConfig::TCA9554_PIN_AUDIO_ENABLE);
    output |= mask;
    direction &= static_cast<uint8_t>(~mask);

    Wire1.beginTransmission(BoardConfig::TCA9554_ADDRESS);
    Wire1.write(kIoOutputRegister);
    Wire1.write(output);
    if (Wire1.endTransmission(true) != 0) return false;

    Wire1.beginTransmission(BoardConfig::TCA9554_ADDRESS);
    Wire1.write(kIoConfigRegister);
    Wire1.write(direction);
    return Wire1.endTransmission(true) == 0;
}

// ─── Codec Register Access ──────────────────────────────────────────────────

bool AudioRecorder::readCodecRegister(uint8_t reg, uint8_t& value) {
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(BoardConfig::ES8311_ADDRESS);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(static_cast<int>(BoardConfig::ES8311_ADDRESS), 1, 1) != 1) return false;
    value = Wire1.read();
    return true;
}

bool AudioRecorder::writeCodecRegister(uint8_t reg, uint8_t value) {
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(BoardConfig::ES8311_ADDRESS);
    Wire1.write(reg);
    Wire1.write(value);
    return Wire1.endTransmission(true) == 0;
}

// ─── WAV Header ─────────────────────────────────────────────────────────────

static bool writeWavHeader(File& file, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels) {
    AudioRecorder::WavHeader hdr;
    hdr.sampleRate = sampleRate;
    hdr.numChannels = numChannels;
    hdr.bitsPerSample = bitsPerSample;
    hdr.byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    hdr.blockAlign = numChannels * (bitsPerSample / 8);
    // Write placeholder header (fileSize and dataSize will be filled on stop)
    return file.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) == sizeof(hdr);
}

static bool finalizeWavHeader(File& file, uint32_t dataBytes, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels) {
    AudioRecorder::WavHeader hdr;
    hdr.sampleRate = sampleRate;
    hdr.numChannels = numChannels;
    hdr.bitsPerSample = bitsPerSample;
    hdr.byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    hdr.blockAlign = numChannels * (bitsPerSample / 8);
    hdr.dataSize = dataBytes;
    hdr.fileSize = sizeof(AudioRecorder::WavHeader) - 8 + dataBytes;

    file.seek(0);
    return file.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) == sizeof(hdr);
}

// ─── Record Task ────────────────────────────────────────────────────────────

void AudioRecorder::recordTaskEntry(void* param) {
    static_cast<AudioRecorder*>(param)->recordTaskLoop();
    vTaskDelete(nullptr);
}

void AudioRecorder::recordTaskLoop() {
    // Hardware setup (runs on recording task's core, non-blocking for plugin task)
    if (!enableAudioRail()) {
        ESP_LOGE(TAG, "Cannot enable audio rail");
        recording_ = false;
        recordTask_ = nullptr;
        return;
    }

    delay(15);

    // Uninstall existing I2S driver (AudioManager may have it in TX mode)
    i2s_driver_uninstall(kI2sPort);

    if (!configureCodecForRecording()) {
        ESP_LOGE(TAG, "Failed to configure codec for recording");
        recording_ = false;
        recordTask_ = nullptr;
        return;
    }

    if (!configureI2sForRecording()) {
        ESP_LOGE(TAG, "Failed to configure I2S for recording");
        recording_ = false;
        recordTask_ = nullptr;
        return;
    }

    // Reset start time to exclude hardware setup duration
    recordStartMs_ = millis();

    File file = SD_MMC.open(currentFilePath_, FILE_WRITE);
    if (!file) {
        ESP_LOGE(TAG, "Cannot create recording file: %s", currentFilePath_.c_str());
        deinitI2s();
        recording_ = false;
        recordTask_ = nullptr;
        return;
    }

    writeWavHeader(file, kSampleRate, kBitsPerSample, kChannels);

    uint8_t buffer[kRecordBufferSize];
    int16_t monoBuffer[kRecordBufferSize / 4];  // stereo → mono
    uint32_t totalDataBytes = 0;
    uint32_t loopCount = 0;
    esp_err_t lastReadErr = ESP_OK;
    size_t zeroReadStreak = 0;

    while (!stopRequested_) {
        // Check max duration
        if ((millis() - recordStartMs_) >= kMaxRecordingMs) {
            ESP_LOGI(TAG, "Max recording duration reached");
            break;
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(kI2sPort, buffer, kRecordBufferSize, &bytesRead, pdMS_TO_TICKS(100));
        lastReadErr = err;

        if (err != ESP_OK || bytesRead == 0) {
            zeroReadStreak++;
            if (zeroReadStreak == 1 || zeroReadStreak % 50 == 0) {
                ESP_LOGW(TAG, "i2s_read empty (err=%s, streak=%u)", esp_err_to_name(err),
                         static_cast<unsigned>(zeroReadStreak));
            }
            taskYIELD();
            continue;
        }
        zeroReadStreak = 0;

        // Convert stereo 16-bit to mono. Average both channels rather than
        // keeping only the left one — the ES8311 mono mic path duplicates
        // its signal onto both slots on this board, but if it turns out to
        // only be present on one slot, averaging still captures it (at half
        // amplitude) instead of silently discarding it by picking the wrong
        // slot.
        size_t stereoSamples = bytesRead / 4;  // 2 bytes * 2 channels per sample
        const int16_t* stereoData = reinterpret_cast<const int16_t*>(buffer);
        int16_t peakSample = 0;
        int16_t leftMin = 0, leftMax = 0, rightMin = 0, rightMax = 0;
        for (size_t i = 0; i < stereoSamples; i++) {
            int32_t left = stereoData[i * 2];
            int32_t right = stereoData[i * 2 + 1];
            if (i == 0) {
                leftMin = leftMax = static_cast<int16_t>(left);
                rightMin = rightMax = static_cast<int16_t>(right);
            } else {
                leftMin = std::min(leftMin, static_cast<int16_t>(left));
                leftMax = std::max(leftMax, static_cast<int16_t>(left));
                rightMin = std::min(rightMin, static_cast<int16_t>(right));
                rightMax = std::max(rightMax, static_cast<int16_t>(right));
            }
            int16_t mixed = static_cast<int16_t>((left + right) / 2);
            monoBuffer[i] = mixed;
            int16_t absMixed = static_cast<int16_t>(mixed < 0 ? -mixed : mixed);
            if (absMixed > peakSample) peakSample = absMixed;
        }
        recordingPeakLevel_ = static_cast<uint8_t>((static_cast<uint32_t>(peakSample) * 100U) / 32767U);

        loopCount++;
        if (loopCount <= 3 || loopCount % 20 == 0) {
            ESP_LOGI(TAG, "buf#%u bytesRead=%u L[min=%d max=%d] R[min=%d max=%d] first16=%02X %02X %02X %02X %02X %02X %02X %02X",
                     static_cast<unsigned>(loopCount), static_cast<unsigned>(bytesRead),
                     leftMin, leftMax, rightMin, rightMax,
                     buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
        }

        size_t monoBytes = stereoSamples * 2;
        size_t written = file.write(reinterpret_cast<const uint8_t*>(monoBuffer), monoBytes);
        if (written != monoBytes) {
            ESP_LOGE(TAG, "SD write error during recording");
            break;
        }

        totalDataBytes += written;

        // Yield to allow other tasks on same core to run
        vTaskDelay(1);
    }

    // Finalize WAV header
    finalizeWavHeader(file, totalDataBytes, kSampleRate, kBitsPerSample, kChannels);
    file.close();

    deinitI2s();
    recording_ = false;
    recordTask_ = nullptr;
    ESP_LOGI(TAG, "Recording complete: %u bytes", totalDataBytes);
}

// ─── Playback Task ──────────────────────────────────────────────────────────

void AudioRecorder::playbackTaskEntry(void* param) {
    static_cast<AudioRecorder*>(param)->playbackTaskLoop();
    vTaskDelete(nullptr);
}

void AudioRecorder::playbackTaskLoop() {
    // Hardware setup (runs on playback task's core, non-blocking for plugin task)
    if (!enableAudioRail()) {
        ESP_LOGE(TAG, "Cannot enable audio rail for playback");
        playing_ = false;
        playbackTask_ = nullptr;
        return;
    }

    delay(15);

    // Uninstall existing I2S driver (AudioManager may have it in TX mode)
    i2s_driver_uninstall(kI2sPort);

    if (!configureCodecForPlayback()) {
        ESP_LOGE(TAG, "Failed to configure codec for playback");
        playing_ = false;
        playbackTask_ = nullptr;
        return;
    }

    if (!configureI2sForPlayback()) {
        ESP_LOGE(TAG, "Failed to configure I2S for playback");
        playing_ = false;
        playbackTask_ = nullptr;
        return;
    }

    // Reset start time to exclude hardware setup duration
    playbackStartMs_ = millis();

    File file = SD_MMC.open(currentFilePath_, FILE_READ);
    if (!file) {
        ESP_LOGE(TAG, "Cannot open playback file: %s", currentFilePath_.c_str());
        deinitI2s();
        playing_ = false;
        playbackTask_ = nullptr;
        return;
    }

    // Skip WAV header
    file.seek(sizeof(WavHeader));

    uint8_t monoBuffer[kPlaybackBufferSize / 2];
    int16_t stereoBuffer[kPlaybackBufferSize / 2];  // mono → stereo

    while (!stopRequested_) {
        if (paused_) {
            // Don't read or write anything while paused — just idle. The
            // DMA buffer already queued drains naturally (a few tens of ms
            // of tail), which is preferable to zeroing it here and risking
            // a pop.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int32_t seekTarget = seekTargetMs_;
        if (seekTarget >= 0) {
            seekTargetMs_ = -1;
            uint32_t offset = sizeof(WavHeader) +
                               static_cast<uint32_t>(seekTarget) * playbackBytesPerMs_;
            if (offset > file.size()) offset = file.size();
            file.seek(offset);
            continue;
        }

        if (!file.available()) break;

        size_t monoBytes = file.read(monoBuffer, sizeof(monoBuffer));
        if (monoBytes == 0) break;

        // Convert mono 16-bit to stereo (duplicate to both channels)
        size_t monoSamples = monoBytes / 2;
        const int16_t* monoData = reinterpret_cast<const int16_t*>(monoBuffer);
        for (size_t i = 0; i < monoSamples; i++) {
            stereoBuffer[i * 2] = monoData[i];      // left
            stereoBuffer[i * 2 + 1] = monoData[i];  // right
        }

        size_t stereoBytes = monoSamples * 4;
        size_t bytesWritten = 0;
        i2s_write(kI2sPort, stereoBuffer, stereoBytes, &bytesWritten, pdMS_TO_TICKS(250));
    }

    file.close();
    deinitI2s();
    playing_ = false;
    playbackTask_ = nullptr;
    ESP_LOGI(TAG, "Playback complete");
}
