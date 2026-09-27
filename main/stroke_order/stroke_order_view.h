#pragma once

#include "device_state.h"
#include "display.h"
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_pinyin.h"
#include "stroke_order/stroke_order_session.h"
#include "stroke_order/stroke_order_ui_action.h"
#include "stroke_order/stroke_round_coordinator.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
#include "stroke_order/stroke_order_worker.h"
#endif

#if defined(HAVE_LVGL)
#include <lvgl.h>
#endif

/**
 * LVGL overlay and 笔划 entry for CoreS3. Board files must not copy this UI.
 * Coordinates 0..1024 y-down are scaled only here.
 *
 * External lifecycle methods take DisplayLockGuard once. LVGL click/timer/delete
 * callbacks execute directly on the LVGL task and never schedule a second task.
 * Lifecycle checks, controller changes, and view changes are therefore one
 * serialized operation. Helpers require that same display/LVGL lock.
 */
class StrokeOrderView {
public:
    static StrokeOrderView& GetInstance();

    StrokeOrderView();
    ~StrokeOrderView();
    StrokeOrderView(const StrokeOrderView&) = delete;
    StrokeOrderView& operator=(const StrokeOrderView&) = delete;

    bool Attach(Display* display, StrokeOrderController* controller,
                StrokeRoundCoordinator* coordinator);
    bool RebindAssets();
    bool SuspendAssets();
    void OnDeviceStateChanged(DeviceState state);
    void OnPowerSave(bool on);
    void OnInterruption();
    void SetVoiceTransportAvailable(bool available);
    void Shutdown();

    // Network-thread safe; no LVGL access.
    bool IsOverlayActive() const;

    // Main-task only. Each takes the display lock and updates lifecycle/controller/view atomically.
    bool StartVoiceSessionFromMain(uint64_t generation);
    bool ShowListeningFromMain(uint64_t generation);
    bool StartLocalCandidateSessionFromMain(uint64_t generation);
    bool HandleVoiceSttFromMain(uint64_t generation, const std::string& text);
    void ShowSpeechTimedOutFromMain();
    void AbortFromMain(uint64_t expected_generation, bool hide_entry = false);

private:
#if defined(HAVE_LVGL)
    struct CachedPoint {
        uint16_t x = 0;
        uint16_t y = 0;
    };
    struct CachedStroke {
        std::vector<CachedPoint> outline;
        std::vector<CachedPoint> median;
    };
    struct CachedGlyph {
        uint32_t codepoint = 0;
        std::vector<CachedStroke> strokes;
    };

    static void EntryClicked(lv_event_t* event);
    static void CandidateClicked(lv_event_t* event);
    static void ControlClicked(lv_event_t* event);
    static void OverlayDeleted(lv_event_t* event);
    static void EntryDeleted(lv_event_t* event);
    static void CandidateDraw(lv_event_t* event);
    static void AnimTimerCb(lv_timer_t* timer);
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    static void PrepareTimerCb(lv_timer_t* timer);
    bool StartPrepareWorkerLocked();
    bool PrepareCandidatesLocked(uint64_t generation, uint32_t primary);
    void CancelPrepareLocked(bool cancel_bundle = false);
    std::shared_ptr<StrokeOrderWorker> prepare_worker_;
    lv_timer_t* prepare_timer_ = nullptr;
    StrokeOrderWorker::Result prepare_result_;
    uint64_t assets_generation_ = 0;
    std::shared_ptr<const StrokeOrderBundleOwner> pending_bundle_;
    uint16_t bundle_drain_polls_ = 0;
    bool preparing_bundle_ = false;
    bool prepare_shutdown_ = false;  // Shutdown is terminal; no overlapping replacement task.
#endif

    static void DisarmClick(lv_obj_t* obj);
    static uint32_t IndexFromUserData(lv_obj_t* obj);

    // Requires the display lock, or the LVGL task's implicit lock.
    bool HasPointerIndev() const;
    bool RebindAssetsLocked();
    bool ShowEntryLocked();
    void HideEntryLocked();
    void ReevaluateEntryLocked();
    void CancelSessionLocked(bool hide_entry);
    void InvalidateVisualsLocked(bool hide_entry);
    void RequestAbortLocked(StrokeAbortReason reason);
    bool EnsureOverlay();
    void DestroyOverlay();
    void StopAnimTimer(bool reset_clock = true);
    bool SyncAnimTimer();
    static uint8_t ClassifyDeviceState(DeviceState state);
    bool RenderCandidates();
    bool RenderAnimationPage();
    bool RenderErrorPage();
    bool RenderConnecting();
    bool RenderAwaitingSpeech();
    bool RenderNoMatch();
    bool RenderStatusPage(const char* title, bool show_retry);
    bool ApplyVoiceUtteranceLocked(uint64_t generation, const std::string& text);
    bool CacheLoadedGlyph();
    bool CacheCandidateGlyph(uint32_t index, CachedGlyph* out);
    void DrawTianGrid(lv_layer_t* layer, int x, int y, int size, lv_color_t color);
    void DrawGlyphOutlines(lv_layer_t* layer, const CachedGlyph& glyph, int x, int y, int size,
                           lv_color_t color, int width);
    void DrawStrokeOutline(lv_layer_t* layer, const CachedStroke& stroke, int x, int y, int size,
                           lv_color_t color, int width);
    void DrawStartMarker(lv_layer_t* layer, const CachedStroke& stroke, int x, int y, int size,
                         lv_color_t color);
    // Page rebuilds and admitted controls force presentation, even at the same
    // phase. Only ordinary timer holds may reuse the pixels already on canvas.
    void RedrawCanvas(bool force = true);
    void UpdateControlLabels();
    bool HandleControlLocked(uint32_t index);
    void HandleStatePresentationLocked();

    Display* display_ = nullptr;
    StrokeOrderController* controller_ = nullptr;
    lv_obj_t* entry_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* control_buttons_[StrokeOrderLayout::kControlCount] = {};
    uint32_t candidate_ids_[StrokeOrderLayout::kMaxCandidates] = {};
    uint32_t candidate_select_ids_[StrokeOrderLayout::kMaxCandidates] = {};
    uint32_t control_ids_[StrokeOrderLayout::kControlCount] = {};
    lv_draw_buf_t* canvas_buf_ = nullptr;
    struct CanvasVisualState {
        uint16_t current;
        uint16_t completed;
        bool gap;
        StrokeOrderUiState state;
        uint32_t background;
        uint32_t foreground;
        bool operator==(const CanvasVisualState& other) const {
            return current == other.current && completed == other.completed && gap == other.gap &&
                   state == other.state && background == other.background &&
                   foreground == other.foreground;
        }
    };
    CanvasVisualState canvas_visual_{};
    bool canvas_rendered_ = false;
    // LVGL copies polyline points at submission. One bounded, display-lock-owned
    // scratch array avoids per-outline C++ allocations and extra task-stack use.
    lv_point_precise_t outline_points_[StrokeOrderStore::kMaxOutlinePointsPerStroke];
    lv_timer_t* anim_timer_ = nullptr;
    StrokeOrderAnimationClock anim_clock_;
    std::vector<CachedGlyph> candidate_glyphs_;
    CachedGlyph current_glyph_;
    StrokeOrderLifecycle lifecycle_;
    StrokeOrderSession session_;
    StrokeOrderPinyinIndex pinyin_index_;
    StrokeOrderPinyinProvider pinyin_provider_{&pinyin_index_};
    StrokeRoundCoordinator* coordinator_ = nullptr;
    std::atomic<bool> overlay_visible_{false};
    uint64_t presented_generation_ = 0;
    bool deleting_overlay_ = false;
    bool showing_candidates_ = false;
#else
    Display* display_ = nullptr;
    StrokeOrderController* controller_ = nullptr;
    StrokeRoundCoordinator* coordinator_ = nullptr;
    StrokeOrderSession session_;
    std::atomic<bool> overlay_visible_{false};
#endif
};
