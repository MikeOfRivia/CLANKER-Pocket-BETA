#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "axp2101.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "epaper_panel.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

constexpr int kRawWidth = 800;
constexpr int kRawHeight = 480;
constexpr int kPortraitWidth = 480;
constexpr int kPortraitHeight = 800;
constexpr int kFramebufferBytes = (kRawWidth * kRawHeight) / 8;

std::unique_ptr<Axp2101> s_pmic;
std::unique_ptr<EpaperPanel> s_panel;

struct Glyph {
    char ch;
    std::array<uint8_t, 7> rows;
};

constexpr Glyph kFont[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'-', {0,0,0,31,0,0,0}},
    {':', {0,4,0,0,4,0,0}},
    {'0', {14,17,19,21,25,17,14}},
    {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}},
    {'3', {30,1,1,14,1,1,30}},
    {'4', {2,6,10,18,31,2,2}},
    {'5', {31,16,16,30,1,1,30}},
    {'6', {14,16,16,30,17,17,14}},
    {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}},
    {'9', {14,17,17,15,1,1,14}},
    {'A', {14,17,17,31,17,17,17}},
    {'B', {30,17,17,30,17,17,30}},
    {'C', {14,17,16,16,16,17,14}},
    {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}},
    {'F', {31,16,16,30,16,16,16}},
    {'G', {14,17,16,23,17,17,15}},
    {'H', {17,17,17,31,17,17,17}},
    {'I', {14,4,4,4,4,4,14}},
    {'J', {7,2,2,2,2,18,12}},
    {'K', {17,18,20,24,20,18,17}},
    {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}},
    {'N', {17,25,21,19,17,17,17}},
    {'O', {14,17,17,17,17,17,14}},
    {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}},
    {'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}},
    {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}},
    {'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,21,10}},
    {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}},
    {'Z', {31,1,2,4,8,16,31}},
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
    c.cs = kEpdCs;
    c.dc = kEpdDc;
    c.rst = kEpdReset;
    c.busy = kEpdBusy;
    c.mosi = kEpdMosi;
    c.miso = GPIO_NUM_NC;
    c.sck = kEpdSck;
    c.external_spi_bus = false;
    c.buffer_len = kFramebufferBytes;
    c.busy_timeout_ms = 10000;
    c.reset_low_ms = 2;
    c.reset_high_ms = 50;
    c.busy_level = 1;
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

    i2c_master_bus_handle_t bus = nullptr;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), kTag, "I2C init");

    s_pmic = std::make_unique<Axp2101>(bus, kAxp2101Address, GPIO_NUM_38);
    s_pmic->setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    s_pmic->setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_900MA);
    s_pmic->setSysPowerDownVoltage(2800);
    s_pmic->enableDC1();
    s_pmic->setDC1Voltage(3300);
    s_pmic->enableALDO1();
    s_pmic->setALDO1Voltage(3300);
    s_pmic->enableALDO2();
    s_pmic->setALDO2Voltage(3300);
    s_pmic->enableALDO3();
    s_pmic->setALDO3Voltage(3300);
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

void Render(const char* last_button, uint32_t press_count)
{
    auto* fb = s_panel->framebuffer();
    s_panel->Clear(true);

    FillRect(fb, 20, 20, kPortraitWidth - 40, 8);
    FillRect(fb, 20, kPortraitHeight - 28, kPortraitWidth - 40, 8);
    FillRect(fb, 20, 20, 8, kPortraitHeight - 40);
    FillRect(fb, kPortraitWidth - 28, 20, 8, kPortraitHeight - 40);

    DrawText(fb, 48, 80, "CLANKER", 6);
    DrawText(fb, 78, 145, "POCKET BETA", 4);
    DrawText(fb, 120, 260, "DISPLAY OK", 3);
    DrawText(fb, 65, 350, "LAST BUTTON:", 3);
    DrawText(fb, 95, 405, last_button, 4);

    char count[16] = {};
    std::snprintf(count, sizeof(count), "%lu", static_cast<unsigned long>(press_count));
    DrawText(fb, 110, 500, "PRESS COUNT:", 3);
    DrawText(fb, 200, 550, count, 4);

    DrawText(fb, 60, 680, "PHASE 1 HARDWARE TEST", 2);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "CLANKER Pocket BETA boot");

    if (InitPower() != ESP_OK) {
        ESP_LOGE(kTag, "Power init failed");
        return;
    }
    if (InitButtons() != ESP_OK) {
        ESP_LOGE(kTag, "Button init failed");
        return;
    }

    s_panel = std::make_unique<EpaperPanel>(kRawWidth, kRawHeight, PanelConfig());
    if (s_panel->Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "Display init failed");
        return;
    }

    uint32_t press_count = 0;
    Render("NONE", press_count);
    if (s_panel->RefreshFullBase() != ESP_OK) {
        ESP_LOGE(kTag, "Initial display refresh failed");
        return;
    }

    constexpr std::array<gpio_num_t, 4> pins = {
        kButtonBoot, kButtonUp, kButtonSelect, kButtonDown
    };
    std::array<int, 4> last = {1,1,1,1};

    while (true) {
        for (size_t i = 0; i < pins.size(); ++i) {
            const int now = gpio_get_level(pins[i]);
            if (last[i] == 1 && now == 0) {
                ++press_count;
                ESP_LOGI(kTag, "Button %s pressed (%lu)", ButtonName(static_cast<int>(i)),
                         static_cast<unsigned long>(press_count));
                Render(ButtonName(static_cast<int>(i)), press_count);
                const esp_err_t err = s_panel->RefreshFastBase();
                if (err != ESP_OK) {
                    ESP_LOGW(kTag, "Button display refresh failed: %s", esp_err_to_name(err));
                }
            }
            last[i] = now;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
