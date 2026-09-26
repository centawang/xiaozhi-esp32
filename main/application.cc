#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "listening_mode_selection.h"
#include "listening_start_policy.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "protocol_selection.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"
#if CONFIG_STROKE_ORDER_LOCAL
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_parse.h"
#include "stroke_order/stroke_order_view.h"
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

#define TAG "Application"

static_assert(static_cast<int>(QueuedStartListeningMode::AutoStop) == kListeningModeAutoStop,
              "QueuedStartListeningMode::AutoStop matches ListeningMode");
static_assert(static_cast<int>(QueuedStartListeningMode::ManualStop) == kListeningModeManualStop,
              "QueuedStartListeningMode::ManualStop matches ListeningMode");
static_assert(static_cast<int>(QueuedStartListeningMode::Realtime) == kListeningModeRealtime,
              "QueuedStartListeningMode::Realtime matches ListeningMode");

#if CONFIG_STROKE_ORDER_LOCAL
namespace {

struct BoundedStringProbe {
    const char* data = nullptr;
    size_t size = 0;
    bool present = false;
    bool valid = false;
};

BoundedStringProbe ProbeJsonString(const cJSON* root, const char* name, size_t max_bytes) {
    BoundedStringProbe result;
    const cJSON* item = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return result;
    }
    result.present = true;
    result.data = item->valuestring;
    const void* terminator = std::memchr(result.data, '\0', max_bytes + 1U);
    if (terminator == nullptr) {
        result.size = max_bytes + 1U;
        return result;
    }
    result.size = static_cast<const char*>(terminator) - result.data;
    result.valid = true;
    return result;
}

}  // namespace
#endif

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

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    const bool transitioned = state_machine_.TransitionTo(state);
#if CONFIG_STROKE_ORDER_LOCAL
    if (transitioned) {
        // This narrow hook closes the overlay immediately on every non-Idle
        // transition, even before the queued state-change UI refresh runs.
        StrokeOrderView::GetInstance().OnDeviceStateChanged(state);
    }
#endif
    return transitioned;
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
#if CONFIG_STROKE_ORDER_LOCAL
    StrokeOrderView::GetInstance().Attach(display, &StrokeOrderController::GetInstance(),
                                          &stroke_round_);
#endif
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
#if CONFIG_STROKE_ORDER_LOCAL
        PublishStrokeCancelFence(StrokeAbortReason::NewNormalSession);
#endif
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
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
#if CONFIG_STROKE_ORDER_LOCAL
                PublishStrokeCancelFence(StrokeAbortReason::ChannelClosed);
#endif
                Schedule([]() {
                    Board::GetInstance().GetDisplay()->ShowNotification(
                        Lang::Strings::SCANNING_WIFI, 30000);
                });
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
#if CONFIG_STROKE_ORDER_LOCAL
                PublishStrokeCancelFence(StrokeAbortReason::ChannelClosed);
#endif
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
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
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
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED
#if CONFIG_STROKE_ORDER_LOCAL
        | MAIN_EVENT_STROKE_START | MAIN_EVENT_STROKE_ABORT
#endif
        ;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
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

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

#if CONFIG_STROKE_ORDER_LOCAL
        if (bits & MAIN_EVENT_STROKE_ABORT) {
            HandleStrokeAbortEvent();
        }

        if (bits & MAIN_EVENT_STROKE_START) {
            HandleStrokeStartEvent();
        }
#endif

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
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
#if CONFIG_STROKE_ORDER_LOCAL
            const auto timeout =
                stroke_round_.CheckTimeouts(static_cast<uint64_t>(esp_timer_get_time() / 1000));
            if (timeout.kind == StrokeRoundCoordinator::TimeoutKind::Speech) {
                AbortStrokeRound(timeout.generation, StrokeAbortReason::SpeechTimeout);
            } else if (timeout.kind == StrokeRoundCoordinator::TimeoutKind::Candidates) {
                AbortStrokeRound(timeout.generation, StrokeAbortReason::CandidateTimeout);
            }
#endif

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::ChannelClosed);
    }
#endif
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
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

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

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
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets. Assets::Apply() unconditionally rebinds (or invalidates)
    // the local StrokeOrder copy after the new partition mapping is active.
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

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
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
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

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    const bool has_mqtt_config = ota_->HasMqttConfig();
    const bool has_websocket_config = ota_->HasWebsocketConfig();
#if CONFIG_STROKE_ORDER_LOCAL
    const bool prefer_websocket_for_stroke_voice = true;
#else
    const bool prefer_websocket_for_stroke_voice = false;
#endif
    const ProtocolSelectionInput selection_input{has_mqtt_config, has_websocket_config,
                                                 prefer_websocket_for_stroke_voice};
    const ProtocolTransport selected = SelectProtocolTransport(selection_input);
    ESP_LOGI(
        TAG,
        "Protocol selection: mqtt_config=%d websocket_config=%d stroke_local_pref=%d selected=%s",
        has_mqtt_config ? 1 : 0, has_websocket_config ? 1 : 0,
        prefer_websocket_for_stroke_voice ? 1 : 0, ProtocolTransportName(selected));

    if (selected == ProtocolTransport::Websocket) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        if (!has_mqtt_config && !has_websocket_config) {
            ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        }
        protocol_ = std::make_unique<MqttProtocol>();
    }

    ESP_LOGI(TAG, "Stroke voice availability: correlated_open=%d stroke_voice=%d",
             protocol_->SupportsCorrelatedSessionOpen() ? 1 : 0,
             protocol_->SupportsStrokeVoiceRouting() ? 1 : 0);

#if CONFIG_STROKE_ORDER_LOCAL
    const bool stroke_voice = StrokeVoiceRoutingAvailable();
    stroke_voice_transport_available_.store(stroke_voice, std::memory_order_release);
    StrokeOrderView::GetInstance().SetVoiceTransportAvailable(stroke_voice);
#endif

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
#if CONFIG_STROKE_ORDER_LOCAL
        PublishStrokeCancelFence(StrokeAbortReason::ChannelClosed);
#endif
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
#if CONFIG_STROKE_ORDER_LOCAL
        if (packet == nullptr) {
            return;
        }
        const bool session_valid = StrokeRoundCoordinator::ValidateSessionId(
            packet->session_id.data(), packet->session_id.size());
        const auto route = stroke_round_.CaptureRoute(StrokeRoundCoordinator::MessageKind::Audio,
                                                      packet->session_id.data(),
                                                      packet->session_id.size(), session_valid);
        if (route.decision == StrokeRoundCoordinator::RouteDecision::FailStroke) {
            if (!route.session_id_valid) {
                stroke_voice_transport_available_.store(false, std::memory_order_release);
            }
            RequestAbortStrokeRound(route.generation,
                                    route.session_id_valid
                                        ? StrokeAbortReason::InvalidSessionIdentity
                                        : StrokeAbortReason::MissingSessionIdentity);
            return;
        }
        if (route.decision != StrokeRoundCoordinator::RouteDecision::PassNormal) {
            return;
        }
        std::lock_guard<std::mutex> route_lock(stroke_audio_route_mutex_);
        if (stroke_round_.RevalidateNormal(route) && GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
#else
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
#endif
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board](const AudioChannelCloseInfo& info) {
#if CONFIG_STROKE_ORDER_LOCAL
        const uint64_t opening_generation = MatchStrokeOpenAttempt(info.open_attempt_id);
        const bool valid = StrokeRoundCoordinator::ValidateSessionId(info.session_id.data(),
                                                                     info.session_id.size());
        const auto close = stroke_round_.CaptureChannelClose(info.session_id.data(),
                                                             info.session_id.size(), valid);
        const uint64_t closing_generation =
            opening_generation != 0
                ? opening_generation
                : (info.open_attempt_id == 0 &&
                           close.decision ==
                               StrokeRoundCoordinator::ChannelCloseDecision::AbortStroke
                       ? close.generation
                       : 0);
        if (closing_generation != 0) {
            // Fence AND invalidate any pending/in-flight replacement now. A
            // scheduled abort alone runs after START in the main loop.
            RequestAbortStrokeRound(closing_generation, StrokeAbortReason::ChannelClosed);
            return;
        }
        Schedule([this, &board, close]() {
            if (close.decision != StrokeRoundCoordinator::ChannelCloseDecision::Normal ||
                !stroke_round_.CommitNormalChannelClose(close)) {
                return;
            }
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            DrainStreamingAudio();
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
#else
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
#endif
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
#if CONFIG_STROKE_ORDER_LOCAL
            const auto session =
                ProbeJsonString(root, "session_id", StrokeRoundCoordinator::kMaxSessionIdBytes);
            const bool session_valid = session.valid && StrokeRoundCoordinator::ValidateSessionId(
                                                            session.data, session.size);
            const auto route =
                stroke_round_.CaptureRoute(StrokeRoundCoordinator::MessageKind::Tts, session.data,
                                           session.size, session_valid);
            if (route.decision == StrokeRoundCoordinator::RouteDecision::FailStroke) {
                if (!route.session_id_valid) {
                    stroke_voice_transport_available_.store(false, std::memory_order_release);
                }
                RequestAbortStrokeRound(route.generation,
                                        route.session_id_valid
                                            ? StrokeAbortReason::InvalidSessionIdentity
                                            : StrokeAbortReason::MissingSessionIdentity);
                return;
            }
            if (route.decision != StrokeRoundCoordinator::RouteDecision::PassNormal) {
                return;
            }
#endif
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this
#if CONFIG_STROKE_ORDER_LOCAL
                          ,
                          route
#endif
                ]() {
#if CONFIG_STROKE_ORDER_LOCAL
                    if (!stroke_round_.RevalidateNormal(route)) {
                        return;
                    }
#endif
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this
#if CONFIG_STROKE_ORDER_LOCAL
                          ,
                          route
#endif
                ]() {
#if CONFIG_STROKE_ORDER_LOCAL
                    if (!stroke_round_.RevalidateNormal(route)) {
                        return;
                    }
#endif
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
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp
#if CONFIG_STROKE_ORDER_LOCAL
                              ,
                              route
#endif
                    ]() {
#if CONFIG_STROKE_ORDER_LOCAL
                        if (!stroke_round_.RevalidateNormal(route)) {
                            return;
                        }
#endif
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
#if CONFIG_STROKE_ORDER_LOCAL
            const auto session =
                ProbeJsonString(root, "session_id", StrokeRoundCoordinator::kMaxSessionIdBytes);
            const bool session_valid = session.valid && StrokeRoundCoordinator::ValidateSessionId(
                                                            session.data, session.size);
            const auto route =
                stroke_round_.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt, session.data,
                                           session.size, session_valid);
            if (route.decision == StrokeRoundCoordinator::RouteDecision::FailStroke) {
                if (!route.session_id_valid) {
                    stroke_voice_transport_available_.store(false, std::memory_order_release);
                }
                RequestAbortStrokeRound(route.generation,
                                        route.session_id_valid
                                            ? StrokeAbortReason::InvalidSessionIdentity
                                            : StrokeAbortReason::MissingSessionIdentity);
                return;
            }
            if (route.decision == StrokeRoundCoordinator::RouteDecision::Drop) {
                return;
            }
            auto text = cJSON_GetObjectItem(root, "text");
            if (route.decision == StrokeRoundCoordinator::RouteDecision::InterceptStrokeStt) {
                const auto bounded_text =
                    ProbeJsonString(root, "text", StrokeOrderParse::kMaxBytes);
                if (!bounded_text.valid) {
                    RequestAbortStrokeRound(route.generation, StrokeAbortReason::InvalidPayload);
                    return;
                }
                const auto parsed = StrokeOrderParse::Parse(bounded_text.data, bounded_text.size);
                if (parsed.status == StrokeOrderParse::Status::InvalidUtf8 ||
                    parsed.status == StrokeOrderParse::Status::TooLong) {
                    RequestAbortStrokeRound(route.generation, StrokeAbortReason::InvalidPayload);
                    return;
                }
                std::string message(bounded_text.data, bounded_text.size);
                Schedule([this, route, message = std::move(message)]() {
                    if (!stroke_round_.CommitStrokeStt(route)) {
                        return;
                    }
                    FinishStrokeListening(route.generation);
                    if (!StrokeOrderView::GetInstance().HandleVoiceSttFromMain(route.generation,
                                                                               message)) {
                        AbortStrokeRound(route.generation, StrokeAbortReason::StartFailed);
                    }
                });
                return;
            }
            if (cJSON_IsString(text) && text->valuestring != nullptr) {
                std::string message(text->valuestring);
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", message.c_str());
                Schedule([this, display, route, message = std::move(message),
                          glyphs = std::move(glyphs), bpp]() {
                    if (!stroke_round_.RevalidateNormal(route)) {
                        return;
                    }
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
#else
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring != nullptr) {
                std::string message(text->valuestring);
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", message.c_str());
                Schedule(
                    [display, message = std::move(message), glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("user", message.c_str());
                    });
            }
#endif
        } else if (strcmp(type->valuestring, "llm") == 0) {
#if CONFIG_STROKE_ORDER_LOCAL
            const auto session =
                ProbeJsonString(root, "session_id", StrokeRoundCoordinator::kMaxSessionIdBytes);
            const bool session_valid = session.valid && StrokeRoundCoordinator::ValidateSessionId(
                                                            session.data, session.size);
            const auto route =
                stroke_round_.CaptureRoute(StrokeRoundCoordinator::MessageKind::Llm, session.data,
                                           session.size, session_valid);
            if (route.decision == StrokeRoundCoordinator::RouteDecision::FailStroke) {
                if (!route.session_id_valid) {
                    stroke_voice_transport_available_.store(false, std::memory_order_release);
                }
                RequestAbortStrokeRound(route.generation,
                                        route.session_id_valid
                                            ? StrokeAbortReason::InvalidSessionIdentity
                                            : StrokeAbortReason::MissingSessionIdentity);
                return;
            }
            if (route.decision != StrokeRoundCoordinator::RouteDecision::PassNormal) {
                return;
            }
#endif
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)
#if CONFIG_STROKE_ORDER_LOCAL
                                             ,
                          route
#endif
                ]() {
#if CONFIG_STROKE_ORDER_LOCAL
                    if (!stroke_round_.RevalidateNormal(route)) {
                        return;
                    }
#endif
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
#if CONFIG_STROKE_ORDER_LOCAL
                    PublishStrokeCancelFence(StrokeAbortReason::Reboot);
#endif
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
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
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

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

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::Alert);
#endif
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
#if CONFIG_STROKE_ORDER_LOCAL
        StrokeOrderView::GetInstance().OnDeviceStateChanged(kDeviceStateIdle);
#endif
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::NewNormalSession);
#endif
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::NewNormalSession);
#endif
    QueueListeningRequest(0);
}

void Application::QueueListeningRequest(uint64_t expected_generation) {
    {
        std::lock_guard<std::mutex> lock(listening_request_mutex_);
        // One bounded slot is sufficient because an input gesture is idempotent.
        // A newer request replaces an older one, but its explicit generation is
        // never converted to the ordinary generation-0 route.
        listening_request_generation_ = expected_generation;
        listening_request_pending_ = true;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::NewNormalSession);
#endif
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

#if CONFIG_STROKE_ORDER_LOCAL
void Application::RequestStartStrokeRound(uint64_t expected_generation) {
    // Publish the fence and the bounded command in the same domain as cancel.
    // In particular, an abort cannot slip between those two publications.
    std::lock_guard<std::recursive_mutex> start_lock(stroke_listening_start_mutex_);
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        const uint64_t current = stroke_round_.CurrentGeneration();
        if ((expected_generation != 0 && expected_generation != current) ||
            (current != 0 && stroke_round_.HasCancelFence(current)) ||
            stroke_start_sequence_ >= UINT64_MAX - 1) {
            return;
        }
        stroke_round_.PublishCancelFence(current);
        ++stroke_start_sequence_;
        stroke_start_pending_ = true;
        // Generation 0 means a fresh SO click, not a wildcard delayed restart.
        stroke_start_expected_generation_ = current;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STROKE_START);
}

void Application::InvalidateStrokeStartLocked(uint64_t expected_generation) {
    // The source remains recorded after dequeue, until BeginRound commits.
    // Thus cancel invalidates an in-flight start as well as a pending slot.
    if (expected_generation == 0 || expected_generation == stroke_start_expected_generation_) {
        stroke_start_pending_ = false;
        if (stroke_start_sequence_ != UINT64_MAX) {
            ++stroke_start_sequence_;
        }
    }
}

void Application::RequestAbortStrokeRound(uint64_t expected_generation, StrokeAbortReason reason) {
    std::lock_guard<std::recursive_mutex> start_lock(stroke_listening_start_mutex_);
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        const uint64_t current = stroke_round_.CurrentGeneration();
        // A stale old abort must not overwrite the one abort slot for a newer
        // round. During replacement teardown current may be 0: retain identity
        // checking there, but never use IsLatestGeneration as start admission.
        if (expected_generation != 0 && expected_generation != current &&
            !(current == 0 && stroke_round_.IsLatestGeneration(expected_generation))) {
            return;
        }
        InvalidateStrokeStartLocked(expected_generation);
        if (expected_generation == 0) {
            expected_generation = current;
        }
        if (expected_generation == 0) {
            return;  // cancelled a fresh SO/in-flight start; no active round to hide
        }
        stroke_round_.PublishCancelFence(expected_generation);
        stroke_abort_pending_ = true;
        stroke_abort_expected_generation_ = expected_generation;
        stroke_abort_reason_ = reason;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STROKE_ABORT);
}

uint64_t Application::CurrentStrokeGeneration() const { return stroke_round_.CurrentGeneration(); }

void Application::PublishStrokeCancelFence(StrokeAbortReason reason) {
    // Resolve current AND cancel pending starts atomically, including the
    // inactive gap between retiring the old round and committing its successor.
    RequestAbortStrokeRound(0, reason);
}

bool Application::StrokeVoiceRoutingAvailable() const {
    return protocol_ && protocol_->SupportsCorrelatedSessionOpen() &&
           protocol_->SupportsStrokeVoiceRouting();
}

void Application::BindStrokeOpenAttempt(uint64_t generation, uint64_t open_attempt_id) {
    std::lock_guard<std::mutex> lock(stroke_open_attempt_mutex_);
    stroke_open_attempt_generation_ = generation;
    stroke_open_attempt_id_ = open_attempt_id;
}

uint64_t Application::MatchStrokeOpenAttempt(uint64_t open_attempt_id) {
    if (open_attempt_id == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(stroke_open_attempt_mutex_);
    return stroke_open_attempt_id_ == open_attempt_id ? stroke_open_attempt_generation_ : 0;
}

void Application::ClearStrokeOpenAttempt(uint64_t generation) {
    std::lock_guard<std::mutex> lock(stroke_open_attempt_mutex_);
    if (generation == 0 || stroke_open_attempt_generation_ != generation) {
        return;
    }
    stroke_open_attempt_id_ = 0;
    stroke_open_attempt_generation_ = 0;
}

void Application::AbandonCancelledStrokeListening(uint64_t expected_generation) {
    if (expected_generation == 0) {
        return;
    }
    ClearStrokeOpenAttempt(expected_generation);
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        stroke_round_.CloseCurrentChannel();
        protocol_->CloseAudioChannel();
    }
    DrainStreamingAudio();
    if (IsStrokeAbortPending(expected_generation)) {
        HandleStrokeAbortEvent();
        return;
    }
    AbortStrokeRound(expected_generation, StrokeAbortReason::NewNormalSession);
}

void Application::HandleStrokeStartEvent() {
    uint64_t expected_generation = 0;
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        if (!stroke_start_pending_) {
            return;
        }
        expected_generation = stroke_start_expected_generation_;
        sequence = stroke_start_sequence_;
        stroke_start_pending_ = false;
    }
    BeginStrokeRoundFromMain(expected_generation, sequence);
}

bool Application::IsStrokeAbortPending(uint64_t expected_generation) {
    std::lock_guard<std::mutex> lock(stroke_command_mutex_);
    return stroke_abort_pending_ && expected_generation != 0 &&
           stroke_abort_expected_generation_ == expected_generation;
}

void Application::HandleStrokeAbortEvent() {
    uint64_t expected_generation = 0;
    StrokeAbortReason reason = StrokeAbortReason::UserClose;
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        if (!stroke_abort_pending_) {
            return;
        }
        expected_generation = stroke_abort_expected_generation_;
        reason = stroke_abort_reason_;
        stroke_abort_pending_ = false;
    }
    AbortStrokeRound(expected_generation, reason);
}

void Application::BeginStrokeRoundFromMain(uint64_t expected_generation, uint64_t sequence) {
    if (GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    uint64_t current = 0;
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        current = stroke_round_.CurrentGeneration();
        if (sequence != stroke_start_sequence_ || sequence == UINT64_MAX ||
            expected_generation != current) {
            return;
        }
    }
    if (current != 0) {
        AbortStrokeRound(current, StrokeAbortReason::ReplacedByNewStroke);
    } else if (StrokeOrderView::GetInstance().IsOverlayActive()) {
        StrokeOrderView::GetInstance().AbortFromMain(0);
    }

    const bool stroke_voice = StrokeVoiceRoutingAvailable() &&
                              stroke_voice_transport_available_.load(std::memory_order_acquire);
    // No command lock spans LVGL, protocol callbacks, or audio teardown. A
    // reentrant/async cancel during any of these calls invalidates sequence.
    if (stroke_voice) {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            stroke_round_.CloseCurrentChannel();
            protocol_->CloseAudioChannel();
        }
        DrainStreamingAudio();
    }
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        if (sequence != stroke_start_sequence_ || sequence == UINT64_MAX ||
            stroke_round_.CurrentGeneration() != 0) {
            return;
        }
        // This is the start/cancel linearization point. Before it, cancel wins
        // via sequence; after it, cancel fences the new generation and the
        // existing open/listening-readiness gates prevent capture or Speak.
        generation = stroke_round_.BeginRound(static_cast<uint64_t>(esp_timer_get_time() / 1000));
        stroke_start_expected_generation_ = generation;
    }
    if (stroke_round_.HasCancelFence(generation) || IsStrokeAbortPending(generation)) {
        return;
    }
    if (!stroke_voice) {
        BeginLocalStrokeCandidatesFromMain(generation);
        return;
    }
    if (!StrokeOrderView::GetInstance().StartVoiceSessionFromMain(generation)) {
        AbortStrokeRound(generation, StrokeAbortReason::StartFailed);
        return;
    }
    QueueListeningRequest(generation);
}

void Application::BeginLocalStrokeCandidatesFromMain(uint64_t generation) {
    if (generation == 0) {
        return;
    }
    if (!StrokeOrderView::GetInstance().StartLocalCandidateSessionFromMain(generation)) {
        AbortStrokeRound(generation, StrokeAbortReason::StartFailed);
        Board::GetInstance().GetDisplay()->ShowNotification("请点选候选字或关闭后重试");
        return;
    }
    Board::GetInstance().GetDisplay()->ShowNotification("当前连接仅支持点选候选字");
}

void Application::AbortStrokeRound(uint64_t expected_generation, StrokeAbortReason reason) {
    if (reason != StrokeAbortReason::ReplacedByNewStroke) {
        // Main-task direct aborts (timeouts, close, etc.) share the same start
        // invalidation as asynchronous publishers. Replacement's own teardown
        // is the sole exception; it must not revoke its own admission token.
        std::lock_guard<std::mutex> lock(stroke_command_mutex_);
        InvalidateStrokeStartLocked(expected_generation);
        stroke_round_.PublishCancelFence(expected_generation);
    }
    if (expected_generation == 0) {
        StrokeOrderView::GetInstance().AbortFromMain(0);
        return;
    }

    ClearStrokeOpenAttempt(expected_generation);
    const auto state = GetDeviceState();
    StrokeRoundCoordinator::AbortResult aborted;
    {
        // Invalidate routing before any externally visible stop/close action.
        // The same lock prevents a downlink packet from passing revalidation
        // while the decoder queues are being isolated.
        std::lock_guard<std::mutex> route_lock(stroke_audio_route_mutex_);
        aborted = stroke_round_.AbortRound(expected_generation);
        if (!aborted.matched) {
            return;
        }
        DrainStreamingAudio();
    }

    if (protocol_ && state == kDeviceStateListening) {
        protocol_->SendStopListening();
    } else if (protocol_ && state == kDeviceStateSpeaking) {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
    }

    {
        std::lock_guard<std::mutex> lock(listening_request_mutex_);
        if (listening_request_pending_ && listening_request_generation_ == expected_generation) {
            listening_request_pending_ = false;
        }
    }
    active_listening_generation_ = 0;
    pending_listening_start_ = false;

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    StrokeOrderView::GetInstance().AbortFromMain(expected_generation);
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    }

    if (!stroke_voice_transport_available_.load(std::memory_order_acquire)) {
        StrokeOrderView::GetInstance().SetVoiceTransportAvailable(false);
    }
    if (reason == StrokeAbortReason::SpeechTimeout) {
        StrokeOrderView::GetInstance().ShowSpeechTimedOutFromMain();
    } else if (reason == StrokeAbortReason::MissingSessionIdentity) {
        Board::GetInstance().GetDisplay()->ShowNotification("Stroke voice unavailable");
    } else if (reason == StrokeAbortReason::InvalidSessionIdentity) {
        Board::GetInstance().GetDisplay()->ShowNotification("Stroke session identity error");
    }
}

void Application::FinishStrokeListening(uint64_t expected_generation) {
    if (!stroke_round_.IsCurrentGeneration(expected_generation)) {
        return;
    }
    ClearStrokeOpenAttempt(expected_generation);
    const auto state = GetDeviceState();
    if (protocol_ && state == kDeviceStateListening) {
        protocol_->SendStopListening();
    }
    {
        std::lock_guard<std::mutex> route_lock(stroke_audio_route_mutex_);
        if (!stroke_round_.RetireStrokeChannel(expected_generation)) {
            return;
        }
        DrainStreamingAudio();
    }
    active_listening_generation_ = 0;
    pending_listening_start_ = false;
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    }
}

bool Application::BindOpenedAudioChannel(uint64_t expected_generation,
                                         std::string_view session_id) {
    if (expected_generation != 0 &&
        !StrokeRoundCoordinator::ValidateSessionId(session_id.data(), session_id.size())) {
        stroke_voice_transport_available_.store(false, std::memory_order_release);
    }
    return stroke_round_.BindOpenedChannel(expected_generation, session_id);
}

void Application::DrainStreamingAudio() { audio_service_.ResetStreamingState(); }
#endif

void Application::RecoverOrdinaryListeningStartFailure() {
    pending_listening_start_ = false;
    active_listening_generation_ = 0;
    play_popup_on_listening_ = false;
    audio_service_.ResetStreamingState();
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
#if CONFIG_STROKE_ORDER_LOCAL
        stroke_round_.CloseCurrentChannel();
#endif
        protocol_->CloseAudioChannel();
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    SetDeviceState(kDeviceStateIdle);
    // Do not wait for the queued Idle UI handler: a failed wake-word re-listen
    // must immediately restore the detector that fired the event.
    audio_service_.EnableWakeWordDetection(true);
}

void Application::HandleToggleChatEvent() {
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::NewNormalSession);
    } else if (StrokeOrderView::GetInstance().IsOverlayActive()) {
        StrokeOrderView::GetInstance().AbortFromMain(0);
    }
#endif
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
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode, 0); });
            return;
        }
#if CONFIG_STROKE_ORDER_LOCAL
        if (!BindOpenedAudioChannel(0, protocol_->session_id())) {
            stroke_round_.CloseCurrentChannel();
            protocol_->CloseAudioChannel();
            DrainStreamingAudio();
            Board::GetInstance().GetDisplay()->ShowNotification("Session identity unavailable");
            return;
        }
#endif
        active_listening_generation_ = 0;
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode, uint64_t expected_generation) {
    // Check both state and generation before entering the blocking open.
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
#if CONFIG_STROKE_ORDER_LOCAL
    if (expected_generation != 0 && (stroke_round_.HasCancelFence(expected_generation) ||
                                     !stroke_round_.CanContinueOpen(expected_generation) ||
                                     IsStrokeAbortPending(expected_generation))) {
        AbandonCancelledStrokeListening(expected_generation);
        return;
    }
    if (expected_generation == 0 &&
        (!stroke_round_.CanContinueOpen(expected_generation) || stroke_round_.IsRoundActive())) {
        return;
    }
#endif

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

#if CONFIG_STROKE_ORDER_LOCAL
    if (expected_generation != 0 && protocol_->IsAudioChannelOpened()) {
        AbortStrokeRound(expected_generation, StrokeAbortReason::StartFailed);
        return;
    }
    std::string opened_session_id;
    uint64_t open_attempt_id = 0;
    if (expected_generation != 0) {
        open_attempt_id = protocol_->ReserveAudioChannelOpenAttempt();
        if (open_attempt_id == 0) {
            AbortStrokeRound(expected_generation, StrokeAbortReason::StartFailed);
            return;
        }
        BindStrokeOpenAttempt(expected_generation, open_attempt_id);
    }
    const bool opened = protocol_->IsAudioChannelOpened() ||
                        protocol_->OpenAudioChannel(&opened_session_id, open_attempt_id);
#else
    const bool opened = protocol_->IsAudioChannelOpened() || protocol_->OpenAudioChannel();
#endif
    if (!opened || !protocol_->IsAudioChannelOpened()) {
        if (expected_generation != 0) {
#if CONFIG_STROKE_ORDER_LOCAL
            ClearStrokeOpenAttempt(expected_generation);
            AbortStrokeRound(expected_generation, StrokeAbortReason::StartFailed);
            protocol_->CloseAudioChannel();
#endif
        } else {
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

#if CONFIG_STROKE_ORDER_LOCAL
    // Cancellation may happen while OpenAudioChannel blocks. Never let that
    // continuation become an ordinary listen request or start the microphone.
    if (expected_generation != 0 && (stroke_round_.HasCancelFence(expected_generation) ||
                                     !stroke_round_.CanContinueOpen(expected_generation) ||
                                     IsStrokeAbortPending(expected_generation))) {
        AbandonCancelledStrokeListening(expected_generation);
        return;
    }
    if (expected_generation == 0 &&
        (!stroke_round_.CanContinueOpen(expected_generation) || stroke_round_.IsRoundActive())) {
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        DrainStreamingAudio();
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (opened_session_id.empty()) {
        opened_session_id = protocol_->session_id();
    }
    if (!BindOpenedAudioChannel(expected_generation, opened_session_id)) {
        ClearStrokeOpenAttempt(expected_generation);
        if (protocol_->IsAudioChannelOpened()) {
            stroke_round_.CloseCurrentChannel();
            protocol_->CloseAudioChannel();
        }
        DrainStreamingAudio();
        if (expected_generation != 0) {
            AbortStrokeRound(expected_generation,
                             stroke_voice_transport_available_.load(std::memory_order_acquire)
                                 ? StrokeAbortReason::InvalidSessionIdentity
                                 : StrokeAbortReason::MissingSessionIdentity);
        } else {
            SetDeviceState(kDeviceStateIdle);
            Board::GetInstance().GetDisplay()->ShowNotification("Session identity unavailable");
        }
        return;
    }
#endif

    active_listening_generation_ = expected_generation;
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    uint64_t expected_generation = 0;
    {
        std::lock_guard<std::mutex> lock(listening_request_mutex_);
        if (!listening_request_pending_) {
            return;
        }
        expected_generation = listening_request_generation_;
        listening_request_pending_ = false;
    }
    HandleStartListeningRequest(expected_generation);
}

void Application::HandleStartListeningRequest(uint64_t expected_generation) {
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

#if CONFIG_STROKE_ORDER_LOCAL
    if (expected_generation == 0) {
        const uint64_t current = stroke_round_.CurrentGeneration();
        if (current != 0) {
            AbortStrokeRound(current, StrokeAbortReason::NewNormalSession);
            state = GetDeviceState();
        }
    } else if (!stroke_round_.CanContinueOpen(expected_generation)) {
        AbortStrokeRound(expected_generation, StrokeAbortReason::StartFailed);
        return;
    }
#endif

    const auto start_mode =
        static_cast<ListeningMode>(ListeningModeForStartGeneration(expected_generation));

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            if (expected_generation != 0) {
#if CONFIG_STROKE_ORDER_LOCAL
                stroke_round_.MarkConnecting(expected_generation);
#endif
            }
            Schedule([this, start_mode, expected_generation]() {
                ContinueOpenAudioChannel(start_mode, expected_generation);
            });
            return;
        }
#if CONFIG_STROKE_ORDER_LOCAL
        if (expected_generation != 0) {
            // A stroke round is never allowed to reuse a pre-existing channel.
            AbortStrokeRound(expected_generation, StrokeAbortReason::StartFailed);
            return;
        }
        if (!BindOpenedAudioChannel(0, protocol_->session_id())) {
            stroke_round_.CloseCurrentChannel();
            protocol_->CloseAudioChannel();
            DrainStreamingAudio();
            Board::GetInstance().GetDisplay()->ShowNotification("Session identity unavailable");
            return;
        }
#endif
        active_listening_generation_ = expected_generation;
        SetListeningMode(start_mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        active_listening_generation_ = expected_generation;
        SetListeningMode(start_mode);
    }
}

void Application::HandleStopListeningEvent() {
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::NewNormalSession);
        return;
    }
#endif
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
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::NewNormalSession);
    } else if (StrokeOrderView::GetInstance().IsOverlayActive()) {
        StrokeOrderView::GetInstance().AbortFromMain(0);
    }
#endif
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            if (!protocol_->SendStartListening(GetDefaultListeningMode())) {
                ESP_LOGW(TAG, "Failed to send wake-word re-listen start");
                RecoverOrdinaryListeningStartFailure();
                return;
            }
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

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    std::string opened_session_id;
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel(&opened_session_id)) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

#if CONFIG_STROKE_ORDER_LOCAL
    if (opened_session_id.empty()) {
        opened_session_id = protocol_->session_id();
    }
    if (!BindOpenedAudioChannel(0, opened_session_id)) {
        if (protocol_->IsAudioChannelOpened()) {
            stroke_round_.CloseCurrentChannel();
            protocol_->CloseAudioChannel();
        }
        DrainStreamingAudio();
        SetDeviceState(kDeviceStateIdle);
        Board::GetInstance().GetDisplay()->ShowNotification("Session identity unavailable");
        return;
    }
#endif

    active_listening_generation_ = 0;
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
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
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
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
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
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

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    const uint64_t start_generation = active_listening_generation_;
#if CONFIG_STROKE_ORDER_LOCAL
    std::unique_lock<std::recursive_mutex> start_lock(stroke_listening_start_mutex_);
    if (start_generation != 0 &&
        (stroke_round_.HasCancelFence(start_generation) || IsStrokeAbortPending(start_generation) ||
         !stroke_round_.CanStartListening(start_generation))) {
        start_lock.unlock();
        AbandonCancelledStrokeListening(start_generation);
        return;
    }
    if (start_generation == 0 && !stroke_round_.CanStartListening(start_generation)) {
        start_lock.unlock();
        RecoverOrdinaryListeningStartFailure();
        return;
    }
#endif

    // Serialize the final gate with every asynchronous fence publisher. A
    // synchronous SendText error may recursively publish a fence, so recheck
    // before enabling capture as well. Do not mark listening or Speak before
    // listen/start is actually sent and voice processing is observably running.
    ListeningStartResult start_result;
    start_result.send_start_succeeded = protocol_->SendStartListening(listening_mode_);
    if (!start_result.send_start_succeeded) {
#if CONFIG_STROKE_ORDER_LOCAL
        const bool fenced =
            start_generation != 0 && (stroke_round_.HasCancelFence(start_generation) ||
                                      IsStrokeAbortPending(start_generation));
        const auto disposition =
            EvaluateListeningStartResult(start_generation, fenced, start_result);
        if (disposition == ListeningStartDisposition::AbandonFencedStroke) {
            start_lock.unlock();
            AbandonCancelledStrokeListening(start_generation);
            return;
        }
        if (disposition == ListeningStartDisposition::AbortStroke) {
            stroke_round_.PublishCancelFence(start_generation);
            start_lock.unlock();
            AbortStrokeRound(start_generation, StrokeAbortReason::StartFailed);
            return;
        }
        start_lock.unlock();
#else
        (void)EvaluateListeningStartResult(start_generation, false, start_result);
#endif
        RecoverOrdinaryListeningStartFailure();
        return;
    }

#if CONFIG_STROKE_ORDER_LOCAL
    if (start_generation != 0 && (stroke_round_.HasCancelFence(start_generation) ||
                                  IsStrokeAbortPending(start_generation))) {
        const auto disposition = EvaluateListeningStartResult(start_generation, true, start_result);
        start_lock.unlock();
        if (disposition == ListeningStartDisposition::AbandonFencedStroke) {
            AbandonCancelledStrokeListening(start_generation);
        }
        return;
    }
#endif

    start_result.voice_processing_enabled = audio_service_.EnableVoiceProcessing(true);
    start_result.audio_processor_running = audio_service_.IsAudioProcessorRunning();
#if CONFIG_STROKE_ORDER_LOCAL
    const bool fenced_after_enable =
        start_generation != 0 &&
        (stroke_round_.HasCancelFence(start_generation) || IsStrokeAbortPending(start_generation));
#else
    const bool fenced_after_enable = false;
#endif
    const auto disposition =
        EvaluateListeningStartResult(start_generation, fenced_after_enable, start_result);
    if (disposition != ListeningStartDisposition::Proceed) {
#if CONFIG_STROKE_ORDER_LOCAL
        if (disposition == ListeningStartDisposition::AbandonFencedStroke) {
            start_lock.unlock();
            AbandonCancelledStrokeListening(start_generation);
            return;
        }
        if (disposition == ListeningStartDisposition::AbortStroke) {
            stroke_round_.PublishCancelFence(start_generation);
            start_lock.unlock();
            AbortStrokeRound(start_generation, StrokeAbortReason::StartFailed);
            return;
        }
        start_lock.unlock();
#endif
        RecoverOrdinaryListeningStartFailure();
        return;
    }

#if CONFIG_STROKE_ORDER_LOCAL
    if (!stroke_round_.MarkListeningStarted(start_generation,
                                            static_cast<uint64_t>(esp_timer_get_time() / 1000))) {
        if (start_generation != 0) {
            const bool fenced = stroke_round_.HasCancelFence(start_generation) ||
                                IsStrokeAbortPending(start_generation);
            if (!fenced) {
                stroke_round_.PublishCancelFence(start_generation);
            }
            start_lock.unlock();
            if (fenced) {
                AbandonCancelledStrokeListening(start_generation);
            } else {
                AbortStrokeRound(start_generation, StrokeAbortReason::StartFailed);
            }
            return;
        }
        start_lock.unlock();
        RecoverOrdinaryListeningStartFailure();
        return;
    }
    const uint64_t speak_generation = start_generation;
    start_lock.unlock();
    if (speak_generation != 0 &&
        !StrokeOrderView::GetInstance().ShowListeningFromMain(speak_generation)) {
        AbortStrokeRound(speak_generation, StrokeAbortReason::StartFailed);
        return;
    }
#endif

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
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

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::Reboot);
    }
    StrokeOrderView::GetInstance().Shutdown();
#endif
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
#if CONFIG_STROKE_ORDER_LOCAL
    const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
    if (stroke_generation != 0) {
        AbortStrokeRound(stroke_generation, StrokeAbortReason::Reboot);
    }
#endif
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

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
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
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::NewNormalSession);
#endif
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
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

#if CONFIG_STROKE_ORDER_LOCAL
    if (StrokeOrderView::GetInstance().IsOverlayActive()) {
        return false;
    }
#endif

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
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

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
#if CONFIG_STROKE_ORDER_LOCAL
    PublishStrokeCancelFence(StrokeAbortReason::ChannelClosed);
#endif
    Schedule([this]() {
#if CONFIG_STROKE_ORDER_LOCAL
        const uint64_t stroke_generation = stroke_round_.CurrentGeneration();
        if (stroke_generation != 0) {
            AbortStrokeRound(stroke_generation, StrokeAbortReason::ChannelClosed);
        }
#endif
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
