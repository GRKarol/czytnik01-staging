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
// Above the font/book/update downloads (priority 1, also on core 0): sharing
// time slices with them, a take answered Stop late and lost samples.
constexpr UBaseType_t kAudioTaskPriority = 5;

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

// 34.5 dB gain, encoded the same way as the reference driver's
// es7210_gain_value_t (raw enum value 12 = GAIN_34_5DB out of a 0-15 range —
// see es7210_reg.h's gain_value enum), OR'd with bit4 (0x10) to enable the
// PGA for that mic channel. Matches Waveshare's own Arduino example for this
// board (Examples/Arduino/08_Audio_Test/08_Audio_Test.ino), which requests
// esp_codec_dev_set_in_gain(record, 35.0) right after open — their
// es7210_open() default (codec->gain, applied inside mic_select()) is
// overridden by that explicit call before recording ever starts, so 35 dB
// (which get_db() buckets down to the nearest defined step, 34.5 dB) is the
// gain the reference actually runs at, not whatever mic_select() set first.
constexpr uint8_t kEs7210MicGainEnabled345db = 0x1C;

// SDP_INTERFACE2_REG12: plain 2-slot framing. es7210_mic_select() writes
// 0x02 here instead once three or more mics are selected; this board uses
// two, so it stays at the plain value.
constexpr uint8_t kEs7210SdpPlainFraming = 0x00;

// The ES7210 does not survive this board's normal Wire1 speed. Measured, not
// assumed: the round-2 diagnostic sweep wrote eight patterns to MIC1_GAIN at
// 300/200/100/50 kHz and read each one back. At 300, 200 and 100 kHz the
// readback is mangled in the same way every time (0xAA>0x00, 0xC1>0x01,
// 0x20>0x00). At 50 kHz every pattern comes back exactly as written once the
// register's own 5-bit width is accounted for (0xAA>0x0A, 0x55>0x15 — both
// are the value ANDed with 0x1F), so the bus is clean there and the chip is
// fine; only the timing above 50 kHz is not.
//
// This is what defeated the nine earlier attempts. MAINCLK_REG02 (0xC1) and
// OSR_REG07 (0x20) set the ADC's clock divider, and at 300 kHz they landed as
// 0x81 and 0x00 — a lost clock doubler (2x) and a lost OSR divider (8x). The
// codec therefore produced one sample per 16 LRCK frames while the I2S side
// clocked all 16, and the capture came out as short bursts of real audio
// separated by 14 zero samples. That periodic gap is the buzz. No amount of
// gain, mic pairing or slot format could fix it, because every one of those
// registers was being written over the same corrupting bus.
//
// Every read-modify-write in the ES7210 path is affected twice over (a bad
// read feeding a bad write), so the speed change wraps the register accessors
// themselves rather than the config function, and restores the bus afterwards
// for the touch controller, IMU, TCA9554 and ES8311 that share it.
constexpr uint32_t kEs7210I2cHz = 50000;
constexpr uint32_t kSharedBusI2cHz = 300000;  // BoardConfig::begin()'s Wire1 speed

// Restores the shared bus speed however the enclosing transaction exits.
struct Es7210BusSpeed {
    Es7210BusSpeed() { Wire1.setClock(kEs7210I2cHz); }
    ~Es7210BusSpeed() { Wire1.setClock(kSharedBusI2cHz); }
};

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
        recordTaskEntry, "rec", kRecordTaskStackSize, this, kAudioTaskPriority, &recordTask_, 0);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create record task");
        recording_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Recording started: %s", absolutePath);
    return true;
}

void AudioRecorder::requestStopRecording() {
    if (recording_) stopRequested_ = true;
}

// Never deletes the record task. A forced vTaskDelete() could land while the
// task held the card's FAT lock or the I2C bus lock, and both stayed taken for
// good: the next take, the plugin and then the whole reader hung on them.
// The task always ends by itself once stopRequested_ is set (i2s_read times
// out every 100 ms), so a slow stop just takes longer.
bool AudioRecorder::stopRecording() {
    if (!recording_) return false;

    requestStopRecording();

    const uint32_t waitStart = millis();
    while (recording_ && (millis() - waitStart) < 1500) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (recording_) {
        ESP_LOGW(TAG, "Recording still finishing after 1.5 s");
        return false;
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
        playbackTaskEntry, "play", kPlaybackTaskStackSize, this, kAudioTaskPriority, &playbackTask_, 0);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        playing_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Playback started: %s", absolutePath);
    return true;
}

// Like stopRecording(): no forced task delete, the task ends by itself.
bool AudioRecorder::stopPlayback() {
    if (!playing_) return false;

    stopRequested_ = true;

    const uint32_t waitStart = millis();
    while (playing_ && (millis() - waitStart) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (playing_) {
        ESP_LOGW(TAG, "Playback still finishing after 2 s");
        return false;
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
    // Plain 2-channel 16-bit I2S, not TDM. This is the configuration the
    // round-2 sweep's variant 4 used, and it is the only one that produced a
    // continuous signal: 2044 of 2048 frames live, longest silent run 1
    // sample, left channel a smooth waveform peaking at 2112. Every TDM
    // variant, and every non-TDM variant running at the uncorrected bus
    // speed, produced the 2-live-then-14-zero burst pattern instead.
    //
    // The TDM reasoning that used to sit here was sound in itself but rested
    // on a false premise: the codec's own framing register never held the
    // value we wrote, because the write went over a corrupting bus (see
    // kEs7210I2cHz). With the bus fixed, the reference driver's non-TDM
    // branch — 2 mics selected, SDP_INTERFACE2_REG12 = 0x00 — matches what
    // the hardware actually does, so there is nothing to push into TDM.
    //
    // Left slot carries MIC1, right slot is dead (peak 2 across a full
    // capture — MIC3 is not populated on this board), which recordTaskLoop()
    // relies on when it takes the left slot alone instead of mixing.
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    // 256 ms of audio: a card write held up by FAT allocation no longer
    // overruns the ring (8 buffers held 128 ms).
    config.dma_buf_count = 16;
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
    // selectEs7210Mics() below. The capture confirms it: with MIC1+MIC3
    // selected the left slot carries a full-rate waveform and the right slot
    // stays at peak 2, so MIC1 is the one populated input.
    //
    // Every write below now goes out at kEs7210I2cHz — before that change the
    // two clock registers (OSR_REG07, MAINCLK_REG02) never actually held the
    // values written here, which is what produced the burst-and-gap capture.
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

    // No LRCK divider write here — pulled the actual reference driver
    // (audio_codec_es7210_t from waveshareteam/ESP32-S3-Touch-LCD-3.49's
    // esp_codec_dev/device/es7210/es7210.c) and found es7210_config_sample()
    // — the only place that ever touches LRCK_DIVH_REG04/LRCK_DIVL_REG05 —
    // returns immediately without writing anything when codec->master_mode
    // is false. That board's codec_init.c never sets master_mode for the
    // ES7210 (the ESP32 I2S peripheral is master, feeding BCLK/WS/MCLK to
    // the codec — same as our configureI2sForRecording()), so master_mode
    // is false there too: the real firmware never writes these two
    // registers. A previous fix here wrote them anyway (0x01/0x00), which
    // was never part of the working reference sequence and is the likely
    // source of the buzzing/distortion that appeared once the mic signal
    // path started carrying real signal (Pzm jumping from ~1-9% to ~100%).
    // OSR_REG07 and MAINCLK_REG02 above stay — those two ARE written
    // unconditionally by es7210_open() regardless of master/slave mode.

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
    if (!writeEs7210Register(kEs7210SdpInterface2Reg12, kEs7210SdpPlainFraming)) return false;
    if (!writeEs7210Register(kEs7210AnalogReg40, 0x43)) return false;
    if (!writeEs7210Register(kEs7210ResetReg00, 0x71)) return false;
    if (!writeEs7210Register(kEs7210ResetReg00, 0x41)) return false;

    // The two clock registers again, after the RESET_REG00 pair that ends
    // es7210_start(). The reference driver leaves them alone here, but the
    // diagnostic build wrote them last and read back 0xC1/0x20 intact, so
    // this is the ordering the working capture was taken with. Cheap
    // insurance against the reset sequence disturbing the divider.
    if (!writeEs7210Register(kEs7210OsrReg07, 0x20)) return false;
    if (!writeEs7210Register(kEs7210MainClkReg02, 0xC1)) return false;

    // Read back the registers that actually gate signal flow. The clock pair
    // is the important one now: gain and mic selection were never really in
    // doubt, but 0x02/0x07 reading back as anything other than C1/20 means
    // the bus is corrupting writes again and the recording will buzz.
    uint8_t chk43 = 0, chk44 = 0, chk45 = 0, chk46 = 0, chk47 = 0, chk49 = 0, chk11 = 0, chk12 = 0;
    uint8_t chk02 = 0, chk07 = 0;
    readEs7210Register(kEs7210Mic1GainReg43, chk43);
    readEs7210Register(kEs7210Mic2GainReg44, chk44);
    readEs7210Register(kEs7210Mic3GainReg45, chk45);
    readEs7210Register(kEs7210Mic4GainReg46, chk46);
    readEs7210Register(kEs7210Mic1PowerReg47, chk47);
    readEs7210Register(kEs7210Mic3PowerReg49, chk49);
    readEs7210Register(kEs7210SdpInterface1Reg11, chk11);
    readEs7210Register(kEs7210SdpInterface2Reg12, chk12);
    readEs7210Register(kEs7210MainClkReg02, chk02);
    readEs7210Register(kEs7210OsrReg07, chk07);
    if (chk02 != 0xC1 || chk07 != 0x20) {
        ESP_LOGE(TAG, "ES7210 clock registers did not stick (02=%02X 07=%02X) — "
                      "recording will come out as bursts separated by silence",
                 chk02, chk07);
    }
    ESP_LOGI(TAG, "ES7210 readback: gain1=%02X gain2=%02X gain3=%02X gain4=%02X pwr1=%02X pwr3=%02X "
                  "sdp1=%02X sdp2=%02X clk02=%02X osr07=%02X (expect gain1/gain3=1C, "
                  "pwr1/pwr3=08, sdp1=60, sdp2=00, clk02=C1, osr07=20)",
             chk43, chk44, chk45, chk46, chk47, chk49, chk11, chk12, chk02, chk07);

    ESP_LOGI(TAG, "Codec configured for recording (ES7210)");
    return true;
}

// Mirrors es7210_mic_select() from the real Waveshare reference driver
// (device/es7210/es7210.c, fetched from waveshareteam/ESP32-S3-Touch-LCD-3.49
// and confirmed to run on this exact board), taking its non-TDM branch:
// MIC1 + MIC3 selected, two mics, which is below the driver's own TDM
// threshold of three and so leaves SDP_INTERFACE2_REG12 at plain framing.
//
// An earlier version here selected all four mics to force the codec into
// 4-slot TDM and match a TDM receiver. That was chasing a framing mismatch
// that did not exist — the capture never changed shape across TDM and
// non-TDM alike, because the codec's sample rate was wrong underneath both
// (see kEs7210I2cHz). With the clock right, two mics and plain framing is
// what the reference driver does for this board and what the working
// capture used.
bool AudioRecorder::selectEs7210Mics() {
    for (uint8_t reg = kEs7210Mic1GainReg43; reg <= kEs7210Mic4GainReg46; reg++) {
        if (!updateEs7210RegisterBits(reg, 0x10, 0x00)) return false;
    }
    if (!writeEs7210Register(kEs7210Mic12PowerReg4B, 0xFF)) return false;
    if (!writeEs7210Register(kEs7210Mic34PowerReg4C, 0xFF)) return false;

    // MIC1 lives in the ADC12 clock domain (clock-off mask 0x0B, power
    // register REG4B); only MIC1's own gain register gets the PGA enabled,
    // since MIC2 is not selected.
    if (!updateEs7210RegisterBits(kEs7210ClockOffReg01, 0x0B, 0x00)) return false;
    if (!writeEs7210Register(kEs7210Mic12PowerReg4B, 0x00)) return false;
    if (!updateEs7210RegisterBits(kEs7210Mic1GainReg43, 0x1F, kEs7210MicGainEnabled345db)) return false;

    // MIC3 lives in the ADC34 domain (mask 0x15, register REG4C). It reads
    // as silence on this board — a full capture peaked at 2 against MIC1's
    // 2112 — but the reference driver selects it, and an unpopulated input
    // costs nothing beyond the right I2S slot it already occupies.
    if (!updateEs7210RegisterBits(kEs7210ClockOffReg01, 0x15, 0x00)) return false;
    if (!writeEs7210Register(kEs7210Mic34PowerReg4C, 0x00)) return false;
    if (!updateEs7210RegisterBits(kEs7210Mic3GainReg45, 0x1F, kEs7210MicGainEnabled345db)) return false;

    // Two mics selected — below es7210_is_tdm_mode()'s threshold, so plain
    // 2-slot framing, matching configureI2sForRecording()'s stereo RX.
    return writeEs7210Register(kEs7210SdpInterface2Reg12, kEs7210SdpPlainFraming);
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
    Es7210BusSpeed slowBus;
    Wire1.beginTransmission(BoardConfig::ES7210_ADDRESS);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(static_cast<int>(BoardConfig::ES7210_ADDRESS), 1, 1) != 1) return false;
    value = Wire1.read();
    return true;
}

bool AudioRecorder::writeEs7210Register(uint8_t reg, uint8_t value) {
    BoardConfig::I2cBusLock lock;
    Es7210BusSpeed slowBus;
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
#if AUDIO_DIAG
    // Diagnostic firmware: pressing Record runs the config sweep instead of
    // recording. See AudioRecorderDiag.cpp.
    runDiagnostics();
    return;
#endif

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
    // 2 channels x 2 bytes per frame, one mono sample out per frame.
    int16_t monoBuffer[kRecordBufferSize / 4];
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

        // Plain 2-channel 16-bit RX (see configureI2sForRecording()): left
        // word is MIC1, right word is MIC3. Only MIC1 is populated on this
        // board — a full capture put MIC3's peak at 2 against MIC1's 2112 —
        // so the mono output takes the left word alone. Averaging the two,
        // as this used to, would have halved every sample against a channel
        // that carries nothing.
        size_t frameCount = bytesRead / 4;  // 2 bytes * 2 channels per frame
        const int16_t* frameData = reinterpret_cast<const int16_t*>(buffer);
        int16_t peakSample = 0;
        int16_t leftMin = 0, leftMax = 0, rightMin = 0, rightMax = 0;
        for (size_t i = 0; i < frameCount; i++) {
            int16_t left = frameData[i * 2 + 0];       // MIC1
            int16_t right = frameData[i * 2 + 1];      // MIC3, unpopulated
            if (i == 0) {
                leftMin = leftMax = left;
                rightMin = rightMax = right;
            } else {
                leftMin = std::min(leftMin, left);
                leftMax = std::max(leftMax, left);
                rightMin = std::min(rightMin, right);
                rightMax = std::max(rightMax, right);
            }
            monoBuffer[i] = left;
            int16_t absLeft = static_cast<int16_t>(left < 0 ? -left : left);
            if (absLeft > peakSample) peakSample = absLeft;
        }
        recordingPeakLevel_ = static_cast<uint8_t>((static_cast<uint32_t>(peakSample) * 100U) / 32767U);

        loopCount++;
        if (loopCount <= 3 || loopCount % 20 == 0) {
            ESP_LOGI(TAG, "buf#%u bytesRead=%u L[min=%d max=%d] R[min=%d max=%d] first16=%02X %02X %02X %02X %02X %02X %02X %02X",
                     static_cast<unsigned>(loopCount), static_cast<unsigned>(bytesRead),
                     leftMin, leftMax, rightMin, rightMax,
                     buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
        }

        size_t monoBytes = frameCount * 2;
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
