// Compile the production notification/abort/view methods verbatim via the test
// driver. Only hardware, LVGL object flags and task scheduling are test doubles.
#include "audio/audio_stream_generation.h"
#include "device_state.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_round_coordinator.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define CONFIG_STROKE_ORDER_LOCAL 1
#define HAVE_LVGL 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
template <class... Args>
void TestLog(Args&&...) {}
constexpr const char* TAG = "notify-test";
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
constexpr int MAIN_EVENT_STROKE_ABORT = 1;
void xEventGroupSetBits(int, int) {}
constexpr int kAbortReasonNone = 0;
namespace Lang::Sounds {
constexpr const char* OGG_POPUP = "popup";
}

enum class PowerSaveLevel { LOW_POWER, PERFORMANCE };
struct Display {
    void ShowNotification(const char*) {}
    void SetChatMessage(const char*, const char*) {}
};
struct DisplayLockGuard {
    explicit DisplayLockGuard(Display*) {}
    explicit operator bool() const { return true; }
};
struct Board {
    static Board& GetInstance() {
        static Board board;
        return board;
    }
    PowerSaveLevel power = PowerSaveLevel::LOW_POWER;
    Display display;
    void SetPowerSaveLevel(PowerSaveLevel level) { power = level; }
    Display* GetDisplay() { return &display; }
};
struct AudioService {
    AudioStreamGenerationGate gate;
    std::vector<std::string> queue;
    int streaming_resets = 0, decoder_resets = 0;
    bool EnableVoiceProcessing(bool) { return true; }
    void EnableWakeWordDetection(bool) {}
    bool IsAfeWakeWord() const { return false; }
    void ReleaseWakeWordResources() {}
    bool PopPacketFromSendQueue() { return false; }
    void ResetStreamingState() {
        ++streaming_resets;
        gate.BumpStreaming();
        queue.clear();
    }
    void ResetDecoder() {
        ++decoder_resets;
        gate.BumpPlayback();
        queue.clear();
    }
    void PlaySound(const char* sound) { queue.emplace_back(sound); }
};
struct NotifySubtitle {};
struct NotifyPlayer {
    explicit NotifyPlayer(AudioService& service) : audio(service) {}
    AudioService& audio;
    bool busy = false;
    int starts = 0;
    std::function<void()> on_start;
    bool IsBusy() const { return busy; }
    template <class Subtitle, class Finished>
    bool Start(std::string, std::vector<NotifySubtitle>, uint32_t, Subtitle, Finished) {
        ++starts;
        if (on_start) {
            on_start();
        }
        audio.queue.emplace_back("remote");
        busy = true;
        return true;
    }
    void Stop() { busy = false; }
};
struct Protocol {
    bool opened = false;
    int closes = 0;
    std::function<void()> on_close;
    bool IsAudioChannelOpened() const { return opened; }
    void CloseAudioChannel() {
        opened = false;
        ++closes;
        if (on_close) {
            on_close();
        }
    }
    void SendStopListening() {}
    void SendAbortSpeaking(int) {}
};

// Exercise the production LVGL hidden/clickable mutations, not just CanShowEntry.
constexpr int LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_CLICKABLE = 2;
struct lv_obj_t {
    int flags = LV_OBJ_FLAG_CLICKABLE;
};
bool lv_obj_is_valid(lv_obj_t*) { return true; }
void lv_obj_add_flag(lv_obj_t* obj, int flag) { obj->flags |= flag; }
void lv_obj_remove_flag(lv_obj_t* obj, int flag) { obj->flags &= ~flag; }
struct Controller {
    bool hit_rect = true;
    void ClearEntryHitRect() { hit_rect = false; }
};
class StrokeOrderView {
public:
    static StrokeOrderView& GetInstance() {
        static StrokeOrderView view;
        return view;
    }
    Display* display_ = Board::GetInstance().GetDisplay();
    Controller controller;
    Controller* controller_ = &controller;
    StrokeRoundCoordinator* coordinator_ = nullptr;
    StrokeOrderLifecycle lifecycle_;
    lv_obj_t entry_object;
    lv_obj_t* entry_ = &entry_object;
    bool overlay = false;
    void Reset(StrokeRoundCoordinator& coordinator) {
        coordinator_ = &coordinator;
        lifecycle_ = StrokeOrderLifecycle{};
        lifecycle_.Initialize();
        lifecycle_.SetAssetsReady(true);
        lifecycle_.SetPointerReady(true);
        lifecycle_.SetDeviceIdle(true);
        overlay = false;
        ShowEntryLocked();
    }
    bool ShowEntryLocked() {
        entry_->flags = LV_OBJ_FLAG_CLICKABLE;
        controller.hit_rect = true;
        return true;
    }
    bool IsOverlayActive() const { return overlay; }
    void AbortFromMain(uint64_t, bool = false) {
        overlay = false;
        lifecycle_.CloseOverlay();
        ReevaluateEntryLocked();
    }
    void SetVoiceTransportAvailable(bool) {}
    void ShowSpeechTimedOutFromMain() {}
    static uint8_t ClassifyDeviceState(DeviceState state);
    void OnDeviceStateChanged(DeviceState state);
    void HideEntryLocked();
    void ReevaluateEntryLocked();
    void RequestAbortLocked(StrokeAbortReason reason);
};
class Application {
public:
    static Application* instance;
    static Application& GetInstance() { return *instance; }
    struct StateMachine {
        DeviceState state = kDeviceStateIdle;
        bool TransitionTo(DeviceState next) {
            state = next;
            return true;
        }
    } state_machine_;
    StrokeRoundCoordinator stroke_round_;
    AudioService audio_service_;
    NotifyPlayer notify_player_{audio_service_};
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    uint32_t notification_playback_id_ = 0;
    std::recursive_mutex stroke_listening_start_mutex_;
    std::mutex stroke_audio_route_mutex_, stroke_command_mutex_, listening_request_mutex_;
    bool stroke_abort_pending_ = false;
    uint64_t stroke_abort_expected_generation_ = 0;
    StrokeAbortReason stroke_abort_reason_ = StrokeAbortReason::UserClose;
    bool listening_request_pending_ = false, pending_listening_start_ = false;
    uint64_t listening_request_generation_ = 0, active_listening_generation_ = 0;
    std::atomic<bool> stroke_voice_transport_available_{true};
    int event_group_ = 0;
    std::deque<std::function<void()>> tasks;
    Application() {
        instance = this;
        StrokeOrderView::GetInstance().Reset(stroke_round_);
        Board::GetInstance().power = PowerSaveLevel::LOW_POWER;
    }
    DeviceState GetDeviceState() const { return state_machine_.state; }
    bool SetDeviceState(DeviceState state);
    void ClearStrokeOpenAttempt(uint64_t) {}
    void DrainStreamingAudio();
    void AbortStrokeRound(uint64_t, StrokeAbortReason);
    void RequestAbortStrokeRound(uint64_t, StrokeAbortReason);
    void HandleStrokeAbortEvent();
    void StartNotification(std::string, std::vector<NotifySubtitle>);
    void StopNotification();
    void HandleNotificationFinished(uint32_t, bool);
    void Schedule(std::function<void()>&& fn) { tasks.push_back(std::move(fn)); }
    void DispatchDeferred() {
        HandleStrokeAbortEvent();
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
};
Application* Application::instance = nullptr;

#include "notify_stroke_production.inc"

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void NotificationOrder(bool playback, bool voice, bool pending_abort) {
    Application app;
    auto& coordinator = app.stroke_round_;
    auto& view = StrokeOrderView::GetInstance();
    const uint64_t generation = coordinator.BeginRound(0);
    if (voice) {
        Expect(coordinator.MarkConnecting(generation), "connecting");
        Expect(coordinator.BindOpenedChannel(generation, "stroke-notify"), "bind channel");
        Expect(coordinator.MarkListeningStarted(generation, 1), "listen");
        auto stt = coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                            "stroke-notify", 13, true);
        Expect(coordinator.CommitStrokeStt(stt), "commit STT");
        Expect(coordinator.RetireStrokeChannel(generation), "retire voice before candidates");
        Expect(coordinator.MarkCandidates(generation, 2), "voice candidates");
    } else {
        Expect(coordinator.MarkLocalCandidates(generation, 2), "local candidates");
    }
    if (playback) {
        Expect(coordinator.MarkLocalPlayback(generation), "local playback");
    }
    view.overlay = true;
    view.lifecycle_.TryOpenOverlay();
    view.HideEntryLocked();
    // Both event-bit and scheduled callback cleanup can arrive after notify.
    if (pending_abort) {
        app.RequestAbortStrokeRound(generation, StrokeAbortReason::UserClose);
    }
    app.Schedule([&]() { app.AbortStrokeRound(generation, StrokeAbortReason::ChannelClosed); });
    app.audio_service_.queue = {"old-round"};
    const auto old_work = AudioStreamInFlightWork{app.audio_service_.gate.playback, false};
    app.notify_player_.on_start = [&]() {
        Expect(coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::Inactive,
               "coordinator inactive before HTTP start");
        Expect(!view.overlay, "overlay closed synchronously before HTTP start");
        Expect(app.audio_service_.streaming_resets == 1, "old round drained exactly once");
        Expect(app.audio_service_.decoder_resets == 1, "notify reset before popup");
        Expect(app.audio_service_.queue == std::vector<std::string>{"popup"},
               "popup precedes HTTP audio without an intervening drain");
        Expect(Board::GetInstance().power == PowerSaveLevel::PERFORMANCE,
               "notification starts in PERFORMANCE");
        Expect(!AudioStreamCanRequeue(old_work, app.audio_service_.gate),
               "old in-flight audio cannot requeue");
    };
    app.StartNotification("https://example.invalid/notify.ogg", {});
    const auto notify_work = AudioStreamInFlightWork{app.audio_service_.gate.playback, false};
    app.DispatchDeferred();
    Expect(app.notify_player_.starts == 1, "notification admitted once");
    Expect(app.GetDeviceState() == kDeviceStateNotifying, "still notifying after late abort");
    Expect(coordinator.CurrentGeneration() == 0, "old generation retired");
    Expect(app.audio_service_.streaming_resets == 1 && app.audio_service_.decoder_resets == 1,
           "late old abort cannot reset notification audio");
    Expect(app.audio_service_.queue == std::vector<std::string>({"popup", "remote"}),
           "popup and remote audio preserved in order");
    Expect(AudioStreamCanRequeue(notify_work, app.audio_service_.gate),
           "notification in-flight generation remains current");
    Expect(Board::GetInstance().power == PowerSaveLevel::PERFORMANCE,
           "late abort cannot lower notification power");
    Expect((view.entry_->flags & LV_OBJ_FLAG_HIDDEN) &&
               !(view.entry_->flags & LV_OBJ_FLAG_CLICKABLE) && !view.controller.hit_rect,
           "SO hidden, unclickable and absent from hit test during notification");
    bool mutated = false;
    Expect(!coordinator.TryUiAction(generation, StrokeRoundCoordinator::UiTransition::None, 3,
                                    [&]() { return mutated = true; }) &&
               !mutated,
           "stale UI token cannot mutate after notification takeover");
}

void EntryVisibility() {
    for (int state = kDeviceStateUnknown; state <= kDeviceStateFatalError; ++state) {
        if (state == kDeviceStateIdle) {
            continue;
        }
        for (bool active : {false, true}) {
            Application app;
            auto& view = StrokeOrderView::GetInstance();
            uint64_t generation = 0;
            if (active) {
                generation = app.stroke_round_.BeginRound(0);
                Expect(app.stroke_round_.MarkLocalCandidates(generation, 1), "entry candidates");
            }
            app.SetDeviceState(static_cast<DeviceState>(state));
            Expect((view.entry_->flags & LV_OBJ_FLAG_HIDDEN) &&
                       !(view.entry_->flags & LV_OBJ_FLAG_CLICKABLE) && !view.controller.hit_rect,
                   "every non-Idle transition hides/disables SO immediately");
            Expect(!view.lifecycle_.TryOpenOverlay(), "non-Idle cannot reopen overlay");
            if (active) {
                Expect(app.stroke_round_.HasCancelFence(generation),
                       "unexpected non-Idle state still fences active round before deferred abort");
                app.DispatchDeferred();
                Expect(app.stroke_round_.CurrentGeneration() == 0, "deferred abort retires round");
            }
            app.SetDeviceState(kDeviceStateIdle);
            Expect(!(view.entry_->flags & LV_OBJ_FLAG_HIDDEN), "Idle restores eligible entry");
        }
    }
}

void BusyRejection() {
    for (bool player_busy : {false, true}) {
        Application app;
        const auto generation = app.stroke_round_.BeginRound(0);
        Expect(app.stroke_round_.MarkLocalCandidates(generation, 1), "rejection candidates");
        if (player_busy) {
            app.notify_player_.busy = true;
        } else {
            app.state_machine_.state = kDeviceStateListening;
        }
        app.StartNotification("https://example.invalid/notify.ogg", {});
        Expect(app.stroke_round_.IsCurrentGeneration(generation), "busy rejection preserves round");
        Expect(app.audio_service_.streaming_resets == 0 && app.notify_player_.starts == 0,
               "busy rejection has no audio/start side effects");
    }
}

int main(int argc, char**) {
    if (argc > 1) {
        EntryVisibility();
        std::cout << "notify_stroke_entry: PASS\n";
        return 0;
    }
    for (bool playback : {false, true}) {
        for (bool voice : {false, true}) {
            for (bool pending : {false, true}) {
                NotificationOrder(playback, voice, pending);
            }
        }
    }
    BusyRejection();
    std::cout << "notify_stroke_order: PASS\n";
}
