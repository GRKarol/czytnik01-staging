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

// ES7210 register addresses (mic ADC codec — see BoardConfig::ES7210_ADDRESS
// for why recording talks to this chip and not the ES8311). Verified against
// Waveshare's own shipped reference firmware for this exact board
// (waveshareteam/ESP32-S3-Touch-LCD-3.49, codec_board component, board id
// "S3_LCD_3_49" — same MCLK/BCLK/WS/DIN/DOUT pins as our BoardConfig, ES7210
// on the ADC side).
constexpr uint8_t kEs7210ResetReg00 = 0x00;
constexpr uint8_t kEs7210ClockOffReg01 = 0x01;
constexpr uint8_t kEs7210MainClkReg02 = 0x02;
constexpr uint8_t kEs7210LrckDivHReg04 = 0x04;
constexpr uint8_t kEs7210LrckDivLReg05 = 0x05;
constexpr uint8_t kEs7210OsrReg07 = 0x07;
constexpr uint8_t kEs7210ModeConfigReg08 = 0x08;
constexpr uint8_t kEs7210TimeControl0Reg09 = 0x09;
constexpr uint8_t kEs7210TimeControl1Reg0A = 0x0A;
constexpr uint8_t kEs7210SdpInterface1Reg11 = 0x11;
constexpr uint8_t kEs7210SdpInterface2Reg12 = 0x12;
constexpr uint8_t kEs7210Adc34Hpf2Reg20 = 0x20;
constexpr uint8_t kEs7210Adc34Hpf1Reg21 = 0x21;
constexpr uint8_t kEs7210Adc12Hpf1Reg22 = 0x22;
constexpr uint8_t kEs7210Adc12Hpf2Reg23 = 0x23;
constexpr uint8_t kEs7210AnalogReg40 = 0x40;
constexpr uint8_t kEs7210Mic12BiasReg41 = 0x41;
constexpr uint8_t kEs7210Mic34BiasReg42 = 0x42;
constexpr uint8_t kEs7210Mic1GainReg43 = 0x43;
constexpr uint8_t kEs7210Mic2GainReg44 = 0x44;
constexpr uint8_t kEs7210Mic3GainReg45 = 0x45;
constexpr uint8_t kEs7210Mic4GainReg46 = 0x46;
constexpr uint8_t kEs7210Mic1PowerReg47 = 0x47;
constexpr uint8_t kEs7210Mic2PowerReg48 = 0x48;
constexpr uint8_t kEs7210Mic3PowerReg49 = 0x49;
constexpr uint8_t kEs7210Mic4PowerReg4A = 0x4A;
constexpr uint8_t kEs7210Mic12PowerReg4B = 0x4B;
constexpr uint8_t kEs7210Mic34PowerReg4C = 0x4C;
constexpr uint8_t kEs7210PowerDownReg06 = 0x06;

// 30 dB gain, encoded the same way as the reference driver's
// es7210_gain_value_t (raw enum value 10 = GAIN_30DB out of a 0-15 range —
// see es7210_reg.h's gain_value enum), OR'd with bit4 (0x10) to enable the
// PGA for that mic channel.
constexpr uint8_t kEs7210MicGainEnabled30db = 0x1A;

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
    // Slot width and frame format here must match how the ES7210 actually
    // drives its output, not how the ES8311 (playback codec) expects to be
    // fed — the two were wrongly assumed identical in every earlier attempt.
    // Waveshare's own reference firmware for this exact board
    // (waveshareteam/ESP32-S3-Touch-LCD-3.49, codec_board component) sets up
    // its ES7210 RX channel with 32-bit MSB-justified slots
    // (I2S_STD_MSB_SLOT_DEFAULT_CONFIG(32, STEREO) in their codec_init.c),
    // while we were reading 16-bit Philips-standard slots — two independent
    // mismatches (slot width AND the 1-BCLK launch-edge shift between MSB
    // and Philips format) that would scramble every sample boundary. That
    // matches the symptom exactly: a live but garbled/buzzing signal whose
    // loudness tracked speech, rather than true silence.  MSB format here
    // means the ES7210's 16 valid bits land at the top of each 32-bit slot
    // (see the >>16 extraction in recordTaskLoop() below), not that the ADC
    // itself runs at 32-bit resolution.
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
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

    i2s_set_clk(kI2sPort, kSampleRate, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
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
    // The microphone on this board does not run through the ES8311 at all.
    // Waveshare's own reference firmware for this exact board (board id
    // "S3_LCD_3_49" in waveshareteam/ESP32-S3-Touch-LCD-3.49's codec_board
    // component) shows two separate codecs sharing one I2S bus: ES8311 for
    // DAC/speaker output, and a second ES7210 ADC codec (I2C address
    // BoardConfig::ES7210_ADDRESS) for the mic. Every previous fix here
    // (warm-up write, SDOUT unmute, PGA gain 0x07/0xC8 — all correct *ES8311*
    // register values, cross-checked against Espressif's own es8311.c) left
    // Pzm at 0% because none of it could ever reach a live signal: the
    // ES8311's ADC path on this board has nothing wired to it. This function
    // now talks to the ES7210 instead, using the same register sequence as
    // Waveshare's shipped example (es7210.c's es7210_open()/es7210_start()),
    // restricted to a plain 2-channel (non-TDM) I2S handoff so it drops
    // straight into the existing 16-bit stereo RX path in
    // configureI2sForRecording() below with no I2S protocol changes needed.
    //
    // Which two of the ES7210's four MIC inputs this board's physical
    // dual-mic array is wired to was previously guessed as MIC1+MIC2. Pulled
    // the actual answer from Waveshare's own codec_init.c (same
    // waveshareteam/ESP32-S3-Touch-LCD-3.49 repo, ESP-IDF variant of this
    // exact board id "S3_LCD_3_49" — i2c/i2s pins there match
    // BoardConfig::PIN_AUDIO_* exactly, confirming it's the same hardware):
    // its default (non-TDM) es7210_codec_cfg_t.mic_selected is
    // ES7120_SEL_MIC1 | ES7120_SEL_MIC3, only widened to all four (TDM) mics
    // when the caller explicitly asks for TDM mode. MIC1+MIC3 stays under
    // the reference driver's TDM threshold (3 mics) exactly like MIC1+MIC2
    // did, so this is still a plain 2-channel handoff — no I2S protocol
    // change needed here, just which ES7210 gain/power registers get hit in
    // selectEs7210Mics() below.
    if (!writeEs7210Register(kEs7210ResetReg00, 0xFF)) return false;
    if (!writeEs7210Register(kEs7210ResetReg00, 0x41)) return false;
    if (!writeEs7210Register(kEs7210ClockOffReg01, 0x3F)) return false;
    if (!writeEs7210Register(kEs7210TimeControl0Reg09, 0x30)) return false;
    if (!writeEs7210Register(kEs7210TimeControl1Reg0A, 0x30)) return false;
    if (!writeEs7210Register(kEs7210Adc12Hpf2Reg23, 0x2A)) return false;
    if (!writeEs7210Register(kEs7210Adc12Hpf1Reg22, 0x0A)) return false;
    if (!writeEs7210Register(kEs7210Adc34Hpf2Reg20, 0x0A)) return false;
    if (!writeEs7210Register(kEs7210Adc34Hpf1Reg21, 0x2A)) return false;

    // Slave mode — the ESP32 I2S peripheral is the master (see
    // configureI2sForRecording()), same as the ES8311 side.
    if (!updateEs7210RegisterBits(kEs7210ModeConfigReg08, 0x01, 0x00)) return false;

    if (!writeEs7210Register(kEs7210AnalogReg40, 0x43)) return false;
    if (!writeEs7210Register(kEs7210Mic12BiasReg41, 0x70)) return false;
    if (!writeEs7210Register(kEs7210Mic34BiasReg42, 0x70)) return false;
    if (!writeEs7210Register(kEs7210OsrReg07, 0x20)) return false;
    if (!writeEs7210Register(kEs7210MainClkReg02, 0xC1)) return false;

    // LRCK divider — set by the reference driver's es7210_config_sample(),
    // looked up from a (MCLK, LRCK) coefficient table for the actual
    // 16 kHz/256x-MCLK case we run (4.096 MHz), never written anywhere
    // else. Without it the ADC's internal sample-rate divider stays at its
    // power-on-reset value instead of matching the 16 kHz the I2S RX side
    // expects, so every captured buffer is sampled at the wrong rate —
    // this is what turned into buzzing/distortion on playback even though
    // Pzm showed a plausible-looking (if quiet) live signal, since the
    // mismatch garbles the stream rather than silencing it.
    if (!writeEs7210Register(kEs7210LrckDivHReg04, 0x01)) return false;
    if (!writeEs7210Register(kEs7210LrckDivLReg05, 0x00)) return false;

    if (!selectEs7210Mics()) return false;

    // Sample format: normal (Philips) I2S, 16-bit.
    uint8_t reg = 0;
    if (!readEs7210Register(kEs7210SdpInterface1Reg11, reg)) return false;
    if (!writeEs7210Register(kEs7210SdpInterface1Reg11, static_cast<uint8_t>(reg & 0xFC))) return false;
    if (!readEs7210Register(kEs7210SdpInterface1Reg11, reg)) return false;
    if (!writeEs7210Register(kEs7210SdpInterface1Reg11, static_cast<uint8_t>((reg & 0x1F) | 0x60))) return false;

    // Power up (es7210_start(), called a second time by the reference driver
    // on top of the mic_select() already done above). The reference driver
    // passes in whatever CLOCK_OFF_REG01 happened to read back as right
    // after es7210_open()'s mic_select() call (codec->off_reg) rather than a
    // fixed constant — it depends on exactly which mic pair got selected
    // above (MIC1+MIC2 and MIC1+MIC3 leave different clock-domain bits
    // cleared), so read it back here too instead of hardcoding a value that
    // would silently go stale the next time the mic pairing changes.
    uint8_t clockOffAfterMicSelect = 0;
    if (!readEs7210Register(kEs7210ClockOffReg01, clockOffAfterMicSelect)) return false;
    if (!writeEs7210Register(kEs7210ClockOffReg01, clockOffAfterMicSelect)) return false;
    if (!writeEs7210Register(kEs7210PowerDownReg06, 0x00)) return false;
    if (!writeEs7210Register(kEs7210AnalogReg40, 0x43)) return false;
    if (!writeEs7210Register(kEs7210Mic1PowerReg47, 0x08)) return false;
    if (!writeEs7210Register(kEs7210Mic2PowerReg48, 0x08)) return false;
    if (!writeEs7210Register(kEs7210Mic3PowerReg49, 0x08)) return false;
    if (!writeEs7210Register(kEs7210Mic4PowerReg4A, 0x08)) return false;
    if (!selectEs7210Mics()) return false;
    if (!writeEs7210Register(kEs7210AnalogReg40, 0x43)) return false;
    if (!writeEs7210Register(kEs7210ResetReg00, 0x71)) return false;
    if (!writeEs7210Register(kEs7210ResetReg00, 0x41)) return false;

    // Read back the registers that actually gate signal flow, so the next
    // serial capture can tell "wrote correctly but MIC1/MIC3 aren't the
    // populated pair" apart from "an I2C write silently didn't stick" —
    // two previous fixes here turned out to be neither, and guessing a
    // third time blind isn't worth another round trip without this.
    uint8_t chk43 = 0, chk45 = 0, chk47 = 0, chk49 = 0, chk11 = 0, chk12 = 0;
    readEs7210Register(kEs7210Mic1GainReg43, chk43);
    readEs7210Register(kEs7210Mic3GainReg45, chk45);
    readEs7210Register(kEs7210Mic1PowerReg47, chk47);
    readEs7210Register(kEs7210Mic3PowerReg49, chk49);
    readEs7210Register(kEs7210SdpInterface1Reg11, chk11);
    readEs7210Register(kEs7210SdpInterface2Reg12, chk12);
    ESP_LOGI(TAG, "ES7210 readback: gain1=%02X gain3=%02X pwr1=%02X pwr3=%02X sdp1=%02X sdp2=%02X "
                  "(expect gain1/gain3=1A, pwr1/pwr3=08, sdp2=00)",
             chk43, chk45, chk47, chk49, chk11, chk12);

    ESP_LOGI(TAG, "Codec configured for recording (ES7210)");
    return true;
}

// Mirrors es7210_mic_select() from the reference driver, hardcoded to
// MIC1+MIC3 (the confirmed default pairing for this board — see
// configureCodecForRecording()).
bool AudioRecorder::selectEs7210Mics() {
    for (uint8_t reg = kEs7210Mic1GainReg43; reg <= kEs7210Mic4GainReg46; reg++) {
        if (!updateEs7210RegisterBits(reg, 0x10, 0x00)) return false;
    }
    if (!writeEs7210Register(kEs7210Mic12PowerReg4B, 0xFF)) return false;
    if (!writeEs7210Register(kEs7210Mic34PowerReg4C, 0xFF)) return false;

    // MIC1
    if (!updateEs7210RegisterBits(kEs7210ClockOffReg01, 0x0B, 0x00)) return false;
    if (!writeEs7210Register(kEs7210Mic12PowerReg4B, 0x00)) return false;
    if (!updateEs7210RegisterBits(kEs7210Mic1GainReg43, 0x1F, kEs7210MicGainEnabled30db)) return false;

    // MIC3 — clock-off mask 0x15 and MIC34_POWER_REG4C, not the MIC1/2
    // pair's 0x0B/REG4B (mirrors the reference driver's separate MIC3
    // branch in es7210_mic_select(), which uses different bits for the
    // ADC34 domain than the ADC12 one MIC1 lives in).
    if (!updateEs7210RegisterBits(kEs7210ClockOffReg01, 0x15, 0x00)) return false;
    if (!writeEs7210Register(kEs7210Mic34PowerReg4C, 0x00)) return false;
    if (!updateEs7210RegisterBits(kEs7210Mic3GainReg45, 0x1F, kEs7210MicGainEnabled30db)) return false;

    // 2 mics selected — stays below the reference driver's TDM threshold
    // (3+), so this is plain 2-channel (non-TDM) I2S output.
    return writeEs7210Register(kEs7210SdpInterface2Reg12, 0x00);
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

bool AudioRecorder::readEs7210Register(uint8_t reg, uint8_t& value) {
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(BoardConfig::ES7210_ADDRESS);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(static_cast<int>(BoardConfig::ES7210_ADDRESS), 1, 1) != 1) return false;
    value = Wire1.read();
    return true;
}

bool AudioRecorder::writeEs7210Register(uint8_t reg, uint8_t value) {
    BoardConfig::I2cBusLock lock;
    Wire1.beginTransmission(BoardConfig::ES7210_ADDRESS);
    Wire1.write(reg);
    Wire1.write(value);
    return Wire1.endTransmission(true) == 0;
}

bool AudioRecorder::updateEs7210RegisterBits(uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t value = 0;
    if (!readEs7210Register(reg, value)) return false;
    value = static_cast<uint8_t>((value & static_cast<uint8_t>(~mask)) | (mask & data));
    return writeEs7210Register(reg, value);
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

        // Convert stereo 32-bit (MSB-justified, ES7210's 16 valid bits at the
        // top of each slot — see configureI2sForRecording()) to mono 16-bit.
        // The ES7210 puts MIC1 on the left slot and MIC3 on the right (see
        // selectEs7210Mics()) — two distinct real microphones, not a
        // duplicated single signal — so averaging both is a genuine 2-mic
        // downmix, and still degrades gracefully (half amplitude, not
        // silence) if it turns out only one of the two is actually
        // populated on this board.
        size_t stereoSamples = bytesRead / 8;  // 4 bytes * 2 channels per sample
        const int32_t* stereoData = reinterpret_cast<const int32_t*>(buffer);
        int16_t peakSample = 0;
        int16_t leftMin = 0, leftMax = 0, rightMin = 0, rightMax = 0;
        for (size_t i = 0; i < stereoSamples; i++) {
            int32_t left = stereoData[i * 2] >> 16;
            int32_t right = stereoData[i * 2 + 1] >> 16;
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
