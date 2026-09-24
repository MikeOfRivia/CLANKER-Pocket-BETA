#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "axp2101.h"
#include "beta_network.h"
#include "board_es8311_codec.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "epaper_panel.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_allocator.h"

namespace {

constexpr const char* kTag = "ClankerBeta";

constexpr gpio_num_t kButtonBoot = GPIO_NUM_0;
constexpr gpio_num_t kButtonUp = GPIO_NUM_4;
constexpr gpio_num_t kButtonSelect = GPIO_NUM_5;
constexpr gpio_num_t kButtonDown = GPIO_NUM_6;

constexpr gpio_num_t kEpdBusy = GPIO_NUM_3;
constexpr gpio_num_t kEpdReset = GPIO_NUM_46;
constexpr gpio_num_t kEpdDc = GPIO_NUM_9;
constexpr gpio_num_t kEpdCs = GPIO_NUM_10;
constexpr gpio_num_t kEpdMosi = GPIO_NUM_12;
constexpr gpio_num_t kEpdSck = GPIO_NUM_11;

constexpr gpio_num_t kI2cSda = GPIO_NUM_41;
constexpr gpio_num_t kI2cScl = GPIO_NUM_42;
constexpr uint8_t kAxp2101Address = 0x34;

constexpr int kAudioSampleRate = 16000;
constexpr gpio_num_t kAudioMclk = GPIO_NUM_13;
constexpr gpio_num_t kAudioBclk = GPIO_NUM_14;
constexpr gpio_num_t kAudioWs = GPIO_NUM_47;
constexpr gpio_num_t kAudioDin = GPIO_NUM_21;
constexpr gpio_num_t kAudioDout = GPIO_NUM_48;
constexpr gpio_num_t kAudioPa = GPIO_NUM_39;
constexpr uint8_t kEs8311Address = ES8311_CODEC_DEFAULT_ADDR;
constexpr size_t kAudioReadSamples = 320;       // 20 ms at 16 kHz.
constexpr size_t kMaxClipSamples = 160000;      // 10 seconds at 16 kHz.

constexpr int kRawWidth = 800;
constexpr int kRawHeight = 480;
constexpr int kPortraitWidth = 480;
constexpr int kPortraitHeight = 800;
constexpr int kFramebufferBytes = (kRawWidth * kRawHeight) / 8;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
std::unique_ptr<Axp2101> s_pmic;
std::unique_ptr<EpaperPanel> s_panel;
std::unique_ptr<Es8311Codec> s_codec;
PsramVector<int16_t> s_clip;
bool s_recording = false;
int64_t s_recording_started_us = 0;

struct CaptureStats {
    uint32_t samples = 0;
    uint32_t duration_ms = 0;
    int peak = 0;
    int rms = 0;
    bool clipped = false;
};

CaptureStats s_last_capture = {};

struct Glyph {
    char ch;
    std::array<uint8_t, 7> rows;
};

constexpr Glyph kFont[] = {
    {' ', {0,0,0,0,0,0,0}}, {'-', {0,0,0,31,0,0,0}}, {':', {0,4,0,0,4,0,0}},
    {'.', {0,0,0,0,0,12,12}},
    {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}}, {'3', {30,1,1,14,1,1,30}},
    {'4', {2,6,10,18,31,2,2}}, {'5', {31,16,16,30,1,1,30}},
    {'6', {14,16,16,30,17,17,14}}, {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}}, {'9', {14,17,17,15,1,1,14}},
    {'A', {14,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}},
    {'C', {14,17,16,16,16,17,14}}, {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}},
    {'G', {14,17,16,23,17,17,15}}, {'H', {17,17,17,31,17,17,17}},
    {'I', {14,4,4,4,4,4,14}}, {'J', {7,2,2,2,2,18,12}},
    {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}}, {'N', {17,25,21,19,17,17,17}},
    {'O', {14,17,17,17,17,17,14}}, {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}}, {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}}, {'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}}, {'Z', {31,1,2,4,8,16,31}},
};

const Glyph* FindGlyph(char ch)
{
    for (const auto& glyph : kFont) {
        if (glyph.ch == ch) return &glyph;
    }
    return &kFont[0];
}

void RawPixel(uint8_t* fb, int x, int y, bool black)
{
    if (!fb || x < 0 || y < 0 || x >= kRawWidth || y >= kRawHeight) return;
    const size_t index = static_cast<size_t>(y) * (kRawWidth / 8) + static_cast<size_t>(x / 8);
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (black) fb[index] &= static_cast<uint8_t>(~mask);
    else fb[index] |= mask;
}

void Pixel(uint8_t* fb, int x, int y, bool black = true)
{
    if (x < 0 || y < 0 || x >= kPortraitWidth || y >= kPortraitHeight) return;
    RawPixel(fb, y, kRawHeight - 1 - x, black);
}

void FillRect(uint8_t* fb, int x, int y, int w, int h, bool black = true)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            Pixel(fb, xx, yy, black);
}

void DrawText(uint8_t* fb, int x, int y, const char* text, int scale)
{
    int cursor = x;
    for (const char* p = text; *p; ++p) {
        const Glyph* g = FindGlyph(*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g->rows[row] & (1u << (4 - col))) {
                    FillRect(fb, cursor + col * scale, y + row * scale, scale, scale, true);
                }
            }
        }
        cursor += 6 * scale;
    }
}

EpaperPanelConfig PanelConfig()
{
    EpaperPanelConfig c = {};
    c.spi_host = SPI3_HOST;
    c.cs = kEpdCs; c.dc = kEpdDc; c.rst = kEpdReset; c.busy = kEpdBusy;
    c.mosi = kEpdMosi; c.miso = GPIO_NUM_NC; c.sck = kEpdSck;
    c.external_spi_bus = false;
    c.buffer_len = kFramebufferBytes;
    c.busy_timeout_ms = 10000;
    c.reset_low_ms = 2; c.reset_high_ms = 50; c.busy_level = 1;
    return c;
}

esp_err_t InitPower()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_1;
    bus_cfg.sda_io_num = kI2cSda;
    bus_cfg.scl_io_num = kI2cScl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), kTag, "I2C init");
    s_pmic = std::make_unique<Axp2101>(s_i2c_bus, kAxp2101Address, GPIO_NUM_38);
    s_pmic->setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    s_pmic->setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_900MA);
    s_pmic->setSysPowerDownVoltage(2800);
    s_pmic->enableDC1(); s_pmic->setDC1Voltage(3300);
    s_pmic->enableALDO1(); s_pmic->setALDO1Voltage(3300);
    s_pmic->enableALDO2(); s_pmic->setALDO2Voltage(3300);
    s_pmic->enableALDO3(); s_pmic->setALDO3Voltage(3300);
    ESP_LOGI(kTag, "AXP2101 rails enabled");
    return ESP_OK;
}

esp_err_t InitButtons()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << kButtonBoot) | (1ULL << kButtonUp) |
                       (1ULL << kButtonSelect) | (1ULL << kButtonDown);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&cfg);
}

esp_err_t InitAudio()
{
    if (s_i2c_bus == nullptr) return ESP_ERR_INVALID_STATE;
    s_codec = std::make_unique<Es8311Codec>(
        s_i2c_bus, I2C_NUM_1, kAudioSampleRate, kAudioSampleRate,
        kAudioMclk, kAudioBclk, kAudioWs, kAudioDout, kAudioDin, kAudioPa,
        kEs8311Address);
    if (!s_codec) return ESP_ERR_NO_MEM;
    s_codec->SetInputGain(30.0f);
    s_clip.reserve(kMaxClipSamples);
    ESP_LOGI(kTag, "ES8311 microphone ready at %d Hz", kAudioSampleRate);
    return ESP_OK;
}

const char* ButtonName(int index)
{
    switch (index) {
        case 0: return "BOOT";
        case 1: return "UP";
        case 2: return "SELECT";
        case 3: return "DOWN";
        default: return "NONE";
    }
}

void DrawFrame(uint8_t* fb)
{
    FillRect(fb, 20, 20, kPortraitWidth - 40, 8);
    FillRect(fb, 20, kPortraitHeight - 28, kPortraitWidth - 40, 8);
    FillRect(fb, 20, 20, 8, kPortraitHeight - 40);
    FillRect(fb, kPortraitWidth - 28, 20, 8, kPortraitHeight - 40);
    DrawText(fb, 48, 70, "CLANKER", 6);
    DrawText(fb, 78, 135, "POCKET BETA", 4);
}

void RenderIdle(const char* last_button, uint32_t press_count)
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);
    DrawText(fb, 105, 205, "PHASE 3", 4);

    const beta_network::Snapshot net = beta_network::GetSnapshot();
    if (net.mode == beta_network::Mode::kProvisioning) {
        DrawText(fb, 55, 275, "WIFI PROVISIONING", 3);
        DrawText(fb, 45, 325, net.ap_name.c_str(), 3);
        DrawText(fb, 65, 375, "OPEN 192.168.4.1", 2);
    } else if (net.mode == beta_network::Mode::kConnected) {
        DrawText(fb, 70, 275, "WIFI CONNECTED", 3);
        char http[32] = {};
        if (net.http_status > 0) {
            std::snprintf(http, sizeof(http), "HTTPS STATUS: %d", net.http_status);
        } else if (!net.probe_error.empty()) {
            std::snprintf(http, sizeof(http), "HTTPS ERROR");
        } else {
            std::snprintf(http, sizeof(http), "HTTPS NOT TESTED");
        }
        DrawText(fb, 48, 325, http, 2);
    } else {
        DrawText(fb, 85, 275, "WIFI OFFLINE", 3);
    }

    DrawText(fb, 65, 430, "LAST BUTTON:", 3);
    DrawText(fb, 95, 480, last_button, 4);

    char count[16] = {};
    std::snprintf(count, sizeof(count), "%lu", static_cast<unsigned long>(press_count));
    DrawText(fb, 110, 550, "PRESS COUNT:", 3);
    DrawText(fb, 200, 600, count, 4);
    DrawText(fb, 50, 675, "BOOT RECORDS", 2);
    DrawText(fb, 50, 710, "SELECT RETESTS HTTPS", 2);
}

void RenderRecording()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);
    DrawText(fb, 92, 270, "RECORDING", 5);
    DrawText(fb, 74, 360, "RELEASE BOOT TO STOP", 2);
}

void RenderCaptureStats(const CaptureStats& stats)
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);
    DrawText(fb, 92, 210, "MIC CAPTURE OK", 4);

    char line[40] = {};
    std::snprintf(line, sizeof(line), "SAMPLES: %lu", static_cast<unsigned long>(stats.samples));
    DrawText(fb, 58, 315, line, 3);
    std::snprintf(line, sizeof(line), "DURATION: %lu MS", static_cast<unsigned long>(stats.duration_ms));
    DrawText(fb, 58, 365, line, 3);
    std::snprintf(line, sizeof(line), "PEAK: %d", stats.peak);
    DrawText(fb, 58, 415, line, 3);
    std::snprintf(line, sizeof(line), "RMS: %d", stats.rms);
    DrawText(fb, 58, 465, line, 3);
    DrawText(fb, 58, 535, stats.clipped ? "BUFFER LIMIT HIT" : "RAW PCM STORED", 3);
    DrawText(fb, 55, 680, "BOOT = RECORD AGAIN", 2);
}

void StartCapture()
{
    s_clip.clear();
    s_last_capture = {};
    s_recording = true;
    s_recording_started_us = esp_timer_get_time();
    s_codec->EnableInput(true);
    RenderRecording();
    (void)s_panel->RefreshFastBase();
    ESP_LOGI(kTag, "Capture started");
}

void PumpCapture()
{
    if (!s_recording) return;
    std::vector<int16_t> chunk(kAudioReadSamples);
    const int read = s_codec->ReadInputSamples(chunk);
    if (read <= 0) return;

    const size_t room = kMaxClipSamples - s_clip.size();
    const size_t keep = std::min(room, static_cast<size_t>(read));
    s_clip.insert(s_clip.end(), chunk.begin(), chunk.begin() + keep);
    if (keep < static_cast<size_t>(read) || s_clip.size() >= kMaxClipSamples) {
        s_last_capture.clipped = true;
    }
}

void FinishCapture()
{
    if (!s_recording) return;
    s_recording = false;
    s_codec->EnableInput(false);

    uint64_t sum_sq = 0;
    int peak = 0;
    for (int16_t sample : s_clip) {
        const int value = sample == INT16_MIN ? 32768 : std::abs(static_cast<int>(sample));
        peak = std::max(peak, value);
        sum_sq += static_cast<uint64_t>(value) * static_cast<uint64_t>(value);
    }

    s_last_capture.samples = static_cast<uint32_t>(s_clip.size());
    s_last_capture.duration_ms =
        static_cast<uint32_t>((s_clip.size() * 1000ULL) / kAudioSampleRate);
    s_last_capture.peak = peak;
    s_last_capture.rms = s_clip.empty()
        ? 0
        : static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / s_clip.size()));

    const uint32_t wall_ms =
        static_cast<uint32_t>((esp_timer_get_time() - s_recording_started_us) / 1000ULL);
    ESP_LOGI(kTag,
             "Capture finished: samples=%lu pcm_ms=%lu wall_ms=%lu peak=%d rms=%d clipped=%d",
             static_cast<unsigned long>(s_last_capture.samples),
             static_cast<unsigned long>(s_last_capture.duration_ms),
             static_cast<unsigned long>(wall_ms),
             s_last_capture.peak, s_last_capture.rms, s_last_capture.clipped ? 1 : 0);

    RenderCaptureStats(s_last_capture);
    (void)s_panel->RefreshFastBase();
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "CLANKER Pocket BETA Phase 2 boot");

    if (InitPower() != ESP_OK || InitButtons() != ESP_OK) {
        ESP_LOGE(kTag, "Core hardware init failed");
        return;
    }

    s_panel = std::make_unique<EpaperPanel>(kRawWidth, kRawHeight, PanelConfig());
    if (s_panel->Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "Display init failed");
        return;
    }

    if (InitAudio() != ESP_OK) {
        ESP_LOGE(kTag, "Audio init failed");
        return;
    }

    const esp_err_t network_err = beta_network::Init();
    if (network_err != ESP_OK) {
        ESP_LOGW(kTag, "Network init returned: %s", esp_err_to_name(network_err));
    }
    beta_network::RunHttpsProbe();

    uint32_t press_count = 0;
    RenderIdle("NONE", press_count);
    if (s_panel->RefreshFullBase() != ESP_OK) {
        ESP_LOGE(kTag, "Initial display refresh failed");
        return;
    }

    constexpr std::array<gpio_num_t, 4> pins = {
        kButtonBoot, kButtonUp, kButtonSelect, kButtonDown
    };
    std::array<int, 4> last = {1,1,1,1};

    while (true) {
        const int boot_now = gpio_get_level(kButtonBoot);
        if (last[0] == 1 && boot_now == 0) {
            ++press_count;
            StartCapture();
        }

        if (s_recording && boot_now == 0) {
            PumpCapture();
        }

        if (last[0] == 0 && boot_now == 1) {
            FinishCapture();
        }
        last[0] = boot_now;

        if (!s_recording) {
            for (size_t i = 1; i < pins.size(); ++i) {
                const int now = gpio_get_level(pins[i]);
                if (last[i] == 1 && now == 0) {
                    ++press_count;
                    ESP_LOGI(kTag, "Button %s pressed (%lu)", ButtonName(static_cast<int>(i)),
                             static_cast<unsigned long>(press_count));
                    if (pins[i] == kButtonSelect) {
                        beta_network::RunHttpsProbe();
                    }
                    RenderIdle(ButtonName(static_cast<int>(i)), press_count);
                    (void)s_panel->RefreshFastBase();
                }
                last[i] = now;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
