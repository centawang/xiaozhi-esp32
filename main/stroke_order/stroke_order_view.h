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
    void RedrawCanvas();
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
