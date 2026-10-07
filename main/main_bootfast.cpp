#include "nvs_flash.h"

// Keep the current application implementation intact while replacing only the
// top-level boot orchestration. This lets the boot behavior prove itself on
// hardware without duplicating or destabilizing the rest of the CHAT/READ UI.
#define app_main clanker_legacy_app_main
#include "main.cpp"
#undef app_main

namespace {

void AppendBootSilence(std::vector<int16_t>& out, int duration_ms)
{
    const int samples = std::max(0, (kAudioSampleRate * duration_ms) / 1000);
    out.insert(out.end(), static_cast<size_t>(samples), 0);
}

void AppendBootChirp(std::vector<int16_t>& out,
                     float start_hz, float end_hz, int duration_ms,
                     float volume, float vibrato, float metal)
{
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    const int samples = std::max(1, (kAudioSampleRate * duration_ms) / 1000);
    const int attack_samples = std::max(1, (kAudioSampleRate * 8) / 1000);
    const int release_samples = std::max(1, (kAudioSampleRate * 45) / 1000);
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float phase3 = 0.0f;

    out.reserve(out.size() + static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / kAudioSampleRate;
        const float x = static_cast<float>(i) /
                        static_cast<float>(std::max(1, samples - 1));
        const float curve = x * x * (3.0f - 2.0f * x);
        float hz = start_hz + (end_hz - start_hz) * curve;
        hz *= 1.0f + vibrato * std::sin(kTwoPi * 17.0f * t);

        phase1 += kTwoPi * hz / kAudioSampleRate;
        phase2 += kTwoPi * (hz * metal) / kAudioSampleRate;
        phase3 += kTwoPi * (hz * 4.2f) / kAudioSampleRate;

        const float attack =
            std::min(1.0f, static_cast<float>(i) / attack_samples);
        const float release =
            std::min(1.0f, static_cast<float>(samples - i - 1) /
                               release_samples);
        const float envelope = std::min(attack, release);
        const float wave =
            std::sin(phase1) + 0.26f * std::sin(phase2) +
            0.08f * std::sin(phase3);
        const float sample =
            (wave / 1.34f) * volume * envelope * 32767.0f;
        out.push_back(static_cast<int16_t>(
            std::clamp(sample, -32760.0f, 32760.0f)));
    }
}

void AppendBootTone(std::vector<int16_t>& out, float hz, int duration_ms,
                    float volume, float metal = 2.9f)
{
    AppendBootChirp(out, hz, hz, duration_ms, volume, 0.004f, metal);
}

void AppendBootClick(std::vector<int16_t>& out, int duration_ms,
                     float volume, float pitch_hz, uint32_t& noise_state)
{
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    const int samples = std::max(1, (kAudioSampleRate * duration_ms) / 1000);
    out.reserve(out.size() + static_cast<size_t>(samples));

    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / kAudioSampleRate;
        const float decay = std::exp(-t / 0.007f);
        noise_state = noise_state * 1664525u + 1013904223u;
        const float noise =
            (static_cast<float>((noise_state >> 8) & 0xffffu) / 32767.5f) - 1.0f;
        const float wave =
            0.52f * noise + 0.48f * std::sin(kTwoPi * pitch_hz * t);
        const float sample = volume * decay * wave * 32767.0f;
        out.push_back(static_cast<int16_t>(
            std::clamp(sample, -32760.0f, 32760.0f)));
    }
}

void PlayOwlBootTune()
{
    if (!s_codec || s_recording) return;

    // Exact approved cadence: C4 robot-bird sequence, immediately followed by
    // C3 system-online sequence. Roughly 2.05 seconds total.
    std::vector<int16_t> sound;
    sound.reserve(static_cast<size_t>(kAudioSampleRate * 2.1f));
    uint32_t noise_state = 11u;

    // C4: robot bird.
    AppendBootChirp(sound, 880.0f, 650.0f, 150, 0.33f, 0.018f, 2.85f);
    AppendBootSilence(sound, 18);
    AppendBootClick(sound, 20, 0.19f, 2300.0f, noise_state);
    AppendBootSilence(sound, 12);
    AppendBootChirp(sound, 650.0f, 1190.0f, 130, 0.33f, 0.024f, 2.85f);
    AppendBootSilence(sound, 10);
    AppendBootChirp(sound, 1190.0f, 940.0f, 110, 0.32f, 0.020f, 2.85f);
    AppendBootSilence(sound, 12);
    AppendBootChirp(sound, 940.0f, 720.0f, 150, 0.34f, 0.018f, 2.85f);
    AppendBootSilence(sound, 28);
    AppendBootClick(sound, 18, 0.18f, 2300.0f, noise_state);
    AppendBootSilence(sound, 10);
    AppendBootChirp(sound, 820.0f, 1060.0f, 100, 0.28f, 0.012f, 2.85f);
    AppendBootSilence(sound, 8);
    AppendBootChirp(sound, 1060.0f, 900.0f, 210, 0.30f, 0.010f, 2.85f);

    // C3: system online.
    AppendBootClick(sound, 30, 0.25f, 1900.0f, noise_state);
    AppendBootSilence(sound, 12);
    AppendBootChirp(sound, 760.0f, 560.0f, 150, 0.31f, 0.008f, 2.90f);
    AppendBootSilence(sound, 18);
    AppendBootClick(sound, 20, 0.18f, 2700.0f, noise_state);
    AppendBootSilence(sound, 12);
    AppendBootChirp(sound, 610.0f, 1080.0f, 170, 0.35f, 0.014f, 2.90f);
    AppendBootSilence(sound, 15);
    AppendBootChirp(sound, 1080.0f, 820.0f, 150, 0.34f, 0.010f, 2.90f);
    AppendBootSilence(sound, 28);
    AppendBootTone(sound, 988.0f, 90, 0.25f);
    AppendBootSilence(sound, 10);
    AppendBootTone(sound, 1175.0f, 100, 0.27f);
    AppendBootSilence(sound, 10);
    AppendBootTone(sound, 1318.5f, 250, 0.34f);

    s_codec->SetOutputVolume(80);
    s_codec->SetOutputMuted(true);
    s_codec->EnableOutput(true);
    vTaskDelay(pdMS_TO_TICKS(160));
    s_codec->SetOutputMuted(false);
    vTaskDelay(pdMS_TO_TICKS(30));
    (void)s_codec->OutputData(sound.data(), sound.size());
    s_codec->SetOutputMuted(true);
    vTaskDelay(pdMS_TO_TICKS(70));
    s_codec->EnableOutput(false);
}

esp_err_t InitNvsForUi()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "NVS erase");
        err = nvs_flash_init();
    }
    return err;
}

void StartNetworkInBackground()
{
    const BaseType_t created = xTaskCreate(
        [](void*) {
            const esp_err_t err = beta_network::Init();
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Background network init returned: %s",
                         esp_err_to_name(err));
            }
            // No boot-time HTTPS probe. Wi-Fi state is already event-driven and
            // Settings reports the live connection/SSID when it becomes ready.
            vTaskDelete(nullptr);
        },
        "clanker_net_init", 8192, nullptr, 5, nullptr);

    if (created != pdPASS) {
        ESP_LOGW(kTag, "Could not create background network task");
    }
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "CLANKER Pocket fast boot");

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

    // Do local state/SD preparation while the splash is already visible. The
    // old fixed 1.8-second dead delay is intentionally gone.
    const esp_err_t nvs_err = InitNvsForUi();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(kTag, "NVS init returned: %s", esp_err_to_name(nvs_err));
    }

    const esp_err_t reader_err = beta_reader::Init();
    if (reader_err != ESP_OK) {
        ESP_LOGW(kTag, "Reader SD init returned: %s", esp_err_to_name(reader_err));
    }
    LoadUiState();

    if (InitAudio() != ESP_OK) {
        ESP_LOGE(kTag, "Audio init failed");
        return;
    }
    PlayOwlBootTune();

    // Song ends -> UI immediately. Network association happens only after the
    // interactive surface is coming up and never gates boot anymore.
    RenderUi();
    if (s_panel->RefreshFastBase() != ESP_OK) {
        ESP_LOGE(kTag, "Initial UI refresh failed");
        return;
    }
    StartNetworkInBackground();

    std::array<int, 4> last = {1,1,1,1};
    int64_t boot_down_us = 0;
    int64_t select_down_us = 0;
    int64_t up_down_us = 0;
    int64_t down_down_us = 0;
    int64_t up_last_repeat_us = 0;
    int64_t down_last_repeat_us = 0;
    bool select_consumed = false;
    bool boot_consumed = false;

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
        }

        if (select_now == 0 && boot_now == 1 && select_down_us != 0 &&
            !select_consumed &&
            (esp_timer_get_time() - select_down_us) >= kSelectLongPressIgnoreUs) {
            select_consumed = true;
            ESP_LOGI(kTag, "SELECT long press ignored");
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
            if (last[2] == 0 && select_now == 1) {
                if (!select_consumed && boot_now == 1) {
                    HandleSelectShort(false);
                }
                select_down_us = 0;
                select_consumed = false;
            }
            last[2] = select_now;

            const bool free_chat_scroll =
                s_ui_mode == UiMode::kChat &&
                s_ui_menu == UiMenu::kNone &&
                !s_settings_open && !s_power_menu;
            const int64_t repeat_delay_us =
                free_chat_scroll ? kChatScrollRepeatDelayUs
                                 : kDirectionRepeatDelayUs;
            const int64_t repeat_interval_us =
                free_chat_scroll ? kChatScrollRepeatIntervalUs
                                 : kDirectionRepeatIntervalUs;

            const bool direction_audible =
                !(s_ui_mode == UiMode::kRead &&
                  s_ui_menu == UiMenu::kNone &&
                  !s_settings_open && !s_power_menu);

            const int up_now = gpio_get_level(kButtonUp);
            const int64_t now_us = esp_timer_get_time();
            if (last[1] == 1 && up_now == 0) {
                up_down_us = now_us;
                up_last_repeat_us = now_us;
                HandleDirection(true, direction_audible);
            } else if (up_now == 0 && up_down_us != 0 &&
                       now_us - up_down_us >= repeat_delay_us &&
                       now_us - up_last_repeat_us >= repeat_interval_us) {
                up_last_repeat_us = now_us;
                HandleDirection(true, false);
            } else if (last[1] == 0 && up_now == 1) {
                if (free_chat_scroll) SaveUiState();
                up_down_us = 0;
                up_last_repeat_us = 0;
            }
            last[1] = up_now;

            const int down_now = gpio_get_level(kButtonDown);
            const int64_t now_down_us = esp_timer_get_time();
            if (last[3] == 1 && down_now == 0) {
                down_down_us = now_down_us;
                down_last_repeat_us = now_down_us;
                HandleDirection(false, direction_audible);
            } else if (down_now == 0 && down_down_us != 0 &&
                       now_down_us - down_down_us >= repeat_delay_us &&
                       now_down_us - down_last_repeat_us >= repeat_interval_us) {
                down_last_repeat_us = now_down_us;
                HandleDirection(false, false);
            } else if (last[3] == 0 && down_now == 1) {
                if (free_chat_scroll) SaveUiState();
                down_down_us = 0;
                down_last_repeat_us = 0;
            }
            last[3] = down_now;

            ServiceUiAudioIdle();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
