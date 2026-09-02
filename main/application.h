#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <chrono>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "vmr/vmr_client.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    DeviceStateMachine& GetStateMachine() { return state_machine_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    VmrClient& GetVmrClient() { return vmr_client_; }

    /**
     * Play the latest VMR (voice message) if any are pending.
     * Thread-safe — can be called from button ISRs.
     */
    void PlayVmrMessage();

    /**
     * Acknowledge the current VMR message as "heard".
     * Thread-safe — can be called from button ISRs.
     */
    void AcknowledgeVmrMessage();

    /**
     * Confirm Read (true) or Unread (false) after VMR playback.
     * Thread-safe — can be called from touch / LVGL handlers.
     */
    void OnVmrReadConfirm(bool marked_read);
    bool IsVmrReadConfirmPending() const {
        return vmr_read_confirm_pending_.load(std::memory_order_acquire);
    }

    /**
     * Confirm or cancel an intent (Phase V3).
     * Thread-safe — can be called from button handlers.
     */
    void ConfirmIntent(bool confirm);
    bool HasPendingIntent() const { return intent_pending_.load(std::memory_order_acquire); }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t speaking_watchdog_timer_ = nullptr;
    static constexpr int SPEAKING_IDLE_TIMEOUT_MS = 45000;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;
    VmrClient vmr_client_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    std::atomic<bool> intent_pending_{false}; // Phase V3: waiting for user to confirm/cancel intent
    std::atomic<bool> vmr_read_confirm_pending_{false};
    std::atomic<bool> vmr_autoplay_wanted_{false};  // new msgs arrived while busy
    std::string vmr_confirm_message_id_;
    esp_timer_handle_t vmr_read_confirm_timer_ = nullptr;
    static constexpr int VMR_READ_CONFIRM_TIMEOUT_MS = 10000;
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;

    void StartVmrReadConfirm(const std::string& message_id);
    void CancelVmrReadConfirmTimer();
    void ResolveVmrReadConfirm(bool marked_read, bool from_timeout);
    void TryChainPlayNextVmr();
    void UpdateVmrBanner(int count);
    /// Start VMR play if Idle/ready; returns true if play kicked off.
    bool TryStartVmrAutoPlay(const char* reason);
    static void VmrReadConfirmTimerCallback(void* arg);


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void InitializeVmr();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // Speaking idle watchdog: exit speaking if no downlink audio for SPEAKING_IDLE_TIMEOUT_MS
    void StartSpeakingIdleWatchdog();
    void FeedSpeakingIdleWatchdog();
    void CancelSpeakingIdleWatchdog();
    void OnSpeakingIdleWatchdogTimeout();

    // Speaking-period downlink arrival gaps (into decode queue) — isolates WS receive holes
    void ResetSpeakingAudioArrivalStats();
    void NoteSpeakingAudioArrival();
    void LogSpeakingAudioArrivalSummary(const char* reason);
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);

    // Speaking audio arrival telemetry (only packets accepted into decode)
    static constexpr int AUDIO_GAP_WARN_MS = 150;  // >~2.5× 55ms pace
    bool speaking_audio_have_last_ = false;
    int speaking_audio_count_ = 0;
    int speaking_audio_max_gap_ms_ = 0;
    int speaking_audio_sum_gap_ms_ = 0;
    std::chrono::steady_clock::time_point speaking_audio_last_{};
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
