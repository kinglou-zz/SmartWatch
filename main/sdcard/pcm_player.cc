#include "pcm_player.h"
#include "audio_codec.h"
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#define TAG "PcmPlayer"
#define PCM_CHUNK_SAMPLES 512
#define PCM_SAMPLE_RATE 16000

// Match "1980.wav", "1980-7-15.wav", "1980_song.pcm"; reject "19801.wav"
static bool BasenameMatchesYear(const std::string& name, const char* year_str) {
    size_t dot = name.rfind('.');
    std::string base = (dot != std::string::npos) ? name.substr(0, dot) : name;
    size_t ylen = strlen(year_str);
    if (base.size() < ylen) return false;
    if (base.compare(0, ylen, year_str) != 0) return false;
    if (base.size() == ylen) return true;
    char next = base[ylen];
    return next == '-' || next == '_' || next == ' ';
}

PcmPlayer::PcmPlayer() {}

PcmPlayer::~PcmPlayer() {
    Stop();
}

void PcmPlayer::SetFileList(const std::vector<std::string>& files) {
    Stop();
    files_ = files;
    current_index_ = files_.empty() ? -1 : 0;
}

std::string PcmPlayer::GetCurrentFile() const {
    if (current_index_ < 0 || current_index_ >= (int)files_.size()) return "";
    return files_[current_index_];
}

void PcmPlayer::NextTrack() {
    if (files_.empty()) return;
    current_index_ = (current_index_ + 1) % files_.size();
}

void PcmPlayer::PrevTrack() {
    if (files_.empty()) return;
    current_index_--;
    if (current_index_ < 0) current_index_ = files_.size() - 1;
}

bool PcmPlayer::SelectByYear(int year) {
    if (files_.empty()) return false;

    char year_str[8];
    snprintf(year_str, sizeof(year_str), "%d", year);

    for (size_t i = 0; i < files_.size(); i++) {
        if (BasenameMatchesYear(files_[i], year_str)) {
            current_index_ = (int)i;
            ESP_LOGI(TAG, "Selected year %d → %s", year, files_[i].c_str());
            return true;
        }
    }
    ESP_LOGW(TAG, "No file matching year %d", year);
    return false;
}

bool PcmPlayer::CurrentMatchesYear(int year) const {
    char year_str[8];
    snprintf(year_str, sizeof(year_str), "%d", year);
    return BasenameMatchesYear(GetCurrentFile(), year_str);
}

void PcmPlayer::Play() {
    if (files_.empty() || !codec_ || current_index_ < 0) return;

    if (playing_) {
        // Stop current playback, then restart
        Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    playing_ = true;
    stop_requested_ = false;

    // Re-enable audio output (may have been disabled by AI chat)
    if (codec_) {
        codec_->EnableOutput(true);
    }

    if (on_status_change) {
        on_status_change(GetCurrentFile(), true);
    }

    xTaskCreate(PlayTaskEntry, "pcm_play", 4096, this, 5, &play_task_);
}

void PcmPlayer::Stop() {
    if (!playing_) return;
    stop_requested_ = true;

    // Wait for task to finish
    int timeout = 500;
    while (play_task_ != nullptr && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        timeout--;
    }
    playing_ = false;

    if (on_status_change) {
        on_status_change(GetCurrentFile(), false);
    }
}

void PcmPlayer::PlayTaskEntry(void* arg) {
    auto* player = static_cast<PcmPlayer*>(arg);
    player->PlayTask();
    player->play_task_ = nullptr;
    vTaskDelete(nullptr);
}

// Skip WAV header, return data start offset. Returns 0 if not WAV.
static long SkipWavHeader(FILE* fp) {
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) return 0;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        // Not a WAV file, rewind and treat as raw PCM
        rewind(fp);
        return 0;
    }

    // Scan chunks to find "data"
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, fp) == 8) {
        uint32_t chunk_size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);
        if (memcmp(chunk, "data", 4) == 0) {
            ESP_LOGI(TAG, "WAV data chunk at offset %ld, size=%lu", ftell(fp), chunk_size);
            return 0;  // data starts here
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            // Read fmt chunk for info
            uint8_t fmt[16];
            size_t n = fread(fmt, 1, (chunk_size < 16) ? chunk_size : 16, fp);
            if (n >= 14) {
                int channels = fmt[2] | (fmt[3] << 8);
                int rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
                int bps = fmt[14] | (fmt[15] << 8);
                ESP_LOGI(TAG, "WAV: %dHz %dch %dbit", rate, channels, bps);
            }
            // Skip remaining fmt bytes
            if (chunk_size > 16) fseek(fp, chunk_size - 16, SEEK_CUR);
        } else {
            // Skip this chunk
            fseek(fp, chunk_size, SEEK_CUR);
        }
    }
    return 0;  // "data" chunk not found, play from current position (unlikely)
}

void PcmPlayer::PlayTask() {
    std::string filepath = "/sdcard/" + GetCurrentFile();
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open: %s", filepath.c_str());
        playing_ = false;
        if (on_status_change) on_status_change(GetCurrentFile(), false);
        return;
    }

    // Detect and skip WAV header
    SkipWavHeader(fp);

    ESP_LOGI(TAG, "Playing: %s", filepath.c_str());
    std::vector<int16_t> buffer(PCM_CHUNK_SAMPLES);
    int chunk = 0;

    while (!stop_requested_) {
        size_t samples_read = fread(buffer.data(), sizeof(int16_t), PCM_CHUNK_SAMPLES, fp);
        if (samples_read == 0) break;  // EOF

        buffer.resize(samples_read);
        if (codec_) {
            codec_->OutputData(buffer);
        }
        buffer.resize(PCM_CHUNK_SAMPLES);
        chunk++;
    }

    fclose(fp);
    playing_ = false;

    if (stop_requested_) {
        ESP_LOGI(TAG, "Playback stopped");
    } else {
        ESP_LOGI(TAG, "Playback finished");
    }
    if (on_status_change) on_status_change(GetCurrentFile(), false);
}
