#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "provision.h"
#include <wifi_manager.h>

#include <cstring>
#include <esp_log.h>
#include <esp_sntp.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <sys/time.h>
#include <ctime>

#define TAG "Application"

// Staging skips OTA server_time; sync wall clock via SNTP for UI date/time.
static bool SyncTimeViaSntp() {
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();

    // Wait until wall clock leaves unix epoch (async SNTP)
    for (int i = 0; i < 40; ++i) {
        time_t now = time(nullptr);
        if (now > 1700000000) {  // after 2023-11
            struct tm tm_info = {};
            localtime_r(&now, &tm_info);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
            ESP_LOGI(TAG, "SNTP synced: %s", buf);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(TAG, "SNTP sync timeout");
    return false;
}


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    esp_timer_create_args_t speaking_idle_args = {
        .callback = [](void* arg) {
            static_cast<Application*>(arg)->OnSpeakingIdleWatchdogTimeout();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "speak_idle_wd",
        .skip_unhandled_events = true
    };
    esp_timer_create(&speaking_idle_args, &speaking_watchdog_timer_);
}

Application::~Application() {
    CancelVmrReadConfirmTimer();
    if (vmr_read_confirm_timer_ != nullptr) {
        esp_timer_delete(vmr_read_confirm_timer_);
        vmr_read_confirm_timer_ = nullptr;
    }
    CancelSpeakingIdleWatchdog();
    if (speaking_watchdog_timer_ != nullptr) {
        esp_timer_delete(speaking_watchdog_timer_);
        speaking_watchdog_timer_ = nullptr;
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Apply staging provisioning (force-overwrite, skip OTA when active)
    Provision::ApplyDefaults();

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    // Always re-enable VMR HTTP on link-up (even if activation is already running).
    vmr_client_.SetNetworkReady(true);
    vmr_client_.PollNow();

    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar(true);
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Gate VMR polls/reports while the link is down (avoids EspTcp 0x71/0x76).
    vmr_client_.SetNetworkReady(false);

    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    // Keep SNTP-set flag if staging path already synced.
    if (ota_ && ota_->HasServerTime()) {
        has_server_time_ = true;
    }

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");
    display->UpdateStatusBar(true);

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    if (Provision::IsStagingProvisioned()) {
        // Staging mode: skip xiaozhi OTA entirely.
        // WS URL / token are provided by Provision, not by xiaozhi's server.
        ESP_LOGI(TAG, "Staging provisioning active — skipping OTA check");
        // OTA was the only wall-clock source; sync via SNTP instead.
        if (SyncTimeViaSntp()) {
            has_server_time_ = true;
        }
    } else {
        // Production mode: check for new firmware version via OTA
        CheckNewVersion();
        if (ota_ && !ota_->HasServerTime()) {
            if (SyncTimeViaSntp()) {
                has_server_time_ = true;
            }
        }
    }

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    // Staging provisioning: force-overwrite WS URL / token after OTA
    // (OTA response may have overwritten them with xiaozhi.me config)
    Provision::ApplyDefaults();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (Provision::IsStagingProvisioned()) {
        // Staging mode: force WebSocket to our staging server
        ESP_LOGI(TAG, "Staging provisioning active — using WebSocket protocol");
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            FeedSpeakingIdleWatchdog();
            // Only count packets that actually enter decode (drop=full skipped).
            if (audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
                NoteSpeakingAudioArrival();
            }
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        // Server is still generating — keep speaking watchdog alive across TTS gaps.
                        FeedSpeakingIdleWatchdog();
                        // Only re-arm prebuffer when the play queue is nearly empty.
                        // Resetting while buffered PCM remains causes an audible mid-reply stall.
                        if (audio_service_.IsIdle()) {
                            audio_service_.ResetPrebuffer();
                        }
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "intent_confirmation") == 0) {
            // Phase V3: VMR conversation creation — server sends intent confirmation
            auto summary = cJSON_GetObjectItem(root, "summary");
            auto intent = cJSON_GetObjectItem(root, "intent");
            if (cJSON_IsString(summary)) {
                ESP_LOGI(TAG, "Intent confirmation: %s (intent=%s)",
                         summary->valuestring,
                         intent && cJSON_IsString(intent) ? intent->valuestring : "unknown");
                Schedule([this, display, msg = std::string(summary->valuestring)]() {
                    intent_pending_.store(true, std::memory_order_release);
                    display->SetChatMessage("system", msg.c_str());
                    display->ShowNotification("Confirm? Press button");
                });
            }
        } else if (strcmp(type->valuestring, "message_upload_complete") == 0) {
            // Phase V3: server confirms message was saved
            auto mid = cJSON_GetObjectItem(root, "message_id");
            if (cJSON_IsString(mid)) {
                ESP_LOGI(TAG, "Message upload complete: %s", mid->valuestring);
                Schedule([this, display, msg_id = std::string(mid->valuestring)]() {
                    intent_pending_.store(false, std::memory_order_release);
                    display->ShowNotification("Message saved");
                    // Voice-confirmed reminder: poll densely until due item shows up.
                    vmr_client_.ArmReminderCatchupPolls();
                });
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();

    // Initialize VMR (Voice Message Recording) HTTP client
    InitializeVmr();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Wait for WiFi to be ready (P4 remote WiFi may need time to reconnect)
        auto& wifi = WifiManager::GetInstance();
        for (int i = 0; i < 30 && !wifi.IsConnected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!wifi.IsConnected()) {
            ESP_LOGE(TAG, "WiFi not connected, aborting OpenAudioChannel");
            SetDeviceState(kDeviceStateIdle);
            return;
        }

        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Wait for WiFi to be ready (P4 remote WiFi may need time to reconnect)
        auto& wifi = WifiManager::GetInstance();
        for (int i = 0; i < 30 && !wifi.IsConnected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!wifi.IsConnected()) {
            ESP_LOGE(TAG, "WiFi not connected, aborting wake word invoke");
            SetDeviceState(kDeviceStateIdle);
            audio_service_.EnableWakeWordDetection(true);
            return;
        }

        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);

    // Set flag to play popup sound after state changes to listening
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    if (new_state == kDeviceStateSpeaking) {
        // Always park VMR HTTP during TTS — queue GET/SD sync share Wi-Fi/TCP with
        // the WS audio path and cause multi-second AUDIO GAP / underruns.
        // Reminders catch up when leaving speaking (polling resumed below).
        vmr_client_.SetPollingSuspended(true);
        StartSpeakingIdleWatchdog();
    } else {
        CancelSpeakingIdleWatchdog();
        vmr_client_.SetPollingSuspended(false);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
            break;
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            // Wake word detection disabled — button is used for wake-up instead
            // audio_service_.EnableWakeWordDetection(true);
            // Catch up: new messages may have arrived while Connecting/Speaking
            if (vmr_autoplay_wanted_.load(std::memory_order_acquire)) {
                TryStartVmrAutoPlay("enter_idle");
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                // Park uplink AFE entirely during TTS — wake-word feed contended
                // with Opus decode (AFE FEED full + multi-second AUDIO GAP).
                // Button / AbortSpeaking still works; realtime keeps barge-in.
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::StartSpeakingIdleWatchdog() {
    if (speaking_watchdog_timer_ == nullptr) {
        return;
    }
    ResetSpeakingAudioArrivalStats();
    esp_timer_stop(speaking_watchdog_timer_);
    esp_err_t err = esp_timer_start_once(speaking_watchdog_timer_,
                                         SPEAKING_IDLE_TIMEOUT_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start speaking idle watchdog: %s", esp_err_to_name(err));
    }
}

void Application::FeedSpeakingIdleWatchdog() {
    if (speaking_watchdog_timer_ == nullptr || GetDeviceState() != kDeviceStateSpeaking) {
        return;
    }
    esp_timer_stop(speaking_watchdog_timer_);
    esp_timer_start_once(speaking_watchdog_timer_, SPEAKING_IDLE_TIMEOUT_MS * 1000ULL);
}

void Application::CancelSpeakingIdleWatchdog() {
    LogSpeakingAudioArrivalSummary("leave_speaking");
    if (speaking_watchdog_timer_ != nullptr) {
        esp_timer_stop(speaking_watchdog_timer_);
    }
}

void Application::ResetSpeakingAudioArrivalStats() {
    speaking_audio_have_last_ = false;
    speaking_audio_count_ = 0;
    speaking_audio_max_gap_ms_ = 0;
    speaking_audio_sum_gap_ms_ = 0;
}

void Application::NoteSpeakingAudioArrival() {
    auto now = std::chrono::steady_clock::now();
    if (speaking_audio_have_last_) {
        int gap_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - speaking_audio_last_).count());
        speaking_audio_sum_gap_ms_ += gap_ms;
        if (gap_ms > speaking_audio_max_gap_ms_) {
            speaking_audio_max_gap_ms_ = gap_ms;
        }
        // >~2.5× of cloud 55ms pace → likely a receive hole before underrun.
        if (gap_ms >= AUDIO_GAP_WARN_MS) {
            ESP_LOGW(TAG, "AUDIO GAP: %dms (max=%d, n=%d)",
                     gap_ms, speaking_audio_max_gap_ms_, speaking_audio_count_ + 1);
        }
    }
    speaking_audio_last_ = now;
    speaking_audio_have_last_ = true;
    speaking_audio_count_++;
}

void Application::LogSpeakingAudioArrivalSummary(const char* reason) {
    if (speaking_audio_count_ <= 0) {
        return;
    }
    int avg_gap = 0;
    if (speaking_audio_count_ > 1) {
        avg_gap = speaking_audio_sum_gap_ms_ / (speaking_audio_count_ - 1);
    }
    ESP_LOGI(TAG, "AUDIO ARRIVE [%s]: n=%d max_gap=%dms avg_gap=%dms",
             reason, speaking_audio_count_, speaking_audio_max_gap_ms_, avg_gap);
    ResetSpeakingAudioArrivalStats();
}

void Application::OnSpeakingIdleWatchdogTimeout() {
    Schedule([this]() {
        if (GetDeviceState() != kDeviceStateSpeaking) {
            return;
        }
        // Local decode/playback still draining — network gap, not true idle.
        if (!audio_service_.IsIdle()) {
            ESP_LOGW(TAG, "Speaking idle watchdog: no downlink for %dms but queue not empty — re-arm",
                     SPEAKING_IDLE_TIMEOUT_MS);
            FeedSpeakingIdleWatchdog();
            return;
        }
        ESP_LOGE(TAG, "Speaking idle watchdog timeout (%dms) — forcing exit from speaking",
                 SPEAKING_IDLE_TIMEOUT_MS);
        aborted_ = true;
        if (protocol_) {
            protocol_->SendAbortSpeaking(kAbortReasonNone);
        }
        if (listening_mode_ == kListeningModeManualStop) {
            SetDeviceState(kDeviceStateIdle);
        } else {
            SetDeviceState(kDeviceStateListening);
        }
    });
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

// ============================================================
// VMR (Voice Message Recording) — Phase V1
// ============================================================

void Application::InitializeVmr() {
    Settings settings("websocket", false);
    std::string token = settings.GetString("token");
    std::string device_id = settings.GetString("device_id");

    // Use the device ID from NVS, or fall back to the staging default
    std::string vmr_device_id = settings.GetString("vmr_device_id");
    if (vmr_device_id.empty()) {
        // Derive from JWT "did" claim via staging default
        vmr_device_id = Provision::STAGING_VMR_DEVICE_ID;
    }

    // Use the staging VMR server URL (HTTP, port 80)
    std::string vmr_server = settings.GetString("vmr_server");
    if (vmr_server.empty()) {
        vmr_server = Provision::STAGING_VMR_SERVER;
    }

    std::string vmr_device_uid = settings.GetString("device_id");  // matches JWT duid
    vmr_client_.Initialize(vmr_device_id, token, vmr_device_uid, vmr_server);

    // Wire up callbacks
    vmr_client_.on_new_messages = [this](int count) {
        Schedule([this, count]() {
            UpdateVmrBanner(count);
            audio_service_.PlaySound(Lang::Sounds::OGG_VIBRATION);
            ESP_LOGI(TAG, "VMR: %d unplayed message(s)", count);

            if (count <= 0) {
                return;
            }
            // Always request autoplay; TryStart will run now or when Idle/confirm clears
            vmr_autoplay_wanted_.store(true, std::memory_order_release);
            if (!TryStartVmrAutoPlay("on_new_messages")) {
                ESP_LOGW(TAG, "VMR: defer auto-play (state=%d playing=%d confirm=%d)",
                         (int)GetDeviceState(), vmr_client_.IsPlaying() ? 1 : 0,
                         IsVmrReadConfirmPending() ? 1 : 0);
            }
        });
    };

    vmr_client_.on_playback_started = [this](const std::string& message_id) {
        Schedule([this, message_id]() {
            // Re-assert AFE park in case Listening raced back on before first PCM.
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.SuspendOutput();
            auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(Lang::Strings::MESSAGE_PLAYING);
            display->SetEmotion("microphone");
            ESP_LOGI(TAG, "VMR: playback started for %s", message_id.c_str());
        });
    };

    vmr_client_.on_playback_finished = [this](const std::string& message_id, bool success) {
        Schedule([this, message_id, success]() {
            audio_service_.ResumeOutput();
            auto display = Board::GetInstance().GetDisplay();
            if (success) {
                display->SetEmotion("neutral");
                const bool need_confirm = vmr_client_.LastPlayNeedsReadConfirm();
                if (need_confirm) {
                    StartVmrReadConfirm(message_id);
                } else {
                    display->SetStatus(Lang::Strings::STANDBY);
                    TryChainPlayNextVmr();
                }
            } else {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ShowNotification("Playback failed");
            }
            ESP_LOGI(TAG, "VMR: playback %s for %s (remaining=%d confirm=%d)",
                     success ? "completed" : "failed", message_id.c_str(),
                     vmr_client_.UnplayedCount(),
                     vmr_client_.LastPlayNeedsReadConfirm() ? 1 : 0);
        });
    };

    vmr_client_.on_error = [](const std::string& error) {
        ESP_LOGE(TAG, "VMR error: %s", error.c_str());
    };

    // Activation already required a live link; allow HTTP until a disconnect arrives.
    vmr_client_.SetNetworkReady(true);
    // Start polling for messages
    vmr_client_.StartPolling();

    ESP_LOGI(TAG, "VMR initialized: device=%s, server=%s", vmr_device_id.c_str(), vmr_server.c_str());
}

void Application::PlayVmrMessage() {
    Schedule([this]() {
        if (IsVmrReadConfirmPending()) {
            // Dismiss confirm as unread before starting another play? Keep waiting.
            ESP_LOGW(TAG, "VMR: confirm pending — ignore play request");
            return;
        }
        auto try_play = [this]() {
            auto display = Board::GetInstance().GetDisplay();
            // Download may take seconds — only switch to "playing" when audio starts
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            audio_service_.SuspendOutput();
            if (!vmr_client_.PlayLatestMessage()) {
                audio_service_.ResumeOutput();
                display->SetStatus(Lang::Strings::STANDBY);
            }
        };

        if (GetDeviceState() == kDeviceStateIdle) {
            try_play();
        } else if (GetDeviceState() == kDeviceStateSpeaking) {
            // Abort current TTS, then play VMR
            AbortSpeaking(kAbortReasonNone);
            SetDeviceState(kDeviceStateIdle);
            try_play();
        } else if (GetDeviceState() == kDeviceStateListening) {
            // Stop listening (park AFE) before VMR — otherwise feed>>fetch → AFE FEED full
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
            SetDeviceState(kDeviceStateIdle);
            try_play();
        } else {
            ESP_LOGW(TAG, "Cannot play VMR in state %d", (int)(GetDeviceState()));
        }
    });
}

void Application::AcknowledgeVmrMessage() {
    Schedule([this]() {
        // Prefer screen Read/Unread confirm when pending
        if (vmr_read_confirm_pending_.load(std::memory_order_acquire)) {
            ResolveVmrReadConfirm(true, false);
            return;
        }
        vmr_client_.AcknowledgeCurrentMessage();
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::VMR_MARKED_READ, 2500);
        UpdateVmrBanner(vmr_client_.UnplayedCount());
    });
}

void Application::OnVmrReadConfirm(bool marked_read) {
    Schedule([this, marked_read]() {
        ResolveVmrReadConfirm(marked_read, false);
    });
}

void Application::VmrReadConfirmTimerCallback(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->Schedule([app]() {
        app->ResolveVmrReadConfirm(false, true);
    });
}

void Application::CancelVmrReadConfirmTimer() {
    if (vmr_read_confirm_timer_ != nullptr) {
        esp_timer_stop(vmr_read_confirm_timer_);
    }
}

void Application::StartVmrReadConfirm(const std::string& message_id) {
    CancelVmrReadConfirmTimer();
    vmr_confirm_message_id_ = message_id;
    vmr_read_confirm_pending_.store(true, std::memory_order_release);

    auto display = Board::GetInstance().GetDisplay();
    display->on_vmr_read_confirm = [this](bool marked_read) {
        OnVmrReadConfirm(marked_read);
    };
    display->ShowVmrReadConfirm(Lang::Strings::VMR_READ, Lang::Strings::VMR_UNREAD);
    ESP_LOGI(TAG, "VMR: waiting Read/Unread confirm for %s (%ds)",
             message_id.c_str(), VMR_READ_CONFIRM_TIMEOUT_MS / 1000);

    if (vmr_read_confirm_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &Application::VmrReadConfirmTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vmr_read_cfm",
            .skip_unhandled_events = true,
        };
        esp_err_t cerr = esp_timer_create(&args, &vmr_read_confirm_timer_);
        if (cerr != ESP_OK) {
            ESP_LOGE(TAG, "VMR confirm timer create failed: %s", esp_err_to_name(cerr));
            vmr_read_confirm_timer_ = nullptr;
        }
    }
    if (vmr_read_confirm_timer_ != nullptr) {
        esp_err_t serr = esp_timer_start_once(vmr_read_confirm_timer_,
                                              (uint64_t)VMR_READ_CONFIRM_TIMEOUT_MS * 1000);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "VMR confirm timer start failed: %s", esp_err_to_name(serr));
        }
    }
}

void Application::ResolveVmrReadConfirm(bool marked_read, bool from_timeout) {
    // Atomic claim — gesture + LVGL click can both Schedule us.
    bool expected = true;
    if (!vmr_read_confirm_pending_.compare_exchange_strong(expected, false,
                                                          std::memory_order_acq_rel)) {
        return;
    }
    CancelVmrReadConfirmTimer();

    const std::string mid = vmr_confirm_message_id_;
    vmr_confirm_message_id_.clear();

    auto display = Board::GetInstance().GetDisplay();
    // Hide also nulls on_vmr_read_confirm under the LVGL lock.
    display->HideVmrReadConfirm();

    // Show tip FIRST (before any network / queue work that can stall the UI)
    display->SetStatus(Lang::Strings::STANDBY);
    if (marked_read) {
        display->ShowNotification(Lang::Strings::VMR_MARKED_READ, 2500);
        ESP_LOGI(TAG, "VMR: confirmed READ %s", mid.c_str());
        vmr_client_.MarkMessageRead(mid);
        const int remaining = vmr_client_.UnplayedCount();
        UpdateVmrBanner(remaining);
        if (remaining == 0) {
            vmr_autoplay_wanted_.store(false, std::memory_order_release);
            // After Message read tip, show All messages played
            BaseType_t ok = xTaskCreate([](void* arg) {
                auto* self = static_cast<Application*>(arg);
                vTaskDelay(pdMS_TO_TICKS(2500));
                self->Schedule([]() {
                    auto d = Board::GetInstance().GetDisplay();
                    d->SetStatus(Lang::Strings::STANDBY);
                    d->ShowNotification(Lang::Strings::VMR_ALL_PLAYED, 2500);
                });
                vTaskDelete(nullptr);
            }, "vmr_all_tip", 3072, this, 3, nullptr);
            if (ok != pdPASS) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ShowNotification(Lang::Strings::VMR_ALL_PLAYED, 2500);
            }
        } else {
            // Defer next play so Message read tip stays visible
            BaseType_t ok = xTaskCreate([](void* arg) {
                auto* self = static_cast<Application*>(arg);
                vTaskDelay(pdMS_TO_TICKS(2500));
                self->Schedule([self]() { self->TryChainPlayNextVmr(); });
                vTaskDelete(nullptr);
            }, "vmr_read_tip", 3072, this, 3, nullptr);
            if (ok != pdPASS) {
                TryChainPlayNextVmr();
            }
        }
    } else {
        display->ShowNotification(Lang::Strings::VMR_KEPT_UNREAD, 2500);
        ESP_LOGI(TAG, "VMR: confirmed UNREAD %s (timeout=%d)", mid.c_str(), from_timeout ? 1 : 0);
        vmr_client_.KeepMessageUnread(mid);
        UpdateVmrBanner(vmr_client_.UnplayedCount());
        // Current item was appended to the end — only chain if other unread exist
        if (vmr_client_.UnplayedCount() > 1) {
            vmr_autoplay_wanted_.store(true, std::memory_order_release);
            BaseType_t ok = xTaskCreate([](void* arg) {
                auto* self = static_cast<Application*>(arg);
                vTaskDelay(pdMS_TO_TICKS(2500));
                self->Schedule([self]() { self->TryChainPlayNextVmr(); });
                vTaskDelete(nullptr);
            }, "vmr_unrd_tip", 3072, this, 3, nullptr);
            if (ok != pdPASS) {
                TryChainPlayNextVmr();
            }
        } else if (vmr_autoplay_wanted_.load(std::memory_order_acquire)) {
            TryStartVmrAutoPlay("after_unread_confirm");
        }
    }
}

void Application::TryChainPlayNextVmr() {
    if (vmr_read_confirm_pending_.load(std::memory_order_acquire)) {
        return;
    }
    int remaining = vmr_client_.UnplayedCount();
    UpdateVmrBanner(remaining);
    if (remaining > 0 && !vmr_client_.IsPlaying() && GetDeviceState() == kDeviceStateIdle) {
        ESP_LOGI(TAG, "VMR: auto-play next (%d remaining)", remaining);
        vmr_autoplay_wanted_.store(true, std::memory_order_release);
        TryStartVmrAutoPlay("chain");
    } else if (remaining == 0) {
        vmr_autoplay_wanted_.store(false, std::memory_order_release);
        // Do not ShowNotification here — would overwrite "Message read" tip
    }
}

void Application::UpdateVmrBanner(int count) {
    auto display = Board::GetInstance().GetDisplay();
    if (count > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), Lang::Strings::VMR_NEW_MESSAGE, count);
        display->ShowVmrBanner(msg);
    } else {
        display->HideVmrBanner();
    }
}

bool Application::TryStartVmrAutoPlay(const char* reason) {
    if (vmr_client_.IsPlaying() || IsVmrReadConfirmPending()) {
        return false;
    }
    if (vmr_client_.UnplayedCount() <= 0) {
        vmr_autoplay_wanted_.store(false, std::memory_order_release);
        return false;
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        // Reminders/messages preempt TTS; retry once state settles on Idle
        ESP_LOGI(TAG, "VMR: abort speaking for auto-play (%s)", reason ? reason : "");
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateIdle);
        Schedule([this]() { TryStartVmrAutoPlay("retry_after_speaking"); });
        return false;
    }
    if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "VMR: stop listening for auto-play (%s)", reason ? reason : "");
        if (protocol_) {
            protocol_->CloseAudioChannel();
        }
        SetDeviceState(kDeviceStateIdle);
        Schedule([this]() { TryStartVmrAutoPlay("retry_after_listening"); });
        return false;
    }
    if (state != kDeviceStateIdle) {
        return false;
    }

    auto display = Board::GetInstance().GetDisplay();
    ESP_LOGI(TAG, "VMR: auto-play start (%s) pending=%d",
             reason ? reason : "?", vmr_client_.UnplayedCount());
    display->SetStatus(Lang::Strings::PLEASE_WAIT);
    // Park uplink AFE for the whole download+play window (not only after first PCM).
    // audio_input prio=8 keeps feeding while AFE fetch is prio=3 → FEED ring overflow
    // under VMR HTTPS/CPU load (same class of bug as Speaking TTS).
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.SuspendOutput();
    if (!vmr_client_.PlayLatestMessage()) {
        audio_service_.ResumeOutput();
        display->SetStatus(Lang::Strings::STANDBY);
        return false;
    }
    vmr_autoplay_wanted_.store(false, std::memory_order_release);
    return true;
}

void Application::ConfirmIntent(bool confirm) {
    Schedule([this, confirm]() {
        if (!intent_pending_.load(std::memory_order_acquire)) return;
        intent_pending_.store(false, std::memory_order_release);

        if (protocol_) {
            protocol_->SendIntentConfirmAck(confirm ? "confirm" : "cancel");
        }

        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(confirm ? "Confirmed" : "Cancelled");
    });
}

