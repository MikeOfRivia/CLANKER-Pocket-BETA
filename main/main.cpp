#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "axp2101.h"
#include "beta_network.h"
#include "beta_reader.h"
#include "beta_clanker.h"
#include "beta_transcription.h"
#include "board_es8311_codec.h"
#include "clanker_brand_assets.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "epaper_panel.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psram_allocator.h"

namespace {

constexpr const char* kTag = "ClankerBeta";

extern const uint8_t kLogoStart[] asm("_binary_clanker_pocket_logo_rle_start");
extern const uint8_t kLogoEnd[] asm("_binary_clanker_pocket_logo_rle_end");

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
bool s_status_screen = false;
bool s_power_menu = false;
std::atomic<bool> s_power_short_pending{false};
std::atomic<bool> s_power_long_pending{false};
int64_t s_recording_started_us = 0;

enum class UiMode : uint8_t { kChat = 0, kRead = 1 };
enum class MicState : uint8_t { kIdle, kRecording, kProcessing };
enum class UiMenu : uint8_t { kNone, kChat, kClearConfirm, kReader };

struct ChatMessage {
    bool user = false;
    std::string text;
};

UiMode s_ui_mode = UiMode::kChat;
MicState s_mic_state = MicState::kIdle;
UiMenu s_ui_menu = UiMenu::kNone;
std::vector<ChatMessage> s_chat_messages;
std::string s_previous_response_id;
int s_chat_scroll_offset = 0;
int s_menu_index = 0;
bool s_reader_in_book = false;
int s_reader_page = 0;
int s_reader_library_index = 0;
int s_reader_page_turns = 0;
bool s_swap_notice = false;
bool s_settings_open = false;
bool s_settings_notice = false;
int s_settings_index = 0;

constexpr int64_t kModeSwapNoticeUs = 1500000;
constexpr int64_t kModeSwapCommitUs = 3000000;
constexpr int64_t kSettingsNoticeUs = 1500000;
constexpr int64_t kSettingsCommitUs = 3000000;
constexpr int64_t kBootPttGraceUs = 250000;
constexpr int kReaderFullRefreshEveryPages = 10;
constexpr int kChatVisibleMessages = 4;

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
    {'.', {0,0,0,0,0,12,12}}, {',', {0,0,0,0,4,4,8}},
    {'!', {4,4,4,4,4,0,4}}, {'?', {14,17,1,2,4,0,4}},
    {'\'', {4,4,2,0,0,0,0}}, {'"', {10,10,10,0,0,0,0}},
    {'(', {2,4,8,8,8,4,2}}, {')', {8,4,2,2,2,4,8}},
    {'/', {1,2,4,8,16,0,0}},
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

void FillCircle(uint8_t* fb, int cx, int cy, int radius, bool black = true)
{
    const int r2 = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= r2) {
                Pixel(fb, cx + x, cy + y, black);
            }
        }
    }
}

void DrawCircle(uint8_t* fb, int cx, int cy, int radius, int thickness = 2,
                bool black = true)
{
    const int outer2 = radius * radius;
    const int inner = std::max(0, radius - thickness);
    const int inner2 = inner * inner;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const int d2 = x * x + y * y;
            if (d2 <= outer2 && d2 >= inner2) {
                Pixel(fb, cx + x, cy + y, black);
            }
        }
    }
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

#pragma pack(push, 1)
struct Cpr1Header {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint32_t run_count;
};

struct Cpr1Run {
    uint16_t y;
    uint16_t x;
    uint16_t length;
};
#pragma pack(pop)

bool DrawEmbeddedLogo(uint8_t* fb, int x, int y)
{
    const size_t bytes = static_cast<size_t>(kLogoEnd - kLogoStart);
    if (bytes < sizeof(Cpr1Header)) return false;

    const auto* header = reinterpret_cast<const Cpr1Header*>(kLogoStart);
    if (std::memcmp(header->magic, "CPR1", 4) != 0) return false;

    const size_t required =
        sizeof(Cpr1Header) + static_cast<size_t>(header->run_count) * sizeof(Cpr1Run);
    if (required > bytes) return false;

    const auto* runs =
        reinterpret_cast<const Cpr1Run*>(kLogoStart + sizeof(Cpr1Header));
    for (uint32_t i = 0; i < header->run_count; ++i) {
        const Cpr1Run& run = runs[i];
        FillRect(fb, x + run.x, y + run.y, run.length, 1, true);
    }
    return true;
}

void DrawBrandRuns(uint8_t* fb, int x, int y,
                   const BrandRun* runs, size_t run_count)
{
    if (!fb || !runs) return;
    for (size_t i = 0; i < run_count; ++i) {
        FillRect(fb, x + runs[i].x, y + runs[i].y, runs[i].len, 1, true);
    }
}

void DrawClankerHead(uint8_t* fb, int x, int y)
{
    DrawBrandRuns(fb, x, y, kClankerHeadRuns,
                  sizeof(kClankerHeadRuns) / sizeof(kClankerHeadRuns[0]));
}

void DrawClankerWordmark(uint8_t* fb, int x, int y)
{
    DrawBrandRuns(fb, x, y, kClankerWordmarkRuns,
                  sizeof(kClankerWordmarkRuns) / sizeof(kClankerWordmarkRuns[0]));
}

void DrawDivider(uint8_t* fb, int y)
{
    FillRect(fb, 42, y, kPortraitWidth - 84, 3, true);
}

void RenderSplash()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    DrawClankerHead(fb, (kPortraitWidth - kClankerHeadWidth) / 2, 105);
    DrawClankerWordmark(fb, (kPortraitWidth - kClankerWordmarkWidth) / 2, 270);

    DrawDivider(fb, 355);
    DrawText(fb, 177, 405, "BETA", 4);
    DrawText(fb, 102, 480, "POWERING UP!", 3);
}

void RenderHome()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    DrawText(fb, 54, 70, "CLANKER POCKET", 4);
    DrawDivider(fb, 125);

    const beta_network::Snapshot net = beta_network::GetSnapshot();
    if (net.mode == beta_network::Mode::kProvisioning) {
        DrawText(fb, 75, 210, "SETUP REQUIRED", 4);
        DrawText(fb, 58, 315, "JOIN:", 2);
        DrawText(fb, 58, 355, net.ap_name.c_str(), 2);
        DrawText(fb, 58, 420, "OPEN 192.168.4.1", 2);
        DrawDivider(fb, 535);
        DrawText(fb, 80, 590, "ENTER WIFI + API KEY", 2);
    } else {
        DrawText(fb, 142, 205, "READY", 5);
        DrawText(fb, 64, 315, "HOLD BOOT TO TALK", 3);

        const bool wifi_ok = net.mode == beta_network::Mode::kConnected;
        const bool api_ok = beta_transcription::HasApiKey();
        DrawText(fb, 78, 420, wifi_ok ? "WIFI  ONLINE" : "WIFI  OFFLINE", 2);
        DrawText(fb, 78, 465, api_ok ? "OPENAI READY" : "OPENAI MISSING", 2);

        DrawDivider(fb, 555);
        DrawText(fb, 102, 610, "SELECT  STATUS", 2);
        DrawText(fb, 111, 665, "BOOT  TALK", 2);
    }

    DrawText(fb, 155, 735, "POLISH A", 1);
}

void RenderStatus()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    DrawText(fb, 72, 70, "SYSTEM STATUS", 4);
    DrawDivider(fb, 125);

    const beta_network::Snapshot net = beta_network::GetSnapshot();
    const char* wifi =
        net.mode == beta_network::Mode::kConnected ? "CONNECTED" :
        net.mode == beta_network::Mode::kProvisioning ? "PROVISIONING" : "OFFLINE";

    DrawText(fb, 55, 180, "WIFI:", 2);
    DrawText(fb, 190, 180, wifi, 2);

    char https[32] = {};
    if (net.http_status > 0) {
        std::snprintf(https, sizeof(https), "%d", net.http_status);
    } else if (!net.probe_error.empty()) {
        std::snprintf(https, sizeof(https), "ERROR");
    } else {
        std::snprintf(https, sizeof(https), "NOT TESTED");
    }
    DrawText(fb, 55, 235, "HTTPS:", 2);
    DrawText(fb, 190, 235, https, 2);

    DrawText(fb, 55, 290, "OPENAI:", 2);
    DrawText(fb, 190, 290,
             beta_transcription::HasApiKey() ? "READY" : "MISSING", 2);

    DrawText(fb, 55, 345, "BATTERY:", 2);
    char battery[32] = {};
    if (s_pmic && s_pmic->isBatteryConnect()) {
        const int level = s_pmic->GetBatteryLevel();
        if (level >= 0) {
            std::snprintf(battery, sizeof(battery), "%d%% %s", level,
                          s_pmic->IsCharging() ? "CHARGING" : "");
        } else {
            std::snprintf(battery, sizeof(battery), "CONNECTED");
        }
    } else {
        std::snprintf(battery, sizeof(battery), "NO PACK");
    }
    DrawText(fb, 190, 345, battery, 2);

    DrawDivider(fb, 430);
    DrawText(fb, 62, 490, "SELECT  RETEST HTTPS", 2);
    DrawText(fb, 96, 550, "UP/DOWN  HOME", 2);
    DrawText(fb, 70, 610, "PWR  POWER MENU", 2);
}

void RenderPowerMenu()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    DrawText(fb, 95, 70, "POWER", 5);
    DrawDivider(fb, 145);

    DrawText(fb, 70, 230, "SELECT", 3);
    DrawText(fb, 218, 230, "REBOOT", 3);

    DrawText(fb, 70, 330, "HOLD PWR", 3);
    DrawText(fb, 250, 330, "SHUT DOWN", 2);

    DrawDivider(fb, 445);
    DrawText(fb, 90, 515, "UP/DOWN  HOME", 2);
    DrawText(fb, 67, 575, "6S PWR  HARD OFF", 2);
}

void RenderShuttingDown()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    DrawClankerHead(fb, (kPortraitWidth - kClankerHeadWidth) / 2, 90);
    DrawClankerWordmark(fb, (kPortraitWidth - kClankerWordmarkWidth) / 2, 250);

    DrawDivider(fb, 335);
    DrawText(fb, 105, 390, "POWERED DOWN", 4);
    DrawText(fb, 78, 465, "PRESS POWER TO REVIVE", 2);
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

    // Product power-key behavior:
    // - PMIC long-press IRQ at ~2s -> firmware clean shutdown.
    // - Hardware 6s hold remains an independent emergency power-off path.
    s_pmic->SetIrqLevelTime(Axp2101::IrqLevelTime::k2S);
    s_pmic->SetPowerKeyPressOffTime(Axp2101::PowerKeyPressOffTime::k6S);
    s_pmic->SetPowerKeyPressOnTime(Axp2101::PowerKeyPressOnTime::k512Ms);
    s_pmic->SetButtonPowerOffEnabled(true);
    s_pmic->SetButtonPowerOffRestarts(false);
    s_pmic->ClearIrqStatus();
    s_pmic->EnablePowerKeyIrq(false);
    s_pmic->SetInterruptCallback([](const Axp2101::InterruptEvent& event) {
        // Long wins if the PMIC latches both bits during one hold.
        if ((event.irq_status & XPOWERS_AXP2101_PKEY_LONG_IRQ) != 0) {
            s_power_long_pending.store(true);
            s_power_short_pending.store(false);
        } else if ((event.irq_status & XPOWERS_AXP2101_PKEY_SHORT_IRQ) != 0) {
            s_power_short_pending.store(true);
        }
    });

    ESP_LOGI(kTag, "AXP2101 rails and power key enabled");
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

void PlayUiTick()
{
    if (!s_codec || s_recording) return;

    constexpr int kSamples = 160;  // 10 ms at 16 kHz.
    std::array<int16_t, kSamples> tick = {};
    for (int i = 0; i < kSamples; ++i) {
        const float envelope = 1.0f - static_cast<float>(i) / kSamples;
        const float phase = 2.0f * 3.14159265f * 1800.0f *
                            static_cast<float>(i) / kAudioSampleRate;
        tick[static_cast<size_t>(i)] =
            static_cast<int16_t>(std::sin(phase) * envelope * 3500.0f);
    }

    s_codec->SetOutputVolume(18);
    s_codec->SetOutputMuted(false);
    s_codec->EnableOutput(true);
    (void)s_codec->OutputData(tick.data(), tick.size());
    s_codec->EnableOutput(false);
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
    FillRect(fb, 20, 20, kPortraitWidth - 40, 5);
    FillRect(fb, 20, kPortraitHeight - 25, kPortraitWidth - 40, 5);
    FillRect(fb, 20, 20, 5, kPortraitHeight - 40);
    FillRect(fb, kPortraitWidth - 25, 20, 5, kPortraitHeight - 40);
    DrawText(fb, 72, 65, "CLANKER POCKET", 3);
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

std::string DisplaySafeUpper(std::string text)
{
    for (char& ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::islower(uch)) {
            ch = static_cast<char>(std::toupper(uch));
        } else if (!(std::isupper(uch) || std::isdigit(uch) || ch == ' ' ||
                     std::strchr("-:.,!?\'\"()/", ch) != nullptr)) {
            ch = ' ';
        }
    }
    return text;
}

std::vector<std::string> WrapTextLines(const std::string& input, int max_chars)
{
    std::vector<std::string> lines;
    std::string text = DisplaySafeUpper(input);
    size_t pos = 0;

    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        if (pos >= text.size()) break;

        size_t end = std::min(text.size(), pos + static_cast<size_t>(max_chars));
        if (end < text.size()) {
            const size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > pos) end = space;
        }
        if (end <= pos) {
            end = std::min(text.size(), pos + static_cast<size_t>(max_chars));
        }

        lines.push_back(text.substr(pos, end - pos));
        pos = end;
    }

    if (lines.empty()) lines.push_back("");
    return lines;
}

void DrawWrappedText(uint8_t* fb, int x, int y, const std::string& input,
                     int scale, int max_chars, int max_lines)
{
    const auto lines = WrapTextLines(input, max_chars);
    const int count = std::min(max_lines, static_cast<int>(lines.size()));
    for (int line = 0; line < count; ++line) {
        DrawText(fb, x, y + line * (8 * scale + 8),
                 lines[static_cast<size_t>(line)].c_str(), scale);
    }
}


void DrawOutlineRect(uint8_t* fb, int x, int y, int w, int h, int thickness = 2)
{
    FillRect(fb, x, y, w, thickness, true);
    FillRect(fb, x, y + h - thickness, w, thickness, true);
    FillRect(fb, x, y, thickness, h, true);
    FillRect(fb, x + w - thickness, y, thickness, h, true);
}

void LoadUiState()
{
    nvs_handle_t handle = 0;
    if (nvs_open("ui_state", NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t mode = 0;
    if (nvs_get_u8(handle, "mode", &mode) == ESP_OK) {
        s_ui_mode = mode == 1 ? UiMode::kRead : UiMode::kChat;
    }
    int32_t scroll = 0;
    if (nvs_get_i32(handle, "chat_scroll", &scroll) == ESP_OK) {
        s_chat_scroll_offset = std::max(0, static_cast<int>(scroll));
    }
    int32_t page = 0;
    if (nvs_get_i32(handle, "reader_page", &page) == ESP_OK) {
        s_reader_page = std::max(0, static_cast<int>(page));
    }

    auto load_string = [&](const char* key) -> std::string {
        size_t len = 0;
        if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len <= 1) return {};
        std::string value(len, '\0');
        if (nvs_get_str(handle, key, value.data(), &len) != ESP_OK) return {};
        if (!value.empty() && value.back() == '\0') value.pop_back();
        return value;
    };

    s_previous_response_id = load_string("response_id");
    const std::string last_user = load_string("last_user");
    const std::string last_ai = load_string("last_ai");
    if (!last_user.empty()) s_chat_messages.push_back({true, last_user});
    if (!last_ai.empty()) s_chat_messages.push_back({false, last_ai});

    uint8_t reader_open = 0;
    (void)nvs_get_u8(handle, "reader_open", &reader_open);
    const std::string reader_book = load_string("reader_book");
    if (!reader_book.empty() && beta_reader::Ready() &&
        beta_reader::OpenPath(reader_book)) {
        beta_reader::SetPage(s_reader_page);
        s_reader_page = beta_reader::CurrentPage();
        s_reader_in_book = reader_open != 0;
    }
    nvs_close(handle);
}

void SaveUiState()
{
    nvs_handle_t handle = 0;
    if (nvs_open("ui_state", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "mode", s_ui_mode == UiMode::kRead ? 1 : 0);
    nvs_set_i32(handle, "chat_scroll", s_chat_scroll_offset);
    nvs_set_i32(handle, "reader_page", s_reader_page);
    nvs_set_u8(handle, "reader_open", s_reader_in_book ? 1 : 0);
    nvs_set_str(handle, "reader_book",
                beta_reader::HasOpenBook() ? beta_reader::CurrentPath().c_str() : "");
    nvs_set_str(handle, "response_id", s_previous_response_id.c_str());

    std::string last_user;
    std::string last_ai;
    for (auto it = s_chat_messages.rbegin(); it != s_chat_messages.rend(); ++it) {
        if (it->user && last_user.empty()) last_user = it->text;
        if (!it->user && last_ai.empty()) last_ai = it->text;
        if (!last_user.empty() && !last_ai.empty()) break;
    }
    nvs_set_str(handle, "last_user", last_user.c_str());
    nvs_set_str(handle, "last_ai", last_ai.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

void ClearChat()
{
    s_chat_messages.clear();
    s_previous_response_id.clear();
    s_chat_scroll_offset = 0;
    SaveUiState();
}

void DrawTab(uint8_t* fb, int x, const char* label, bool selected)
{
    constexpr int kTabWidth = 62;
    constexpr int kTabHeight = 48;
    DrawOutlineRect(fb, x, 24, kTabWidth, kTabHeight, selected ? 4 : 2);
    DrawText(fb, x + 7, 38, label, 2);
    if (selected) FillRect(fb, x + 6, 66, kTabWidth - 12, 4, true);
}

void DrawBatteryIndicator(uint8_t* fb)
{
    int level = -1;
    const bool charging = s_pmic && s_pmic->IsCharging();
    if (s_pmic && s_pmic->isBatteryConnect()) {
        level = s_pmic->GetBatteryLevel();
    }

    const int x = 414;
    const int y = 31;
    const int w = 46;
    const int h = 24;

    if (charging) {
        // Compact lightning bolt immediately left of the battery.
        FillRect(fb, 402, 31, 5, 9, true);
        FillRect(fb, 398, 39, 9, 5, true);
        FillRect(fb, 402, 43, 5, 10, true);
        FillRect(fb, 407, 40, 4, 5, true);
    }

    DrawOutlineRect(fb, x, y, w, h, 2);
    FillRect(fb, x + w, y + 7, 4, 10, true);

    if (level < 0) return;

    int segments = 0;
    if (level >= 15) segments = 1;
    if (level >= 45) segments = 2;
    if (level >= 75) segments = 3;

    for (int i = 0; i < segments; ++i) {
        FillRect(fb, x + 5 + i * 12, y + 5, 9, h - 10, true);
    }
}

void DrawTopBar(uint8_t* fb)
{
    DrawClankerWordmark(fb, 12, 31);

    if (s_settings_open) {
        DrawText(fb, 304, 38, "SETTINGS", 2);
    } else {
        const char* mode = s_ui_mode == UiMode::kChat ? "< CHAT >" : "< READ >";
        DrawText(fb, 292, 38, mode, 2);
    }

    DrawBatteryIndicator(fb);
    FillRect(fb, 18, 92, kPortraitWidth - 36, 3, true);
}

void DrawMicIcon(uint8_t* fb)
{
    constexpr int cx = 240;
    constexpr int cy = 746;
    constexpr int radius = 30;
    const bool recording = s_mic_state == MicState::kRecording;
    const bool ink = !recording;

    if (recording) {
        FillCircle(fb, cx, cy, radius, true);
    } else {
        FillCircle(fb, cx, cy, radius, false);
        DrawCircle(fb, cx, cy, radius, 2, true);
    }

    const int x = cx - 8;
    const int y = cy - 17;

    // Mic capsule.
    if (recording) {
        FillRect(fb, x, y, 16, 24, false);
        FillRect(fb, x + 3, y + 3, 10, 18, true);
    } else {
        DrawOutlineRect(fb, x, y, 16, 24, 2);
    }

    // Mic cradle and stem, inverted with the circle while recording.
    FillRect(fb, x - 5, y + 18, 5, 10, ink);
    FillRect(fb, x + 16, y + 18, 5, 10, ink);
    FillRect(fb, x, y + 28, 16, 2, ink);
    FillRect(fb, x + 7, y + 30, 2, 8, ink);
    FillRect(fb, x + 1, y + 38, 14, 2, ink);

    if (s_mic_state == MicState::kProcessing) {
        FillRect(fb, cx + 40, cy - 2, 4, 4, true);
        FillRect(fb, cx + 50, cy - 2, 4, 4, true);
        FillRect(fb, cx + 60, cy - 2, 4, 4, true);
    }
}

int ChatMessageMaxChars(const ChatMessage& msg)
{
    return msg.user ? 27 : 34;
}

int ChatMessageWidth(const ChatMessage& msg)
{
    return msg.user ? 336 : 432;
}

int ChatMessageLineCount(const ChatMessage& msg)
{
    return static_cast<int>(
        WrapTextLines(msg.text, ChatMessageMaxChars(msg)).size());
}

int ChatMessageHeight(const ChatMessage& msg)
{
    constexpr int kLineStep = 24;
    constexpr int kPadTop = 14;
    constexpr int kPadBottom = 14;
    return kPadTop + ChatMessageLineCount(msg) * kLineStep + kPadBottom;
}

int ChatTranscriptHeight()
{
    constexpr int kGap = 10;
    int height = 0;
    for (size_t i = 0; i < s_chat_messages.size(); ++i) {
        if (i > 0) height += kGap;
        height += ChatMessageHeight(s_chat_messages[i]);
    }
    return height;
}

int ChatMaxScrollLines()
{
    constexpr int kBodyTop = 112;
    constexpr int kBodyBottom = 688;
    constexpr int kLineStep = 24;
    const int overflow = std::max(
        0, ChatTranscriptHeight() - (kBodyBottom - kBodyTop));
    return (overflow + kLineStep - 1) / kLineStep;
}

void DrawChatMessageClipped(uint8_t* fb, int y, const ChatMessage& msg,
                            int clip_top, int clip_bottom)
{
    constexpr int kLineStep = 24;
    constexpr int kTextHeight = 14;

    const int w = ChatMessageWidth(msg);
    const int x = msg.user ? (kPortraitWidth - 24 - w) : 24;
    const int h = ChatMessageHeight(msg);
    const int bottom = y + h;

    if (bottom <= clip_top || y >= clip_bottom) return;

    // Bubble outline, clipped to the chat viewport.
    if (y >= clip_top && y + 2 <= clip_bottom) {
        FillRect(fb, x, y, w, 2, true);
    }
    const int bottom_line = bottom - 2;
    if (bottom_line >= clip_top && bottom_line + 2 <= clip_bottom) {
        FillRect(fb, x, bottom_line, w, 2, true);
    }

    const int side_top = std::max(y, clip_top);
    const int side_bottom = std::min(bottom, clip_bottom);
    if (side_bottom > side_top) {
        FillRect(fb, x, side_top, 2, side_bottom - side_top, true);
        FillRect(fb, x + w - 2, side_top, 2, side_bottom - side_top, true);
    }

    const auto lines = WrapTextLines(msg.text, ChatMessageMaxChars(msg));
    for (size_t i = 0; i < lines.size(); ++i) {
        const int line_y = y + 14 + static_cast<int>(i) * kLineStep;
        if (line_y < clip_top || line_y + kTextHeight > clip_bottom) continue;
        DrawText(fb, x + 12, line_y, lines[i].c_str(), 2);
    }
}

void DrawChatBody(uint8_t* fb)
{
    constexpr int kBodyTop = 112;
    constexpr int kBodyBottom = 688;
    constexpr int kBodyHeight = kBodyBottom - kBodyTop;
    constexpr int kGap = 10;
    constexpr int kLineStep = 24;

    if (s_chat_messages.empty()) {
        DrawText(fb, 104, 300, "HOLD BOOT TO TALK", 3);
        DrawText(fb, 91, 355, "UP DOWN SCROLL CHAT", 2);
    } else {
        const int transcript_h = ChatTranscriptHeight();
        const int overflow = std::max(0, transcript_h - kBodyHeight);
        const int max_scroll = ChatMaxScrollLines();
        s_chat_scroll_offset =
            std::clamp(s_chat_scroll_offset, 0, max_scroll);

        // Offset is measured in text lines from the newest/bottom position.
        const int scroll_px =
            std::min(overflow, s_chat_scroll_offset * kLineStep);
        int y = overflow > 0
            ? kBodyTop - overflow + scroll_px
            : kBodyTop;

        for (const auto& msg : s_chat_messages) {
            DrawChatMessageClipped(fb, y, msg, kBodyTop, kBodyBottom);
            y += ChatMessageHeight(msg) + kGap;
        }
    }

    FillRect(fb, 18, 700, kPortraitWidth - 36, 3, true);
    DrawMicIcon(fb);
}

std::string ClipDisplayText(const std::string& input, size_t max_chars)
{
    std::string text = DisplaySafeUpper(input);
    if (text.size() <= max_chars) return text;
    if (max_chars <= 3) return text.substr(0, max_chars);
    return text.substr(0, max_chars - 3) + "...";
}

void DrawReadBody(uint8_t* fb)
{
    if (!s_reader_in_book) {
        DrawText(fb, 24, 122, "LIBRARY", 4);
        DrawDivider(fb, 174);

        if (!beta_reader::Ready()) {
            DrawText(fb, 80, 285, "NO SD CARD", 4);
            DrawText(fb, 52, 350, "INSERT FAT32 TF CARD", 2);
            DrawText(fb, 78, 395, "BOOKS GO IN /BOOKS", 2);
            return;
        }

        const auto& books = beta_reader::Books();
        if (books.empty()) {
            DrawText(fb, 75, 285, "NO TXT BOOKS", 4);
            DrawText(fb, 73, 350, "COPY BOOKS TO", 2);
            DrawText(fb, 92, 390, "/SDCARD/BOOKS", 2);
            return;
        }

        s_reader_library_index =
            std::clamp(s_reader_library_index, 0, static_cast<int>(books.size()) - 1);
        constexpr int kVisible = 8;
        const int start_index = (s_reader_library_index / kVisible) * kVisible;
        int y = 205;
        for (int row = 0; row < kVisible; ++row) {
            const int idx = start_index + row;
            if (idx >= static_cast<int>(books.size())) break;
            const bool selected = idx == s_reader_library_index;
            const std::string title = ClipDisplayText(books[static_cast<size_t>(idx)].name, 30);
            DrawText(fb, 28, y, selected ? ">" : " ", 2);
            DrawText(fb, 52, y, title.c_str(), 2);
            y += 55;
        }

        char count[32] = {};
        std::snprintf(count, sizeof(count), "%d OF %d",
                      s_reader_library_index + 1, static_cast<int>(books.size()));
        DrawText(fb, 318, 660, count, 1);
        return;
    }

    if (!beta_reader::HasOpenBook()) {
        s_reader_in_book = false;
        DrawText(fb, 92, 300, "BOOK NOT FOUND", 3);
        return;
    }

    s_reader_page = beta_reader::CurrentPage();
    char page[40] = {};
    std::snprintf(page, sizeof(page), "%d/%d",
                  s_reader_page + 1, beta_reader::PageCount());

    const std::string title = ClipDisplayText(beta_reader::CurrentName(), 25);
    DrawText(fb, 24, 120, title.c_str(), 2);
    DrawText(fb, 378, 120, page, 1);
    DrawDivider(fb, 158);

    const auto lines = beta_reader::CurrentPageLines();
    int y = 182;
    for (size_t i = 0; i < lines.size() && i < 21; ++i) {
        const std::string line = DisplaySafeUpper(lines[i]);
        DrawText(fb, 28, y, line.c_str(), 2);
        y += 24;
    }

    DrawText(fb, 132, 684, "UP/DOWN PAGE", 1);
}


void DrawSettingsBody(uint8_t* fb)
{
    const beta_network::Snapshot net = beta_network::GetSnapshot();
    const char* wifi =
        net.mode == beta_network::Mode::kConnected ? "ONLINE" :
        net.mode == beta_network::Mode::kProvisioning ? "SETUP" : "OFFLINE";

    DrawText(fb, 28, 128, "SYSTEM", 3);
    DrawText(fb, 28, 185, "WIFI", 2);
    DrawText(fb, 190, 185, wifi, 2);
    DrawText(fb, 28, 225, "OPENAI", 2);
    DrawText(fb, 190, 225, beta_transcription::HasApiKey() ? "READY" : "MISSING", 2);

    char battery[32] = {};
    if (s_pmic && s_pmic->isBatteryConnect()) {
        const int level = s_pmic->GetBatteryLevel();
        if (level >= 0) std::snprintf(battery, sizeof(battery), "%d%%", level);
        else std::snprintf(battery, sizeof(battery), "CONNECTED");
    } else {
        std::snprintf(battery, sizeof(battery), "NO PACK");
    }
    DrawText(fb, 28, 265, "BATTERY", 2);
    DrawText(fb, 190, 265, battery, 2);

    DrawDivider(fb, 320);
    DrawText(fb, 28, 360, s_settings_index == 0 ? "> TEST HTTPS" : "  TEST HTTPS", 2);
    DrawText(fb, 28, 415, s_settings_index == 1 ? "> CLEAN DISPLAY" : "  CLEAN DISPLAY", 2);
    DrawText(fb, 28, 470, s_settings_index == 2 ? "> BACK" : "  BACK", 2);

    DrawDivider(fb, 545);
    DrawText(fb, 28, 585, "UP DOWN SELECT", 2);
    DrawText(fb, 28, 625, "PRESS RADIAL TO OPEN", 2);
}

void DrawSettingsNotice(uint8_t* fb)
{
    if (!s_settings_notice) return;
    FillRect(fb, 66, 316, 348, 128, false);
    DrawOutlineRect(fb, 66, 316, 348, 128, 3);
    DrawText(fb, 82, 345, "OPENING SETTINGS", 3);
    DrawText(fb, 118, 398, "KEEP HOLDING", 2);
}

void DrawMenuOverlay(uint8_t* fb)
{
    if (s_ui_menu == UiMenu::kNone) return;
    FillRect(fb, 55, 260, 370, 250, false);
    DrawOutlineRect(fb, 55, 260, 370, 250, 3);

    if (s_ui_menu == UiMenu::kChat) {
        DrawText(fb, 95, 292, "CHAT MENU", 3);
        DrawText(fb, 86, 360, s_menu_index == 0 ? "> NEW CHAT" : "  NEW CHAT", 3);
        DrawText(fb, 86, 420, s_menu_index == 1 ? "> CANCEL" : "  CANCEL", 3);
    } else if (s_ui_menu == UiMenu::kClearConfirm) {
        DrawText(fb, 93, 300, "CLEAR CHAT", 3);
        DrawText(fb, 87, 365, "PRESS TO CONFIRM", 2);
        DrawText(fb, 82, 420, "UP DOWN TO CANCEL", 2);
    } else if (s_ui_menu == UiMenu::kReader) {
        DrawText(fb, 85, 292, "READER MENU", 3);
        DrawText(fb, 78, 360, s_menu_index == 0 ? "> LIBRARY" : "  LIBRARY", 3);
        DrawText(fb, 78, 420, s_menu_index == 1 ? "> CANCEL" : "  CANCEL", 3);
    }
}

void DrawSwapOverlay(uint8_t* fb)
{
    if (!s_swap_notice) return;
    FillRect(fb, 66, 316, 348, 128, false);
    DrawOutlineRect(fb, 66, 316, 348, 128, 3);
    DrawText(fb, 91, 345, "SWAPPING MODE", 3);
    DrawText(fb, 118, 398, "KEEP HOLDING", 2);
}

void RenderUi()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawTopBar(fb);
    if (s_settings_open) {
        DrawSettingsBody(fb);
    } else {
        if (s_ui_mode == UiMode::kChat) DrawChatBody(fb);
        else DrawReadBody(fb);
        DrawMenuOverlay(fb);
        DrawSwapOverlay(fb);
        DrawSettingsNotice(fb);
    }
}

void RefreshUiPartial()
{
    const esp_err_t err = s_panel->RefreshChangedRegion();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Partial UI refresh failed: %s", esp_err_to_name(err));
    }
}

void ToggleMode()
{
    if (s_settings_open) return;
    s_ui_mode = s_ui_mode == UiMode::kChat ? UiMode::kRead : UiMode::kChat;
    s_ui_menu = UiMenu::kNone;
    s_menu_index = 0;
    s_swap_notice = false;
    SaveUiState();
    RenderUi();
    (void)s_panel->RefreshFastBase();
}

void HandleDirection(bool up)
{
    PlayUiTick();
    if (s_settings_open) {
        if (up) s_settings_index = (s_settings_index + 2) % 3;
        else s_settings_index = (s_settings_index + 1) % 3;
        RenderUi();
        RefreshUiPartial();
        return;
    }

    if (s_power_menu) {
        s_power_menu = false;
        RenderUi();
        (void)s_panel->RefreshFastBase();
        return;
    }

    if (s_ui_menu == UiMenu::kClearConfirm) {
        s_ui_menu = UiMenu::kNone;
        RenderUi();
        RefreshUiPartial();
        return;
    }

    if (s_ui_menu == UiMenu::kChat || s_ui_menu == UiMenu::kReader) {
        s_menu_index = s_menu_index == 0 ? 1 : 0;
        RenderUi();
        RefreshUiPartial();
        return;
    }

    if (s_ui_mode == UiMode::kChat) {
        const int max_offset = ChatMaxScrollLines();
        if (up) {
            s_chat_scroll_offset =
                std::min(max_offset, s_chat_scroll_offset + 1);
        } else {
            s_chat_scroll_offset =
                std::max(0, s_chat_scroll_offset - 1);
        }
        SaveUiState();
        RenderUi();
        RefreshUiPartial();
        return;
    }

    if (!s_reader_in_book) {
        const int count = static_cast<int>(beta_reader::Books().size());
        if (count <= 0) return;
        if (up) {
            s_reader_library_index =
                (s_reader_library_index + count - 1) % count;
        } else {
            s_reader_library_index =
                (s_reader_library_index + 1) % count;
        }
        RenderUi();
        RefreshUiPartial();
        return;
    }

    const int max_page = std::max(0, beta_reader::PageCount() - 1);
    if (up) s_reader_page = std::max(0, s_reader_page - 1);
    else s_reader_page = std::min(max_page, s_reader_page + 1);
    beta_reader::SetPage(s_reader_page);
    s_reader_page = beta_reader::CurrentPage();
    ++s_reader_page_turns;
    SaveUiState();
    RenderUi();
    if ((s_reader_page_turns % kReaderFullRefreshEveryPages) == 0) {
        (void)s_panel->RefreshFullBase();
    } else {
        (void)s_panel->RefreshFastBase();
    }
}

void HandleSelectShort()
{
    PlayUiTick();
    if (s_settings_open) {
        if (s_settings_index == 0) {
            beta_network::RunHttpsProbe();
            RenderUi();
            RefreshUiPartial();
        } else if (s_settings_index == 1) {
            RenderUi();
            (void)s_panel->RefreshFullBase();
        } else {
            s_settings_open = false;
            s_settings_index = 0;
            RenderUi();
            (void)s_panel->RefreshFastBase();
        }
        return;
    }

    if (s_power_menu) {
        ESP_LOGI(kTag, "Power menu: reboot requested");
        RenderShuttingDown();
        (void)s_panel->RefreshFullBase();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    if (s_ui_mode == UiMode::kChat) {
        if (s_ui_menu == UiMenu::kNone) {
            s_ui_menu = UiMenu::kChat;
            s_menu_index = 0;
        } else if (s_ui_menu == UiMenu::kChat) {
            if (s_menu_index == 0) s_ui_menu = UiMenu::kClearConfirm;
            else s_ui_menu = UiMenu::kNone;
        } else if (s_ui_menu == UiMenu::kClearConfirm) {
            ClearChat();
            s_ui_menu = UiMenu::kNone;
        }
        RenderUi();
        RefreshUiPartial();
        return;
    }

    if (!s_reader_in_book) {
        const auto& books = beta_reader::Books();
        if (!books.empty()) {
            const int idx = std::clamp(
                s_reader_library_index, 0, static_cast<int>(books.size()) - 1);
            const std::string selected_path = books[static_cast<size_t>(idx)].path;
            if (!beta_reader::HasOpenBook() ||
                beta_reader::CurrentPath() != selected_path) {
                if (beta_reader::OpenBook(static_cast<size_t>(idx))) {
                    s_reader_page = 0;
                    beta_reader::SetPage(0);
                }
            } else {
                s_reader_page = beta_reader::CurrentPage();
            }
            s_reader_in_book = beta_reader::HasOpenBook();
            SaveUiState();
        }
        RenderUi();
        (void)s_panel->RefreshFastBase();
        return;
    }

    if (s_ui_menu == UiMenu::kNone) {
        s_ui_menu = UiMenu::kReader;
        s_menu_index = 0;
    } else if (s_ui_menu == UiMenu::kReader) {
        if (s_menu_index == 0) {
            s_reader_in_book = false;
            s_ui_menu = UiMenu::kNone;
            SaveUiState();
        } else {
            s_ui_menu = UiMenu::kNone;
        }
    }
    RenderUi();
    RefreshUiPartial();
}


void RenderTranscribing()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);
    DrawText(fb, 95, 260, "TRANSCRIBING", 4);
    DrawText(fb, 72, 345, "OPENAI REQUEST ACTIVE", 2);
    DrawText(fb, 80, 395, "PLEASE WAIT", 3);
}

void RenderTranscriptionResult(const beta_transcription::Result& result)
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);

    if (result.success) {
        DrawText(fb, 75, 205, "TRANSCRIPT OK", 4);
        char http[32] = {};
        std::snprintf(http, sizeof(http), "HTTP: %d", result.http_status);
        DrawText(fb, 60, 285, http, 2);
        DrawWrappedText(fb, 48, 345, result.transcript, 2, 30, 7);
        DrawText(fb, 55, 690, "BOOT = RECORD AGAIN", 2);
    } else {
        DrawText(fb, 55, 205, "TRANSCRIPTION FAILED", 3);
        char http[32] = {};
        std::snprintf(http, sizeof(http), "HTTP: %d", result.http_status);
        DrawText(fb, 60, 285, http, 2);
        DrawWrappedText(fb, 48, 345,
                        result.error_code + " " + result.error_message,
                        2, 30, 6);
        DrawText(fb, 55, 690, "BOOT = TRY AGAIN", 2);
    }
}

void RenderThinking()
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);
    DrawText(fb, 110, 250, "CLANKER", 4);
    DrawText(fb, 95, 330, "THINKING", 4);
    DrawText(fb, 75, 405, "PLEASE WAIT", 3);
}

void RenderClankerResult(const std::string& transcript,
                         const beta_clanker::Result& result)
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);
    DrawFrame(fb);

    if (result.success) {
        DrawText(fb, 82, 190, "CLANKER SAYS", 4);
        DrawText(fb, 45, 265, "YOU:", 2);
        DrawWrappedText(fb, 105, 265, transcript, 2, 24, 3);
        DrawText(fb, 45, 395, "CLANKER:", 2);
        DrawWrappedText(fb, 45, 440, result.response, 2, 30, 7);
        DrawText(fb, 55, 690, "BOOT = ASK AGAIN", 2);
    } else {
        DrawText(fb, 82, 205, "CLANKER ERROR", 4);
        char http[32] = {};
        std::snprintf(http, sizeof(http), "HTTP: %d", result.http_status);
        DrawText(fb, 60, 285, http, 2);
        DrawWrappedText(fb, 48, 345,
                        result.error_code + " " + result.error_message,
                        2, 30, 6);
        DrawText(fb, 55, 690, "BOOT = TRY AGAIN", 2);
    }
}

void StartCapture()
{
    if (s_ui_mode != UiMode::kChat || s_power_menu || s_settings_open) return;
    s_clip.clear();
    s_last_capture = {};
    s_recording = true;
    s_recording_started_us = esp_timer_get_time();
    s_codec->EnableInput(true);
    s_mic_state = MicState::kRecording;
    RenderUi();
    RefreshUiPartial();
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

    ESP_LOGI(kTag, "Capture finished: samples=%lu pcm_ms=%lu peak=%d rms=%d clipped=%d",
             static_cast<unsigned long>(s_last_capture.samples),
             static_cast<unsigned long>(s_last_capture.duration_ms),
             s_last_capture.peak, s_last_capture.rms, s_last_capture.clipped ? 1 : 0);

    s_mic_state = MicState::kProcessing;
    RenderUi();
    RefreshUiPartial();

    const beta_network::Snapshot net = beta_network::GetSnapshot();
    if (net.mode != beta_network::Mode::kConnected || !beta_transcription::HasApiKey()) {
        s_chat_messages.push_back({false, "NETWORK OR OPENAI NOT READY"});
        s_mic_state = MicState::kIdle;
        s_chat_scroll_offset = 0;
        SaveUiState();
        RenderUi();
        RefreshUiPartial();
        return;
    }

    const beta_transcription::Result tx =
        beta_transcription::TranscribePcm16(s_clip.data(), s_clip.size(), kAudioSampleRate);
    if (!tx.success) {
        s_chat_messages.push_back({false, "TRANSCRIPTION FAILED " + tx.error_code});
        s_mic_state = MicState::kIdle;
        s_chat_scroll_offset = 0;
        SaveUiState();
        RenderUi();
        RefreshUiPartial();
        return;
    }

    s_chat_messages.push_back({true, tx.transcript});
    s_chat_scroll_offset = 0;
    RenderUi();
    RefreshUiPartial();

    const beta_clanker::Result answer =
        beta_clanker::Ask(tx.transcript, s_previous_response_id);
    if (answer.success) {
        s_chat_messages.push_back({false, answer.response});
        if (!answer.response_id.empty()) s_previous_response_id = answer.response_id;
    } else {
        s_chat_messages.push_back({false, "CLANKER ERROR " + answer.error_code});
    }

    s_mic_state = MicState::kIdle;
    s_chat_scroll_offset = 0;
    SaveUiState();
    RenderUi();
    RefreshUiPartial();
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "CLANKER Pocket CHAT/READ UI boot");

    if (InitPower() != ESP_OK || InitButtons() != ESP_OK) {
        ESP_LOGE(kTag, "Core hardware init failed");
        return;
    }

    s_panel = std::make_unique<EpaperPanel>(kRawWidth, kRawHeight, PanelConfig());
    if (s_panel->Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "Display init failed");
        return;
    }

    RenderSplash();
    if (s_panel->RefreshFullBase() != ESP_OK) {
        ESP_LOGE(kTag, "Splash refresh failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1800));

    if (InitAudio() != ESP_OK) {
        ESP_LOGE(kTag, "Audio init failed");
        return;
    }

    const esp_err_t network_err = beta_network::Init();
    if (network_err != ESP_OK) {
        ESP_LOGW(kTag, "Network init returned: %s", esp_err_to_name(network_err));
    }
    beta_network::RunHttpsProbe();

    const esp_err_t reader_err = beta_reader::Init();
    if (reader_err != ESP_OK) {
        ESP_LOGW(kTag, "Reader SD init returned: %s", esp_err_to_name(reader_err));
    }

    LoadUiState();

    RenderUi();
    if (s_panel->RefreshFastBase() != ESP_OK) {
        ESP_LOGE(kTag, "Initial UI refresh failed");
        return;
    }

    std::array<int, 4> last = {1,1,1,1};
    int64_t select_down_us = 0;
    int64_t boot_down_us = 0;
    int64_t settings_combo_started_us = 0;
    bool select_consumed = false;
    bool boot_consumed = false;
    bool settings_combo_consumed = false;

    while (true) {
        if (s_power_long_pending.exchange(false)) {
            ESP_LOGI(kTag, "Power key long press: clean shutdown");
            s_power_menu = false;
            RenderShuttingDown();
            (void)s_panel->RefreshFullBase();
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_codec) s_codec->Shutdown();
            vTaskDelay(pdMS_TO_TICKS(100));
            if (s_pmic) s_pmic->PowerOff();
            while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (s_power_short_pending.exchange(false)) {
            ESP_LOGI(kTag, "Power key short press: power menu");
            s_power_menu = true;
            s_ui_menu = UiMenu::kNone;
            RenderPowerMenu();
            (void)s_panel->RefreshFastBase();
        }

        const int boot_now = gpio_get_level(kButtonBoot);
        const int select_now = gpio_get_level(kButtonSelect);

        if (last[0] == 1 && boot_now == 0) {
            boot_down_us = esp_timer_get_time();
            boot_consumed = false;
        }
        if (last[2] == 1 && select_now == 0) {
            select_down_us = esp_timer_get_time();
            select_consumed = false;
            s_swap_notice = false;
        }

        const bool settings_combo =
            boot_now == 0 && select_now == 0 && !s_power_menu && !s_recording;
        if (settings_combo) {
            if (settings_combo_started_us == 0) {
                settings_combo_started_us = esp_timer_get_time();
                s_swap_notice = false;
                s_settings_notice = false;
            }
            const int64_t combo_us = esp_timer_get_time() - settings_combo_started_us;
            if (combo_us >= kSettingsNoticeUs && !s_settings_notice) {
                s_settings_notice = true;
                RenderUi();
                RefreshUiPartial();
            }
            if (combo_us >= kSettingsCommitUs && !settings_combo_consumed) {
                settings_combo_consumed = true;
                select_consumed = true;
                boot_consumed = true;
                s_settings_notice = false;
                s_settings_open = !s_settings_open;
                s_ui_menu = UiMenu::kNone;
                s_settings_index = 0;
                RenderUi();
                (void)s_panel->RefreshFastBase();
            }
        } else if (settings_combo_started_us != 0) {
            if (!settings_combo_consumed && s_settings_notice) {
                s_settings_notice = false;
                RenderUi();
                RefreshUiPartial();
            }
            settings_combo_started_us = 0;
            settings_combo_consumed = false;
        }

        if (!s_recording && boot_now == 0 && select_now == 1 &&
            boot_down_us != 0 && !boot_consumed && !s_settings_open &&
            s_ui_mode == UiMode::kChat && !s_power_menu &&
            (esp_timer_get_time() - boot_down_us) >= kBootPttGraceUs) {
            boot_consumed = true;
            StartCapture();
        }

        if (s_recording && boot_now == 0) PumpCapture();

        if (last[0] == 0 && boot_now == 1) {
            if (s_recording) FinishCapture();
            boot_down_us = 0;
            boot_consumed = false;
        }
        last[0] = boot_now;

        if (!s_recording) {
            if (select_now == 0 && boot_now == 1 && select_down_us != 0 &&
                !select_consumed && !s_power_menu && !s_settings_open) {
                const int64_t held_us = esp_timer_get_time() - select_down_us;
                if (held_us >= kModeSwapNoticeUs && !s_swap_notice) {
                    s_swap_notice = true;
                    RenderUi();
                    RefreshUiPartial();
                }
                if (held_us >= kModeSwapCommitUs) {
                    select_consumed = true;
                    ToggleMode();
                }
            }

            if (last[2] == 0 && select_now == 1) {
                if (!select_consumed && boot_now == 1) {
                    if (s_swap_notice) {
                        s_swap_notice = false;
                        RenderUi();
                        RefreshUiPartial();
                    } else {
                        HandleSelectShort();
                    }
                }
                select_down_us = 0;
                select_consumed = false;
                s_swap_notice = false;
            }
            last[2] = select_now;

            const int up_now = gpio_get_level(kButtonUp);
            if (last[1] == 1 && up_now == 0) HandleDirection(true);
            last[1] = up_now;

            const int down_now = gpio_get_level(kButtonDown);
            if (last[3] == 1 && down_now == 0) HandleDirection(false);
            last[3] = down_now;

            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
