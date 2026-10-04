#include "board_es8311_codec.h"

#include <array>
#include <cassert>
#include <esp_log.h>
#include <soc/soc_caps.h>

#define TAG "Es8311Codec"

Es8311Codec::Es8311Codec(void* i2c_master_handle, i2c_port_t i2c_port,
                                   int input_sample_rate, int output_sample_rate,
                                   gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                   gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin,
                                   uint8_t es8311_addr, bool use_mclk, bool pa_inverted) {
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    output_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    input_gain_ = 30;

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
        .clk_src = 0,  // 0 selects the default I2S clock source
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted_;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Es8311Codec");
    } else {
        ESP_LOGI(TAG, "Es8311Codec initialized");
    }
}

Es8311Codec::~Es8311Codec() {
    Shutdown();
    if (tx_handle_ != nullptr) {
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    if (rx_handle_ != nullptr) {
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311Codec::SetChannelsEnabled(bool enabled) {
    if (enabled == channels_enabled_) {
        return;
    }

    auto toggle_channel = [enabled](i2s_chan_handle_t handle, const char* channel_name) {
        if (handle == nullptr) {
            return;
        }

        esp_err_t err =
            enabled ? i2s_channel_enable(handle) : i2s_channel_disable(handle);
        if (err == ESP_OK) {
            return;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "%s channel already %s", channel_name,
                     enabled ? "enabled" : "disabled");
            return;
        }
        ESP_ERROR_CHECK(err);
    };

    toggle_channel(tx_handle_, "TX");
    toggle_channel(rx_handle_, "RX");
    channels_enabled_ = enabled;
}

void Es8311Codec::UpdatePaState() {
    if (pa_pin_ == GPIO_NUM_NC) {
        return;
    }
    int level = output_enabled_ ? 1 : 0;
    gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
}

void Es8311Codec::EnsurePlaybackDevice() {
    if (playback_dev_ != nullptr || codec_if_ == nullptr || data_if_ == nullptr) return;

    SetChannelsEnabled(true);
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    playback_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(playback_dev_ != nullptr);

    // Waveshare's reference playback path for this board is stereo. Keeping
    // playback and record devices separate lets the microphone remain mono.
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = static_cast<uint32_t>(output_sample_rate_),
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(playback_dev_, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(playback_dev_, output_volume_));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(playback_dev_, output_muted_));
    ESP_LOGI(TAG, "Playback device opened stereo at %d Hz", output_sample_rate_);
}

void Es8311Codec::EnsureRecordDevice() {
    if (record_dev_ != nullptr || codec_if_ == nullptr || data_if_ == nullptr) return;

    SetChannelsEnabled(true);
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    record_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(record_dev_ != nullptr);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = static_cast<uint32_t>(input_sample_rate_),
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(record_dev_, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(record_dev_, input_gain_));
    ESP_LOGI(TAG, "Record device opened mono at %d Hz", input_sample_rate_);
}

void Es8311Codec::UpdateDeviceState() {
    if (output_enabled_) EnsurePlaybackDevice();
    if (input_enabled_) EnsureRecordDevice();
    UpdatePaState();
}

void Es8311Codec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                            gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        // Keep the I2S power domain up across light sleep. Enabling this would trade RAM
        // for the register backup/restore needed to power it down.
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        // Field order below must match the struct declaration order -- C++ designated
        // initializers may skip fields but not reorder them.
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            // Only consulted when clk_src is I2S_CLK_SRC_EXTERNAL, which it is not.
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            // IDF's default. Only takes effect in slave role; this channel is master.
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            // Matches I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG. Alignment only matters when the
            // slot is wider than the data; here both are 16-bit, so this is a no-op.
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    SetChannelsEnabled(true);
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8311Codec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    AudioCodec::SetInputGain(gain);
    if (record_dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(record_dev_, input_gain_));
    }
}

void Es8311Codec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    AudioCodec::SetOutputVolume(volume);
    if (playback_dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(playback_dev_, output_volume_));
    }
}

void Es8311Codec::SetOutputMuted(bool muted) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    AudioCodec::SetOutputMuted(muted);
    if (playback_dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(playback_dev_, output_muted_));
    }
}

void Es8311Codec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311Codec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311Codec::Read(int16_t* dest, int samples) {
    esp_codec_dev_handle_t dev = nullptr;
    {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        if (!input_enabled_ || record_dev_ == nullptr) return 0;
        dev = record_dev_;
    }

    const int ret =
        esp_codec_dev_read(dev, static_cast<void*>(dest), samples * sizeof(int16_t));
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Audio read failed: %d", ret);
        return 0;
    }
    return samples;
}

int Es8311Codec::Write(const int16_t* data, int samples) {
    esp_codec_dev_handle_t dev = nullptr;
    {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        if (!output_enabled_ || playback_dev_ == nullptr || data == nullptr ||
            samples <= 0) {
            return 0;
        }
        dev = playback_dev_;
    }

    // The application generates mono PCM, while this board's proven playback
    // path is stereo. Duplicate each frame into L/R and feed the codec in small
    // DMA-friendly chunks instead of one giant write.
    constexpr int kMonoFramesPerChunk = 64;  // 256 stereo bytes at 16-bit.
    std::array<int16_t, kMonoFramesPerChunk * 2> stereo = {};

    int consumed = 0;
    while (consumed < samples) {
        const int frames = std::min(kMonoFramesPerChunk, samples - consumed);
        for (int i = 0; i < frames; ++i) {
            const int16_t sample = data[consumed + i];
            stereo[static_cast<size_t>(i) * 2] = sample;
            stereo[static_cast<size_t>(i) * 2 + 1] = sample;
        }

        const int bytes = frames * 2 * static_cast<int>(sizeof(int16_t));
        const int ret = esp_codec_dev_write(dev, stereo.data(), bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Audio write failed after %d/%d samples: %d",
                     consumed, samples, ret);
            return consumed;
        }
        consumed += frames;
    }
    return consumed;
}

void Es8311Codec::Shutdown() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);

    if (playback_dev_ != nullptr) {
        (void)esp_codec_dev_set_out_mute(playback_dev_, true);
        (void)esp_codec_dev_set_out_vol(playback_dev_, 0);
        esp_codec_dev_close(playback_dev_);
        esp_codec_dev_delete(playback_dev_);
        playback_dev_ = nullptr;
    }

    if (record_dev_ != nullptr) {
        esp_codec_dev_close(record_dev_);
        esp_codec_dev_delete(record_dev_);
        record_dev_ = nullptr;
    }

    output_muted_ = true;
    output_enabled_ = false;
    input_enabled_ = false;
    UpdatePaState();

    if (channels_enabled_) SetChannelsEnabled(false);
    ESP_LOGI(TAG, "ES8311 audio shutdown complete");
}
