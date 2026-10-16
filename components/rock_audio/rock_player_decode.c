#include "rock_player_internal.h"
#include "audio_bsp.h"

#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <strings.h>

static const char *TAG = "rock-player";

esp_audio_type_t rock_decoder_detect_type(const char *content_type)
{
    if (!content_type) return ESP_AUDIO_TYPE_UNSUPPORT;

    if (strcasestr(content_type, "audio/mpeg") || strcasestr(content_type, "audio/mp3")
        || strcasestr(content_type, "audio/x-mpeg")
        || strcasestr(content_type, "application/octet-stream")) {
        return ESP_AUDIO_TYPE_MP3;
    }

    if (strcasestr(content_type, "audio/aac") || strcasestr(content_type, "audio/aacp")
        || strcasestr(content_type, "audio/x-aac") || strcasestr(content_type, "audio/mp4")) {
        return ESP_AUDIO_TYPE_AAC;
    }

    return ESP_AUDIO_TYPE_UNSUPPORT;
}

int rock_decoder_open(esp_audio_type_t type, esp_audio_dec_handle_t *out_dec)
{
    if (!out_dec) return -1;

    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_audio_dec_cfg_t dec_cfg = {
        .type = type,
        .cfg = NULL,
        .cfg_sz = 0,
    };

    if (type == ESP_AUDIO_TYPE_AAC) {
        // HE-AAC (aacPlus) radio streams decode only with SBR/PS enabled.
        aac_cfg.aac_plus_enable = true;
        dec_cfg.cfg = &aac_cfg;
        dec_cfg.cfg_sz = sizeof(aac_cfg);
    }

    if (esp_audio_dec_open(&dec_cfg, out_dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open audio decoder for type %d", type);
        *out_dec = NULL;
        return -1;
    }

    return 0;
}

void rock_decoder_close(esp_audio_dec_handle_t dec)
{
    if (dec) {
        esp_audio_dec_close(dec);
    }
}

void rock_format_gate_init(rock_format_gate_t *gate)
{
    if (!gate) return;
    gate->pending_fs = 0;
    gate->pending_ch = 0;
    gate->stable_count = 0;
    gate->decode_started_us = esp_timer_get_time();
    gate->last_reconfig_us = 0; // 0 means "never": rate limit will not delay first legitimate reconfig
}

static bool check_format_change(esp_audio_dec_handle_t dec,
                                uint32_t *current_fs, uint8_t *current_channels,
                                rock_format_gate_t *gate)
{
    esp_audio_dec_info_t info;
    if (esp_audio_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK) {
        return true;
    }

    if (info.sample_rate == *current_fs && info.channel == *current_channels) {
        // Live format reported again: the alternative lost the frame.
        gate->stable_count = 0;
        return true;
    }

    if (gate->pending_fs != info.sample_rate || gate->pending_ch != info.channel) {
        gate->pending_fs = info.sample_rate;
        gate->pending_ch = info.channel;
        gate->stable_count = 1;
    } else {
        gate->stable_count++;
    }

    int64_t now_us = esp_timer_get_time();
    bool stable = gate->stable_count >= FORMAT_STABLE_FRAMES;
    bool rate_ok = (now_us - gate->last_reconfig_us) >= FORMAT_RECONFIG_MIN_INTERVAL_MS * 1000;
    bool grace_ok = (now_us - gate->decode_started_us) >= FORMAT_INITIAL_GRACE_MS * 1000;

    if (stable && rate_ok && grace_ok) {
        ESP_LOGI(TAG, "Decoder format %lu Hz, %u ch stable (%u frames, reconfiguring codec)",
                 (unsigned long)info.sample_rate, info.channel, gate->stable_count);
        if (rock_audio_reconfig(info.sample_rate, info.channel) != 0) {
            ESP_LOGE(TAG, "rock_audio_reconfig failed, aborting local decode");
            return false;
        }
        rock_player_set_audio_format(info.sample_rate, info.channel);
        *current_fs = info.sample_rate;
        *current_channels = info.channel;
        gate->last_reconfig_us = now_us;
        gate->stable_count = 0;
    }

    return true;
}

int rock_decoder_step(esp_audio_dec_handle_t dec,
                      const char *station_id,
                      uint8_t *in_buf, size_t in_len,
                      uint8_t *out_buf,
                      uint32_t *current_fs, uint8_t *current_channels,
                      rock_format_gate_t *gate)
{
    size_t in_pos = 0;
    while (in_len > in_pos && !rock_player_is_stop_signaled()) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = in_buf + in_pos,
            .len = (uint32_t)(in_len - in_pos),
            .consumed = 0,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = out_buf,
            .len = DECODE_OUT_CAP,
            .needed_size = 0,
            .decoded_size = 0,
        };

        esp_audio_err_t dret = esp_audio_dec_process(dec, &raw, &out_frame);
        if (dret == ESP_AUDIO_ERR_OK || dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            in_pos += raw.consumed;
            if (out_frame.decoded_size > 0) {
                if (!check_format_change(dec, current_fs, current_channels, gate)) {
                    return -1;
                }

                size_t written = rock_pcm_ring_write(rock_player_get_pcm_ring(), out_buf, out_frame.decoded_size);
                rock_player_stat_add_pcm_bytes((uint32_t)written);

                rock_player_check_preroll(station_id);
            }
        } else if (dret == ESP_AUDIO_ERR_DATA_LACK) {
            // Need more data from the source
            break;
        } else {
            rock_player_stat_inc_decode_errors();
            in_pos++; // skip byte to resynchronize
        }
    }
    return (int)in_pos;
}
