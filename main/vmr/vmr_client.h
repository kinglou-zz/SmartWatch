#ifndef VMR_CLIENT_H_
#define VMR_CLIENT_H_

#include <string>
#include <vector>
#include <functional>
#include <set>
#include <map>
#include <mutex>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

class AudioCodec;

// ============================================================
// Data structures
// ============================================================

struct VmrQueueItem {
    std::string message_id;
    std::string audio_url;
    int priority = 0;
    int position = -1;          // queue slot (not reliable for wall-clock recency)
    std::string message_type;   // "voice", "reminder_immediate", "reminder_scheduled", etc.
    std::string queue_kind;     // "original", "immediate", "scheduled", etc.
    std::string scheduled_at;   // ISO8601 cloud schedule/play time
    std::string created_at;     // ISO8601 create/update time if present
};

struct VmrQueueResponse {
    int64_t version = 0;
    std::vector<VmrQueueItem> items;
};

enum VmrPlayEvent {
    kVmrEventStarted,
    kVmrEventCompleted,
    kVmrEventAcknowledged,
    kVmrEventFailed,
};

/** Playback strategy per 留言/提醒边下边播&边读边播方案. */
enum class VmrPlaybackMode {
    kStreamWhileDownload,  // HTTPS → ring → play; SD filled in background if available
    kStreamWhileRead,      // SD ready: fread chunks → play
    kStreamWhileWrite,     // HTTPS → ring + write SD → play (optional / slower)
    kDownloadThenPlay,     // fallback only
};

// ============================================================
// VmrClient — HTTP polling, download, playback, event reporting
// ============================================================

/**
 * @brief Voice Message Recording (VMR) client.
 *
 * Implements "轨道 B" (HTTP polling) with streaming playback:
 *   1. Poll GET /devices/{id}/messages/queue?since_version=N every 30s
 *   2. Select mode: stream_while_read | stream_while_write | stream_while_download
 *   3. Stream-decode WAV through AudioCodec::OutputData()
 *   4. Report play events via POST /devices/{id}/messages/queue/sync
 *
 * Thread-safe: all public methods can be called from any task.
 * Callbacks fire on the main task via Application::Schedule().
 */
class VmrClient {
public:
    VmrClient();
    ~VmrClient();

    // ---- Lifecycle ----

    /**
     * Initialize with device identity.
     * @param device_id  Device ID used in API paths (matches JWT "did")
     * @param token      JWT Bearer token for Authorization header
     * @param device_uid Device UID for Device-Id header (matches JWT "duid")
     * @param server_url Base HTTP URL, e.g. "http://***"
     */
    void Initialize(const std::string& device_id,
                    const std::string& token,
                    const std::string& device_uid,
                    const std::string& server_url);

    /**
     * Start periodic queue polling (every 30 seconds).
     * Safe to call multiple times — subsequent calls are no-ops.
     */
    void StartPolling();

    /**
     * Stop periodic queue polling.
     */
    void StopPolling();

    /**
     * Trigger an immediate poll (e.g. after playback finishes).
     * No-op while suspended (see SetPollingSuspended).
     */
    void PollNow();

    /**
     * After voice-confirmed reminder save: poll every few seconds for ~90s so a
     * short scheduled reminder is noticed soon after it becomes due.
     */
    void ArmReminderCatchupPolls();

    /**
     * Suspend/resume queue polling without tearing down the timer.
     * Use during TTS speaking so HTTP does not contend with WS audio.
     */
    void SetPollingSuspended(bool suspended);
    bool IsPollingSuspended() const { return poll_suspended_; }

    /**
     * Gate HTTP while the link is down (WiFi reconnect / modem drop).
     * Polls and event reports no-op until ready again — avoids EspTcp 0x71/0x76 noise.
     */
    void SetNetworkReady(bool ready);
    bool IsNetworkReady() const { return network_ready_.load(); }

    /// True while queue HTTP / playback needs reliable Wi-Fi (block LOW_POWER PS).
    bool IsWifiHoldActive() const { return wifi_hold_count_.load(std::memory_order_acquire) > 0; }
    void AcquireWifiHold();
    void ReleaseWifiHold();

    // ---- Playback control ----

    /**
     * Play the latest unplayed message.
     * Downloads audio, plays it, reports events.
     * @return true if a message was queued for playback
     */
    bool PlayLatestMessage();

    /**
     * Play embedded generated test tone (24kHz WAV) through the same codec path.
     * Used to verify speaker / VMR playback without depending on server audio.
     */
    bool PlayTestAudio();

    /**
     * Acknowledge the currently/last played message as "heard".
     */
    void AcknowledgeCurrentMessage();

    /**
     * Stop current playback (if any).
     */
    void StopPlayback();

    // ---- State queries ----

    bool IsPlaying() const { return playing_; }
    bool HasUnplayedMessages() const { return !pending_items_.empty(); }
    int UnplayedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (int)pending_items_.size();
    }
    const std::string& GetCurrentMessageId() const { return current_message_id_; }

    // ---- Callbacks (set before Initialize) ----

    /// Called when new messages are detected
    std::function<void(int count)> on_new_messages;

    /// Called when playback starts
    std::function<void(const std::string& message_id)> on_playback_started;

    /// Called when playback finishes (success or failure)
    std::function<void(const std::string& message_id, bool success)> on_playback_finished;

    /// Called on unrecoverable error
    std::function<void(const std::string& error)> on_error;

    /// True if the last successful play was of an unread item (needs Read/Unread confirm).
    bool LastPlayNeedsReadConfirm() const { return play_needs_read_confirm_; }

    /// Mark message as locally read (NVS) and report cloud acknowledged.
    void MarkMessageRead(const std::string& message_id);

    /// Keep as unread: ensure it stays in pending queue for next play.
    void KeepMessageUnread(const std::string& message_id);

private:
    // ---- HTTP helpers ----
    bool HttpGet(const std::string& url, std::string& response_body);
    bool HttpPost(const std::string& url, const std::string& body, std::string& response_body);
    bool DownloadToBuffer(const std::string& url, std::vector<uint8_t>& buffer);
    /// Background/SD-sync helper: prefer ready cache, else download (may still buffer).
    bool AcquireAudio(const std::string& message_id, const std::string& audio_url,
                      std::vector<uint8_t>& buffer);
    /// Stream HTTP body directly to SD file (no full-file RAM). Used by sync/prefetch.
    bool DownloadToSdFile(const std::string& message_id, const std::string& audio_url);
    /// Same as DownloadToSdFile but caller already holds audio_fetch_mutex_.
    bool DownloadToSdFileLocked(const std::string& message_id, const std::string& audio_url);

    // ---- SD cache (/sdcard/vmr/{id}.wav) ----
    bool IsSdAvailable() const;
    std::string SdPathFor(const std::string& message_id) const;
    bool EnsureSdVmrDir() const;
    bool IsSdCacheReady(const std::string& message_id) const;
    bool LoadAudioFromSd(const std::string& message_id, std::vector<uint8_t>& buffer) const;
    bool SaveAudioToSd(const std::string& message_id, const std::vector<uint8_t>& buffer) const;
    bool IsWavBufferComplete(const std::vector<uint8_t>& buffer) const;
    void InvalidateSdAudio(const std::string& message_id) const;
    void RememberCatalogItem(const VmrQueueItem& item);
    void ScheduleSdSync();
    void RunSdSync();
    void PruneSdCacheLocked();  // caller holds mutex_ for played_order_/catalog_

    // ---- Playback mode / streaming ----
    VmrPlaybackMode SelectPlaybackMode(const std::string& message_id,
                                       const std::string& audio_url) const;
    static const char* PlaybackModeName(VmrPlaybackMode mode);
    bool PlayStreaming(VmrPlaybackMode mode, const std::string& message_id,
                       const std::string& audio_url);
    bool PlayStreamingFromSd(const std::string& message_id);
    bool PlayStreamingFromHttp(const std::string& message_id, const std::string& audio_url,
                               bool write_disk);

    // ---- Internal flow ----
    void DoPoll();
    void ProcessQueueResponse(const std::string& body, bool include_played = false);
    /// Sync fetch queue and pick one item to play (allows already-played when needed).
    bool FetchPlayableItem(VmrQueueItem& out);
    void ProcessNextMessage();
    void FinishPlaybackSession(const std::string& mid, bool success);
    void DoPlayback(const std::vector<uint8_t>& buffer);  // legacy whole-buffer path
    bool PlayFromBuffer(const std::vector<uint8_t>& buffer);
    void PrefetchFrontAudio();
    void ReportPlayEvent(const std::string& message_id, VmrPlayEvent event,
                         const std::string& reason = "");
    void ReportPlayEventAsync(const std::string& message_id, VmrPlayEvent event,
                              const std::string& reason = "");

    // ---- Timer callbacks ----
    static void PollTimerCallback(void* arg);
    static void CatchupTimerCallback(void* arg);

    // ---- Persistence ----
    void LoadPlayedIds();
    void SavePlayedId(const std::string& message_id);
    void ClearPlayedId(const std::string& message_id);
    void PersistPlayedIds();
    bool HasBeenPlayed(const std::string& message_id) const;

    // ---- State ----
    std::string device_id_;
    std::string device_uid_;     // Device-Id header value (matches JWT duid)
    std::string token_;          // "Bearer xxx" format
    std::string server_url_;
    int64_t local_version_ = 0;

    esp_timer_handle_t poll_timer_ = nullptr;
    bool polling_ = false;
    bool poll_in_progress_ = false;
    int64_t poll_started_us_ = 0;  // for stuck-poll watchdog
    std::atomic<bool> poll_suspended_{false};
    std::atomic<bool> network_ready_{false};
    std::atomic<int> wifi_hold_count_{0};

    // After voice-confirmed reminder save: denser polls until due item appears
    esp_timer_handle_t catchup_timer_ = nullptr;
    int catchup_remaining_ = 0;

    std::vector<VmrQueueItem> pending_items_;
    std::map<std::string, VmrQueueItem> catalog_;  // known id → metadata/url
    std::string current_message_id_;
    std::string play_audio_url_;
    VmrPlaybackMode play_mode_ = VmrPlaybackMode::kStreamWhileDownload;
    std::string prefetched_id_;  // SD warmed / marked ready (no full-file RAM)
    std::atomic<bool> prefetch_in_progress_{false};
    std::atomic<bool> sd_sync_in_progress_{false};
    /// Set while a play task needs the fetch lock; background download should exit.
    std::atomic<bool> audio_fetch_abort_{false};
    VmrQueueItem last_played_item_;  // newest successfully played (for unread-empty replay)
    AudioCodec* codec_ = nullptr;

    bool playing_ = false;
    bool stop_requested_ = false;
    /// Set when starting play of an item not yet in played_ids_ (needs user confirm).
    bool play_needs_read_confirm_ = false;
    TaskHandle_t play_task_ = nullptr;
    SemaphoreHandle_t play_done_sem_ = nullptr;  // signaled when play task exits

    std::set<std::string> played_ids_;                    // fast lookup
    std::vector<std::string> played_order_;                // insertion order (recent last)
    bool unmark_last2_once_ = false;                       // one-shot already consumed

    // Protects: pending_items_, playing_, stop_requested_, current_message_id_,
    //           prefetched_*, play_audio_url_, play_mode_, play_task_, catalog_
    mutable std::mutex mutex_;
    // Serializes download + SD write so prefetch/sync/play cannot truncate the same wav
    mutable std::mutex audio_fetch_mutex_;

    static constexpr int POLL_INTERVAL_SEC = 10;  // was 30 — short scheduled reminders (e.g. 20s) need denser polls
    // EspTcp connect_id must not be shared across concurrent HTTP or Abort/Close
    // on SD prefetch kills the in-flight play stream (bytes=0 → Playback failed).
    static constexpr int kHttpIdPoll = 0;   // queue poll + event POST
    static constexpr int kHttpIdPlay = 1;   // live stream_while_download producer
    static constexpr int kHttpIdBg = 2;    // SD prefetch / sync / buffer download
    static constexpr int SD_KEEP_READ_COUNT = 20;
    // ~8s @16k mono. Adaptive open may fill most of the ring before first audio.
    static constexpr size_t kRingCapDownload = 256 * 1024;
    static constexpr size_t kRingCapSdRead = 64 * 1024;
    // Minimum open buffer when download ≥ realtime; slow links wait for catch-up.
    // 16kHz mono 16-bit → 32KB/s; 2000ms ≈ 64KB.
    static constexpr int kPrebufferDownloadMs = 2000;
    static constexpr int kPrebufferSdMs = 400;
    static constexpr int kRebufferDownloadMs = 1500;
    static constexpr int kRebufferSdMs = 400;
    static constexpr int kUnderrunGiveupDownloadMs = 30000;
    static constexpr int kUnderrunGiveupSdMs = 8000;
    // Keep modest: SD reader task stack is tight; HTTP uses the same constant.
    static constexpr size_t kIoChunkBytes = 2048;
    static constexpr const char* NVS_NS = "vmr";
    static constexpr const char* SD_VMR_DIR = "/sdcard/vmr";
};

#endif // VMR_CLIENT_H_
