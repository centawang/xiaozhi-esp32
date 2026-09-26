#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "ota.h"
#include "protocol.h"
#if CONFIG_STROKE_ORDER_LOCAL
#include "stroke_order/stroke_round_coordinator.h"
#endif

// Main event bits
#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT (1 << 9)
#define MAIN_EVENT_START_LISTENING (1 << 10)
#define MAIN_EVENT_STOP_LISTENING (1 << 11)
#define MAIN_EVENT_STATE_CHANGED (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED (1 << 13)
#if CONFIG_STROKE_ORDER_LOCAL
#define MAIN_EVENT_STROKE_START (1 << 14)
#define MAIN_EVENT_STROKE_ABORT (1 << 15)
#endif

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
    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
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

#if CONFIG_STROKE_ORDER_LOCAL
    /** Bounded cross-task requests. The corresponding mutations run on main. */
    void RequestStartStrokeRound(uint64_t expected_generation = 0);
    // 0 cancels current plus any uncommitted start, resolved atomically. Explicit
    // generations are for correlated callbacks and never cancel a newer round.
    void RequestAbortStrokeRound(uint64_t expected_generation, StrokeAbortReason reason);
    uint64_t CurrentStrokeGeneration() const;
#endif

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

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
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ =
        false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ =
        false;  // Waiting for playback to drain before starting listening (auto mode)
    uint64_t active_listening_generation_ = 0;  // 0 is the explicit ordinary-chat route.
    std::mutex listening_request_mutex_;
    bool listening_request_pending_ = false;
    uint64_t listening_request_generation_ = 0;
#if CONFIG_STROKE_ORDER_LOCAL
    StrokeRoundCoordinator stroke_round_;
    std::recursive_mutex stroke_listening_start_mutex_;
    std::mutex stroke_command_mutex_;
    // Protected by stroke_command_mutex_; never hold it across external calls.
    uint64_t stroke_start_sequence_ = 0;
    bool stroke_start_pending_ = false;
    uint64_t stroke_start_expected_generation_ = 0;
    bool stroke_abort_pending_ = false;
    uint64_t stroke_abort_expected_generation_ = 0;
    StrokeAbortReason stroke_abort_reason_ = StrokeAbortReason::UserClose;
    std::mutex stroke_audio_route_mutex_;
    std::mutex stroke_open_attempt_mutex_;
    uint64_t stroke_open_attempt_id_ = 0;
    uint64_t stroke_open_attempt_generation_ = 0;
    std::atomic<bool> stroke_voice_transport_available_{true};
#endif
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;

    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStartListeningRequest(uint64_t expected_generation);
    void QueueListeningRequest(uint64_t expected_generation);
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode, uint64_t expected_generation);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void RecoverOrdinaryListeningStartFailure();
#if CONFIG_STROKE_ORDER_LOCAL
    void HandleStrokeStartEvent();
    void HandleStrokeAbortEvent();
    bool IsStrokeAbortPending(uint64_t expected_generation);
    void PublishStrokeCancelFence(StrokeAbortReason reason);
    void AbandonCancelledStrokeListening(uint64_t expected_generation);
    bool StrokeVoiceRoutingAvailable() const;
    void BindStrokeOpenAttempt(uint64_t generation, uint64_t open_attempt_id);
    uint64_t MatchStrokeOpenAttempt(uint64_t open_attempt_id);
    void ClearStrokeOpenAttempt(uint64_t generation);
    void InvalidateStrokeStartLocked(uint64_t expected_generation);
    void BeginStrokeRoundFromMain(uint64_t expected_generation, uint64_t sequence);
    void BeginLocalStrokeCandidatesFromMain(uint64_t generation);
    void AbortStrokeRound(uint64_t expected_generation, StrokeAbortReason reason);
    void FinishStrokeListening(uint64_t expected_generation);
    bool BindOpenedAudioChannel(uint64_t expected_generation, std::string_view session_id);
    void DrainStreamingAudio();
#endif
    void ConfigureWakeWordForListening();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() { vTaskPrioritySet(NULL, original_priority_); }

private:
    BaseType_t original_priority_;
};

#endif  // _APPLICATION_H_
