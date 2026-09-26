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
class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() { return state; }
    void RequestAbortStrokeRound(uint64_t generation, StrokeAbortReason reason) {
        ++aborts;
        aborted_generation = generation;
        abort_reason = reason;
    }
    void RequestStartStrokeRound(uint64_t generation = 0) {
        ++starts;
        started_generation = generation;
    }
    DeviceState state = kDeviceStateIdle;
    int aborts = 0, starts = 0;
    uint64_t aborted_generation = 0, started_generation = 0;
    StrokeAbortReason abort_reason = StrokeAbortReason::UserClose;
};

#include "production.inc"
StrokeOrderView::StrokeOrderView() = default;
StrokeOrderView::~StrokeOrderView() { DestroyOverlay(); }
void StrokeOrderView::ReevaluateEntryLocked() {}

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
    StrokeRoundCoordinator coordinator;
    StrokeOrderView view;
    uint64_t generation;
    explicit Fixture(const std::vector<uint8_t>& blob, bool start_playback = true) {
        Application::GetInstance() = Application{};
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
        assert(Application::GetInstance().aborts == 0 && Application::GetInstance().starts == 0);
        auto timeout = coordinator.CheckTimeouts(now_us / 1000 + 59999);
        assert(timeout.kind == StrokeRoundCoordinator::TimeoutKind::None);
        timeout = coordinator.CheckTimeouts(now_us / 1000 + 60000);
        assert(timeout.kind == StrokeRoundCoordinator::TimeoutKind::Candidates);
        assert(timeout.generation == generation);
    }
};

static unsigned deleted = 0;
static void Deletion(lv_event_t* event) {
    // Admission must disarm the OLD target before RenderCandidates deletes it.
    // Merely checking lv_obj_is_valid AFTER rendering permits allocator ABA.
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
        assert(deleted == 1);
    f.CheckCandidates();
    for (unsigned index : {0u, 2u, 1u}) {
        f.Select(index);
        f.Complete();
        Click(f.view.control_buttons_[4]);
        f.CheckCandidates();
    }
    // X on the candidate menu is STILL Exit (exactly one abort).
    auto* close = f.view.control_buttons_[4];
    Click(close);
    assert(f.controller.state() == StrokeOrderUiState::Hidden);
    assert(Application::GetInstance().aborts == 1);
    Click(close);  // forced duplicate event: no second accepted mutation
    assert(Application::GetInstance().aborts == 1);
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
    assert(Application::GetInstance().aborts == 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_one();
    holder.join();
    Click(close);
    f.CheckCandidates();
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
        assert(!f.view.showing_candidates_ && Application::GetInstance().aborts == 0);
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
        assert(f.controller.state() == StrokeOrderUiState::Hidden);
        assert(Application::GetInstance().aborts == (state == StrokeOrderUiState::Hidden ? 0 : 1));
        if (state != StrokeOrderUiState::Hidden) {
            assert(f.view.anim_timer_ == nullptr);
            assert(!lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
            assert(Application::GetInstance().aborted_generation == f.generation);
            assert(Application::GetInstance().abort_reason == StrokeAbortReason::UserClose);
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
        assert(Application::GetInstance().aborts == 1);  // fail closed, never empty menu success
        assert(Application::GetInstance().aborted_generation == f.generation);
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
    assert(Application::GetInstance().aborts == 0);
    assert(lv_obj_has_flag(f.view.control_buttons_[4], LV_OBJ_FLAG_CLICKABLE));
    lv_obj_delete(old);
    Click(f.view.control_buttons_[4]);
    f.CheckCandidates();
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
        } else if (page == Page::Retry) {
            assert(controller.EnterError());
            view.session_.MarkError();
        } else {
            // Start a real voice generation and advance its public routing API.
            generation = coordinator.BeginRound(now_us / 1000);
            view.presented_generation_ = generation;
            assert(coordinator.MarkConnecting(generation));
            assert(view.session_.BeginConnecting(generation));
            assert(controller.EnterConnecting());
            if (page != Page::Connect) {
                assert(coordinator.BindOpenedChannel(generation, "status-test"));
                assert(coordinator.MarkListeningStarted(generation, now_us / 1000));
                assert(view.session_.MarkListeningReady(generation));
                assert(controller.EnterAwaitingSpeech());
            }
            if (page == Page::NoMatch) {
                const auto route = coordinator.CaptureRoute(
                    StrokeRoundCoordinator::MessageKind::Stt, "status-test", 11, true);
                assert(coordinator.CommitStrokeStt(route));
                assert(coordinator.MarkNoMatch(generation));
                view.session_.MarkNoMatch();
                assert(controller.EnterNoMatch());
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
        if (slot == 4) {
            assert(controller.state() == StrokeOrderUiState::Hidden);
            assert(app.aborts == 1 && app.starts == 0);
            assert(app.aborted_generation == generation);
            assert(app.abort_reason == StrokeAbortReason::UserClose);
            auto* close = Button(4);
            assert(!lv_obj_has_flag(close, LV_OBJ_FLAG_CLICKABLE));
            Click(close);
            assert(app.aborts == 1);  // even a forced duplicate cannot abort twice
        } else if (page != Page::Error) {
            assert(app.starts == 1 && app.aborts == 0);
            assert(app.started_generation == generation);
        } else if (slot == 3) {
            CheckCandidates();
            Select(0);  // the replacement candidate page is live
        } else {
            assert(controller.state() == StrokeOrderUiState::Animating);
            assert(view.session_.phase() == StrokeOrderVoicePhase::LocalPlayback);
            assert(coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::LocalPlayback);
            assert(view.canvas_ && view.anim_timer_ && view.anim_clock_.first_frame_pending());
            assert(app.aborts == 0 && app.starts == 0);
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
    if (page == Page::Error) {
        assert(count == 1);  // Retry/Back synchronously delete their event target
    } else {
        assert(count == 0);
        // Application's asynchronous abort/restart is a boundary stub here.
        // Explicit teardown/reopen verifies slots, not the real main-task work.
        f.view.DestroyOverlay();
        assert(count == 1);
        for (auto* control : f.view.control_buttons_)
            assert(control == nullptr);
        assert(f.view.EnsureOverlay());
        assert(f.controller.EnterConnecting());
        assert(f.view.RenderStatusPage("Loading", false));
        Click(f.Button(4));
        assert(Application::GetInstance().aborts == (slot == 4 ? 2 : 1));
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
            assert(Application::GetInstance().starts == 0 &&
                   Application::GetInstance().aborts == 0);
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
        assert(Application::GetInstance().starts == 0 && Application::GetInstance().aborts == 0);
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
    f.CheckCandidates();
    lv_indev_read(indev);
    lv_indev_read(indev);
    f.CheckCandidates();
    // Selecting another glyph through the actual pointer path works too.
    lv_obj_update_layout(f.view.overlay_);
    lv_obj_get_coords(lv_obj_get_child(f.view.overlay_, 2), &area);
    pointer_data.point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    assert(f.controller.state() == StrokeOrderUiState::Animating);
    assert(f.controller.loaded_codepoint() == 0x4E00);
    assert(f.view.anim_clock_.first_frame_pending());
    lv_indev_delete(indev);
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
    else
        assert(false);
    lv_display_delete(display);
    lv_deinit();
    std::cout << mode << ": PASS\n";
}
