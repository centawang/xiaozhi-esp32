// Real LVGL event dispatch + verbatim production View methods (production.inc).
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lvgl.h>
#include "display.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_parse.h"
#include "stroke_order/stroke_order_pinyin.h"
#include "stroke_order/stroke_order_source.h"
// Only unreachable/transient-state failure injection uses private fields.
#define private public
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_view.h"
#undef private
#include "stroke_order/stroke_order_ui_action.h"

static uint64_t now_us = 1000000;
static uint64_t esp_timer_get_time() { return now_us; }
#define ESP_LOGE(...) ((void)0)
static constexpr int MAIN_EVENT_STROKE_START = 1, MAIN_EVENT_STROKE_ABORT = 2;
static constexpr int kAbortReasonNone = 0;
enum class PowerSaveLevel { LOW_POWER };
struct Board {
    static Board& GetInstance() {
        static Board board;
        return board;
    }
    void SetPowerSaveLevel(PowerSaveLevel) {}
    Display* GetDisplay() {
        static Display display;
        return &display;
    }
};
static std::function<void(const char*)> boundary_hook;
static void TestBoundary(const char* name) {
    if (boundary_hook) {
        // Copy allows a one-shot callback to clear itself before reentry.
        auto hook = boundary_hook;
        hook(name);
    }
}
static void xEventGroupSetBits(std::atomic<int>& events, int bits) { events.fetch_or(bits); }
struct AudioChannelCloseInfo {
    uint64_t open_attempt_id = 0;
    std::string session_id;
};
struct FakeProtocol {
    bool IsAudioChannelOpened() { return false; }
    void CloseAudioChannel() {}
    void SendStopListening() {}
    void SendAbortSpeaking(int) {}
};
class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() { return state; }
    void RequestAbortStrokeRound(uint64_t generation, StrokeAbortReason reason);
    void PublishStrokeCancelFence(StrokeAbortReason reason);
    void HandleStrokeAbortEvent();
    void AbortStrokeRound(uint64_t generation, StrokeAbortReason reason);
    void RequestStartStrokeRound(uint64_t expected_generation = 0);
    void HandleStrokeStartEvent();
    void BeginStrokeRoundFromMain(uint64_t expected_generation, uint64_t sequence);
    void InvalidateStrokeStartLocked(uint64_t expected_generation);
    bool IsStrokeAbortPending(uint64_t expected_generation);
    void ClearStrokeOpenAttempt(uint64_t) {}
    uint64_t MatchStrokeOpenAttempt(uint64_t) { return 0; }
    void OnClosed(const AudioChannelCloseInfo& info);
    std::vector<std::function<void()>> scheduled;
    void Schedule(std::function<void()> task) { scheduled.push_back(std::move(task)); }
    void RunScheduled() {
        auto tasks = std::move(scheduled);
        for (auto& task : tasks)
            task();
    }
    void SetDeviceState(DeviceState value) { state = value; }
    void Dispatch() {
        const auto bits = event_group_.exchange(0);
#include "dispatch.inc"
    }
    int Aborts() const { return stroke_abort_pending_ ? 1 : 0; }
    bool StrokeVoiceRoutingAvailable() const { return voice_available; }
    void BeginLocalStrokeCandidatesFromMain(uint64_t) { ++local_fallbacks; }
    void DrainStreamingAudio() { TestBoundary("drain"); }
    void QueueListeningRequest(uint64_t generation) { listening_generation = generation; }
    void Reset() {
        state = kDeviceStateIdle;
        scheduled.clear();
        stroke_abort_pending_ = false;
        event_group_ = 0;
        stroke_abort_expected_generation_ = listening_generation = 0;
        stroke_start_pending_ = false;
        stroke_start_expected_generation_ = 0;
        voice_available = true;
        local_fallbacks = 0;
    }
    int Starts() const { return stroke_start_pending_ ? 1 : 0; }
    uint64_t StartedGeneration() const { return stroke_start_expected_generation_; }
    DeviceState state = kDeviceStateIdle;
    int local_fallbacks = 0;
    uint64_t listening_generation = 0;
    bool stroke_abort_pending_ = false;
    uint64_t stroke_abort_expected_generation_ = 0;
    StrokeAbortReason stroke_abort_reason_ = StrokeAbortReason::UserClose;
    std::mutex stroke_audio_route_mutex_, listening_request_mutex_;
    bool listening_request_pending_ = false, pending_listening_start_ = false;
    uint64_t listening_request_generation_ = 0, active_listening_generation_ = 0;
    StrokeRoundCoordinator stroke_round_;
    StrokeOrderView* view = nullptr;
    FakeProtocol transport;
    FakeProtocol* protocol_ = &transport;
    bool voice_available = true;
    std::atomic<bool> stroke_voice_transport_available_{true};
    std::recursive_mutex stroke_listening_start_mutex_;
    std::mutex stroke_command_mutex_;
    uint64_t stroke_start_sequence_ = 0;
    bool stroke_start_pending_ = false;
    uint64_t stroke_start_expected_generation_ = 0;
    std::atomic<int> event_group_{0};
};

#include "application.inc"
#include "production.inc"
StrokeOrderView& StrokeOrderView::GetInstance() { return *Application::GetInstance().view; }
StrokeOrderView::StrokeOrderView() = default;
StrokeOrderView::~StrokeOrderView() { DestroyOverlay(); }
void StrokeOrderView::ReevaluateEntryLocked() {}
void StrokeOrderView::SetVoiceTransportAvailable(bool) {}
void StrokeOrderView::ShowSpeechTimedOutFromMain() {}

static std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void Click(lv_obj_t* target) {
    assert(target && lv_obj_is_valid(target));
    now_us += 200000;
    lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
}

struct Fixture {
    Display display;
    StrokeOrderController controller;
    StrokeRoundCoordinator& coordinator = Application::GetInstance().stroke_round_;
    StrokeOrderView view;
    uint64_t generation;
    explicit Fixture(const std::vector<uint8_t>& blob, bool start_playback = true) {
        Application::GetInstance().Reset();
        Application::GetInstance().view = &view;
        assert(controller.BindStore(blob.data(), blob.size()));
        const uint32_t cps[] = {0x4E00, 0x4EBA, 0x53E3};
        assert(controller.SetCandidates(cps, 3));
        assert(controller.OpenCandidates());
        view.display_ = &display;
        view.controller_ = &controller;
        view.coordinator_ = &coordinator;
        generation = coordinator.BeginRound(now_us / 1000);
        assert(coordinator.MarkLocalCandidates(generation, now_us / 1000));
        assert(view.session_.BeginLocalCandidates(generation));
        view.presented_generation_ = generation;
        view.lifecycle_.Initialize();
        view.lifecycle_.SetAssetsReady(true);
        view.lifecycle_.SetPointerReady(true);
        view.lifecycle_.SetDeviceIdle(true);
        assert(view.lifecycle_.TryOpenOverlay());
        assert(view.EnsureOverlay());
        assert(view.RenderCandidates());
        if (start_playback)
            Select(1);
    }
    void Select(unsigned index) {
        assert(view.showing_candidates_ && index < view.candidate_glyphs_.size());
        // RenderCandidates creates title, X, then its candidate buttons.
        Click(lv_obj_get_child(view.overlay_, index + 2));
        assert(controller.state() == StrokeOrderUiState::Animating);
        assert(view.session_.phase() == StrokeOrderVoicePhase::LocalPlayback);
        assert(coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::LocalPlayback);
        assert(view.canvas_ && view.anim_timer_ && view.anim_clock_.first_frame_pending());
    }
    void Complete() {
        for (unsigned i = 0; controller.state() == StrokeOrderUiState::Animating; ++i) {
            assert(i < 100);
            now_us += 160000;
            StrokeOrderView::AnimTimerCb(view.anim_timer_);
        }
        assert(controller.state() == StrokeOrderUiState::Completed);
    }
    void CheckRestartPending() {
        auto& app = Application::GetInstance();
        assert(controller.state() == StrokeOrderUiState::Hidden);
        assert(app.Starts() == 1 && app.Aborts() == 0);
        assert(app.StartedGeneration() == generation);
        assert(coordinator.HasCancelFence(generation));
        assert(view.anim_timer_ == nullptr && !view.anim_clock_.running());
        assert(!view.anim_clock_.first_frame_pending());
        auto* close = view.control_buttons_[4];
        assert(!lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
        Click(close);  // duplicate old X before the main task runs
        assert(app.Starts() == 1 && app.Aborts() == 0);
    }
    void FinishRestart() {
        auto& app = Application::GetInstance();
        app.HandleStrokeStartEvent();
        const auto fresh = coordinator.CurrentGeneration();
        assert(fresh != 0 && fresh != generation);
        assert(app.listening_generation == fresh);
        assert(controller.state() == StrokeOrderUiState::Connecting);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(view.overlay_, 0)), "Connect") == 0);
        // Only the hardware/network readiness boundary is simulated. Speak is
        // rendered by the real View method, after a fresh correlated channel.
        assert(coordinator.MarkConnecting(fresh));
        const auto id = std::string("fresh-") + std::to_string(fresh);
        assert(coordinator.BindOpenedChannel(fresh, id));
        assert(coordinator.MarkListeningStarted(fresh, now_us / 1000));
        app.state = kDeviceStateListening;
        assert(view.ShowListeningFromMain(fresh));
        assert(controller.state() == StrokeOrderUiState::AwaitingSpeech);
        assert(view.session_.phase() == StrokeOrderVoicePhase::AwaitingSpeech);
        assert(view.presented_generation_ == fresh);
        assert(controller.candidate_count() == 0 && controller.selected_codepoint_ == 0);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(view.overlay_, 0)), "Speak") == 0);
        assert(view.canvas_ == nullptr && view.anim_timer_ == nullptr);
        assert(app.Aborts() == 0);
    }
    void CheckCandidates() {
        assert(controller.state() == StrokeOrderUiState::Candidates);
        assert(controller.candidate_count() == 3);
        assert(view.session_.generation() == generation);
        assert(view.presented_generation_ == generation);
        assert(view.session_.phase() == StrokeOrderVoicePhase::Candidates);
        assert(coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::Candidates);
        assert(view.showing_candidates_ && view.candidate_glyphs_.size() == 3);
        assert(view.overlay_visible_.load() && view.canvas_ == nullptr &&
               view.anim_timer_ == nullptr);
        assert(!view.anim_clock_.running() && !view.anim_clock_.first_frame_pending());
        assert(lv_obj_has_flag(view.control_buttons_[4], LV_OBJ_FLAG_CLICKABLE));
        assert(Application::GetInstance().Aborts() == 0 &&
               Application::GetInstance().Starts() == 0);
        auto timeout = coordinator.CheckTimeouts(now_us / 1000 + 59999);
        assert(timeout.kind == StrokeRoundCoordinator::TimeoutKind::None);
        timeout = coordinator.CheckTimeouts(now_us / 1000 + 60000);
        assert(timeout.kind == StrokeRoundCoordinator::TimeoutKind::Candidates);
        assert(timeout.generation == generation);
    }
};

static unsigned deleted = 0;
static void Deletion(lv_event_t* event) {
    // Admission must disarm the OLD target before the main task deletes it.
    // Merely checking lv_obj_is_valid AFTER teardown permits allocator ABA.
    assert(!lv_obj_has_flag(lv_event_get_target_obj(event), LV_OBJ_FLAG_CLICKABLE));
    ++deleted;
}

static void Completed(const std::vector<uint8_t>& blob, bool deletion_order) {
    Fixture f(blob);
    f.Complete();
    if (deletion_order)
        lv_obj_add_event_cb(f.view.control_buttons_[4], Deletion, LV_EVENT_DELETE, nullptr);
    Click(f.view.control_buttons_[4]);
    if (deletion_order)
        assert(deleted == 0);
    f.CheckRestartPending();
    f.FinishRestart();
    if (deletion_order)
        assert(deleted == 1);
}

static void Contention(const std::vector<uint8_t>& blob) {
    Fixture f(blob);
    f.Complete();
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::thread holder([&] {
        assert(f.coordinator.TryUiAction(f.generation, StrokeRoundCoordinator::UiTransition::None,
                                         0, [&] {
                                             std::unique_lock<std::mutex> lock(mutex);
                                             entered = true;
                                             cv.notify_one();
                                             cv.wait(lock, [&] { return release; });
                                             return true;
                                         }));
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    auto* close = f.view.control_buttons_[4];
    Click(close);
    assert(f.controller.state() == StrokeOrderUiState::Completed);
    assert(lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
    assert(Application::GetInstance().Aborts() == 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_one();
    holder.join();
    Click(close);
    f.CheckRestartPending();
    f.FinishRestart();
}

static void Fences(const std::vector<uint8_t>& blob) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f(blob);
        f.Complete();
        if (mode == 0)
            assert(f.coordinator.PublishCancelFence(f.generation));
        if (mode == 1) {
            const auto replacement = f.coordinator.BeginRound(now_us / 1000);
            assert(replacement != f.generation);
        }
        if (mode == 2)
            assert(f.view.session_.BeginLocalCandidates(f.generation + 1));
        if (mode == 3)
            f.view.presented_generation_ = 0;
        auto* close = f.view.control_buttons_[4];
        const auto phase = f.coordinator.CurrentPhase();
        Click(close);
        assert(f.controller.state() == StrokeOrderUiState::Completed);
        assert(lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
        assert(f.coordinator.CurrentPhase() == phase);
        assert(!f.view.showing_candidates_ && Application::GetInstance().Aborts() == 0);
    }
}

static void OtherStates(const std::vector<uint8_t>& blob) {
    for (auto state :
         {StrokeOrderUiState::Animating, StrokeOrderUiState::Paused, StrokeOrderUiState::Loading,
          StrokeOrderUiState::Error, StrokeOrderUiState::Candidates, StrokeOrderUiState::Hidden}) {
        Fixture f(blob);
        if (state == StrokeOrderUiState::Paused)
            Click(f.view.control_buttons_[0]);
        else if (state == StrokeOrderUiState::Candidates)
            Click(f.view.control_buttons_[3]);
        else
            f.controller.state_ = state;  // inject transient Loading / defensive Hidden
        auto* close = f.view.control_buttons_[4];
        Click(close);
        if (state != StrokeOrderUiState::Animating && state != StrokeOrderUiState::Paused) {
            assert(f.controller.state() == StrokeOrderUiState::Hidden);
            assert(Application::GetInstance().Aborts() ==
                   (state == StrokeOrderUiState::Hidden ? 0 : 1));
            assert(Application::GetInstance().Starts() == 0);
        } else {
            f.CheckRestartPending();
            f.FinishRestart();
        }
    }
}

static void Unavailable(const std::vector<uint8_t>& blob) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f(blob);
        f.Complete();
        if (mode == 0)
            f.controller.candidate_count_ = 0;  // no menu to return to
        if (mode == 1)
            f.view.session_.MarkError();  // not the playback presentation session
        if (mode == 2)
            Application::GetInstance().state = kDeviceStateSpeaking;
        if (mode == 3)
            f.controller.candidates_[0] = f.controller.candidates_[1] =
                f.controller.candidates_[2] = 0x9FFF;  // all glyph decodes fail at RenderCandidates
        Click(f.view.control_buttons_[4]);
        assert(!f.view.showing_candidates_);
        if (mode == 2) {
            assert(Application::GetInstance().Aborts() == 1);  // non-idle: never restart over chat
        } else {
            f.CheckRestartPending();
            f.FinishRestart();
        }
    }
}

static void OldControl(const std::vector<uint8_t>& blob) {
    Fixture f(blob);
    f.Complete();
    // Retain an old registered button outside the overlay to simulate a delayed
    // event on a retired surface without ever dereferencing a freed LVGL object.
    auto* old = f.view.control_buttons_[4];
    lv_obj_set_parent(old, lv_screen_active());
    assert(f.view.RenderAnimationPage());
    Click(old);
    assert(f.controller.state() == StrokeOrderUiState::Completed);
    assert(Application::GetInstance().Aborts() == 0);
    assert(lv_obj_has_flag(f.view.control_buttons_[4], LV_OBJ_FLAG_CLICKABLE));
    lv_obj_delete(old);
    Click(f.view.control_buttons_[4]);
    f.CheckRestartPending();
    f.FinishRestart();
}

// Real status/error surfaces, NOT animation controls with an injected state.
// Loading deliberately uses EnterConnecting, just like PrepareCandidatesLocked.
enum class Page { Connect, Speak, Loading, NoMatch, Retry, Error };
struct PageCase {
    Page page;
    unsigned slot;
};
static const PageCase page_cases[] = {{Page::Connect, 4}, {Page::Speak, 4}, {Page::Loading, 4},
                                      {Page::NoMatch, 4}, {Page::Retry, 4}, {Page::NoMatch, 2},
                                      {Page::Retry, 2},   {Page::Error, 2}, {Page::Error, 3}};

struct PageFixture : Fixture {
    Page page;
    PageFixture(const std::vector<uint8_t>& blob, Page value) : Fixture(blob, false), page(value) {
        if (page == Page::Error) {
            // A retained selection with a load error; exercise public state APIs.
            assert(controller.SelectCandidate(1));
            assert(coordinator.MarkLocalPlayback(generation));
            view.session_.MarkLocalPlayback();
            assert(controller.EnterError());
        } else if (page == Page::Loading) {
            assert(controller.EnterConnecting());
        } else {
            // Start a real voice generation and advance its public routing API.
            generation = coordinator.BeginRound(now_us / 1000);
            const auto identity = std::string("status-") + std::to_string(generation);
            view.presented_generation_ = generation;
            assert(coordinator.MarkConnecting(generation));
            assert(view.session_.BeginConnecting(generation));
            assert(controller.EnterConnecting());
            if (page != Page::Connect) {
                assert(coordinator.BindOpenedChannel(generation, identity));
                assert(coordinator.MarkListeningStarted(generation, now_us / 1000));
                assert(view.session_.MarkListeningReady(generation));
                assert(controller.EnterAwaitingSpeech());
            }
            if (page == Page::NoMatch || page == Page::Retry) {
                const auto route =
                    coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                             identity.data(), identity.size(), true);
                assert(coordinator.CommitStrokeStt(route));
                // Invalid ASCII STT uses the real parser -> production Retry.
                assert(view.ApplyVoiceUtteranceLocked(generation,
                                                      page == Page::Retry ? "abc" : "你好"));
                assert(view.session_.phase() == (page == Page::Retry
                                                     ? StrokeOrderVoicePhase::Error
                                                     : StrokeOrderVoicePhase::NoMatch));
            }
        }
        Rebuild();
        assert(view.canvas_ == nullptr && view.anim_timer_ == nullptr);
    }
    void Rebuild() {
        switch (page) {
            case Page::Connect:
                assert(view.RenderStatusPage("Connect", false));
                break;
            case Page::Speak:
                assert(view.RenderStatusPage("Speak", false));
                break;
            case Page::Loading:
                assert(view.RenderStatusPage("Loading", false));
                break;
            case Page::NoMatch:
                assert(view.RenderStatusPage("No match", true));
                break;
            case Page::Retry:
                assert(view.RenderStatusPage("Retry", true));
                break;
            case Page::Error:
                assert(view.RenderErrorPage());
                break;
        }
    }
    lv_obj_t* Button(unsigned slot) {
        // Locate the actual child with the production callback independently of
        // control_buttons_: missing registration must fail behavior, not setup.
        for (unsigned i = 0; i < lv_obj_get_child_count(view.overlay_); ++i) {
            auto* child = lv_obj_get_child(view.overlay_, i);
            if (StrokeOrderView::IndexFromUserData(child) != slot)
                continue;
            for (unsigned j = 0; j < lv_obj_get_event_count(child); ++j) {
                auto* dsc = lv_obj_get_event_dsc(child, j);
                if (lv_event_dsc_get_cb(dsc) == StrokeOrderView::ControlClicked)
                    return child;
            }
        }
        assert(false && "real page is missing requested button");
        return nullptr;
    }
    void CheckAccepted(unsigned slot) {
        auto& app = Application::GetInstance();
        if (slot == 4 && page == Page::Loading) {
            CheckRestartPending();
            FinishRestart();
        } else if (slot == 4) {
            assert(controller.state() == StrokeOrderUiState::Hidden);
            assert(app.Aborts() == 1 && app.Starts() == 0);
            assert(app.stroke_abort_expected_generation_ == generation);
            assert(app.stroke_abort_reason_ == StrokeAbortReason::UserClose);
            auto* close = Button(4);
            assert(!lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
            Click(close);
            assert(app.Aborts() == 1);  // even a forced duplicate cannot abort twice
        } else if (page != Page::Error) {
            assert(app.Starts() == 1 && app.Aborts() == 0);
            assert(app.StartedGeneration() == generation);
        } else if (slot == 3) {
            CheckCandidates();
            Select(0);  // the replacement candidate page is live
        } else {
            assert(controller.state() == StrokeOrderUiState::Animating);
            assert(view.session_.phase() == StrokeOrderVoicePhase::LocalPlayback);
            assert(coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::LocalPlayback);
            assert(view.canvas_ && view.anim_timer_ && view.anim_clock_.first_frame_pending());
            assert(app.Aborts() == 0 && app.Starts() == 0);
            Click(view.control_buttons_[0]);  // the replacement animation page is live
            assert(controller.state() == StrokeOrderUiState::Paused);
        }
    }
};

static void CountDeletion(lv_event_t* event) {
    ++*static_cast<unsigned*>(lv_event_get_user_data(event));
}

static void PageAction(const std::vector<uint8_t>& blob, Page page, unsigned slot) {
    PageFixture f(blob, page);
    auto* target = f.Button(slot);
    unsigned count = 0;
    lv_obj_add_event_cb(target, CountDeletion, LV_EVENT_DELETE, &count);
    Click(target);
    f.CheckAccepted(slot);
    if (page == Page::Error || (slot == 4 && page == Page::Loading)) {
        assert(count == 1);
    } else {
        assert(count == 0);
        f.view.DestroyOverlay();
        assert(count == 1);
        for (auto* control : f.view.control_buttons_)
            assert(control == nullptr);
    }
}

static void PageRejections(const std::vector<uint8_t>& blob) {
    for (auto test : page_cases) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            PageFixture f(blob, test.page);
            auto* target = f.Button(test.slot);
            if (mode == 1)
                assert(f.coordinator.PublishCancelFence(f.generation));
            else if (mode == 2)
                assert(f.coordinator.BeginRound(now_us / 1000) != f.generation);
            else if (mode == 3)
                assert(f.view.session_.BeginConnecting(f.generation + 1));
            const auto state = f.controller.state();
            const auto phase = f.coordinator.CurrentPhase();
            const auto voice = f.view.session_.phase();
            std::mutex mutex;
            std::condition_variable cv;
            bool entered = false, release = false;
            std::thread holder;
            if (mode == 0) {
                holder = std::thread([&] {
                    assert(f.coordinator.TryUiAction(
                        f.generation, StrokeRoundCoordinator::UiTransition::None, 0, [&] {
                            std::unique_lock<std::mutex> lock(mutex);
                            entered = true;
                            cv.notify_one();
                            cv.wait(lock, [&] { return release; });
                            return true;
                        }));
                });
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return entered; });
            }
            Click(target);
            assert(f.controller.state() == state && f.view.session_.phase() == voice);
            assert(lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE));
            assert(Application::GetInstance().Starts() == 0 &&
                   Application::GetInstance().Aborts() == 0);
            if (mode == 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    release = true;
                }
                cv.notify_one();
                holder.join();
            }
            assert(f.coordinator.CurrentPhase() == phase);
            if (mode == 0) {
                Click(target);  // identical real control remains usable after try-lock miss
                f.CheckAccepted(test.slot);
            }
        }
    }
}

static void RetiredPages(const std::vector<uint8_t>& blob) {
    for (auto test : page_cases) {
        PageFixture f(blob, test.page);
        auto* old = f.Button(test.slot);
        lv_obj_set_parent(old, lv_screen_active());  // retain safely, never use freed pointers
        f.Rebuild();
        assert(old != f.Button(test.slot));
        const auto state = f.controller.state();
        Click(old);
        assert(f.controller.state() == state);
        assert(Application::GetInstance().Starts() == 0 &&
               Application::GetInstance().Aborts() == 0);
        lv_obj_delete(old);
        Click(f.Button(test.slot));
        f.CheckAccepted(test.slot);
    }
}

static void SlotsRetiredBeforeDelete(lv_event_t* event) {
    auto* view = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    for (auto* control : view->control_buttons_)
        assert(control == nullptr);
}

static void PageSlotLifetime(const std::vector<uint8_t>& blob) {
    // All ControlClicked constructors, including the pre-existing candidate and
    // animation pages, must retire slots BEFORE child deletion/rebuild.
    for (unsigned destination = 0; destination < 6; ++destination) {
        Fixture f(blob);
        auto* old = f.view.control_buttons_[4];
        lv_obj_add_event_cb(old, SlotsRetiredBeforeDelete, LV_EVENT_DELETE, &f.view);
        switch (destination) {
            case 0:
                assert(f.view.RenderCandidates());
                break;
            case 1:
                assert(f.view.RenderAnimationPage());
                break;
            case 2:
                assert(f.view.RenderErrorPage());
                break;
            case 3:
                assert(f.view.RenderStatusPage("Retry", true));
                break;
            case 4:
                f.view.DestroyOverlay();
                break;
            case 5:
                lv_obj_delete(f.view.overlay_);
                break;
        }
        for (unsigned slot = 0; slot < StrokeOrderLayout::kControlCount; ++slot) {
            auto* control = f.view.control_buttons_[slot];
            if (control) {
                assert(lv_obj_is_valid(control));
                assert(lv_obj_get_parent(control) == f.view.overlay_);
                assert(StrokeOrderView::IndexFromUserData(control) == slot);
            }
        }
    }
}

static lv_indev_data_t pointer_data{};
static void ReadPointer(lv_indev_t*, lv_indev_data_t* data) { *data = pointer_data; }

static void PointerSequence(const std::vector<uint8_t>& blob) {
    Fixture f(blob);
    f.Complete();
    auto* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ReadPointer);
    lv_obj_update_layout(f.view.overlay_);
    lv_area_t area;
    lv_obj_get_coords(f.view.control_buttons_[4], &area);
    pointer_data.point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    assert(f.controller.state() == StrokeOrderUiState::Completed);
    // Held samples must not dispatch; only the one CLICKED on release does.
    lv_indev_read(indev);
    now_us += 200000;
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    f.CheckRestartPending();
    f.FinishRestart();
    lv_indev_read(indev);
    lv_indev_read(indev);
    assert(f.controller.state() == StrokeOrderUiState::AwaitingSpeech);
    assert(Application::GetInstance().Aborts() == 0);
    lv_indev_delete(indev);
}

static void Back(const std::vector<uint8_t>& blob) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture f(blob);
        if (mode == 1)
            Click(f.view.control_buttons_[0]);
        if (mode == 2)
            f.Complete();
        Click(f.view.control_buttons_[3]);
        f.CheckCandidates();
        f.Select(0);
    }
}

static void StaleSpeak(const std::vector<uint8_t>& blob) {
    Fixture f(blob, false);
    f.generation = f.coordinator.BeginRound(now_us / 1000);
    f.view.presented_generation_ = f.generation;
    assert(f.view.session_.BeginConnecting(f.generation));
    assert(f.coordinator.MarkConnecting(f.generation));
    assert(f.coordinator.BindOpenedChannel(f.generation, "old-stt"));
    assert(f.coordinator.MarkListeningStarted(f.generation, now_us / 1000));
    assert(f.view.session_.MarkListeningReady(f.generation));
    auto stt =
        f.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt, "old-stt", 7, true);
    auto close = f.coordinator.CaptureChannelClose("old-stt", 7, true);
    assert(f.coordinator.CommitStrokeStt(stt));
    assert(f.coordinator.MarkCandidates(f.generation, now_us / 1000));
    f.view.session_.MarkCandidates();
    f.Select(1);
    // Keep real old controls alive, away from the overlay being rebuilt.
    auto* old_back = f.view.control_buttons_[3];
    auto* old_x = f.view.control_buttons_[4];
    lv_obj_set_parent(old_back, lv_screen_active());
    lv_obj_set_parent(old_x, lv_screen_active());
    Click(old_x);
    f.CheckRestartPending();
    // A pending load completion may not reopen Candidates after X admission.
    bool mutated = false;
    assert(!f.coordinator.TryUiAction(f.generation, StrokeRoundCoordinator::UiTransition::None, 0,
                                      [&] {
                                          mutated = true;
                                          return f.controller.OpenCandidates();
                                      }));
    assert(!mutated);
    f.FinishRestart();
    const auto fresh = f.view.presented_generation_;
    Click(old_back);
    Click(old_x);
    assert(!StrokeOrderApplyAnimationTick(f.coordinator, f.controller, f.view.session_,
                                          f.view.anim_clock_, f.generation, now_us + 5000000));
    assert(!f.coordinator.TryUiAction(f.generation, StrokeRoundCoordinator::UiTransition::None, 0,
                                      [&] {
                                          mutated = true;
                                          return f.controller.OpenCandidates();
                                      }));
    assert(!mutated);  // late load completion cannot replace fresh Speak with Candidates
    assert(!f.coordinator.CommitStrokeStt(stt));
    assert(!f.coordinator.RevalidateStrokeChannelClose(close));
    assert(!f.view.ShowListeningFromMain(f.generation));
    f.view.AbortFromMain(f.generation);
    assert(f.controller.state() == StrokeOrderUiState::AwaitingSpeech);
    assert(f.view.presented_generation_ == fresh);
    assert(Application::GetInstance().Aborts() == 0 && Application::GetInstance().Starts() == 0);
    lv_obj_delete(old_back);
    lv_obj_delete(old_x);
}

static void StaleStart(const std::vector<uint8_t>& blob) {
    Fixture f(blob);
    Click(f.view.control_buttons_[4]);
    f.CheckRestartPending();
    const auto newer = f.coordinator.BeginRound(now_us / 1000);
    Application::GetInstance().HandleStrokeStartEvent();
    assert(f.coordinator.CurrentGeneration() == newer);
    assert(Application::GetInstance().listening_generation == 0);
}

static void Offline(const std::vector<uint8_t>& blob) {
    Fixture f(blob);
    Application::GetInstance().voice_available = false;
    Click(f.view.control_buttons_[4]);
    f.CheckRestartPending();
    Application::GetInstance().HandleStrokeStartEvent();
    assert(Application::GetInstance().local_fallbacks == 1);
    assert(Application::GetInstance().listening_generation == 0);
    assert(f.controller.state() != StrokeOrderUiState::AwaitingSpeech);
}

// Real publisher, consumer, AbortRound and production abort-before-start loop.
static void AbortOrdering(const std::vector<uint8_t>& blob) {
    for (auto reason : {StrokeAbortReason::Alert, StrokeAbortReason::UserClose,
                        StrokeAbortReason::ChannelClosed, StrokeAbortReason::AssetSuspend,
                        StrokeAbortReason::PowerSave, StrokeAbortReason::NewNormalSession}) {
        for (bool offline : {false, true}) {
            Fixture f(blob);
            auto& app = Application::GetInstance();
            app.voice_available = !offline;
            Click(f.view.control_buttons_[4]);
            f.CheckRestartPending();
            app.RequestAbortStrokeRound(f.generation, reason);
            app.Dispatch();
            assert(!f.coordinator.IsRoundActive());
            assert(app.listening_generation == 0 && app.local_fallbacks == 0);
            assert(f.controller.state() == StrokeOrderUiState::Hidden);
            assert(!f.view.IsOverlayActive());
            app.HandleStrokeStartEvent();  // a stale event bit cannot re-consume
            assert(!f.coordinator.IsRoundActive());
        }
    }
}

static void CandidateFailure(const std::vector<uint8_t>& blob) {
    Fixture f(blob, false);
    // CandidateClicked's actual decode failure branch leaves the old Candidates
    // surface visible, but changes controller/session to Error.
    f.controller.candidates_[1] = 0x9fff;
    Click(lv_obj_get_child(f.view.overlay_, 3));
    assert(f.controller.state() == StrokeOrderUiState::Error);
    assert(f.view.showing_candidates_);
    Click(f.view.control_buttons_[4]);
    auto& app = Application::GetInstance();
    assert(app.Starts() == 0 && app.Aborts() == 1);
    app.Dispatch();
    assert(!f.coordinator.IsRoundActive() && !f.view.IsOverlayActive());
}

static void ChannelClose(const std::vector<uint8_t>& blob) {
    Fixture f(blob, false);
    auto& app = Application::GetInstance();
    f.generation = f.coordinator.BeginRound(now_us / 1000);
    f.view.presented_generation_ = f.generation;
    assert(f.view.session_.BeginConnecting(f.generation));
    assert(f.coordinator.MarkConnecting(f.generation));
    assert(f.coordinator.BindOpenedChannel(f.generation, "closing-old"));
    assert(f.coordinator.MarkListeningStarted(f.generation, now_us / 1000));
    assert(f.view.session_.MarkListeningReady(f.generation));
    const auto stt = f.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                                "closing-old", 11, true);
    assert(f.coordinator.CommitStrokeStt(stt));
    assert(f.coordinator.MarkCandidates(f.generation, now_us / 1000));
    f.view.session_.MarkCandidates();
    f.Select(1);
    Click(f.view.control_buttons_[4]);
    app.OnClosed({0, "closing-old"});
    app.Dispatch();
    app.RunScheduled();  // Production SCHEDULE is after STROKE_START.
    assert(!f.coordinator.IsRoundActive());
    assert(app.listening_generation == 0 && app.local_fallbacks == 0);
    assert(!f.view.IsOverlayActive());
}

static void AssertInactive(Fixture& f) {
    auto& app = Application::GetInstance();
    app.Dispatch();
    assert(!f.coordinator.IsRoundActive());
    assert(app.listening_generation == 0 && app.local_fallbacks == 0);
    assert(!f.view.IsOverlayActive());
    assert(f.controller.state() == StrokeOrderUiState::Hidden);
}

static void CancelWindows(const std::vector<uint8_t>& blob) {
    for (const char* boundary : {"dequeued", "drain", "before-commit", "after-commit"}) {
        for (bool global : {false, true}) {
            Fixture f(blob);
            auto& app = Application::GetInstance();
            Click(f.view.control_buttons_[4]);
            bool hit = false;
            boundary_hook = [&](const char* name) {
                if (std::strcmp(name, boundary) != 0)
                    return;
                hit = true;
                boundary_hook = {};
                const auto target = f.coordinator.CurrentGeneration();
                // Abort on a different publisher thread while main is paused
                // at a precise production boundary (no sleeps or stub abort).
                std::thread publisher([&] {
                    if (global)
                        f.view.RequestAbortLocked(StrokeAbortReason::Alert);
                    else
                        app.RequestAbortStrokeRound(target ? target : f.generation,
                                                    StrokeAbortReason::UserClose);
                });
                publisher.join();
            };
            app.HandleStrokeStartEvent();
            assert(hit && !boundary_hook);
            AssertInactive(f);
        }
    }
    // Direct main-task aborts use the same invalidation (not only publishers).
    Fixture f(blob);
    auto& app = Application::GetInstance();
    Click(f.view.control_buttons_[4]);
    app.AbortStrokeRound(f.generation, StrokeAbortReason::ChannelClosed);
    app.HandleStrokeStartEvent();
    AssertInactive(f);
}

static void SlotOrder(const std::vector<uint8_t>& blob) {
    {  // Abort before start publication rejects a delayed X, even after consume.
        Fixture f(blob);
        auto& app = Application::GetInstance();
        app.RequestAbortStrokeRound(f.generation, StrokeAbortReason::Alert);
        app.RequestStartStrokeRound(f.generation);
        assert(app.Starts() == 0);
        AssertInactive(f);
        app.RequestStartStrokeRound(f.generation);
        assert(app.Starts() == 0);
    }
    {  // A stale start/abort may not overwrite either slot for the new generation.
        Fixture f(blob);
        auto& app = Application::GetInstance();
        const auto old = f.generation;
        Click(f.view.control_buttons_[4]);
        f.FinishRestart();
        const auto fresh = f.coordinator.CurrentGeneration();
        app.RequestAbortStrokeRound(fresh, StrokeAbortReason::Alert);
        app.RequestAbortStrokeRound(old, StrokeAbortReason::UserClose);
        assert(app.stroke_abort_expected_generation_ == fresh);
        assert(app.stroke_abort_reason_ == StrokeAbortReason::Alert);
        app.Dispatch();
        assert(!f.coordinator.IsRoundActive());
    }
    {  // Different-generation legitimate start survives a queued old abort.
        Fixture f(blob);
        auto& app = Application::GetInstance();
        app.RequestAbortStrokeRound(f.generation, StrokeAbortReason::UserClose);
        const auto newer = f.coordinator.BeginRound(now_us / 1000);
        assert(f.view.session_.BeginLocalCandidates(newer));
        f.view.presented_generation_ = newer;
        app.RequestStartStrokeRound(newer);
        app.RequestStartStrokeRound(f.generation);  // cannot replace new slot
        app.RequestAbortStrokeRound(f.generation, StrokeAbortReason::Alert);
        assert(app.Starts() == 1 && app.StartedGeneration() == newer);
        app.Dispatch();
        assert(app.listening_generation > newer);
        assert(f.controller.state() == StrokeOrderUiState::Connecting);
        assert(!f.coordinator.HasCancelFence(app.listening_generation));
    }
    {  // Fresh SO queued with no active round is also cancelled by Alert.
        Fixture f(blob);
        auto& app = Application::GetInstance();
        app.AbortStrokeRound(f.generation, StrokeAbortReason::UserClose);
        app.RequestStartStrokeRound();
        assert(app.Starts() == 1);
        app.PublishStrokeCancelFence(StrokeAbortReason::Alert);
        AssertInactive(f);
    }
    {  // Two complete X cycles, duplicate request in each cycle stays one slot.
        Fixture f(blob);
        auto& app = Application::GetInstance();
        for (unsigned i = 0; i < 2; ++i) {
            Click(f.view.control_buttons_[4]);
            const auto sequence = app.stroke_start_sequence_;
            app.RequestStartStrokeRound(f.generation);
            assert(app.stroke_start_sequence_ == sequence);
            f.FinishRestart();
            if (i == 0) {
                f.generation = f.coordinator.CurrentGeneration();
                const auto identity = std::string("fresh-") + std::to_string(f.generation);
                const auto route =
                    f.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                               identity.data(), identity.size(), true);
                assert(f.coordinator.CommitStrokeStt(route));
                assert(f.coordinator.MarkCandidates(f.generation, now_us / 1000));
                assert(f.coordinator.RetireStrokeChannel(f.generation));
                f.view.session_.MarkCandidates();
                const uint32_t cps[] = {0x4E00, 0x4EBA, 0x53E3};
                assert(f.controller.SetCandidates(cps, 3));
                assert(f.controller.OpenCandidates());
                app.state = kDeviceStateIdle;
                assert(f.view.RenderCandidates());
                f.Select(1);
            }
        }
    }
}

static void PublishRace(const std::vector<uint8_t>& blob) {
    for (unsigned i = 0; i < 64; ++i) {
        Fixture f(blob);
        auto& app = Application::GetInstance();
        std::mutex mutex;
        std::condition_variable cv;
        bool go = false;
        auto wait = [&] {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return go; });
        };
        std::thread start([&] {
            wait();
            app.RequestStartStrokeRound(f.generation);
        });
        std::thread abort([&] {
            wait();
            app.RequestAbortStrokeRound(f.generation, StrokeAbortReason::Alert);
        });
        {
            std::lock_guard<std::mutex> lock(mutex);
            go = true;
        }
        cv.notify_all();
        start.join();
        abort.join();
        AssertInactive(f);
    }
}

int main(int argc, char** argv) {
    assert(argc == 3);
    lv_init();
    auto* display = lv_display_create(320, 240);
    assert(display);
    const auto blob = Read(argv[1]);
    const std::string mode = argv[2];
    if (mode == "completed" || mode == "delete-order")
        Completed(blob, mode == "delete-order");
    else if (mode == "contention")
        Contention(blob);
    else if (mode == "fences")
        Fences(blob);
    else if (mode == "other-states")
        OtherStates(blob);
    else if (mode == "unavailable")
        Unavailable(blob);
    else if (mode == "old-control")
        OldControl(blob);
    else if (mode == "pointer")
        PointerSequence(blob);
    else if (mode == "connect-x")
        PageAction(blob, Page::Connect, 4);
    else if (mode == "speak-x")
        PageAction(blob, Page::Speak, 4);
    else if (mode == "loading-x")
        PageAction(blob, Page::Loading, 4);
    else if (mode == "nomatch-x")
        PageAction(blob, Page::NoMatch, 4);
    else if (mode == "retry-x")
        PageAction(blob, Page::Retry, 4);
    else if (mode == "voice-retry") {
        PageAction(blob, Page::NoMatch, 2);
        PageAction(blob, Page::Retry, 2);
    } else if (mode == "error-retry")
        PageAction(blob, Page::Error, 2);
    else if (mode == "error-back")
        PageAction(blob, Page::Error, 3);
    else if (mode == "page-rejections")
        PageRejections(blob);
    else if (mode == "retired-pages")
        RetiredPages(blob);
    else if (mode == "slot-lifetime")
        PageSlotLifetime(blob);
    else if (mode == "back")
        Back(blob);
    else if (mode == "stale-speak")
        StaleSpeak(blob);
    else if (mode == "stale-start")
        StaleStart(blob);
    else if (mode == "channel-close")
        ChannelClose(blob);
    else if (mode == "cancel-windows")
        CancelWindows(blob);
    else if (mode == "slot-order")
        SlotOrder(blob);
    else if (mode == "publish-race")
        PublishRace(blob);
    else if (mode == "abort-order")
        AbortOrdering(blob);
    else if (mode == "candidate-failure")
        CandidateFailure(blob);
    else if (mode == "offline")
        Offline(blob);
    else
        assert(false);
    lv_display_delete(display);
    lv_deinit();
    std::cout << mode << ": PASS\n";
}
