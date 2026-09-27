// Compiles verbatim production View methods with real LVGL, not a draw mock.
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lvgl.h>
#include <src/draw/lv_draw_private.h>
#include "display.h"
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_pinyin.h"
#include "stroke_order/stroke_order_session.h"
#include "stroke_order/stroke_order_ui_action.h"
#include "stroke_order/stroke_round_coordinator.h"
#define private public
#include "stroke_order/stroke_order_view.h"
#undef private

static uint64_t now_us = 1000000;
static uint64_t esp_timer_get_time() { return now_us; }
#define ESP_LOGE(...) ((void)0)
class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() { return kDeviceStateIdle; }
    void RequestAbortStrokeRound(uint64_t, StrokeAbortReason) { ++aborts; }
    void RequestStartStrokeRound(uint64_t = 0) {}
    int aborts = 0;
};

static unsigned fills = 0, labels = 0, tasks = 0;
static void CountFill(lv_obj_t* canvas, lv_color_t color, lv_opa_t opa) {
    ++fills;
    lv_canvas_fill_bg(canvas, color, opa);
}
static void CountLabel(lv_obj_t* label, const char* text) {
    ++labels;
    lv_label_set_text(label, text);
}
static void CountFinish(lv_obj_t* canvas, lv_layer_t* layer) {
    for (auto* task = layer->draw_task_head; task != nullptr; task = task->next) {
        ++tasks;
    }
    lv_canvas_finish_layer(canvas, layer);
}
#define lv_canvas_fill_bg CountFill
#define lv_label_set_text CountLabel
#define lv_canvas_finish_layer CountFinish
#include "production.inc"
#undef lv_canvas_fill_bg
#undef lv_label_set_text
#undef lv_canvas_finish_layer

// These are outside the rendering seam (hardware entry/asset readiness).
StrokeOrderView::StrokeOrderView() = default;
StrokeOrderView::~StrokeOrderView() { DestroyOverlay(); }
void StrokeOrderView::ReevaluateEntryLocked() {}

static std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Fixture {
    Display display;
    StrokeOrderController controller;
    StrokeRoundCoordinator coordinator;
    StrokeOrderView view;
    Fixture(const std::vector<uint8_t>& blob, uint32_t cp) {
        assert(controller.BindStore(blob.data(), blob.size()));
        assert(controller.SetCandidates(&cp, 1));
        assert(controller.OpenCandidates());
        view.display_ = &display;
        view.controller_ = &controller;
        view.coordinator_ = &coordinator;
        const auto generation = coordinator.BeginRound(1);
        assert(coordinator.MarkLocalCandidates(generation, 1));
        assert(view.session_.BeginLocalCandidates(generation));
        view.presented_generation_ = generation;
        view.lifecycle_.Initialize();
        view.lifecycle_.SetAssetsReady(true);
        view.lifecycle_.SetPointerReady(true);
        view.lifecycle_.SetDeviceIdle(true);
        assert(view.lifecycle_.TryOpenOverlay());
        assert(StrokeOrderApplyUiAction(coordinator, controller, view.session_, view.anim_clock_,
                                        generation, StrokeOrderUiAction::Candidate, now_us));
        assert(view.EnsureOverlay());
        assert(view.RenderAnimationPage());
    }
    void Tick(uint64_t delta = 33000) {
        now_us += delta;
        StrokeOrderView::AnimTimerCb(view.anim_timer_);
    }
    void Control(unsigned index) {
        now_us += 200000;  // clear bounded Step debounce, not a sleep
        assert(view.HandleControlLocked(index));
    }
    std::vector<uint8_t> Pixels() {
        const auto* b = view.canvas_buf_;
        return {b->data, b->data + b->data_size};
    }
};

// Independent old per-edge implementation: same closed input, integer scaling,
// grid/cue order, opacity, round caps and reference/done widths as pre-patch.
static void LegacyOutline(lv_layer_t* layer, const StrokeOrderView::CachedStroke& stroke, int x,
                          int y, int size, lv_color_t color, int width) {
    for (size_t i = 1; i < stroke.outline.size(); ++i) {
        DrawLine(layer, StrokeOrderLayout::Scale(stroke.outline[i - 1].x, x + 12, size - 24),
                 StrokeOrderLayout::Scale(stroke.outline[i - 1].y, y + 12, size - 24),
                 StrokeOrderLayout::Scale(stroke.outline[i].x, x + 12, size - 24),
                 StrokeOrderLayout::Scale(stroke.outline[i].y, y + 12, size - 24), color, width);
    }
}

static unsigned pixel_checks = 0;
static void CheckPixels(Fixture& f) {
    const auto expected = f.Pixels();
    auto& v = f.view;
    auto* canvas = lv_canvas_create(lv_screen_active());
    auto* buf = lv_draw_buf_create(200, 200, LV_COLOR_FORMAT_RGB565, 0);
    assert(buf);
    lv_canvas_set_draw_buf(canvas, buf);
    auto& theme = f.display.theme;
    lv_canvas_fill_bg(canvas, theme.bg, LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    v.DrawTianGrid(&layer, 0, 0, 200, MixLight(theme.fg, theme.bg));
    for (const auto& s : v.current_glyph_.strokes) {
        LegacyOutline(&layer, s, 0, 0, 200, MixLight(theme.fg, theme.bg), 2);
    }
    for (unsigned i = 0; i < f.controller.completed_stroke_count(); ++i) {
        LegacyOutline(&layer, v.current_glyph_.strokes[i], 0, 0, 200, theme.fg, 3);
    }
    const auto state = f.controller.state();
    if ((state == StrokeOrderUiState::Animating || state == StrokeOrderUiState::Paused) &&
        !f.controller.in_gap()) {
        v.DrawStartMarker(&layer, v.current_glyph_.strokes[f.controller.current_stroke()], 0, 0,
                          200, lv_color_hex(0xFF0000));
    }
    lv_canvas_finish_layer(canvas, &layer);
    assert(expected == std::vector<uint8_t>(buf->data, buf->data + buf->data_size));
    ++pixel_checks;
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}

static void Playback(const std::vector<uint8_t>& blob, uint32_t cp, bool dark) {
    Fixture f(blob, cp);
    if (dark) {
        f.display.theme.bg = lv_color_hex(0x101820);
        f.display.theme.fg = lv_color_hex(0xF4E6CF);
        f.view.RedrawCanvas();
    }
    CheckPixels(f);
    const auto start = now_us;
    const unsigned before = fills, label_before = labels, task_before = tasks;
    unsigned last_frame_tasks = 0;
    const unsigned n = f.controller.stroke_count();
    unsigned ticks = 0, boundaries = 0;
    while (f.controller.state() == StrokeOrderUiState::Animating) {
        const auto old_current = f.controller.current_stroke();
        const auto old_completed = f.controller.completed_stroke_count();
        const auto old_gap = f.controller.in_gap();
        const unsigned old_fills = fills;
        const auto tasks_before_tick = tasks;
        f.Tick();
        last_frame_tasks = tasks - tasks_before_tick;
        const bool boundary = old_current != f.controller.current_stroke() ||
                              old_completed != f.controller.completed_stroke_count() ||
                              old_gap != f.controller.in_gap();
        ++ticks;
        boundaries += boundary;
        // First arm-only turn must still physically submit the cue canvas.
        assert(fills == old_fills + (boundary || ticks == 1 ? 1u : 0u));
        CheckPixels(f);  // also compare skipped holds to independent old raster
        assert(ticks <= 500);
    }
    assert(boundaries == 2 * n - 1);
    assert(ticks == 10 * n - 4);
    assert(fills - before == 2 * n);
    assert(last_frame_tasks == 6 + 2 * n);
    assert(labels == label_before);  // "II" is unchanged including Completed
    assert(now_us - start == uint64_t(ticks) * 33000);
    std::cout << "glyph=" << cp << " dark=" << dark << " ticks=" << ticks
              << " timer_redraws=" << fills - before << " timer_tasks=" << tasks - task_before
              << " final_tasks=" << last_frame_tasks << " completion_us=" << now_us - start << '\n';
}

static void ControlsAndInvalidation(const std::vector<uint8_t>& blob) {
    Fixture f(blob, 0x4EBA);
    const auto initial = f.Pixels();
    unsigned before = fills;
    f.Tick(5000000);  // arm only, even very late
    assert(fills == before + 1 && f.Pixels() == initial);
    f.Tick();
    before = fills;
    f.Control(2);  // Replay same phase/key: must redraw and re-arm
    assert(fills == before + 1 && f.view.anim_clock_.first_frame_pending());
    before = fills;
    f.Tick();
    assert(fills == before + 1 && f.controller.completed_stroke_count() == 0);
    f.Control(0);  // Pause settles one phase only
    assert(f.controller.state() == StrokeOrderUiState::Paused);
    assert(std::strcmp(lv_label_get_text(lv_obj_get_child(f.view.control_buttons_[0], 0)), ">") ==
           0);
    CheckPixels(f);
    before = fills;
    f.Tick();
    assert(fills == before);
    f.Control(1);  // Step from gap -> next cue while paused
    CheckPixels(f);
    f.Control(0);  // Resume
    assert(f.controller.state() == StrokeOrderUiState::Animating);
    // Theme change within a hold must invalidate even without a phase change.
    before = fills;
    f.display.theme.bg = lv_color_hex(0x123456);
    f.Tick(1000);
    assert(fills == before + 1);
    CheckPixels(f);
    before = fills;
    assert(f.view.RenderAnimationPage());  // same glyph/phase, new canvas and controls
    assert(fills == before + 1);
    CheckPixels(f);
    f.Control(3);  // Back destroys canvas, stops timer
    assert(f.view.canvas_ == nullptr && f.view.anim_timer_ == nullptr);
    assert(f.controller.state() == StrokeOrderUiState::Candidates);
    const uint32_t next = 0x53E3;
    assert(f.controller.SetCandidates(&next, 1));
    assert(f.view.RenderCandidates());
    assert(StrokeOrderApplyUiAction(f.coordinator, f.controller, f.view.session_,
                                    f.view.anim_clock_, f.view.presented_generation_,
                                    StrokeOrderUiAction::Candidate, now_us));
    f.view.HandleStatePresentationLocked();
    assert(f.view.current_glyph_.codepoint == next);
    CheckPixels(f);
    before = fills;
    const auto baseline = f.view.anim_clock_.last_tick_us();
    assert(f.coordinator.PublishCancelFence(f.view.presented_generation_));
    f.Tick(5000000);
    assert(fills == before && f.view.anim_clock_.last_tick_us() == baseline);
    assert(!f.view.HandleControlLocked(2));
    assert(fills == before);
    f.view.DestroyOverlay();
    f.view.RedrawCanvas();
    assert(fills == before);  // no cached destroyed canvas access
}

static void ContentionExitAndDelete(const std::vector<uint8_t>& blob) {
    Fixture f(blob, 0x4EBA);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::thread holder([&] {
        assert(f.coordinator.TryUiAction(f.view.presented_generation_,
                                         StrokeRoundCoordinator::UiTransition::None, 0, [&] {
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
    unsigned before = fills;
    const auto baseline = f.view.anim_clock_.last_tick_us();
    f.Tick(5000000);
    assert(fills == before && f.view.anim_clock_.last_tick_us() == baseline);
    assert(f.view.anim_clock_.first_frame_pending());
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_one();
    holder.join();
    f.Tick(5000000);
    assert(fills == before + 1 && f.controller.completed_stroke_count() == 0);
    before = fills;
    f.Control(4);
    assert(f.view.anim_timer_ == nullptr && fills == before);
    lv_obj_delete(f.view.overlay_);  // real LVGL delete callback, no manual reset
    assert(f.view.canvas_ == nullptr && !f.view.overlay_visible_.load());
    f.view.RedrawCanvas();
    assert(fills == before);
}

// Candidate-sized/translated/clipped outlines, repeated points (including
// scaled zero-length edges), maximum 256 points and the defensive fallback.
static void OutlineCases(const std::vector<uint8_t>& blob) {
    Fixture f(blob, 0x4EBA);
    auto cases = f.view.current_glyph_.strokes;
    StrokeOrderView::CachedStroke maximum;
    for (unsigned i = 0; i < 255; ++i) {
        maximum.outline.push_back(
            {static_cast<uint16_t>((i * 37) % 1025), static_cast<uint16_t>((i * 71) % 1025)});
    }
    maximum.outline[1] = maximum.outline[0];
    maximum.outline[2] = {1, 1};  // collapses to the first point at small scale
    maximum.outline.push_back(maximum.outline[0]);
    cases.push_back(maximum);
    maximum.outline.push_back(maximum.outline[0]);
    cases.push_back(maximum);  // out-of-contract cache: bounded fallback only
    cases.push_back({});
    unsigned comparisons = 0;
    auto* canvas = lv_canvas_create(lv_screen_active());
    auto* buf = lv_draw_buf_create(200, 200, LV_COLOR_FORMAT_RGB565, 0);
    assert(buf);
    lv_canvas_set_draw_buf(canvas, buf);
    for (const auto& stroke : cases)
        for (int size : {48, 72, 200})
            for (int origin : {-25, 0, 33})
                for (int width : {2, 3})
                    for (bool dark : {false, true}) {
                        const auto background = dark ? lv_color_black() : lv_color_white();
                        const auto color = dark ? lv_color_hex(0x87B9F1) : lv_color_hex(0x2A4968);
                        std::vector<uint8_t> legacy;
                        for (bool old : {true, false}) {
                            lv_canvas_fill_bg(canvas, background, LV_OPA_COVER);
                            lv_layer_t layer;
                            lv_canvas_init_layer(canvas, &layer);
                            layer._clip_area = {7, 9, 179, 181};
                            if (old)
                                LegacyOutline(&layer, stroke, origin, origin + 4, size, color,
                                              width);
                            else
                                f.view.DrawStrokeOutline(&layer, stroke, origin, origin + 4, size,
                                                         color, width);
                            lv_canvas_finish_layer(canvas, &layer);
                            const std::vector<uint8_t> pixels(buf->data,
                                                              buf->data + buf->data_size);
                            if (old)
                                legacy = pixels;
                            else
                                assert(legacy == pixels);
                        }
                        ++comparisons;
                    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
    std::cout << "outline_pixel_comparisons=" << comparisons << '\n';
}

static void DelayedSequence(const std::vector<uint8_t>& blob) {
    Fixture f(blob, 0x987A);
    constexpr uint64_t intervals[] = {33000, 149000, 150000, 159000, 160000, 161000, 5000000, 1000};
    unsigned frames = 0;
    while (f.controller.state() == StrokeOrderUiState::Animating) {
        const auto phase = 2 * f.controller.current_stroke() + f.controller.in_gap();
        const auto before = fills;
        f.Tick(intervals[frames % 8]);
        const auto after =
            2 * f.controller.current_stroke() +
            (f.controller.in_gap() || f.controller.state() == StrokeOrderUiState::Completed);
        assert(after == phase || after == phase + 1);
        assert(fills == before + (after != phase || frames == 0 ? 1u : 0u));
        CheckPixels(f);
        assert(++frames < 100);
    }
    std::cout << "mixed_delay_ticks=" << frames << '\n';
}

static void TaskBudget(const std::vector<uint8_t>& blob) {
    Fixture f(blob, 0x987A);
    f.controller.Tick(160);  // cue -> gap for the task budget only
    tasks = 0;
    f.view.RedrawCanvas();
    assert(tasks == 6 + f.controller.stroke_count() + 1);  // grid + reference + done
    std::cout << "shun_gap_tasks=" << tasks << '\n';
}

int main(int argc, char** argv) {
    assert(argc == 4);
    lv_init();
    auto* display = lv_display_create(320, 240);
    assert(display);
    const auto smoke = Read(argv[1]), shun = Read(argv[2]);
    for (bool dark : {false, true}) {
        for (uint32_t cp : {0x4E00u, 0x4EBAu, 0x53E3u})
            Playback(smoke, cp, dark);
        Playback(shun, 0x987A, dark);
    }
    const auto real = Read(argv[3]);
    for (bool dark : {false, true}) {
        for (uint32_t cp : {0x4E00u, 0x4E59u, 0x4EBAu, 0x53E3u, 0x987Au, 0x8D62u, 0x56CAu}) {
            Playback(real, cp, dark);
        }
    }
    ControlsAndInvalidation(smoke);
    ContentionExitAndDelete(smoke);
    OutlineCases(smoke);
    DelayedSequence(shun);
    TaskBudget(shun);
    lv_display_delete(display);
    lv_deinit();
    std::cout << "pixel_checks=" << pixel_checks << " stroke_order_render_harness: PASS\n";
}
