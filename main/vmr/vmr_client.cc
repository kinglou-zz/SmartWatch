#include "vmr_client.h"
#include "vmr_stream.h"
#include "board.h"
#include "settings.h"
#include "audio_codec.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <climits>
#include <cmath>
#include <algorithm>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

#define TAG "VmrClient"

namespace {

struct VmrWifiHold {
    VmrClient* self;
    explicit VmrWifiHold(VmrClient* s) : self(s) { self->AcquireWifiHold(); }
    ~VmrWifiHold() { self->ReleaseWifiHold(); }
    VmrWifiHold(const VmrWifiHold&) = delete;
    VmrWifiHold& operator=(const VmrWifiHold&) = delete;
};

}  // namespace

// Best cloud wall-clock key available on the item (no position — unreliable).
static std::string CloudTimeKey(const VmrQueueItem& it) {
    if (!it.scheduled_at.empty()) return it.scheduled_at;
    if (!it.created_at.empty()) return it.created_at;
    return "";
}

// True if CloudTimeKey looks older than ~2 days (ISO8601 prefix compare).
static bool IsStaleCloudTime(const VmrQueueItem& it) {
    const std::string key = CloudTimeKey(it);
    if (key.size() < 10) return false;
    time_t now = time(nullptr);
    if (now < 1700000000) return false;  // clock not synced yet
    time_t cutoff = now - 2 * 24 * 3600;
    struct tm tm_cut = {};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    gmtime_r(&cutoff, &tm_cut);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm_cut.tm_year + 1900, tm_cut.tm_mon + 1, tm_cut.tm_mday);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return key.substr(0, 10) < buf;
}

static bool IsCloudNewer(const VmrQueueItem& a, const VmrQueueItem& b) {
    const std::string ka = CloudTimeKey(a);
    const std::string kb = CloudTimeKey(b);
    if (!ka.empty() && !kb.empty() && ka != kb) {
        return ka > kb;
    }
    return false;
}

// Play order per product: immediate > scheduled > voice.
// Cloud often sets scheduled.priority=20 > immediate.priority=10 — sorting by
// raw priority alone buries "尽快提醒我" behind ancient scheduled backlog.
static int PlayTypeRank(const VmrQueueItem& it) {
    const std::string& t = it.message_type;
    const std::string& k = it.queue_kind;
    if (t.find("immediate") != std::string::npos || k == "immediate") return 300;
    if (t.find("scheduled") != std::string::npos || k == "scheduled") return 200;
    if (t.find("voice") != std::string::npos || k == "original") return 100;
    return 50 + it.priority;
}

static bool ShouldPlayBefore(const VmrQueueItem& a, const VmrQueueItem& b) {
    const int ra = PlayTypeRank(a);
    const int rb = PlayTypeRank(b);
    if (ra != rb) return ra > rb;
    if (a.priority != b.priority) return a.priority > b.priority;
    return IsCloudNewer(a, b);
}

/// True if a should rank above b for "latest replay" when cloud times are tied/missing.
static bool IsReplayNewer(const VmrQueueItem& a, int ord_a,
                          const VmrQueueItem& b, int ord_b,
                          const std::string& last_played_id) {
    if (IsCloudNewer(a, b)) return true;
    if (IsCloudNewer(b, a)) return false;
    // Prefer fresher (non-stale) over stale-dated when one side has a time
    const std::string ka = CloudTimeKey(a);
    const std::string kb = CloudTimeKey(b);
    if (ka.empty() != kb.empty()) {
        if (!ka.empty() && IsStaleCloudTime(a) && kb.empty()) return false;
        if (!kb.empty() && IsStaleCloudTime(b) && ka.empty()) return true;
    }
    if (ord_a != ord_b) return ord_a > ord_b;
    if (!last_played_id.empty()) {
        if (a.message_id == last_played_id) return true;
        if (b.message_id == last_played_id) return false;
    }
    return false;
}

static void FillQueueItemFromJson(cJSON* item, VmrQueueItem& qi) {
    auto mid = cJSON_GetObjectItem(item, "message_id");
    if (cJSON_IsString(mid)) qi.message_id = mid->valuestring;
    auto aurl = cJSON_GetObjectItem(item, "audio_url");
    if (cJSON_IsString(aurl)) qi.audio_url = aurl->valuestring;
    auto pri = cJSON_GetObjectItem(item, "priority");
    if (cJSON_IsNumber(pri)) qi.priority = pri->valueint;
    auto mtype = cJSON_GetObjectItem(item, "message_type");
    if (cJSON_IsString(mtype)) qi.message_type = mtype->valuestring;
    auto qkind = cJSON_GetObjectItem(item, "queue_kind");
    if (cJSON_IsString(qkind)) qi.queue_kind = qkind->valuestring;
    auto pos = cJSON_GetObjectItem(item, "position");
    if (cJSON_IsNumber(pos)) qi.position = pos->valueint;
    auto sat = cJSON_GetObjectItem(item, "scheduled_at");
    if (cJSON_IsString(sat)) qi.scheduled_at = sat->valuestring;
    auto cat = cJSON_GetObjectItem(item, "created_at");
    if (cJSON_IsString(cat)) qi.created_at = cat->valuestring;
    if (qi.created_at.empty()) {
        auto upd = cJSON_GetObjectItem(item, "updated_at");
        if (cJSON_IsString(upd)) qi.created_at = upd->valuestring;
    }
    if (qi.created_at.empty()) {
        auto enq = cJSON_GetObjectItem(item, "enqueued_at");
        if (cJSON_IsString(enq)) qi.created_at = enq->valuestring;
    }
}

// ============================================================
// WAV header helpers (reused from pcm_player.cc pattern)
// ============================================================

struct WavHeader {
    int16_t audio_format = 0;
    int16_t num_channels = 0;
    int32_t sample_rate = 0;
    int32_t byte_rate = 0;
    int16_t block_align = 0;
    int16_t bits_per_sample = 0;
    uint32_t data_size = 0;
    long data_offset = 0;
};

// ============================================================
// Construction / Destruction
// ============================================================

VmrClient::VmrClient() {
    play_done_sem_ = xSemaphoreCreateBinary();
}

VmrClient::~VmrClient() {
    StopPolling();
    StopPlayback();
    if (play_done_sem_ != nullptr) {
        vSemaphoreDelete(play_done_sem_);
        play_done_sem_ = nullptr;
    }
}

// ============================================================
// Lifecycle
// ============================================================

void VmrClient::Initialize(const std::string& device_id,
                           const std::string& token,
                           const std::string& device_uid,
                           const std::string& server_url) {
    device_id_ = device_id;
    device_uid_ = device_uid;
    server_url_ = server_url;

    // Ensure token has "Bearer " prefix
    if (!token.empty() && token.find(" ") == std::string::npos) {
        token_ = "Bearer " + token;
    } else {
        token_ = token;
    }

    // Get audio codec for playback
    codec_ = Board::GetInstance().GetAudioCodec();

    LoadPlayedIds();

    ESP_LOGI(TAG, "Initialized: device_id=%s, device_uid=%s, server=%s, played_ids=%d",
             device_id_.c_str(), device_uid_.c_str(), server_url_.c_str(), (int)played_ids_.size());
}

void VmrClient::StartPolling() {
    if (polling_) return;
    polling_ = true;

    // Create periodic timer
    esp_timer_create_args_t timer_args = {
        .callback = PollTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vmr_poll",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &poll_timer_);
    esp_timer_start_periodic(poll_timer_, POLL_INTERVAL_SEC * 1000000ULL);

    ESP_LOGI(TAG, "Polling started (every %ds)", POLL_INTERVAL_SEC);

    // Do an immediate poll
    PollNow();
}

void VmrClient::StopPolling() {
    if (!polling_) return;
    polling_ = false;
    poll_suspended_ = false;

    if (catchup_timer_ != nullptr) {
        esp_timer_stop(catchup_timer_);
        esp_timer_delete(catchup_timer_);
        catchup_timer_ = nullptr;
    }
    catchup_remaining_ = 0;

    if (poll_timer_ != nullptr) {
        esp_timer_stop(poll_timer_);
        esp_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
    ESP_LOGI(TAG, "Polling stopped");
}

void VmrClient::SetPollingSuspended(bool suspended) {
    bool prev = poll_suspended_.exchange(suspended);
    if (prev == suspended) {
        return;
    }
    ESP_LOGI(TAG, "Polling %s (avoid contending with WS audio)",
             suspended ? "suspended" : "resumed");
    // Catch up immediately after TTS: a 20s reminder can become due while
    // speaking, and waiting for the next periodic tick would miss it.
    if (!suspended) {
        PollNow();
    }
}

void VmrClient::SetNetworkReady(bool ready) {
    bool prev = network_ready_.exchange(ready);
    if (prev == ready) {
        return;
    }
    ESP_LOGI(TAG, "Network %s for VMR HTTP", ready ? "ready" : "down");
}

void VmrClient::AcquireWifiHold() {
    if (wifi_hold_count_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
}

void VmrClient::ReleaseWifiHold() {
    wifi_hold_count_.fetch_sub(1, std::memory_order_acq_rel);
}

void VmrClient::PollNow() {
    if (!network_ready_.load()) {
        ESP_LOGD(TAG, "Poll skipped (network down)");
        return;
    }
    if (poll_suspended_.load()) {
        ESP_LOGD(TAG, "Poll skipped (suspended)");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (poll_in_progress_) {
            // ReadAll/Open can hang under Wi-Fi power-save; without this the
            // flag stays true forever and every later reminder poll is skipped.
            const int64_t now = esp_timer_get_time();
            if (poll_started_us_ > 0 && (now - poll_started_us_) > 25000000LL) {
                ESP_LOGW(TAG, "Poll watchdog: clearing stuck in-progress after %lld ms",
                         (long long)((now - poll_started_us_) / 1000));
                poll_in_progress_ = false;
            } else {
                return;
            }
        }
        poll_in_progress_ = true;
        poll_started_us_ = esp_timer_get_time();
    }

    // Larger stack: queue JSON can be large; 4KB was easy to overflow silently.
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        if (!self->network_ready_.load()) {
            ESP_LOGI(TAG, "Poll task aborted (network down before HTTP)");
        } else if (!self->poll_suspended_.load()) {
            self->DoPoll();
        } else {
            ESP_LOGI(TAG, "Poll task aborted (suspended before HTTP)");
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->poll_in_progress_ = false;
            self->poll_started_us_ = 0;
        }
        vTaskDelete(nullptr);
    }, "vmr_poll", 8192, this, 5, nullptr);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create vmr_poll task");
        std::lock_guard<std::mutex> lock(mutex_);
        poll_in_progress_ = false;
        poll_started_us_ = 0;
    }
}

void VmrClient::ArmReminderCatchupPolls() {
    // 5s × 18 ≈ 90s — covers short "remind me in 20s" after voice confirm.
    catchup_remaining_ = 18;
    if (catchup_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = CatchupTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vmr_catchup",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &catchup_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create catchup timer");
            catchup_remaining_ = 0;
            PollNow();
            return;
        }
    }
    esp_timer_stop(catchup_timer_);
    esp_err_t err = esp_timer_start_periodic(catchup_timer_, 5 * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start catchup timer: %s", esp_err_to_name(err));
        catchup_remaining_ = 0;
    } else {
        ESP_LOGI(TAG, "Reminder catch-up polls armed (every 5s × %d)", catchup_remaining_);
    }
    PollNow();
}

// ============================================================
// Timer callback (runs in ESP_TIMER_TASK)
// ============================================================

void VmrClient::PollTimerCallback(void* arg) {
    auto* self = static_cast<VmrClient*>(arg);
    self->PollNow();
}

void VmrClient::CatchupTimerCallback(void* arg) {
    auto* self = static_cast<VmrClient*>(arg);
    if (self->catchup_remaining_ <= 0) {
        if (self->catchup_timer_ != nullptr) {
            esp_timer_stop(self->catchup_timer_);
        }
        return;
    }
    self->catchup_remaining_--;
    self->PollNow();
    if (self->catchup_remaining_ <= 0 && self->catchup_timer_ != nullptr) {
        esp_timer_stop(self->catchup_timer_);
        ESP_LOGI(TAG, "Reminder catch-up polls finished");
    }
}

// ============================================================
// Internal: HTTP helpers
// ============================================================

bool VmrClient::HttpGet(const std::string& url, std::string& response_body) {
    VmrWifiHold wifi_hold(this);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    auto http = network->CreateHttp(kHttpIdPoll);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetHeader("Authorization", token_.c_str());
    if (!device_uid_.empty()) {
        http->SetHeader("Device-Id", device_uid_.c_str());
    }
    http->SetHeader("Accept", "application/json");
    http->SetTimeout(15000);

    ESP_LOGI(TAG, "HTTP GET %s", url.c_str());
    if (!http->Open("GET", url.c_str())) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", url.c_str());
        http->Close();
        return false;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP GET %s → %d", url.c_str(), status);
        http->Close();
        return false;
    }

    response_body = http->ReadAll();
    http->Close();
    if (response_body.empty()) {
        ESP_LOGE(TAG, "HTTP GET %s → %d but empty body (truncated/incomplete)", url.c_str(), status);
        return false;
    }
    ESP_LOGI(TAG, "HTTP GET %s → %d (%zu bytes)", url.c_str(), status, response_body.size());
    return true;
}

bool VmrClient::HttpPost(const std::string& url, const std::string& body, std::string& response_body) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    auto http = network->CreateHttp(kHttpIdPoll);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetHeader("Authorization", token_.c_str());
    if (!device_uid_.empty()) {
        http->SetHeader("Device-Id", device_uid_.c_str());
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetTimeout(10000);
    // SetContent must come BEFORE Open (matches OTA pattern)
    http->SetContent(std::string(body));

    ESP_LOGI(TAG, "HTTP POST %s", url.c_str());
    if (!http->Open("POST", url.c_str())) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", url.c_str());
        http->Close();
        return false;
    }

    int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP POST %s → %d", url.c_str(), status);
        http->Close();
        return false;
    }

    response_body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "HTTP POST %s → %d (%zu bytes)", url.c_str(), status, response_body.size());
    return true;
}

bool VmrClient::DownloadToBuffer(const std::string& url, std::vector<uint8_t>& buffer) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network not available for download");
        return false;
    }

    VmrWifiHold wifi_hold(this);
    auto http = network->CreateHttp(kHttpIdBg);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client for download");
        return false;
    }

    http->SetHeader("Authorization", token_.c_str());
    if (!device_uid_.empty()) {
        http->SetHeader("Device-Id", device_uid_.c_str());
    }
    http->SetTimeout(120000);  // slow cloud links need well over 60s for ~150KB wav

    ESP_LOGI(TAG, "Downloading %s", url.c_str());
    if (!http->Open("GET", url.c_str())) {
        ESP_LOGE(TAG, "Download open failed: %s", url.c_str());
        return false;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Download HTTP %d for %s", status, url.c_str());
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();

    // Stream into memory buffer
    buffer.clear();
    if (content_length > 0) {
        buffer.reserve(content_length);
    }
    char chunk[2048];
    size_t total = 0;
    while (true) {
        int ret = http->Read(chunk, sizeof(chunk));
        if (ret < 0) {
            ESP_LOGW(TAG, "Download read error (%d) after %u bytes", ret, (unsigned)total);
            break;
        }
        if (ret == 0) break;
        buffer.insert(buffer.end(), chunk, chunk + ret);
        total += ret;
    }

    http->Close();
    ESP_LOGI(TAG, "Downloaded %u bytes (Content-Length=%u)",
             (unsigned)total, (unsigned)content_length);

    if (total == 0) {
        buffer.clear();
        return false;
    }
    if (content_length > 0 && total < content_length) {
        ESP_LOGW(TAG, "Download truncated: got %u / %u bytes",
                 (unsigned)total, (unsigned)content_length);
        buffer.clear();
        return false;
    }
    if (!IsWavBufferComplete(buffer)) {
        ESP_LOGW(TAG, "Downloaded WAV incomplete/corrupt (%u bytes)", (unsigned)total);
        buffer.clear();
        return false;
    }
    return true;
}

// ============================================================
// SD cache
// ============================================================

bool VmrClient::IsSdAvailable() const {
    struct stat st;
    if (stat("/sdcard", &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

std::string VmrClient::SdPathFor(const std::string& message_id) const {
    return std::string(SD_VMR_DIR) + "/" + message_id + ".wav";
}

bool VmrClient::EnsureSdVmrDir() const {
    if (!IsSdAvailable()) return false;
    struct stat st;
    if (stat(SD_VMR_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    if (mkdir(SD_VMR_DIR, 0755) == 0) return true;
    // Race: another task created it
    return (stat(SD_VMR_DIR, &st) == 0 && S_ISDIR(st.st_mode));
}

bool VmrClient::IsSdCacheReady(const std::string& message_id) const {
    if (!IsSdAvailable() || message_id.empty()) return false;
    return VmrProbeWavFileComplete(SdPathFor(message_id).c_str());
}

const char* VmrClient::PlaybackModeName(VmrPlaybackMode mode) {
    switch (mode) {
        case VmrPlaybackMode::kStreamWhileDownload: return "stream_while_download";
        case VmrPlaybackMode::kStreamWhileRead: return "stream_while_read";
        case VmrPlaybackMode::kStreamWhileWrite: return "stream_while_write";
        case VmrPlaybackMode::kDownloadThenPlay: return "download_then_play";
    }
    return "unknown";
}

VmrPlaybackMode VmrClient::SelectPlaybackMode(const std::string& message_id,
                                              const std::string& audio_url) const {
    if (IsSdCacheReady(message_id)) {
        return VmrPlaybackMode::kStreamWhileRead;
    }
    if (!audio_url.empty()) {
        return VmrPlaybackMode::kStreamWhileDownload;
    }
    return VmrPlaybackMode::kDownloadThenPlay;
}

bool VmrClient::IsWavBufferComplete(const std::vector<uint8_t>& buffer) const {
    const uint8_t* data = buffer.data();
    size_t total = buffer.size();
    if (total < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t pos = 12;
    while (pos + 8 <= total) {
        uint32_t chunk_size = data[pos + 4] | (data[pos + 5] << 8) |
                              (data[pos + 6] << 16) | (data[pos + 7] << 24);
        if (memcmp(data + pos, "data", 4) == 0) {
            size_t data_offset = pos + 8;
            // Header claims more PCM than we actually have → truncated download/cache
            if (data_offset > total) return false;
            if ((uint64_t)data_offset + chunk_size > (uint64_t)total) {
                ESP_LOGW(TAG, "WAV truncated: have %u bytes, data needs %u+%u",
                         (unsigned)total, (unsigned)data_offset, (unsigned)chunk_size);
                return false;
            }
            // Reject empty / tiny PCM payloads
            return chunk_size >= 64;
        }
        // Prevent infinite loop on corrupt sizes
        if (chunk_size > total) return false;
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    return false;
}

void VmrClient::InvalidateSdAudio(const std::string& message_id) const {
    if (message_id.empty() || !IsSdAvailable()) return;
    std::string path = SdPathFor(message_id);
    if (unlink(path.c_str()) == 0) {
        ESP_LOGW(TAG, "Removed incomplete SD cache %s", path.c_str());
    }
    unlink((path + ".tmp").c_str());
}

bool VmrClient::LoadAudioFromSd(const std::string& message_id, std::vector<uint8_t>& buffer) const {
    if (!IsSdAvailable() || message_id.empty()) return false;
    std::string path = SdPathFor(message_id);
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return false;
    }
    rewind(fp);
    buffer.resize((size_t)sz);
    size_t n = fread(buffer.data(), 1, buffer.size(), fp);
    fclose(fp);
    if (n != buffer.size()) {
        buffer.clear();
        return false;
    }
    if (!IsWavBufferComplete(buffer)) {
        ESP_LOGW(TAG, "SD cache incomplete %s (%u bytes)", path.c_str(), (unsigned)n);
        buffer.clear();
        InvalidateSdAudio(message_id);
        return false;
    }
    ESP_LOGI(TAG, "SD load %s (%u bytes)", path.c_str(), (unsigned)n);
    return true;
}

bool VmrClient::SaveAudioToSd(const std::string& message_id, const std::vector<uint8_t>& buffer) const {
    if (!IsSdAvailable() || message_id.empty() || buffer.empty()) return false;
    if (!IsWavBufferComplete(buffer)) {
        ESP_LOGW(TAG, "Refuse to cache incomplete WAV for %s (%u bytes)",
                 message_id.c_str(), (unsigned)buffer.size());
        return false;
    }
    if (!EnsureSdVmrDir()) {
        ESP_LOGW(TAG, "Cannot create %s", SD_VMR_DIR);
        return false;
    }
    std::string path = SdPathFor(message_id);
    std::string tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) {
        ESP_LOGW(TAG, "SD write open failed: %s errno=%d", tmp.c_str(), errno);
        // Fallback: write final path directly (some FAT configs dislike .tmp suffix)
        fp = fopen(path.c_str(), "wb");
        if (!fp) {
            ESP_LOGW(TAG, "SD write open failed: %s errno=%d", path.c_str(), errno);
            return false;
        }
        size_t n = fwrite(buffer.data(), 1, buffer.size(), fp);
        fclose(fp);
        if (n != buffer.size()) {
            unlink(path.c_str());
            return false;
        }
        ESP_LOGI(TAG, "SD saved %s (%u bytes)", path.c_str(), (unsigned)n);
        return true;
    }
    size_t n = fwrite(buffer.data(), 1, buffer.size(), fp);
    fclose(fp);
    if (n != buffer.size()) {
        unlink(tmp.c_str());
        return false;
    }
    unlink(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        // rename failed — try copy via direct write
        unlink(tmp.c_str());
        return false;
    }
    ESP_LOGI(TAG, "SD saved %s (%u bytes)", path.c_str(), (unsigned)n);
    return true;
}

bool VmrClient::AcquireAudio(const std::string& message_id, const std::string& audio_url,
                             std::vector<uint8_t>& buffer) {
    // Fast path: complete SD cache must NOT wait behind background SD-sync downloads.
    if (IsSdCacheReady(message_id) && LoadAudioFromSd(message_id, buffer)) {
        return true;
    }
    if (audio_url.empty()) {
        ESP_LOGE(TAG, "No SD cache and empty audio_url for %s", message_id.c_str());
        return false;
    }

    // If prefetch is already pulling this id, wait briefly for SD — do not stack a second download.
    for (int i = 0; i < 30 && prefetch_in_progress_.load(); i++) {
        if (IsSdCacheReady(message_id) && LoadAudioFromSd(message_id, buffer)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Ask any background holder (prefetch/sync) to release; never block play forever.
    audio_fetch_abort_.store(true);
    bool locked = false;
    for (int waited = 0; waited < 2500; waited += 50) {
        if (audio_fetch_mutex_.try_lock()) {
            locked = true;
            break;
        }
        if (IsSdCacheReady(message_id) && LoadAudioFromSd(message_id, buffer)) {
            audio_fetch_abort_.store(false);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!locked) {
        ESP_LOGW(TAG, "AcquireAudio: fetch lock busy → direct buffer download for play");
        audio_fetch_abort_.store(false);
        for (int attempt = 1; attempt <= 2; attempt++) {
            ESP_LOGI(TAG, "Downloading message %s (attempt %d, no-lock) from %s",
                     message_id.c_str(), attempt, audio_url.c_str());
            if (DownloadToBuffer(audio_url, buffer)) {
                return true;
            }
            buffer.clear();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return false;
    }

    std::lock_guard<std::mutex> fetch_lock(audio_fetch_mutex_, std::adopt_lock);
    audio_fetch_abort_.store(false);

    // Another task may have finished writing while we waited for the lock
    if (IsSdCacheReady(message_id) && LoadAudioFromSd(message_id, buffer)) {
        return true;
    }

    // Prefer streaming to SD (no full-file RAM) when card is present.
    // IMPORTANT: call Locked variant — we already hold audio_fetch_mutex_.
    if (IsSdAvailable()) {
        if (DownloadToSdFileLocked(message_id, audio_url) &&
            LoadAudioFromSd(message_id, buffer)) {
            return true;
        }
    }

    // Fallback: whole-buffer download (no SD or SD write failed)
    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "Downloading message %s (attempt %d) from %s",
                 message_id.c_str(), attempt, audio_url.c_str());
        if (DownloadToBuffer(audio_url, buffer)) {
            if (IsSdAvailable()) {
                SaveAudioToSd(message_id, buffer);
            }
            return true;
        }
        buffer.clear();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

bool VmrClient::DownloadToSdFile(const std::string& message_id, const std::string& audio_url) {
    if (!IsSdAvailable() || message_id.empty() || audio_url.empty()) return false;
    if (!EnsureSdVmrDir()) return false;

    std::lock_guard<std::mutex> fetch_lock(audio_fetch_mutex_);
    return DownloadToSdFileLocked(message_id, audio_url);
}

bool VmrClient::DownloadToSdFileLocked(const std::string& message_id, const std::string& audio_url) {
    // Caller must hold audio_fetch_mutex_
    if (IsSdCacheReady(message_id)) return true;
    if (!EnsureSdVmrDir()) return false;
    // Never contend with live play HTTP on EspTcp — even with separate connect
    // ids, abort storms and Wi-Fi contention still cause play bytes=0.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_) {
            ESP_LOGD(TAG, "SD download skipped (playback active): %s", message_id.c_str());
            return false;
        }
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp(kHttpIdBg);
    if (http == nullptr) return false;

    http->SetHeader("Authorization", token_.c_str());
    if (!device_uid_.empty()) {
        http->SetHeader("Device-Id", device_uid_.c_str());
    }
    http->SetTimeout(20000);

    ESP_LOGI(TAG, "SD stream-download %s ← %s", message_id.c_str(), audio_url.c_str());
    if (!http->Open("GET", audio_url.c_str())) {
        ESP_LOGE(TAG, "SD download open failed");
        return false;
    }
    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "SD download HTTP %d", status);
        http->Close();
        return false;
    }

    std::string path = SdPathFor(message_id);
    std::string tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) {
        fp = fopen(path.c_str(), "wb");
        if (!fp) {
            http->Close();
            return false;
        }
        tmp = path;  // writing final path directly
    }

    char chunk[kIoChunkBytes];
    size_t total = 0;
    size_t content_length = http->GetBodyLength();
    bool ok = true;
    int64_t t0 = esp_timer_get_time();
    while (ok) {
        if (audio_fetch_abort_.load()) {
            ESP_LOGW(TAG, "SD stream-download aborted (%u bytes) for play priority",
                     (unsigned)total);
            ok = false;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Yield if playback started for a different message
            if (playing_ && !current_message_id_.empty() &&
                current_message_id_ != message_id) {
                ESP_LOGW(TAG, "SD stream-download yield to play %s",
                         current_message_id_.c_str());
                ok = false;
                break;
            }
        }
        int ret = http->Read(chunk, sizeof(chunk));
        if (ret < 0) {
            ok = false;
            break;
        }
        if (ret == 0) break;
        if (fwrite(chunk, 1, (size_t)ret, fp) != (size_t)ret) {
            ok = false;
            break;
        }
        total += (size_t)ret;
        if ((total & 0x7FFF) == 0) {  // ~32KB
            ESP_LOGI(TAG, "SD stream-download progress %u/%u",
                     (unsigned)total, (unsigned)content_length);
        }
        // Hard cap: don't hold the fetch lock for more than 45s
        if (esp_timer_get_time() - t0 > 45000000LL) {
            ESP_LOGW(TAG, "SD stream-download timeout (%u bytes)", (unsigned)total);
            ok = false;
            break;
        }
    }
    fclose(fp);
    http->Close();

    if (!ok || total == 0 || (content_length > 0 && total < content_length)) {
        ESP_LOGW(TAG, "SD stream-download failed/truncated (%u bytes)", (unsigned)total);
        unlink(tmp.c_str());
        if (tmp != path) unlink(path.c_str());
        return false;
    }

    if (tmp != path) {
        unlink(path.c_str());
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            return false;
        }
    }

    if (!IsSdCacheReady(message_id)) {
        ESP_LOGW(TAG, "SD stream-download incomplete WAV %s", path.c_str());
        InvalidateSdAudio(message_id);
        return false;
    }
    ESP_LOGI(TAG, "SD stream-download ready %s (%u bytes)", path.c_str(), (unsigned)total);
    return true;
}

void VmrClient::RememberCatalogItem(const VmrQueueItem& item) {
    if (item.message_id.empty() || item.audio_url.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    catalog_[item.message_id] = item;
}

void VmrClient::ScheduleSdSync() {
    if (!IsSdAvailable()) return;
    bool expected = false;
    if (!sd_sync_in_progress_.compare_exchange_strong(expected, true)) {
        return;
    }
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        self->RunSdSync();
        self->sd_sync_in_progress_.store(false);
        vTaskDelete(nullptr);
    }, "vmr_sd_sync", 8192, this, 3, nullptr);
    if (ok != pdPASS) {
        sd_sync_in_progress_.store(false);
        ESP_LOGW(TAG, "Failed to start SD sync task");
    }
}

void VmrClient::PruneSdCacheLocked() {
    // Build keep-set: all unread in catalog + last SD_KEEP_READ_COUNT played ids
    std::set<std::string> keep;
    for (const auto& kv : catalog_) {
        if (!HasBeenPlayed(kv.first)) {
            keep.insert(kv.first);
        }
    }
    int kept_read = 0;
    for (int i = (int)played_order_.size() - 1; i >= 0 && kept_read < SD_KEEP_READ_COUNT; i--) {
        keep.insert(played_order_[i]);
        kept_read++;
    }

    DIR* dir = opendir(SD_VMR_DIR);
    if (!dir) return;
    std::vector<std::string> to_delete;
    while (auto* ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        std::string name = ent->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".wav") continue;
        std::string id = name.substr(0, name.size() - 4);
        if (keep.find(id) == keep.end()) {
            to_delete.push_back(id);
        }
    }
    closedir(dir);

    for (const auto& id : to_delete) {
        std::string path = SdPathFor(id);
        if (unlink(path.c_str()) == 0) {
            ESP_LOGI(TAG, "SD pruned %s", path.c_str());
        }
    }
}

void VmrClient::RunSdSync() {
    if (!IsSdAvailable()) return;
    if (!EnsureSdVmrDir()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_) {
            ESP_LOGI(TAG, "SD sync skipped (playback in progress)");
            return;
        }
    }

    // Snapshot work list under lock
    std::vector<VmrQueueItem> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::string> want;

        // All unread reminders/messages known from catalog
        for (const auto& kv : catalog_) {
            if (!HasBeenPlayed(kv.first)) {
                want.insert(kv.first);
            }
        }
        // Recent read (max 20)
        int kept_read = 0;
        for (int i = (int)played_order_.size() - 1; i >= 0 && kept_read < SD_KEEP_READ_COUNT; i--) {
            want.insert(played_order_[i]);
            kept_read++;
        }

        for (const auto& id : want) {
            auto it = catalog_.find(id);
            if (it == catalog_.end()) continue;
            if (it->second.audio_url.empty()) continue;
            targets.push_back(it->second);
        }

        PruneSdCacheLocked();
    }

    ESP_LOGI(TAG, "SD sync: %d target(s)", (int)targets.size());
    for (const auto& item : targets) {
        if (stop_requested_) break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (playing_) {
                ESP_LOGI(TAG, "SD sync yielding to playback");
                break;
            }
        }
        if (IsSdCacheReady(item.message_id)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (playing_) {
                ESP_LOGI(TAG, "SD sync yielding to playback");
                break;
            }
        }
        ESP_LOGI(TAG, "SD sync download %s type=%s",
                 item.message_id.c_str(), item.message_type.c_str());
        if (!DownloadToSdFile(item.message_id, item.audio_url)) {
            ESP_LOGW(TAG, "SD sync failed for %s", item.message_id.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Final prune after downloads
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PruneSdCacheLocked();
    }
    ESP_LOGI(TAG, "SD sync done");
}

// ============================================================
// Internal: Poll & process
// ============================================================

void VmrClient::DoPoll() {
    if (device_id_.empty()) return;
    if (!network_ready_.load()) {
        ESP_LOGD(TAG, "DoPoll skipped (network down)");
        return;
    }

    std::string url = server_url_ + "/devices/" + device_id_ + "/messages/queue?since_version=" + std::to_string(local_version_);
    ESP_LOGI(TAG, "Polling queue (since_version=%lld)", (long long)local_version_);

    std::string body;
    if (!HttpGet(url, body)) {
        // Link flaps are common; don't surface as a hard VMR error while offline.
        if (!network_ready_.load()) {
            ESP_LOGW(TAG, "Queue poll aborted (network down)");
            return;
        }
        if (on_error) {
            on_error("Queue poll failed");
        }
        return;
    }

    ProcessQueueResponse(body);
}

void VmrClient::ProcessQueueResponse(const std::string& body, bool include_played) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse queue response: %s", body.c_str());
        return;
    }

    // Parse version
    auto version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version)) {
        local_version_ = (int64_t)version->valuedouble;
        ESP_LOGI(TAG, "Queue version updated to %lld", local_version_);
    }

    // Parse items[]
    auto items = cJSON_GetObjectItem(root, "items");
    int new_count = 0;
    if (cJSON_IsArray(items)) {
        int item_count = cJSON_GetArraySize(items);

        // One-shot: mark the last 2 unique queue messages as unread
        if (unmark_last2_once_ && !include_played) {
            unmark_last2_once_ = false;
            std::vector<std::string> unique_ids;
            for (int i = 0; i < item_count; i++) {
                cJSON* item = cJSON_GetArrayItem(items, i);
                if (!cJSON_IsObject(item)) continue;
                auto mid = cJSON_GetObjectItem(item, "message_id");
                if (!cJSON_IsString(mid) || mid->valuestring == nullptr) continue;
                std::string id = mid->valuestring;
                bool seen = false;
                for (const auto& u : unique_ids) {
                    if (u == id) { seen = true; break; }
                }
                if (!seen) unique_ids.push_back(id);
            }
            int unmarked = 0;
            for (int n = 0; n < 2 && !unique_ids.empty(); n++) {
                std::string id = unique_ids.back();
                unique_ids.pop_back();
                if (HasBeenPlayed(id)) {
                    ClearPlayedId(id);
                    unmarked++;
                    ESP_LOGW(TAG, "Marked unread (last-%d): %s", n + 1, id.c_str());
                } else {
                    ESP_LOGI(TAG, "Already unread (last-%d): %s", n + 1, id.c_str());
                }
            }
            if (unmarked > 0) {
                ESP_LOGW(TAG, "Forced %d message(s) back to unread", unmarked);
            }
        }

        for (int i = 0; i < item_count; i++) {
            cJSON* item = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(item)) continue;

            VmrQueueItem qi;
            FillQueueItemFromJson(item, qi);

            if (!qi.message_id.empty() && !qi.audio_url.empty()) {
                RememberCatalogItem(qi);
            }

            // Skip already played messages (unless include_played for user long-press replay)
            if (!include_played && HasBeenPlayed(qi.message_id)) {
                ESP_LOGI(TAG, "Skipping already played: %s", qi.message_id.c_str());
                continue;
            }

            // Skip duplicates already queued (server may list the same message
            // under multiple queue_kind entries, e.g. immediate + scheduled)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                bool already_queued = false;
                for (const auto& p : pending_items_) {
                    if (p.message_id == qi.message_id) {
                        already_queued = true;
                        break;
                    }
                }
                if (already_queued) {
                    ESP_LOGI(TAG, "Skipping duplicate in queue: %s", qi.message_id.c_str());
                    continue;
                }

                pending_items_.push_back(qi);
                new_count++;
            }
            ESP_LOGI(TAG, "New message: id=%s, type=%s, priority=%d%s",
                     qi.message_id.c_str(), qi.message_type.c_str(), qi.priority,
                     include_played && HasBeenPlayed(qi.message_id) ? " (replay)" : "");
        }
    }

    cJSON_Delete(root);

    if (new_count > 0) {
        // Sort by priority (higher first)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::sort(pending_items_.begin(), pending_items_.end(), ShouldPlayBefore);
        }

        // Report total unplayed remaining (not just this poll batch)
        if (on_new_messages && !include_played) {
            on_new_messages(UnplayedCount());
        }
        // Do not prefetch/SD-sync here — they steal Wi-Fi from first autoplay
        // (seen as ~12s prebuffer stall). FinishPlaybackSession schedules sync.
    } else if (!include_played) {
        // No brand-new items this poll — safe to warm SD cache in background
        ScheduleSdSync();
    }
}

bool VmrClient::FetchPlayableItem(VmrQueueItem& out) {
    if (device_id_.empty()) return false;

    // Full queue so already-played items are visible for manual replay
    std::string url = server_url_ + "/devices/" + device_id_ + "/messages/queue?since_version=0";
    ESP_LOGI(TAG, "Fetching playable message (include already-played)");

    std::string body;
    if (!HttpGet(url, body)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse queue response for replay");
        return false;
    }

    auto version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version)) {
        local_version_ = (int64_t)version->valuedouble;
    }

    // Cloud time first (scheduled_at / created_at), then position.
    // Do NOT use JSON array index — server may sort by priority, not time.
    auto is_newer = [](const VmrQueueItem& a, const VmrQueueItem& b) {
        return IsCloudNewer(a, b);
    };

    // Deduplicate by message_id. Voice + reminder_immediate/scheduled all count.
    struct RankedItem {
        VmrQueueItem item;
        int index = 0;
    };
    std::vector<RankedItem> unique;

    auto upsert = [&](const VmrQueueItem& qi, int index) {
        if (qi.message_id.empty()) return;
        if (qi.audio_url.empty() && !IsSdAvailable()) return;
        for (auto& u : unique) {
            if (u.item.message_id == qi.message_id) {
                if (is_newer(qi, u.item) || (!is_newer(u.item, qi) && index >= u.index)) {
                    // Prefer richer metadata (non-empty times/url)
                    VmrQueueItem merged = qi;
                    if (merged.audio_url.empty()) merged.audio_url = u.item.audio_url;
                    if (merged.scheduled_at.empty()) merged.scheduled_at = u.item.scheduled_at;
                    if (merged.created_at.empty()) merged.created_at = u.item.created_at;
                    if (merged.position < 0) merged.position = u.item.position;
                    if (merged.message_type.empty()) merged.message_type = u.item.message_type;
                    u.item = merged;
                    u.index = index;
                }
                return;
            }
        }
        unique.push_back({qi, index});
    };

    auto items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int item_count = cJSON_GetArraySize(items);
        for (int i = 0; i < item_count; i++) {
            cJSON* item = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(item)) continue;

            VmrQueueItem qi;
            FillQueueItemFromJson(item, qi);
            if (qi.message_id.empty() || qi.audio_url.empty()) continue;

            upsert(qi, i);
            RememberCatalogItem(qi);
        }
    }

    cJSON_Delete(root);

    // Merge local catalog / last-played: completed items may leave the cloud queue
    // but should still win as "newest" by cloud time when replaying.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int idx = 100000;
        for (const auto& kv : catalog_) {
            upsert(kv.second, idx++);
        }
        if (!last_played_item_.message_id.empty()) {
            upsert(last_played_item_, idx++);
        }
    }

    if (unique.empty()) {
        return false;
    }

    std::string last_played_id;
    auto played_ord = [this](const std::string& id) -> int {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = (int)played_order_.size() - 1; i >= 0; i--) {
            if (played_order_[i] == id) return i;
        }
        return -1;
    };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_played_id = last_played_item_.message_id;
    }

    // 1) Newest already-read.
    //    Prefer last_played when still in the candidate set (most recent session).
    //    Else cloud times, but many immediate reminders omit scheduled_at while
    //    old items keep a stale sat + high position — those must not win.
    //    If any fresh candidate exists (no time, or time within ~2 days), drop stale-dated ones.
    std::vector<const RankedItem*> played_cands;
    bool has_fresh = false;
    for (const auto& u : unique) {
        if (!HasBeenPlayed(u.item.message_id)) continue;
        played_cands.push_back(&u);
        const std::string key = CloudTimeKey(u.item);
        if (key.empty() || !IsStaleCloudTime(u.item)) {
            has_fresh = true;
        }
        ESP_LOGI(TAG, "Replay cand %s type=%s pos=%d sat=%s created=%s stale=%d ord=%d",
                 u.item.message_id.c_str(), u.item.message_type.c_str(), u.item.position,
                 u.item.scheduled_at.c_str(), u.item.created_at.c_str(),
                 IsStaleCloudTime(u.item) ? 1 : 0, played_ord(u.item.message_id));
    }

    if (!last_played_id.empty()) {
        for (const RankedItem* up : played_cands) {
            if (up->item.message_id == last_played_id) {
                out = up->item;
                ESP_LOGI(TAG, "Selected last_played %s type=%s pos=%d sat=%s for replay",
                         out.message_id.c_str(), out.message_type.c_str(), out.position,
                         out.scheduled_at.c_str());
                return true;
            }
        }
    }

    const RankedItem* best_played = nullptr;
    int best_ord = -1;
    for (const RankedItem* up : played_cands) {
        const auto& u = *up;
        if (has_fresh && !CloudTimeKey(u.item).empty() && IsStaleCloudTime(u.item)) {
            continue;  // ignore ancient sat when fresher candidates exist
        }
        int oa = played_ord(u.item.message_id);
        if (best_played == nullptr ||
            IsReplayNewer(u.item, oa, best_played->item, best_ord, last_played_id)) {
            best_played = up;
            best_ord = oa;
        }
    }
    if (best_played != nullptr) {
        out = best_played->item;
        ESP_LOGI(TAG, "Selected newest already-read %s type=%s pos=%d sat=%s created=%s for replay",
                 out.message_id.c_str(), out.message_type.c_str(), out.position,
                 out.scheduled_at.c_str(), out.created_at.c_str());
        return true;
    }

    // 2) Unplayed: highest priority, then newer cloud time
    const RankedItem* best_unplayed = nullptr;
    for (const auto& u : unique) {
        if (HasBeenPlayed(u.item.message_id)) continue;
        if (best_unplayed == nullptr ||
            u.item.priority > best_unplayed->item.priority ||
            (u.item.priority == best_unplayed->item.priority &&
             is_newer(u.item, best_unplayed->item))) {
            best_unplayed = &u;
        }
    }
    if (best_unplayed != nullptr) {
        out = best_unplayed->item;
        ESP_LOGI(TAG, "Selected unplayed %s type=%s for playback",
                 out.message_id.c_str(), out.message_type.c_str());
        return true;
    }

    // 3) Newest of anything by cloud time
    const RankedItem* best_any = &unique.front();
    for (size_t i = 1; i < unique.size(); i++) {
        if (is_newer(unique[i].item, best_any->item) ||
            (!is_newer(best_any->item, unique[i].item) &&
             unique[i].index > best_any->index)) {
            best_any = &unique[i];
        }
    }
    out = best_any->item;
    ESP_LOGI(TAG, "Selected latest queue %s type=%s pos=%d sat=%s for replay",
             out.message_id.c_str(), out.message_type.c_str(), out.position,
             out.scheduled_at.c_str());
    return true;
}

// ============================================================
// Playback
// ============================================================

bool VmrClient::PlayLatestMessage() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_) {
            ESP_LOGW(TAG, "Already playing, stopping first");
        }
    }
    if (playing_) {
        StopPlayback();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    VmrQueueItem item;
    bool have_item = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_items_.empty()) {
            item = pending_items_.front();
            if (item.audio_url.empty() && !IsSdAvailable()) {
                ESP_LOGW(TAG, "Message %s has no audio_url, removing", item.message_id.c_str());
                pending_items_.erase(pending_items_.begin());
                ReportPlayEvent(item.message_id, kVmrEventFailed, "no_audio_url");
                return false;
            }
            pending_items_.erase(pending_items_.begin());
            have_item = true;
        }
    }

    // No unplayed pending — replay the most recent already-read item
    // (do NOT prefer arbitrary SD cache; that often picks an older UUID-ordered hit)
    if (!have_item) {
        ESP_LOGI(TAG, "No pending messages; selecting latest already-read for replay");

        auto played_ord = [this](const std::string& id) -> int {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = (int)played_order_.size() - 1; i >= 0; i--) {
                if (played_order_[i] == id) return i;
            }
            return -1;
        };

        std::string last_played_id;
        VmrQueueItem last_played_snap;
        std::vector<VmrQueueItem> played_candidates;
        std::vector<std::string> played_order_snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_played_id = last_played_item_.message_id;
            last_played_snap = last_played_item_;
            played_order_snap = played_order_;
            for (const auto& kv : catalog_) {
                if (HasBeenPlayed(kv.first)) {
                    played_candidates.push_back(kv.second);
                }
            }
            if (!last_played_item_.message_id.empty()) {
                played_candidates.push_back(last_played_item_);
            }
        }

        auto is_playable = [this](const VmrQueueItem& it) {
            if (it.message_id.empty()) return false;
            if (IsSdCacheReady(it.message_id)) return true;
            return !it.audio_url.empty();
        };

        // 1) Most recently completed playback
        if (!last_played_snap.message_id.empty() && is_playable(last_played_snap)) {
            item = last_played_snap;
            have_item = true;
            ESP_LOGI(TAG, "Replaying last_played %s type=%s",
                     item.message_id.c_str(), item.message_type.c_str());
        }

        // 2) Newest first-play in played_order_ that is still playable
        if (!have_item) {
            for (int i = (int)played_order_snap.size() - 1; i >= 0; i--) {
                const std::string& id = played_order_snap[i];
                const VmrQueueItem* found = nullptr;
                for (const auto& cand : played_candidates) {
                    if (cand.message_id == id) {
                        found = &cand;
                        break;
                    }
                }
                if (found == nullptr || !is_playable(*found)) continue;
                item = *found;
                have_item = true;
                ESP_LOGI(TAG, "Replaying newest-played-order %s type=%s ord=%d",
                         item.message_id.c_str(), item.message_type.c_str(), i);
                break;
            }
        }

        // 3) Network/catalog ranking (cloud time + play order)
        if (!have_item) {
            if (FetchPlayableItem(item)) {
                have_item = true;
            } else {
                const VmrQueueItem* best = nullptr;
                int best_ord = -1;
                for (const auto& cand : played_candidates) {
                    if (!is_playable(cand)) continue;
                    int ord = played_ord(cand.message_id);
                    if (best == nullptr ||
                        IsReplayNewer(cand, ord, *best, best_ord, last_played_id)) {
                        best = &cand;
                        best_ord = ord;
                    }
                }
                if (best != nullptr) {
                    item = *best;
                    have_item = true;
                    ESP_LOGI(TAG, "Replaying offline newest-read %s type=%s pos=%d sat=%s",
                             item.message_id.c_str(), item.message_type.c_str(),
                             item.position, item.scheduled_at.c_str());
                } else {
                    ESP_LOGI(TAG, "No messages available to play");
                    return false;
                }
            }
        }

        if (item.audio_url.empty() && !IsSdCacheReady(item.message_id)) {
            ESP_LOGW(TAG, "Message %s has no audio_url and no SD cache", item.message_id.c_str());
            ReportPlayEvent(item.message_id, kVmrEventFailed, "no_audio_url");
            return false;
        }
    }

    // Unread: SD ready → 边读边播; miss → 边下边播（自适应开播：缓冲追上剩余下载再出声）.
    // 慢网且文件大于 ring 时 stream 失败，再回退 download_then_play.
    const bool unread = !HasBeenPlayed(item.message_id);
    VmrPlaybackMode mode;
    if (unread && !item.audio_url.empty()) {
        if (IsSdCacheReady(item.message_id)) {
            mode = VmrPlaybackMode::kStreamWhileRead;
        } else {
            mode = VmrPlaybackMode::kStreamWhileDownload;
        }
    } else {
        mode = SelectPlaybackMode(item.message_id, item.audio_url);
    }
    if (mode == VmrPlaybackMode::kDownloadThenPlay && item.audio_url.empty() &&
        !IsSdCacheReady(item.message_id)) {
        ESP_LOGE(TAG, "Cannot play %s: no cache and no audio_url", item.message_id.c_str());
        ReportPlayEvent(item.message_id, kVmrEventFailed, "no_audio_url");
        return false;
    }

    ESP_LOGI(TAG, "VMR: play mode=%s message_id=%s type=%s unread=%d sd_ready=%d",
             PlaybackModeName(mode), item.message_id.c_str(), item.message_type.c_str(),
             unread ? 1 : 0, IsSdCacheReady(item.message_id) ? 1 : 0);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_message_id_ = item.message_id;
        play_audio_url_ = item.audio_url;
        play_mode_ = mode;
        playing_ = true;
        stop_requested_ = false;
        // Unread items need screen Read/Unread confirm after play
        play_needs_read_confirm_ = !HasBeenPlayed(item.message_id);
    }
    // Unblock any stuck background SD download holding audio_fetch_mutex_
    audio_fetch_abort_.store(true);
    // Don't let queue poll compete with audio HTTPS during play
    SetPollingSuspended(true);

    xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        std::string url;
        std::string mid;
        VmrPlaybackMode mode;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            mid = self->current_message_id_;
            url = self->play_audio_url_;
            mode = self->play_mode_;
        }

        bool success = self->PlayStreaming(mode, mid, url);
        self->FinishPlaybackSession(mid, success);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->play_task_ = nullptr;
        }
        self->PrefetchFrontAudio();
        vTaskDelete(nullptr);
    }, "vmr_play", 12288, this, 5, &play_task_);

    return true;
}

bool VmrClient::PlayStreaming(VmrPlaybackMode mode, const std::string& message_id,
                              const std::string& audio_url) {
    ESP_LOGI(TAG, "VMR: play streaming mode=%s id=%s",
             PlaybackModeName(mode), message_id.c_str());

    auto play_buf = [this](const std::string& mid, std::vector<uint8_t>& buf) {
        if (on_playback_started) {
            on_playback_started(mid);
        }
        ReportPlayEventAsync(mid, kVmrEventStarted);
        return PlayFromBuffer(buf);
    };

    switch (mode) {
        case VmrPlaybackMode::kStreamWhileRead: {
            if (PlayStreamingFromSd(message_id)) {
                return true;
            }
            // Prefer another stream path over whole-file RAM download (keeps UI responsive).
            ESP_LOGW(TAG, "stream_while_read failed → HTTP stream");
            if (!audio_url.empty() && PlayStreamingFromHttp(message_id, audio_url, false)) {
                return true;
            }
            if (!audio_url.empty()) {
                ESP_LOGW(TAG, "HTTP stream failed → download_then_play");
                std::vector<uint8_t> buf;
                if (AcquireAudio(message_id, audio_url, buf)) {
                    return play_buf(message_id, buf);
                }
            }
            return false;
        }
        case VmrPlaybackMode::kStreamWhileWrite: {
            // Optional: stream + SD write. Prefer ring feed; fall back to full-file.
            if (PlayStreamingFromHttp(message_id, audio_url, true)) {
                return true;
            }
            ESP_LOGW(TAG, "stream_while_write failed → memory stream");
            if (PlayStreamingFromHttp(message_id, audio_url, false)) {
                return true;
            }
            if (!audio_url.empty()) {
                ESP_LOGW(TAG, "HTTP stream failed → download_then_play");
                std::vector<uint8_t> buf;
                if (AcquireAudio(message_id, audio_url, buf)) {
                    return play_buf(message_id, buf);
                }
            }
            return false;
        }
        case VmrPlaybackMode::kStreamWhileDownload: {
            // Fast path: HTTPS → ring → play (SD filled later in background if available)
            if (PlayStreamingFromHttp(message_id, audio_url, false)) {
                return true;
            }
            // Slow HTTPS / thin soft-start can fail open-play; full-file is reliable fallback.
            ESP_LOGW(TAG, "stream_while_download failed → download_then_play");
            if (!audio_url.empty()) {
                std::vector<uint8_t> buf;
                if (AcquireAudio(message_id, audio_url, buf)) {
                    return play_buf(message_id, buf);
                }
            }
            return false;
        }
        case VmrPlaybackMode::kDownloadThenPlay: {
            ESP_LOGI(TAG, "VMR: full download then play id=%s", message_id.c_str());
            std::vector<uint8_t> buf;
            if (!AcquireAudio(message_id, audio_url, buf)) {
                return false;
            }
            return play_buf(message_id, buf);
        }
    }
    return false;
}

bool VmrClient::PlayStreamingFromSd(const std::string& message_id) {
    if (!IsSdCacheReady(message_id)) {
        ESP_LOGW(TAG, "stream_while_read: cache not ready for %s", message_id.c_str());
        return false;
    }
    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "No audio codec");
        return false;
    }

    std::string path = SdPathFor(message_id);
    ESP_LOGI(TAG, "VMR: play mode=stream_while_read path=%s", path.c_str());

    VmrBytePipe pipe(kRingCapSdRead);
    auto* prod_done = new std::atomic<bool>(false);

    struct SdProdCtx {
        VmrClient* self;
        VmrBytePipe* pipe;
        std::string path;
        std::atomic<bool>* done;
    };
    auto* ctx = new SdProdCtx{this, &pipe, path, prod_done};
    // Stack must fit chunk[kIoChunkBytes] + FILE + frame overhead
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* c = static_cast<SdProdCtx*>(arg);
        FILE* fp = fopen(c->path.c_str(), "rb");
        if (!fp) {
            ESP_LOGE(TAG, "SD open failed %s", c->path.c_str());
            c->pipe->MarkError();
            c->done->store(true);
            delete c;
            vTaskDelete(nullptr);
            return;
        }
        uint8_t chunk[kIoChunkBytes];
        while (!c->self->stop_requested_ && !c->pipe->IsAborted()) {
            size_t n = fread(chunk, 1, sizeof(chunk), fp);
            if (n == 0) break;
            if (!c->pipe->Push(chunk, n)) break;
        }
        fclose(fp);
        if (c->self->stop_requested_) {
            c->pipe->Abort();
        } else {
            c->pipe->MarkEof();
        }
        c->done->store(true);
        delete c;
        vTaskDelete(nullptr);
    }, "vmr_sd_rd", 8192, ctx, 4, nullptr);

    if (ok != pdPASS) {
        delete ctx;
        delete prod_done;
        pipe.MarkError();
        ESP_LOGE(TAG, "Failed to start SD reader task");
        return false;
    }

    if (codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    // Re-open applies the user's current output_volume_ — do not clamp/reset it.
    codec_->EnableOutput(true);

    auto mid = message_id;
    bool success = VmrPlayWavFromPipe(
        pipe, codec_->output_sample_rate(), kPrebufferSdMs, kRebufferSdMs,
        kUnderrunGiveupSdMs,
        [this]() { return stop_requested_; },
        [this, mid]() {
            ESP_LOGI(TAG, "VMR: started (stream_while_read)");
            if (on_playback_started) {
                on_playback_started(mid);
            }
            ReportPlayEventAsync(mid, kVmrEventStarted);
        },
        [this](std::vector<int16_t>& pcm) { codec_->OutputData(pcm); });

    pipe.Abort();
    for (int i = 0; i < 100 && !prod_done->load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    delete prod_done;
    return success;
}

bool VmrClient::PlayStreamingFromHttp(const std::string& message_id, const std::string& audio_url,
                                      bool write_disk) {
    if (audio_url.empty()) {
        ESP_LOGE(TAG, "HTTP stream: empty url");
        return false;
    }
    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "No audio codec");
        return false;
    }
    if (write_disk && !EnsureSdVmrDir()) {
        ESP_LOGW(TAG, "SD dir unavailable — falling back to stream_while_download");
        write_disk = false;
    }

    ESP_LOGI(TAG, "VMR: play mode=%s url=%s",
             write_disk ? "stream_while_write" : "stream_while_download",
             audio_url.c_str());

    VmrBytePipe pipe(kRingCapDownload);
    std::string path = write_disk ? SdPathFor(message_id) : std::string();
    std::string tmp = write_disk ? (path + ".tmp") : std::string();
    auto* prod_done = new std::atomic<bool>(false);

    struct HttpProdCtx {
        VmrClient* self;
        VmrBytePipe* pipe;
        std::string url;
        std::string token;
        std::string device_uid;
        std::string tmp_path;
        std::string final_path;
        bool write_disk;
        bool write_ok;
        size_t total;
        std::atomic<bool>* done;
    };
    auto* ctx = new HttpProdCtx{this, &pipe, audio_url, token_, device_uid_, tmp, path,
                                write_disk, true, 0, prod_done};
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* c = static_cast<HttpProdCtx*>(arg);
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            c->pipe->MarkError();
            c->done->store(true);
            delete c;
            vTaskDelete(nullptr);
            return;
        }
        auto http = network->CreateHttp(kHttpIdPlay);
        if (http == nullptr) {
            c->pipe->MarkError();
            c->done->store(true);
            delete c;
            vTaskDelete(nullptr);
            return;
        }
        http->SetHeader("Authorization", c->token.c_str());
        if (!c->device_uid.empty()) {
            http->SetHeader("Device-Id", c->device_uid.c_str());
        }
        // Keep Read() responsive so Abort/join after play does not sit for 60s.
        http->SetTimeout(15000);
        c->self->AcquireWifiHold();

        FILE* fp = nullptr;
        if (c->write_disk) {
            fp = fopen(c->tmp_path.c_str(), "wb");
            if (!fp) {
                fp = fopen(c->final_path.c_str(), "wb");
                if (fp) {
                    c->tmp_path = c->final_path;
                }
            }
            if (!fp) {
                ESP_LOGW(TAG, "Cannot open SD for write — continue memory-only");
                c->write_disk = false;
            }
        }

        if (!http->Open("GET", c->url.c_str())) {
            ESP_LOGE(TAG, "HTTP stream open failed");
            if (fp) {
                fclose(fp);
                unlink(c->tmp_path.c_str());
            }
            c->self->ReleaseWifiHold();
            c->pipe->MarkError();
            c->done->store(true);
            delete c;
            vTaskDelete(nullptr);
            return;
        }
        int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP stream status %d", status);
            http->Close();
            if (fp) {
                fclose(fp);
                unlink(c->tmp_path.c_str());
            }
            c->self->ReleaseWifiHold();
            c->pipe->MarkError();
            c->done->store(true);
            delete c;
            vTaskDelete(nullptr);
            return;
        }

        size_t content_length = http->GetBodyLength();
        uint8_t chunk[kIoChunkBytes];
        while (!c->self->stop_requested_) {
            // Play path owns the ring: once aborted/errored, stop HTTP so join returns fast.
            // SD cache is filled later by FinishPlaybackSession background task.
            if (c->pipe->IsAborted() || c->pipe->HasError()) {
                ESP_LOGI(TAG, "HTTP stream stop: pipe closed (play finished)");
                break;
            }
            int ret = http->Read(reinterpret_cast<char*>(chunk), sizeof(chunk));
            if (ret < 0) {
                c->write_ok = false;
                break;
            }
            if (ret == 0) break;
            // Feed ring first — SD SPI must not stall open-play.
            if (!c->pipe->Push(chunk, (size_t)ret)) {
                ESP_LOGI(TAG, "HTTP stream stop: pipe push rejected");
                break;
            }
            if (fp) {
                if (fwrite(chunk, 1, (size_t)ret, fp) != (size_t)ret) {
                    ESP_LOGW(TAG, "SD write error during stream");
                    fclose(fp);
                    fp = nullptr;
                    unlink(c->tmp_path.c_str());
                    c->write_disk = false;
                    c->write_ok = false;
                }
            }
            c->total += (size_t)ret;
        }
        http->Close();
        if (fp) {
            fclose(fp);
            fp = nullptr;
            const bool truncated =
                (content_length > 0 && c->total < content_length);
            if (c->self->stop_requested_ || truncated) {
                // User abort or incomplete HTTP — drop partial
                unlink(c->tmp_path.c_str());
                if (c->tmp_path != c->final_path) {
                    unlink(c->final_path.c_str());
                }
                c->write_ok = false;
            } else if (c->tmp_path != c->final_path) {
                unlink(c->final_path.c_str());
                if (rename(c->tmp_path.c_str(), c->final_path.c_str()) != 0) {
                    unlink(c->tmp_path.c_str());
                    c->write_ok = false;
                }
            }
        }

        ESP_LOGI(TAG, "HTTP stream producer done bytes=%u write_disk=%d write_ok=%d",
                 (unsigned)c->total, (int)c->write_disk, (int)c->write_ok);
        if (c->self->stop_requested_) {
            c->pipe->Abort();
        } else if (c->total == 0) {
            c->pipe->MarkError();
        } else {
            c->pipe->MarkEof();
        }
        c->self->ReleaseWifiHold();
        c->done->store(true);
        delete c;
        vTaskDelete(nullptr);
    // Priority above vmr_play (5) so the ring stays ahead of the PCM consumer.
    }, "vmr_http", 8192, ctx, 6, nullptr);

    if (ok != pdPASS) {
        delete ctx;
        delete prod_done;
        pipe.MarkError();
        ESP_LOGE(TAG, "Failed to start HTTP fetch task");
        return false;
    }

    if (codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    // Re-open applies the user's current output_volume_ — do not clamp/reset it.
    codec_->EnableOutput(true);

    auto mid = message_id;
    const char* mode_name = write_disk ? "stream_while_write" : "stream_while_download";
    bool success = VmrPlayWavFromPipe(
        pipe, codec_->output_sample_rate(), kPrebufferDownloadMs, kRebufferDownloadMs,
        kUnderrunGiveupDownloadMs,
        [this]() { return stop_requested_; },
        [this, mid, mode_name]() {
            ESP_LOGI(TAG, "VMR: started (%s)", mode_name);
            if (on_playback_started) {
                on_playback_started(mid);
            }
            // Defer sync POST so it does not contend with the audio GET in the first seconds.
            struct EvCtx {
                VmrClient* self;
                std::string mid;
            };
            auto* ev = new EvCtx{this, mid};
            BaseType_t ok = xTaskCreate([](void* arg) {
                auto* e = static_cast<EvCtx*>(arg);
                // Defer sync POST well past the critical open-play window so it
                // does not contend with the audio GET right after first sound.
                vTaskDelay(pdMS_TO_TICKS(8000));
                e->self->ReportPlayEventAsync(e->mid, kVmrEventStarted);
                delete e;
                vTaskDelete(nullptr);
            }, "vmr_st_evt", 3072, ev, 2, nullptr);
            if (ok != pdPASS) {
                delete ev;
            }
        },
        [this](std::vector<int16_t>& pcm) { codec_->OutputData(pcm); });

    pipe.Abort();
    // Producer exits promptly on Abort (no continue-to-EOF for SD during play).
    int join_ms = 0;
    while (!prod_done->load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        join_ms += 20;
        if (join_ms >= 12000) {
            ESP_LOGE(TAG, "HTTP producer join timeout — leaking done flag to avoid UAF");
            break;
        }
    }
    if (prod_done->load()) {
        delete prod_done;
    }

    if (write_disk && success && IsSdCacheReady(message_id)) {
        ESP_LOGI(TAG, "VMR: download complete ready");
    } else if (write_disk && !IsSdCacheReady(message_id)) {
        ESP_LOGW(TAG, "VMR: SD cache deferred after stream (background fill)");
    }
    return success;
}

void VmrClient::FinishPlaybackSession(const std::string& mid, bool success) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = play_audio_url_;
        playing_ = false;
        if (success && !mid.empty()) {
            auto it = catalog_.find(mid);
            if (it != catalog_.end()) {
                last_played_item_ = it->second;
                if (last_played_item_.audio_url.empty()) {
                    last_played_item_.audio_url = url;
                }
            } else {
                last_played_item_.message_id = mid;
                last_played_item_.audio_url = url;
            }
        }
    }
    audio_fetch_abort_.store(false);
    SetPollingSuspended(false);

    // Unread (or any miss): if play finished but SD still incomplete, fill cache in background
    if (!mid.empty() && !url.empty() && IsSdAvailable() && !IsSdCacheReady(mid)) {
        struct CacheFillCtx {
            VmrClient* self;
            std::string id;
            std::string url;
        };
        auto* c = new CacheFillCtx{this, mid, url};
        BaseType_t ok = xTaskCreate([](void* arg) {
            auto* c = static_cast<CacheFillCtx*>(arg);
            ESP_LOGI(TAG, "VMR: background SD cache fill %s", c->id.c_str());
            if (!c->self->DownloadToSdFile(c->id, c->url)) {
                ESP_LOGW(TAG, "VMR: background SD cache fill failed %s", c->id.c_str());
            } else {
                ESP_LOGI(TAG, "VMR: background SD cache ready %s", c->id.c_str());
            }
            delete c;
            vTaskDelete(nullptr);
        }, "vmr_sd_fill", 8192, c, 3, nullptr);
        if (ok != pdPASS) {
            delete c;
        }
    }

    if (success) {
        // Defer SavePlayedId until user confirms Read (or skip confirm for already-read replay)
        if (!play_needs_read_confirm_) {
            SavePlayedId(mid);
        }
        if (IsSdAvailable()) {
            ScheduleSdSync();
        }
        // Async so confirm UI / ResumeOutput are not blocked on HTTP
        ReportPlayEventAsync(mid, kVmrEventCompleted);
    } else if (!stop_requested_) {
        play_needs_read_confirm_ = false;
        ReportPlayEventAsync(mid, kVmrEventFailed, "playback_error");
    }

    if (on_playback_finished) {
        on_playback_finished(mid, success);
    }
    if (play_done_sem_ != nullptr) {
        xSemaphoreGive(play_done_sem_);
    }
    // Skip immediate poll while Application may show Read/Unread confirm
    if (!play_needs_read_confirm_ || !success) {
        PollNow();
    }
}

void VmrClient::MarkMessageRead(const std::string& message_id) {
    if (message_id.empty()) return;
    SavePlayedId(message_id);
    // Drop from pending if requeued as unread earlier
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_items_.erase(
            std::remove_if(pending_items_.begin(), pending_items_.end(),
                           [&](const VmrQueueItem& it) { return it.message_id == message_id; }),
            pending_items_.end());
        play_needs_read_confirm_ = false;
    }
    // Async — must not stall UI tip ("Message read")
    ReportPlayEventAsync(message_id, kVmrEventAcknowledged);
    ESP_LOGI(TAG, "Marked read: %s", message_id.c_str());
    // Defer poll so status tip paints first
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        vTaskDelay(pdMS_TO_TICKS(50));
        self->PollNow();
        vTaskDelete(nullptr);
    }, "vmr_rd_poll", 4096, this, 3, nullptr);
    if (ok != pdPASS) {
        PollNow();
    }
}

void VmrClient::KeepMessageUnread(const std::string& message_id) {
    if (message_id.empty()) return;
    ClearPlayedId(message_id);

    VmrQueueItem item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = catalog_.find(message_id);
        if (it != catalog_.end()) {
            item = it->second;
        } else if (last_played_item_.message_id == message_id) {
            item = last_played_item_;
        } else {
            item.message_id = message_id;
            item.audio_url = play_audio_url_;
        }
    }
    const bool playable = !item.audio_url.empty() || IsSdCacheReady(message_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool already = false;
        for (const auto& p : pending_items_) {
            if (p.message_id == message_id) {
                already = true;
                break;
            }
        }
        if (!already && playable) {
            pending_items_.push_back(item);
        }
        play_needs_read_confirm_ = false;
    }
    ESP_LOGI(TAG, "Kept unread: %s (pending=%d)", message_id.c_str(), UnplayedCount());
    // Defer poll — same as MarkMessageRead — so UI tip can paint first.
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        vTaskDelay(pdMS_TO_TICKS(50));
        self->PollNow();
        vTaskDelete(nullptr);
    }, "vmr_ur_poll", 4096, this, 3, nullptr);
    if (ok != pdPASS) {
        PollNow();
    }
}

void VmrClient::PrefetchFrontAudio() {
    if (prefetch_in_progress_.load() || playing_) {
        return;
    }
    if (!IsSdAvailable()) {
        // No SD: skip full-file prefetch to avoid peak RAM; next play streams.
        return;
    }

    std::string id;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_items_.empty()) return;
        const auto& front = pending_items_.front();
        if (front.message_id.empty()) return;
        if (prefetched_id_ == front.message_id) return;
        id = front.message_id;
        url = front.audio_url;
    }

    if (IsSdCacheReady(id)) {
        std::lock_guard<std::mutex> lock(mutex_);
        prefetched_id_ = id;
        return;
    }
    if (url.empty()) return;

    prefetch_in_progress_.store(true);
    struct PrefetchCtx {
        VmrClient* self;
        std::string id;
        std::string url;
    };
    auto* ctx = new PrefetchCtx{this, std::move(id), std::move(url)};
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* c = static_cast<PrefetchCtx*>(arg);
        auto* self = c->self;
        ESP_LOGI(TAG, "Prefetching to SD %s", c->id.c_str());
        bool ok_dl = self->DownloadToSdFile(c->id, c->url);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (ok_dl && !self->playing_ && !self->pending_items_.empty() &&
                self->pending_items_.front().message_id == c->id) {
                self->prefetched_id_ = c->id;
                ESP_LOGI(TAG, "Prefetch SD ready: %s", c->id.c_str());
            } else if (ok_dl) {
                ESP_LOGI(TAG, "Prefetch discarded (queue changed): %s", c->id.c_str());
            }
        }
        self->prefetch_in_progress_.store(false);
        delete c;
        vTaskDelete(nullptr);
    }, "vmr_prefetch", 8192, ctx, 3, nullptr);

    if (ok != pdPASS) {
        delete ctx;
        prefetch_in_progress_.store(false);
        ESP_LOGW(TAG, "Failed to start prefetch task");
    }
}

bool VmrClient::PlayTestAudio() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_) {
            ESP_LOGW(TAG, "Already playing, stopping first");
        }
    }
    if (playing_) {
        StopPlayback();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "No audio codec available for test tone");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_message_id_ = "local-test-tone";
        playing_ = true;
        stop_requested_ = false;
    }

    ESP_LOGI(TAG, "Playing generated test tone (24kHz beeps)");

    if (on_playback_started) {
        on_playback_started(current_message_id_);
    }

    xTaskCreate([](void* arg) {
        auto* self = static_cast<VmrClient*>(arg);
        bool success = false;
        if (self->codec_ != nullptr) {
            // Force-open codec output; keep whatever volume the user set.
            if (self->codec_->output_enabled()) {
                self->codec_->EnableOutput(false);
            }
            self->codec_->EnableOutput(true);

            const int rate = self->codec_->output_sample_rate() > 0
                                 ? self->codec_->output_sample_rate() : 24000;
            const int chunk = 512;
            std::vector<int16_t> out(chunk);

            auto write_tone = [&](float freq_hz, int duration_ms, float amp) {
                int total = rate * duration_ms / 1000;
                int written = 0;
                while (!self->stop_requested_ && written < total) {
                    int n = std::min(chunk, total - written);
                    out.resize(n);
                    for (int i = 0; i < n; i++) {
                        float t = (float)(written + i) / (float)rate;
                        out[i] = (int16_t)(amp * 32767.0f *
                                           sinf(2.0f * 3.14159265f * freq_hz * t));
                    }
                    self->codec_->OutputData(out);
                    written += n;
                }
            };

            auto write_silence = [&](int duration_ms) {
                int total = rate * duration_ms / 1000;
                int written = 0;
                while (!self->stop_requested_ && written < total) {
                    int n = std::min(chunk, total - written);
                    out.assign(n, 0);
                    self->codec_->OutputData(out);
                    written += n;
                }
            };

            // Pattern: beep-beep-beep + rising chirp (~3s) — easy to hear
            write_tone(880.0f, 350, 0.55f);
            write_silence(150);
            write_tone(880.0f, 350, 0.55f);
            write_silence(150);
            write_tone(880.0f, 350, 0.55f);
            write_silence(200);

            // Chirp 600→1500Hz over ~1.2s
            {
                const int duration_ms = 1200;
                int total = rate * duration_ms / 1000;
                int written = 0;
                while (!self->stop_requested_ && written < total) {
                    int n = std::min(chunk, total - written);
                    out.resize(n);
                    for (int i = 0; i < n; i++) {
                        float t = (float)(written + i) / (float)rate;
                        float f = 600.0f + 900.0f * (t / (duration_ms / 1000.0f));
                        out[i] = (int16_t)(0.45f * 32767.0f *
                                           sinf(2.0f * 3.14159265f * f * t));
                    }
                    self->codec_->OutputData(out);
                    written += n;
                }
            }

            success = !self->stop_requested_;
            ESP_LOGI(TAG, "Generated test tone %s", success ? "finished" : "stopped");
        }

        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->playing_ = false;
            self->play_task_ = nullptr;
        }

        auto mid = self->current_message_id_;
        if (self->on_playback_finished) {
            self->on_playback_finished(mid, success);
        }
        if (self->play_done_sem_ != nullptr) {
            xSemaphoreGive(self->play_done_sem_);
        }
        vTaskDelete(nullptr);
    }, "vmr_play", 8192, this, 5, &play_task_);

    return true;
}

void VmrClient::DoPlayback(const std::vector<uint8_t>& buffer) {
    auto mid = current_message_id_;
    if (on_playback_started) {
        on_playback_started(mid);
    }
    ReportPlayEventAsync(mid, kVmrEventStarted);
    bool success = PlayFromBuffer(buffer);
    if (success && IsSdAvailable() && !buffer.empty()) {
        SaveAudioToSd(mid, buffer);
    }
    FinishPlaybackSession(mid, success);
}

bool VmrClient::PlayFromBuffer(const std::vector<uint8_t>& buffer) {
    if (buffer.empty()) {
        ESP_LOGE(TAG, "Empty audio buffer");
        return false;
    }
    if (!IsWavBufferComplete(buffer)) {
        ESP_LOGE(TAG, "Refusing to play incomplete WAV (%u bytes)", (unsigned)buffer.size());
        return false;
    }

    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "No audio codec available");
        return false;
    }

    // Parse WAV header from memory
    const uint8_t* data = buffer.data();
    size_t total = buffer.size();
    if (total < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "Not a valid WAV buffer (%zu bytes)", total);
        return false;
    }

    WavHeader hdr;
    size_t pos = 12;
    bool has_fmt = false, has_data = false;
    long data_offset = 0;
    while (pos + 8 <= total) {
        uint32_t chunk_size = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24);
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            const uint8_t* f = data + pos + 8;
            size_t avail = (pos + 8 + chunk_size <= total) ? chunk_size : (total - pos - 8);
            if (avail >= 16) {
                hdr.audio_format    = f[0]  | (f[1]  << 8);
                hdr.num_channels    = f[2]  | (f[3]  << 8);
                hdr.sample_rate     = f[4]  | (f[5]  << 8) | (f[6] << 16) | (f[7] << 24);
                hdr.byte_rate       = f[8]  | (f[9]  << 8) | (f[10] << 16) | (f[11] << 24);
                hdr.block_align     = f[12] | (f[13] << 8);
                hdr.bits_per_sample = f[14] | (f[15] << 8);
                ESP_LOGI(TAG, "WAV fmt: %dHz %dch %dbit (format=%d)",
                         (int)hdr.sample_rate, hdr.num_channels, hdr.bits_per_sample, hdr.audio_format);
            }
            has_fmt = true;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            hdr.data_size = chunk_size;
            data_offset = pos + 8;
            has_data = true;
            break;
        }
        pos += 8 + chunk_size + (chunk_size & 1);  // chunks are word-aligned
    }

    if (!has_fmt || !has_data) {
        ESP_LOGE(TAG, "WAV missing fmt/data chunk");
        return false;
    }
    hdr.data_offset = data_offset;

    // Ensure codec output is open (force re-open if already marked enabled but closed).
    // Re-open restores hardware volume from output_volume_ — do not override user setting.
    if (codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    codec_->EnableOutput(true);

    const int out_rate = codec_->output_sample_rate();
    const int CHUNK_SAMPLES = 512;
    std::vector<int16_t> mono;
    std::vector<int16_t> out(CHUNK_SAMPLES);
    int chunks = 0;
    size_t total_samples = 0;

    ESP_LOGI(TAG, "Playing WAV: %dHz %dch %dbit → codec %dHz, buf=%u data_size=%u",
             (int)hdr.sample_rate, hdr.num_channels, hdr.bits_per_sample, out_rate,
             (unsigned)total, (unsigned)hdr.data_size);

    size_t data_bytes = (hdr.data_size > 0 && (size_t)(data_offset + hdr.data_size) <= total)
                        ? hdr.data_size : (total - data_offset);
    size_t bps = hdr.bits_per_sample / 8;
    if (bps == 0) {
        ESP_LOGE(TAG, "Invalid bits_per_sample");
        return false;
    }

    // Decode whole PCM to mono int16 first (files are short VMR clips)
    size_t frame_bytes = bps * hdr.num_channels;
    size_t src_frames = data_bytes / frame_bytes;
    mono.resize(src_frames);
    const uint8_t* raw = data + data_offset;
    int32_t peak = 0;
    for (size_t i = 0; i < src_frames; i++) {
        const uint8_t* f = raw + i * frame_bytes;
        int16_t s = 0;
        if (hdr.bits_per_sample == 16 && hdr.num_channels == 1) {
            s = (int16_t)(f[0] | (f[1] << 8));
        } else if (hdr.bits_per_sample == 16 && hdr.num_channels == 2) {
            int16_t l = (int16_t)(f[0] | (f[1] << 8));
            int16_t r = (int16_t)(f[2] | (f[3] << 8));
            s = (int16_t)(((int32_t)l + r) / 2);
        } else if (hdr.bits_per_sample == 8) {
            s = ((int16_t)f[0] - 128) << 8;
        } else {
            ESP_LOGW(TAG, "Unsupported format: %dbit %dch", hdr.bits_per_sample, hdr.num_channels);
            return false;
        }
        mono[i] = s;
        int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }

    // Quiet / near-silent server clips: boost so speaker path is audible
    if (peak > 0 && peak < 8000) {
        float gain = 16000.0f / (float)peak;
        if (gain > 16.0f) gain = 16.0f;
        ESP_LOGW(TAG, "Low peak=%ld, applying gain=%.1fx", (long)peak, gain);
        for (size_t i = 0; i < mono.size(); i++) {
            int32_t v = (int32_t)(mono[i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mono[i] = (int16_t)v;
        }
        peak = (int32_t)(peak * gain);
        if (peak > 32767) peak = 32767;
    } else {
        ESP_LOGI(TAG, "PCM peak=%ld frames=%u duration_ms≈%u",
                 (long)peak, (unsigned)src_frames,
                 (unsigned)(hdr.sample_rate > 0 ? (src_frames * 1000 / hdr.sample_rate) : 0));
    }

    // Linear resample to codec rate when needed
    std::vector<int16_t> play_buf;
    if (hdr.sample_rate == out_rate || hdr.sample_rate <= 0) {
        play_buf = std::move(mono);
    } else {
        size_t out_frames = (size_t)((int64_t)src_frames * out_rate / hdr.sample_rate);
        play_buf.resize(out_frames);
        for (size_t i = 0; i < out_frames; i++) {
            double src_pos = (double)i * hdr.sample_rate / out_rate;
            size_t i0 = (size_t)src_pos;
            size_t i1 = (src_frames > 0) ? std::min(i0 + 1, src_frames - 1) : 0;
            double frac = src_pos - i0;
            play_buf[i] = (int16_t)(mono[i0] * (1.0 - frac) + mono[i1] * frac);
        }
        ESP_LOGI(TAG, "Resampled %u -> %u frames (%dHz -> %dHz)",
                 (unsigned)src_frames, (unsigned)out_frames, (int)hdr.sample_rate, out_rate);
    }

    // All-zero clip: play a short marker so silence is distinguishable from path failure
    if (peak == 0 && !play_buf.empty()) {
        ESP_LOGW(TAG, "WAV PCM is silent — playing marker beep");
        const int rate = out_rate > 0 ? out_rate : 24000;
        const int beep_n = rate / 5;  // 200ms
        play_buf.assign(beep_n, 0);
        for (int i = 0; i < beep_n; i++) {
            float t = (float)i / (float)rate;
            play_buf[i] = (int16_t)(0.5f * 32767.0f * sinf(2.0f * 3.14159265f * 1000.0f * t));
        }
    }

    size_t cur = 0;
    while (!stop_requested_ && cur < play_buf.size()) {
        size_t n = std::min((size_t)CHUNK_SAMPLES, play_buf.size() - cur);
        out.assign(play_buf.begin() + cur, play_buf.begin() + cur + n);
        codec_->OutputData(out);
        total_samples += n;
        chunks++;
        cur += n;
    }

    if (stop_requested_) {
        ESP_LOGI(TAG, "Playback stopped after %u samples (%d chunks)", (unsigned)total_samples, chunks);
    } else {
        ESP_LOGI(TAG, "Playback finished: %u samples (%d chunks)", (unsigned)total_samples, chunks);
    }

    return !stop_requested_;
}

void VmrClient::AcknowledgeCurrentMessage() {
    if (current_message_id_.empty()) return;

    ESP_LOGI(TAG, "Acknowledging message: %s", current_message_id_.c_str());
    ReportPlayEvent(current_message_id_, kVmrEventAcknowledged);
}

void VmrClient::StopPlayback() {
    TaskHandle_t task_to_wait = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_) return;
        stop_requested_ = true;
        task_to_wait = play_task_;
    }

    // Wait for play task to finish with semaphore (2s timeout)
    if (task_to_wait != nullptr && play_done_sem_ != nullptr) {
        xSemaphoreTake(play_done_sem_, pdMS_TO_TICKS(2000));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
    }
}

// ============================================================
// Play event reporting
// ============================================================

void VmrClient::ReportPlayEvent(const std::string& message_id, VmrPlayEvent event,
                                const std::string& reason) {
    if (device_id_.empty()) return;

    const char* event_str = "unknown";
    switch (event) {
        case kVmrEventStarted:       event_str = "started";       break;
        case kVmrEventCompleted:     event_str = "completed";     break;
        case kVmrEventAcknowledged:  event_str = "acknowledged";  break;
        case kVmrEventFailed:        event_str = "failed";        break;
    }

    // Build JSON body
    cJSON* root = cJSON_CreateObject();
    cJSON* events = cJSON_CreateArray();
    cJSON* evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "message_id", message_id.c_str());
    cJSON_AddStringToObject(evt, "event", event_str);
    if (!reason.empty()) {
        cJSON_AddStringToObject(evt, "reason", reason.c_str());
    }
    // event_at: ISO8601 timestamp (UTC). Device time may be ~epoch until NTP syncs,
    // but a well-formed timestamp avoids 422 from servers that reject empty strings.
    char ts[32] = "";
    time_t now = time(nullptr);
    if (now > 0) {
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }
    cJSON_AddStringToObject(evt, "event_at", ts);
    cJSON_AddItemToArray(events, evt);
    cJSON_AddItemToObject(root, "events", events);
    cJSON_AddNumberToObject(root, "queue_version", local_version_);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string body(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    if (!network_ready_.load()) {
        ESP_LOGW(TAG, "Skip event report (network down): %s → %s",
                 message_id.c_str(), event_str);
        return;
    }

    std::string url = server_url_ + "/devices/" + device_id_ + "/messages/queue/sync";
    std::string response;
    if (HttpPost(url, body, response)) {
        ESP_LOGI(TAG, "Event reported: %s → %s", message_id.c_str(), event_str);
    } else {
        ESP_LOGW(TAG, "Failed to report event: %s → %s", message_id.c_str(), event_str);
    }
}

void VmrClient::ReportPlayEventAsync(const std::string& message_id, VmrPlayEvent event,
                                     const std::string& reason) {
    struct Ctx {
        VmrClient* self;
        std::string mid;
        VmrPlayEvent ev;
        std::string reason;
    };
    auto* ctx = new Ctx{this, message_id, event, reason};
    BaseType_t ok = xTaskCreate([](void* arg) {
        auto* c = static_cast<Ctx*>(arg);
        c->self->ReportPlayEvent(c->mid, c->ev, c->reason);
        delete c;
        vTaskDelete(nullptr);
    }, "vmr_evt", 6144, ctx, 3, nullptr);
    if (ok != pdPASS) {
        delete ctx;
        // Fallback: skip rather than block audio path
        ESP_LOGW(TAG, "Async event report task failed for %s", message_id.c_str());
    }
}

// ============================================================
// Persistence (NVS)
// ============================================================

namespace {

constexpr size_t kMaxPlayedIds = 128;
constexpr const char* kPlayedBlobKey = "played_b";
constexpr const char* kPlayedLegacyKey = "played_ids";

bool UuidToBytes(const std::string& id, uint8_t out[16]) {
    char hex[33];
    size_t n = 0;
    for (unsigned char c : id) {
        if (c == '-') {
            continue;
        }
        if (!std::isxdigit(c) || n >= 32) {
            return false;
        }
        hex[n++] = static_cast<char>(std::tolower(c));
    }
    if (n != 32) {
        return false;
    }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        return 10 + (c - 'a');
    };
    for (int i = 0; i < 16; i++) {
        out[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
    }
    return true;
}

std::string BytesToUuid(const uint8_t in[16]) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[in[i] >> 4]);
        out.push_back(kHex[in[i] & 0xf]);
    }
    return out;
}

}  // namespace

void VmrClient::LoadPlayedIds() {
    played_ids_.clear();
    played_order_.clear();

    Settings settings(NVS_NS, false);

    // Preferred: packed UUID blob (16 bytes/id) — fits many more entries in 16KB NVS.
    std::vector<uint8_t> blob;
    if (settings.GetBlob(kPlayedBlobKey, blob) && blob.size() >= 16 && (blob.size() % 16) == 0) {
        for (size_t i = 0; i + 16 <= blob.size(); i += 16) {
            std::string id = BytesToUuid(blob.data() + i);
            if (played_ids_.insert(id).second) {
                played_order_.push_back(id);
            }
        }
        ESP_LOGI(TAG, "Loaded %d played IDs from NVS blob", (int)played_ids_.size());
        return;
    }

    // Legacy comma-separated UUID string
    std::string data = settings.GetString(kPlayedLegacyKey, "");
    if (data.empty()) {
        ESP_LOGI(TAG, "Loaded 0 played IDs from NVS");
        return;
    }

    size_t pos = 0;
    while (pos < data.size()) {
        size_t next = data.find(',', pos);
        if (next == std::string::npos) next = data.size();
        if (next > pos) {
            std::string id = data.substr(pos, next - pos);
            if (played_ids_.insert(id).second) {
                played_order_.push_back(id);
            }
        }
        pos = next + 1;
    }
    ESP_LOGI(TAG, "Loaded %d played IDs from NVS (legacy string)", (int)played_ids_.size());
    // Migrate to compact blob and free the large string entry (reclaims NVS pages).
    PersistPlayedIds();
}

void VmrClient::SavePlayedId(const std::string& message_id) {
    if (message_id.empty()) return;

    // First-play only: do NOT re-touch on replay. Re-touch made an old replay look
    // "latest" and broke no-unread selection when cloud timestamps were missing.
    if (played_ids_.count(message_id)) {
        return;
    }

    played_ids_.insert(message_id);
    played_order_.push_back(message_id);

    while (played_order_.size() > kMaxPlayedIds) {
        played_ids_.erase(played_order_.front());
        played_order_.erase(played_order_.begin());
    }

    PersistPlayedIds();
}

void VmrClient::ClearPlayedId(const std::string& message_id) {
    if (played_ids_.erase(message_id) == 0) return;
    for (auto it = played_order_.begin(); it != played_order_.end(); ++it) {
        if (*it == message_id) {
            played_order_.erase(it);
            break;
        }
    }
    PersistPlayedIds();
}

void VmrClient::PersistPlayedIds() {
    std::vector<uint8_t> blob;
    blob.reserve(played_order_.size() * 16);
    size_t skipped = 0;
    for (const auto& id : played_order_) {
        uint8_t raw[16];
        if (!UuidToBytes(id, raw)) {
            skipped++;
            continue;
        }
        blob.insert(blob.end(), raw, raw + 16);
    }
    if (skipped > 0) {
        ESP_LOGW(TAG, "PersistPlayedIds: skipped %u non-UUID id(s)", (unsigned)skipped);
    }

    Settings settings(NVS_NS, true);
    // NVS updates need free space for the NEW value while the OLD still exists.
    // Erase+commit first so a full page does not force us to drop played IDs.
    settings.EraseKey(kPlayedLegacyKey);
    settings.EraseKey(kPlayedBlobKey);
    if (!settings.Commit()) {
        ESP_LOGE(TAG, "Failed to commit played_ids erase");
    }

    if (blob.empty()) {
        // Nothing to store; legacy/blob already erased.
        settings.Commit();
        return;
    }

    if (settings.SetBlob(kPlayedBlobKey, blob.data(), blob.size()) && settings.Commit()) {
        ESP_LOGI(TAG, "Persisted %d played IDs (%u bytes blob)",
                 (int)(blob.size() / 16), (unsigned)blob.size());
        return;
    }

    // Absolute last resort: drop oldest until write fits (should be rare after erase).
    while (blob.size() >= 32) {
        blob.erase(blob.begin(), blob.begin() + 16);
        played_ids_.erase(played_order_.front());
        played_order_.erase(played_order_.begin());
        settings.EraseKey(kPlayedBlobKey);
        settings.Commit();
        if (settings.SetBlob(kPlayedBlobKey, blob.data(), blob.size()) && settings.Commit()) {
            ESP_LOGW(TAG, "Persisted played IDs after emergency shrink to %d",
                     (int)(blob.size() / 16));
            return;
        }
    }
    ESP_LOGE(TAG, "Failed to persist played_ids after retries");
}

bool VmrClient::HasBeenPlayed(const std::string& message_id) const {
    return played_ids_.find(message_id) != played_ids_.end();
}
