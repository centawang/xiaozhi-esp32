#include "stroke_order/stroke_order_view.h"

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "lvgl_theme.h"
#include "stroke_order/stroke_order_assets.h"
#include "stroke_order/stroke_order_parse.h"
#include "stroke_order/stroke_order_ui_action.h"

#include <esp_log.h>
#include <esp_timer.h>
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#endif

#if defined(HAVE_LVGL)
#include <esp_lvgl_port.h>
#endif

#define TAG "StrokeOrderView"

namespace {

#if defined(HAVE_LVGL)
constexpr int kGridInset = 12;
constexpr int kOutlineWidth = 2;
constexpr int kMarkerRadius = 5;

class AssetsStrokeOrderShardSource final : public StrokeOrderShardSource {
public:
    bool AcquireShard(const char* name, const uint8_t** data, size_t* size) override {
        if (active_ || name == nullptr || data == nullptr || size == nullptr) {
            return false;
        }
        void* mapped = nullptr;
        size_t mapped_size = 0;
        if (!Assets::GetInstance().GetAssetData(name, mapped, mapped_size)) {
            return false;
        }
        active_ = true;
        *data = static_cast<const uint8_t*>(mapped);
        *size = mapped_size;
        return true;
    }

    void ReleaseShard() override { active_ = false; }

private:
    bool active_ = false;
};

AssetsStrokeOrderShardSource kAssetShardSource;

lv_color_t MixLight(lv_color_t color, lv_color_t background) {
    return lv_color_mix(color, background, LV_OPA_30);
}

void DrawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color, int width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void StyleControl(lv_obj_t* obj, LvglTheme* theme) {
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, theme->border_color(), 0);
    lv_obj_set_style_bg_color(obj, theme->assistant_bubble_color(), 0);
    lv_obj_set_style_text_color(obj, theme->text_color(), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    // LVGL owns press/release feedback: no audio, deferred work or extra timer.
    // Keep glyph/text contrast in both themes; the thicker accent border and
    // tinted background are visible on candidate and playback-control buttons.
    lv_obj_set_style_bg_color(
        obj, lv_color_mix(theme->text_color(), theme->assistant_bubble_color(), LV_OPA_30),
        LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, theme->text_color(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_PRESSED);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}
#endif

}  // namespace

StrokeOrderView& StrokeOrderView::GetInstance() {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    // Attach's receiver is evaluated before its Controller argument. Establish
    // reverse static destruction order explicitly: View must unbind first.
    (void)StrokeOrderController::GetInstance();
#endif
    static StrokeOrderView instance;
    return instance;
}

StrokeOrderView::StrokeOrderView() = default;

StrokeOrderView::~StrokeOrderView() {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    Shutdown();
#endif
}

bool StrokeOrderView::IsOverlayActive() const {
    return overlay_visible_.load(std::memory_order_acquire);
}

uint8_t StrokeOrderView::ClassifyDeviceState(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return 0;
        case kDeviceStateConnecting:
            return 1;
        case kDeviceStateListening:
            return 2;
        case kDeviceStateSpeaking:
            return 3;
        default:
            return 4;
    }
}

bool StrokeOrderView::Attach(Display* display, StrokeOrderController* controller,
                             StrokeRoundCoordinator* coordinator) {
    if (display == nullptr || controller == nullptr || coordinator == nullptr) {
        return false;
    }
    display_ = display;
    controller_ = controller;
    coordinator_ = coordinator;
#if !defined(HAVE_LVGL)
    return false;
#else
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
    lifecycle_.Initialize();
    lifecycle_.SetDeviceIdle(Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
    lifecycle_.SetPointerReady(HasPointerIndev());
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    // Application attaches before CheckAssets/Apply. Do not construct Assets
    // (and checksum its partition) or validate a corpus under this LVGL lock.
    // Assets::Apply invokes RebindAssets after successful enclosing admission.
    lifecycle_.SetAssetsReady(false);
    return true;
#else
    return RebindAssetsLocked();
#endif
#endif
}

bool StrokeOrderView::RebindAssets() {
#if !defined(HAVE_LVGL)
    return false;
#else
    if (display_ == nullptr || controller_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
    return RebindAssetsLocked();
#endif
}

bool StrokeOrderView::SuspendAssets() {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    // Close source admission/generation before waiting for any LVGL callback.
    // Atomic owner snapshot is safe against concurrent Shutdown/task teardown.
    auto suspending_worker = std::atomic_load(&prepare_worker_);
    if (suspending_worker)
        suspending_worker->Suspend();
#endif
    const uint64_t generation = coordinator_ != nullptr ? coordinator_->CurrentGeneration() : 0;
    if (generation != 0) {
        Application::GetInstance().RequestAbortStrokeRound(generation,
                                                           StrokeAbortReason::AssetSuspend);
    }
#if !defined(HAVE_LVGL)
    if (controller_ != nullptr) {
        controller_->Unbind();
    }
    return true;
#else
    if (display_ == nullptr || controller_ == nullptr) {
        return true;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        ESP_LOGE(TAG, "cannot suspend stroke assets without the display lock");
        return false;
    }
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    CancelPrepareLocked(true);
#endif
    lifecycle_.SuspendAssets();
    InvalidateVisualsLocked(true);
    SuspendStrokeOrderAssets(controller_, &pinyin_index_);
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    preparing_bundle_ = false;
    prepare_result_ = {};
    return !prepare_worker_ || prepare_worker_->Suspend();
#else
    return true;
#endif
#endif
}

void StrokeOrderView::OnDeviceStateChanged(DeviceState state) {
#if defined(HAVE_LVGL)
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    const uint64_t generation = coordinator_->CurrentGeneration();
    const bool idle = state == kDeviceStateIdle;
    if (generation != 0 &&
        !coordinator_->AllowsDeviceClass(generation, ClassifyDeviceState(state))) {
        lifecycle_.SetDeviceIdle(idle);
        RequestAbortLocked(StrokeAbortReason::UnexpectedState);
        return;
    }
    lifecycle_.SetDeviceIdle(idle);
    lifecycle_.SetListenHold(generation != 0 &&
                             (state == kDeviceStateConnecting || state == kDeviceStateListening));
    if (idle) {
        ReevaluateEntryLocked();
    }
#else
    (void)state;
#endif
}

void StrokeOrderView::OnPowerSave(bool on) {
#if defined(HAVE_LVGL)
    if (on) {
        RequestAbortLocked(StrokeAbortReason::PowerSave);
    }
    if (display_ == nullptr || controller_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    lifecycle_.SetPowerSave(on);
    if (on) {
        InvalidateVisualsLocked(true);
        return;
    }
    lifecycle_.SetDeviceIdle(Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
    lifecycle_.SetPointerReady(HasPointerIndev());
    ReevaluateEntryLocked();
#else
    (void)on;
#endif
}

void StrokeOrderView::OnInterruption() {
    RequestAbortLocked(StrokeAbortReason::Alert);
#if defined(HAVE_LVGL)
    if (display_ == nullptr || controller_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    lifecycle_.Interrupt();
    InvalidateVisualsLocked(true);
#endif
}

void StrokeOrderView::SetVoiceTransportAvailable(bool available) {
#if defined(HAVE_LVGL)
    if (display_ == nullptr || controller_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    lifecycle_.SetVoiceTransportReady(available);
    ReevaluateEntryLocked();
#else
    (void)available;
#endif
}

void StrokeOrderView::Shutdown() {
    const uint64_t generation = coordinator_ != nullptr ? coordinator_->CurrentGeneration() : 0;
    if (generation != 0) {
        Application::GetInstance().RequestAbortStrokeRound(generation,
                                                           StrokeAbortReason::SurfaceDeleted);
    }
#if defined(HAVE_LVGL)
    if (display_ != nullptr) {
        DisplayLockGuard lock(display_);
        if (!lock) {
            return;
        }
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
        prepare_shutdown_ = true;
        CancelPrepareLocked(true);
#endif
        lifecycle_.Shutdown();
        InvalidateVisualsLocked(true);
        if (entry_ != nullptr && lv_obj_is_valid(entry_)) {
            lv_obj_t* obj = entry_;
            entry_ = nullptr;
            lv_obj_delete(obj);
        }
        entry_ = nullptr;
        if (controller_ != nullptr) {
            controller_->Shutdown();
        }
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
        if (prepare_timer_) {
            lv_timer_delete(prepare_timer_);
            prepare_timer_ = nullptr;
        }
        prepare_result_ = {};
        if (prepare_worker_) {
            auto worker =
                std::atomic_exchange(&prepare_worker_, std::shared_ptr<StrokeOrderWorker>{});
            worker->Stop();  // task owns its lifetime until drain, no View callback
        }
#endif
    } else if (controller_ != nullptr) {
        controller_->Shutdown();
    }
#endif
}

bool StrokeOrderView::StartVoiceSessionFromMain(uint64_t generation) {
#if !defined(HAVE_LVGL)
    (void)generation;
    return false;
#else
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr ||
        generation == 0) {
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        !coordinator_->IsCurrentGeneration(generation) || !controller_->is_ready() ||
        !lifecycle_.CanShowEntry() || !lifecycle_.TryOpenOverlay() ||
        !session_.BeginConnecting(generation)) {
        ReevaluateEntryLocked();
        return false;
    }
    presented_generation_ = generation;
    lifecycle_.SetListenHold(true);
    if (!controller_->EnterConnecting() || !EnsureOverlay() || !RenderConnecting()) {
        CancelSessionLocked(false);
        return false;
    }
    return true;
#endif
}

bool StrokeOrderView::ShowListeningFromMain(uint64_t generation) {
#if !defined(HAVE_LVGL)
    (void)generation;
    return false;
#else
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr ||
        generation == 0) {
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
    if (!coordinator_->IsCurrentGeneration(generation) ||
        !session_.MarkListeningReady(generation) || !controller_->EnterAwaitingSpeech() ||
        !EnsureOverlay() || !RenderAwaitingSpeech()) {
        return false;
    }
    presented_generation_ = generation;
    return true;
#endif
}

bool StrokeOrderView::StartLocalCandidateSessionFromMain(uint64_t generation) {
#if !defined(HAVE_LVGL)
    (void)generation;
    return false;
#else
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr ||
        generation == 0) {
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        !coordinator_->IsCurrentGeneration(generation) || !controller_->is_ready() ||
        !lifecycle_.CanShowEntry() || !lifecycle_.TryOpenOverlay() ||
        !session_.BeginLocalCandidates(generation) ||
        !coordinator_->MarkLocalCandidates(generation,
                                           static_cast<uint64_t>(esp_timer_get_time() / 1000)))
        return false;
    presented_generation_ = generation;
    lifecycle_.SetListenHold(false);
    return PrepareCandidatesLocked(generation, 0);
#else
    uint32_t local_candidates[StrokeOrderLayout::kMaxCandidates] = {0x4E00, 0x4EBA, 0x53E3};
    uint32_t local_count = 3;
    if (pinyin_index_.is_bound()) {
        local_count =
            pinyin_index_.AppendTopRanked(local_candidates, StrokeOrderLayout::kMaxCandidates);
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        !coordinator_->IsCurrentGeneration(generation) || !controller_->is_ready() ||
        !lifecycle_.CanShowEntry() || !lifecycle_.TryOpenOverlay() ||
        !session_.BeginLocalCandidates(generation) ||
        !coordinator_->MarkLocalCandidates(generation,
                                           static_cast<uint64_t>(esp_timer_get_time() / 1000)) ||
        local_count == 0 || !controller_->SetCandidates(local_candidates, local_count) ||
        !controller_->OpenCandidates()) {
        ReevaluateEntryLocked();
        return false;
    }
    presented_generation_ = generation;
    lifecycle_.SetListenHold(false);
    if (!EnsureOverlay() || !RenderCandidates()) {
        CancelSessionLocked(false);
        return false;
    }
    return true;
#endif
#endif
}

bool StrokeOrderView::HandleVoiceSttFromMain(uint64_t generation, const std::string& text) {
#if !defined(HAVE_LVGL)
    (void)generation;
    (void)text;
    return false;
#else
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return false;
    }
    return ApplyVoiceUtteranceLocked(generation, text);
#endif
}

void StrokeOrderView::ShowSpeechTimedOutFromMain() {
#if defined(HAVE_LVGL)
    if (display_ == nullptr || controller_ == nullptr) {
        return;
    }
    {
        DisplayLockGuard lock(display_);
        if (!lock) {
            return;
        }
        // AbortRound already retired this generation. Do not create an inert
        // generation-0 R/X page or weaken the active-round admission fence.
        lifecycle_.SetDeviceIdle(true);
        CancelSessionLocked(false);
    }
    // ShowNotification takes its own display lock. Retry is a fresh SO click.
    display_->ShowNotification("未听到汉字，请点击 SO 重试");
#endif
}

void StrokeOrderView::AbortFromMain(uint64_t expected_generation, bool hide_entry) {
#if defined(HAVE_LVGL)
    if (display_ == nullptr || controller_ == nullptr) {
        session_.Cancel();
        overlay_visible_.store(false, std::memory_order_release);
        return;
    }
    DisplayLockGuard lock(display_);
    if (!lock) {
        return;
    }
    if (expected_generation == 0 || session_.inactive() ||
        session_.generation() == expected_generation) {
        CancelSessionLocked(hide_entry);
    }
#else
    (void)expected_generation;
    (void)hide_entry;
    session_.Cancel();
    overlay_visible_.store(false, std::memory_order_release);
#endif
}

#if defined(HAVE_LVGL)

void StrokeOrderView::DisarmClick(lv_obj_t* obj) {
    if (obj != nullptr && lv_obj_is_valid(obj)) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

uint32_t StrokeOrderView::IndexFromUserData(lv_obj_t* obj) {
    if (obj == nullptr) {
        return UINT32_MAX;
    }
    auto* slot = static_cast<uint32_t*>(lv_obj_get_user_data(obj));
    if (slot == nullptr) {
        return UINT32_MAX;
    }
    return *slot;
}

bool StrokeOrderView::HasPointerIndev() const {
    lv_indev_t* indev = nullptr;
    while ((indev = lv_indev_get_next(indev)) != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            return true;
        }
    }
    return false;
}

bool StrokeOrderView::RebindAssetsLocked() {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    CancelPrepareLocked(true);
#endif
    lifecycle_.SuspendAssets();
    InvalidateVisualsLocked(true);
    SuspendStrokeOrderAssets(controller_, &pinyin_index_);

#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    if (!StartPrepareWorkerLocked())
        return false;
    prepare_worker_->Suspend();
    // Apply can replace Attach while its validation is still running. Retain
    // one pinned latest input, then let the timer retry NONBLOCKING drain only.
    pending_bundle_ = Assets::GetInstance().LeaseStrokeBundle(&assets_generation_);
    bundle_drain_polls_ = 0;
    preparing_bundle_ = pending_bundle_ != nullptr;
    // Readiness is not published until the fenced timer consumes Commit+BindSource.
    return preparing_bundle_;
#else
    void* catalog_ptr = nullptr;
    size_t catalog_size = 0;
    void* pinyin_ptr = nullptr;
    size_t pinyin_size = 0;
    const bool found =
        Assets::GetInstance().GetAssetData("stroke_cat.bin", catalog_ptr, catalog_size) &&
        Assets::GetInstance().GetAssetData("stroke_pinyin.bin", pinyin_ptr, pinyin_size);
    const bool bound =
        found && RebindStrokeOrderAssets(controller_, &pinyin_index_,
                                         static_cast<const uint8_t*>(catalog_ptr), catalog_size,
                                         &kAssetShardSource,
                                         static_cast<const uint8_t*>(pinyin_ptr), pinyin_size);
    if (bound) {
        ESP_LOGI(TAG,
                 "stroke metadata ready catalog=%u pinyin=%u bytes; all shards validated with "
                 "transient views",
                 static_cast<unsigned>(catalog_size), static_cast<unsigned>(pinyin_size));
    }
    const bool has_pointer = HasPointerIndev();
    lifecycle_.SetAssetsReady(bound);
    lifecycle_.SetPointerReady(has_pointer);
    lifecycle_.SetDeviceIdle(Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
    lifecycle_.RebuildSurface();
    ReevaluateEntryLocked();
    if (!bound) {
        ESP_LOGW(TAG, "stroke catalog/shard/pinyin set missing or invalid; entry hidden");
    } else if (!has_pointer) {
        ESP_LOGW(TAG, "no LVGL pointer indev; 笔划 entry hidden");
    }
    return bound && has_pointer;
#endif
}

#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
bool StrokeOrderView::StartPrepareWorkerLocked() {
    if (prepare_shutdown_)
        return false;
    if (prepare_worker_)
        return prepare_timer_ != nullptr;
    auto worker = std::shared_ptr<StrokeOrderWorker>(new (std::nothrow) StrokeOrderWorker);
    if (!worker)
        return false;
    // The task owns ONLY the mailbox/source, never this View or an LVGL object.
    // Stop is asynchronous; the final reference dies on the worker after drain.
    auto* task_owner = new (std::nothrow) std::shared_ptr<StrokeOrderWorker>(worker);
    if (!task_owner)
        return false;
    if (xTaskCreate(
            [](void* arg) {
                auto* owner = static_cast<std::shared_ptr<StrokeOrderWorker>*>(arg);
                auto state = std::move(*owner);
                delete owner;
                while (!state->stopped()) {
                    state->RunOne();
                    // Fixed wait; no unbounded callback/event queue. Idle priority
                    // allows watchdog/idle and audio to run during full validation.
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                state->Suspend();
                state.reset();
                vTaskDelete(nullptr);
            },
            "stroke_prepare", 16384, task_owner, tskIDLE_PRIORITY, nullptr) != pdPASS) {
        delete task_owner;
        return false;
    }
    std::atomic_store(&prepare_worker_, std::move(worker));
    prepare_timer_ = lv_timer_create(PrepareTimerCb, 33, this);
    if (!prepare_timer_) {
        auto stopped = std::atomic_exchange(&prepare_worker_, std::shared_ptr<StrokeOrderWorker>{});
        stopped->Stop();
        return false;
    }
    return true;
}

void StrokeOrderView::CancelPrepareLocked(bool cancel_bundle) {
    // Corpus readiness belongs to the asset generation, not a UI/voice round.
    // Power-save/visual cancellation must not strand an in-flight initial bind.
    if (preparing_bundle_ && !cancel_bundle)
        return;
    pending_bundle_.reset();
    prepare_result_ = {};
    if (!prepare_worker_)
        return;
    if (preparing_bundle_) {
        ESP_LOGI(TAG, "bundle stage=cancel ok=0 reason=explicit_asset_or_shutdown_cancel");
        controller_->Unbind();
        prepare_worker_->Suspend();
        preparing_bundle_ = false;
    } else {
        prepare_worker_->CancelRound();
    }
}

bool StrokeOrderView::PrepareCandidatesLocked(uint64_t generation, uint32_t primary) {
    uint32_t cps[6] = {};
    const uint32_t count = controller_->PlanSourceCandidates(primary, cps, 6);
    prepare_result_ = {};
    if (!count) {
        session_.MarkNoMatch();
        coordinator_->MarkNoMatch(generation);
        return controller_->EnterNoMatch() && EnsureOverlay() && RenderNoMatch();
    }
    if (coordinator_->CurrentPhase() == StrokeRoundCoordinator::Phase::ProcessingStt) {
        // Start the existing 60s timeout only once a metadata plan exists.
        if (!coordinator_->MarkCandidates(generation,
                                          static_cast<uint64_t>(esp_timer_get_time() / 1000)))
            return false;
        session_.MarkCandidates();
    }
    if (!prepare_worker_ || !controller_->EnterConnecting() || !EnsureOverlay() ||
        !RenderStatusPage("Loading", false) ||
        !prepare_worker_->SubmitGlyphs(assets_generation_, generation, cps, count)) {
        CancelSessionLocked(false);
        return false;
    }
    return true;
}

void StrokeOrderView::PrepareTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<StrokeOrderView*>(lv_timer_get_user_data(timer));
    if (!self || self->prepare_timer_ != timer || !self->prepare_worker_)
        return;
    if (self->pending_bundle_) {
        if (self->assets_generation_ != Assets::GetInstance().StrokeAssetsGeneration() ||
            ++self->bundle_drain_polls_ > 300) {
            ESP_LOGI(TAG, "bundle stage=drain ok=0 reason=assets_changed_or_timeout");
            self->CancelPrepareLocked(true);
            return;
        }
        if (!self->prepare_worker_->Suspend())
            return;
        auto owner = std::move(self->pending_bundle_);
        self->preparing_bundle_ =
            self->prepare_worker_->SubmitBundle(std::move(owner), self->assets_generation_);
        if (!self->preparing_bundle_)
            ESP_LOGI(TAG, "bundle stage=submit ok=0 reason=worker_rejected");
        return;
    }
    auto& r = self->prepare_result_;
    if (r.kind == StrokeOrderWorker::Kind::None && !self->prepare_worker_->Take(&r))
        return;
    if (r.assets != self->assets_generation_ ||
        r.assets != Assets::GetInstance().StrokeAssetsGeneration()) {
        if (r.kind == StrokeOrderWorker::Kind::Bundle)
            ESP_LOGI(TAG, "bundle stage=completion ok=0 reason=asset_generation_fence");
        self->controller_->Unbind();
        self->prepare_worker_->Suspend();
        self->preparing_bundle_ = false;
        self->lifecycle_.SetAssetsReady(false);
        self->ReevaluateEntryLocked();
        r = {};
        return;
    }
    if (r.kind == StrokeOrderWorker::Kind::Bundle) {
        const char* reason = "ready";
        bool bound = false;
        if (!self->preparing_bundle_)
            reason = "preparation_cancelled";
        else if (!r.ok)
            reason = "prepare_failed";
        else if (!self->prepare_worker_->source().Commit(r.prepared))
            reason = "commit_fence";
        else if (!self->controller_->BindSource(&self->prepare_worker_->source()))
            reason = "controller_bind_failed";
        else
            bound = true;
        self->preparing_bundle_ = false;
        r = {};
        if (!bound) {
            self->controller_->Unbind();
            self->prepare_worker_->Suspend();
        }
        self->lifecycle_.SetAssetsReady(bound);
        self->lifecycle_.SetPointerReady(self->HasPointerIndev());
        self->lifecycle_.SetDeviceIdle(Application::GetInstance().GetDeviceState() ==
                                       kDeviceStateIdle);
        self->lifecycle_.RebuildSurface();
        self->ReevaluateEntryLocked();
        ESP_LOGI(TAG, "bundle stage=publish ok=%d reason=%s ready=%d entry_eligible=%d",
                 bound, reason, self->controller_->is_ready(),
                 self->lifecycle_.CanShowEntry());
        return;
    }
    if (!self->session_.IsCurrentGeneration(r.round) || self->presented_generation_ != r.round ||
        !self->coordinator_->IsCurrentGeneration(r.round) ||
        self->coordinator_->HasCancelFence(r.round) ||
        r.source != self->prepare_worker_->source().generation()) {
        r = {};
        return;
    }
    // Match the existing cancel-fence linearization. A benign try-lock miss
    // retains exactly one completion for the next timer turn, never a queue.
    bool candidates = false;
    const bool accepted = self->coordinator_->TryUiAction(
        r.round, StrokeRoundCoordinator::UiTransition::None, 0, [&]() {
            candidates = r.ok && self->controller_->SetCandidates(r.cps, r.count) &&
                         self->controller_->OpenCandidates();
            if (!candidates) {
                self->session_.MarkError();
                self->controller_->EnterError();
            }
            return true;
        });
    if (!accepted)
        return;
    r = {};
    if (!(candidates ? self->RenderCandidates() : self->RenderStatusPage("Retry", true)))
        self->RequestAbortLocked(StrokeAbortReason::UnexpectedState);
}
#endif

bool StrokeOrderView::ShowEntryLocked() {
    if (display_ == nullptr || controller_ == nullptr || coordinator_ == nullptr ||
        coordinator_->IsRoundActive() || !controller_->is_ready() || !lifecycle_.CanShowEntry()) {
        return false;
    }
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!StrokeOrderLayout::EntryRect(&x, &y, &w, &h)) {
        return false;
    }
    if (entry_ != nullptr && lv_obj_is_valid(entry_)) {
        controller_->SetEntryHitRect(x, y, w, h);
        lv_obj_add_flag(entry_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(entry_, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return false;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return false;
    }
    entry_ = lv_obj_create(screen);
    lv_obj_set_pos(entry_, x, y);
    lv_obj_set_size(entry_, w, h);
    StyleControl(entry_, theme);
    lv_obj_t* label = lv_label_create(entry_);
    lv_label_set_text(label, "SO");
    lv_obj_center(label);
    lv_obj_add_event_cb(entry_, EntryClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(entry_, EntryDeleted, LV_EVENT_DELETE, this);
    controller_->SetEntryHitRect(x, y, w, h);
    return true;
}

void StrokeOrderView::HideEntryLocked() {
    if (controller_ != nullptr) {
        controller_->ClearEntryHitRect();
    }
    if (entry_ != nullptr && lv_obj_is_valid(entry_)) {
        lv_obj_add_flag(entry_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(entry_, LV_OBJ_FLAG_CLICKABLE);
    }
}

void StrokeOrderView::ReevaluateEntryLocked() {
    if (coordinator_ != nullptr && !coordinator_->IsRoundActive() && lifecycle_.CanShowEntry()) {
        ShowEntryLocked();
    } else {
        HideEntryLocked();
    }
}

void StrokeOrderView::CancelSessionLocked(bool hide_entry) {
    session_.Cancel();
    presented_generation_ = 0;
    InvalidateVisualsLocked(hide_entry);
}

void StrokeOrderView::InvalidateVisualsLocked(bool hide_entry) {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    CancelPrepareLocked();
#endif
    lifecycle_.SetListenHold(false);
    if (controller_ != nullptr) {
        controller_->Exit();
    }
    lifecycle_.CloseOverlay();
    DestroyOverlay();
    if (hide_entry) {
        HideEntryLocked();
    } else {
        ReevaluateEntryLocked();
    }
}

void StrokeOrderView::RequestAbortLocked(StrokeAbortReason reason) {
    const uint64_t generation = coordinator_ != nullptr ? coordinator_->CurrentGeneration() : 0;
    Application::GetInstance().RequestAbortStrokeRound(generation, reason);
}

void StrokeOrderView::StopAnimTimer(bool reset_clock) {
    lv_timer_t* timer = anim_timer_;
    anim_timer_ = nullptr;
    if (reset_clock) {
        anim_clock_.Reset();
    }
    if (timer != nullptr) {
        lv_timer_delete(timer);
    }
}

bool StrokeOrderView::SyncAnimTimer() {
    if (controller_ == nullptr) {
        return false;
    }
    const auto state = controller_->state();
    anim_clock_.Sync(state, static_cast<uint64_t>(esp_timer_get_time()));
    if (state != StrokeOrderUiState::Animating) {
        if (anim_timer_ != nullptr) {
            lv_timer_pause(anim_timer_);
        }
        return true;
    }
    if (anim_timer_ == nullptr) {
        anim_timer_ = lv_timer_create(AnimTimerCb, StrokeOrderController::kTimerPeriodMs, this);
        if (anim_timer_ == nullptr) {
            return false;
        }
    }
    lv_timer_set_period(anim_timer_, StrokeOrderController::kTimerPeriodMs);
    lv_timer_resume(anim_timer_);
    return true;
}

bool StrokeOrderView::EnsureOverlay() {
    if (display_ == nullptr || controller_ == nullptr) {
        return false;
    }
    if (overlay_ != nullptr && lv_obj_is_valid(overlay_)) {
        lv_obj_move_foreground(overlay_);
        HideEntryLocked();
        overlay_visible_.store(true, std::memory_order_release);
        return true;
    }
    overlay_ = nullptr;
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return false;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return false;
    }
    overlay_ = lv_obj_create(screen);
    lv_obj_set_size(overlay_, StrokeOrderLayout::kScreenWidth, StrokeOrderLayout::kScreenHeight);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay_, OverlayDeleted, LV_EVENT_DELETE, this);
    lv_obj_move_foreground(overlay_);
    HideEntryLocked();
    overlay_visible_.store(overlay_ != nullptr, std::memory_order_release);
    return overlay_ != nullptr;
}

void StrokeOrderView::DestroyOverlay() {
    overlay_visible_.store(false, std::memory_order_release);
    showing_candidates_ = false;
    candidate_glyphs_.clear();
    current_glyph_ = CachedGlyph{};
    StopAnimTimer();
    canvas_ = nullptr;
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        control_buttons_[i] = nullptr;
    }
    lv_obj_t* obj = overlay_;
    overlay_ = nullptr;
    if (obj != nullptr && lv_obj_is_valid(obj)) {
        deleting_overlay_ = true;
        lv_obj_delete(obj);
        deleting_overlay_ = false;
    }
    if (canvas_buf_ != nullptr) {
        lv_draw_buf_destroy(canvas_buf_);
        canvas_buf_ = nullptr;
    }
}

bool StrokeOrderView::CacheCandidateGlyph(uint32_t index, CachedGlyph* out) {
    if (controller_ == nullptr || out == nullptr) {
        return false;
    }
    uint32_t codepoint = 0;
    std::vector<StrokeOrderController::DecodedStroke> decoded;
    if (!controller_->CopyCandidateGlyph(index, &codepoint, &decoded) || decoded.empty()) {
        return false;
    }
    CachedGlyph glyph;
    glyph.codepoint = codepoint;
    glyph.strokes.resize(decoded.size());
    for (size_t s = 0; s < decoded.size(); ++s) {
        glyph.strokes[s].outline.resize(decoded[s].outline.size());
        glyph.strokes[s].median.resize(decoded[s].median.size());
        for (size_t p = 0; p < decoded[s].outline.size(); ++p) {
            glyph.strokes[s].outline[p] =
                CachedPoint{decoded[s].outline[p].x, decoded[s].outline[p].y};
        }
        for (size_t p = 0; p < decoded[s].median.size(); ++p) {
            glyph.strokes[s].median[p] =
                CachedPoint{decoded[s].median[p].x, decoded[s].median[p].y};
        }
    }
    *out = std::move(glyph);
    return true;
}

bool StrokeOrderView::CacheLoadedGlyph() {
    if (controller_ == nullptr) {
        return false;
    }
    std::vector<StrokeOrderController::DecodedStroke> decoded;
    if (!controller_->CopyLoadedGlyph(&decoded) || decoded.empty()) {
        current_glyph_ = CachedGlyph{};
        return false;
    }
    current_glyph_.codepoint = controller_->loaded_codepoint();
    current_glyph_.strokes.clear();
    current_glyph_.strokes.resize(decoded.size());
    for (size_t s = 0; s < decoded.size(); ++s) {
        current_glyph_.strokes[s].outline.resize(decoded[s].outline.size());
        current_glyph_.strokes[s].median.resize(decoded[s].median.size());
        for (size_t p = 0; p < decoded[s].outline.size(); ++p) {
            current_glyph_.strokes[s].outline[p] =
                CachedPoint{decoded[s].outline[p].x, decoded[s].outline[p].y};
        }
        for (size_t p = 0; p < decoded[s].median.size(); ++p) {
            current_glyph_.strokes[s].median[p] =
                CachedPoint{decoded[s].median[p].x, decoded[s].median[p].y};
        }
    }
    return true;
}

bool StrokeOrderView::RenderCandidates() {
    if (overlay_ == nullptr || controller_ == nullptr || display_ == nullptr) {
        return false;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return false;
    }
    StopAnimTimer();
    candidate_glyphs_.clear();
    current_glyph_ = CachedGlyph{};
    lv_obj_clean(overlay_);
    canvas_ = nullptr;
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        control_buttons_[i] = nullptr;
    }
    lv_obj_t* title = lv_label_create(overlay_);
    lv_label_set_text(title, "Stroke");
    lv_obj_set_pos(title, StrokeOrderLayout::kPad, 14);
    lv_obj_set_style_text_color(title, theme->text_color(), 0);

    control_ids_[4] = 4;
    lv_obj_t* close = lv_obj_create(overlay_);
    lv_obj_set_pos(close, StrokeOrderLayout::kScreenWidth - StrokeOrderLayout::kMinTouchTarget - 4,
                   4);
    lv_obj_set_size(close, StrokeOrderLayout::kMinTouchTarget, StrokeOrderLayout::kMinTouchTarget);
    StyleControl(close, theme);
    lv_obj_set_user_data(close, &control_ids_[4]);
    lv_obj_add_event_cb(close, ControlClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* close_label = lv_label_create(close);
    lv_label_set_text(close_label, "X");
    lv_obj_center(close_label);
    control_buttons_[4] = close;

    const uint32_t count = controller_->candidate_count();
    candidate_glyphs_.reserve(StrokeOrderLayout::kMaxCandidates);
    for (uint32_t i = 0; i < count && candidate_glyphs_.size() < StrokeOrderLayout::kMaxCandidates;
         ++i) {
        CachedGlyph glyph;
        if (!CacheCandidateGlyph(i, &glyph)) {
            continue;
        }
        const uint32_t display = static_cast<uint32_t>(candidate_glyphs_.size());
        candidate_ids_[display] = display;
        candidate_select_ids_[display] = i;
        candidate_glyphs_.push_back(std::move(glyph));
    }
    for (uint32_t i = 0; i < candidate_glyphs_.size(); ++i) {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (!StrokeOrderLayout::CandidateCell(i, &x, &y, &w, &h)) {
            continue;
        }
        lv_obj_t* button = lv_obj_create(overlay_);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, w, h);
        StyleControl(button, theme);
        lv_obj_set_user_data(button, &candidate_ids_[i]);
        lv_obj_add_event_cb(button, CandidateClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(button, CandidateDraw, LV_EVENT_DRAW_MAIN, this);
    }
    showing_candidates_ = !candidate_glyphs_.empty();
    return showing_candidates_;
}

bool StrokeOrderView::RenderAnimationPage() {
    if (overlay_ == nullptr || display_ == nullptr) {
        return false;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return false;
    }
    if (!CacheLoadedGlyph()) {
        return false;
    }
    // Candidate/RetryLoad admission already established the fresh playback
    // token. Rebuilding this page must not erase it (teardown still resets).
    StopAnimTimer(false);
    lv_obj_clean(overlay_);
    showing_candidates_ = false;
    canvas_ = nullptr;
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        control_buttons_[i] = nullptr;
    }
    int tx = 0;
    int ty = 0;
    int tw = 0;
    int th = 0;
    if (!StrokeOrderLayout::TianRect(&tx, &ty, &tw, &th)) {
        return false;
    }
    if (canvas_buf_ == nullptr) {
        canvas_buf_ = lv_draw_buf_create(tw, th, LV_COLOR_FORMAT_RGB565, 0);
        if (canvas_buf_ == nullptr) {
            ESP_LOGE(TAG, "stroke canvas buffer alloc failed");
            return false;
        }
    }
    canvas_ = lv_canvas_create(overlay_);
    lv_obj_set_pos(canvas_, tx, ty);
    lv_canvas_set_draw_buf(canvas_, canvas_buf_);
    static const char* kLabels[StrokeOrderLayout::kControlCount] = {"II", "+1", "R", "<", "X"};
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (!StrokeOrderLayout::ControlCell(i, &x, &y, &w, &h)) {
            return false;
        }
        control_ids_[i] = i;
        lv_obj_t* button = lv_obj_create(overlay_);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, w, h);
        StyleControl(button, theme);
        lv_obj_set_user_data(button, &control_ids_[i]);
        lv_obj_add_event_cb(button, ControlClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, kLabels[i]);
        lv_obj_center(label);
        control_buttons_[i] = button;
    }
    // Draw cue0 before starting the timer. Even if its first callback is late,
    // the shared admission clock permits only another cue0 draw on that turn.
    RedrawCanvas();
    if (!SyncAnimTimer()) {
        return false;
    }
    UpdateControlLabels();
    (void)th;
    return true;
}

bool StrokeOrderView::RenderErrorPage() {
    if (overlay_ == nullptr || display_ == nullptr) {
        return false;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    StopAnimTimer();
    lv_obj_clean(overlay_);
    showing_candidates_ = false;
    canvas_ = nullptr;
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        control_buttons_[i] = nullptr;
    }
    lv_obj_t* label = lv_label_create(overlay_);
    lv_label_set_text(label, "Retry / Back");
    if (theme != nullptr) {
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
    }
    lv_obj_center(label);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (StrokeOrderLayout::ControlCell(2, &x, &y, &w, &h)) {
        control_ids_[2] = 2;
        lv_obj_t* retry = lv_obj_create(overlay_);
        lv_obj_set_pos(retry, x, y);
        lv_obj_set_size(retry, w, h);
        if (theme != nullptr) {
            StyleControl(retry, theme);
        }
        lv_obj_set_user_data(retry, &control_ids_[2]);
        lv_obj_add_event_cb(retry, ControlClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* retry_label = lv_label_create(retry);
        lv_label_set_text(retry_label, "R");
        lv_obj_center(retry_label);
    }
    if (StrokeOrderLayout::ControlCell(3, &x, &y, &w, &h)) {
        control_ids_[3] = 3;
        lv_obj_t* back = lv_obj_create(overlay_);
        lv_obj_set_pos(back, x, y);
        lv_obj_set_size(back, w, h);
        if (theme != nullptr) {
            StyleControl(back, theme);
        }
        lv_obj_set_user_data(back, &control_ids_[3]);
        lv_obj_add_event_cb(back, ControlClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* back_label = lv_label_create(back);
        lv_label_set_text(back_label, "<");
        lv_obj_center(back_label);
    }
    return true;
}

bool StrokeOrderView::RenderStatusPage(const char* title, bool show_retry) {
    if (overlay_ == nullptr || display_ == nullptr || title == nullptr) {
        return false;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    StopAnimTimer();
    lv_obj_clean(overlay_);
    showing_candidates_ = false;
    canvas_ = nullptr;
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        control_buttons_[i] = nullptr;
    }
    lv_obj_t* label = lv_label_create(overlay_);
    lv_label_set_text(label, title);
    if (theme != nullptr) {
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
    }
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, StrokeOrderLayout::kPad);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (show_retry && StrokeOrderLayout::ControlCell(2, &x, &y, &w, &h)) {
        control_ids_[2] = 2;
        lv_obj_t* retry = lv_obj_create(overlay_);
        lv_obj_set_pos(retry, x, y);
        lv_obj_set_size(retry, w, h);
        if (theme != nullptr) {
            StyleControl(retry, theme);
        }
        lv_obj_set_user_data(retry, &control_ids_[2]);
        lv_obj_add_event_cb(retry, ControlClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* retry_label = lv_label_create(retry);
        lv_label_set_text(retry_label, "R");
        lv_obj_center(retry_label);
    }
    if (StrokeOrderLayout::ControlCell(4, &x, &y, &w, &h)) {
        control_ids_[4] = 4;
        lv_obj_t* close = lv_obj_create(overlay_);
        lv_obj_set_pos(close, x, y);
        lv_obj_set_size(close, w, h);
        if (theme != nullptr) {
            StyleControl(close, theme);
        }
        lv_obj_set_user_data(close, &control_ids_[4]);
        lv_obj_add_event_cb(close, ControlClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* close_label = lv_label_create(close);
        lv_label_set_text(close_label, "X");
        lv_obj_center(close_label);
    }
    return true;
}

bool StrokeOrderView::RenderConnecting() { return RenderStatusPage("Connect", false); }

bool StrokeOrderView::RenderAwaitingSpeech() { return RenderStatusPage("Speak", false); }

bool StrokeOrderView::RenderNoMatch() { return RenderStatusPage("No match", true); }

bool StrokeOrderView::ApplyVoiceUtteranceLocked(uint64_t generation, const std::string& text) {
    if (!session_.AcceptStt(generation) || controller_ == nullptr || coordinator_ == nullptr ||
        !coordinator_->IsCurrentGeneration(generation)) {
        return false;
    }
    presented_generation_ = generation;
    const auto parsed = StrokeOrderParse::Parse(text.data(), text.size());
    if (parsed.status != StrokeOrderParse::Status::Ok) {
        if (parsed.status == StrokeOrderParse::Status::Ambiguous ||
            parsed.status == StrokeOrderParse::Status::NoTarget) {
            session_.MarkNoMatch();
            coordinator_->MarkNoMatch(generation);
            if (!controller_->EnterNoMatch() || !EnsureOverlay() || !RenderNoMatch()) {
                CancelSessionLocked(false);
                return false;
            }
        } else {
            session_.MarkError();
            coordinator_->MarkError(generation);
            if (!controller_->EnterError() || !EnsureOverlay() ||
                !RenderStatusPage("Retry", true)) {
                CancelSessionLocked(false);
                return false;
            }
        }
        return true;
    }
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    return PrepareCandidatesLocked(generation, parsed.codepoint);
#else
    const StrokeOrderCandidateProvider* provider = &pinyin_provider_;
    static const StrokeOrderNullHomophoneProvider kNoHomophones;
    if (!pinyin_index_.is_bound()) {
        provider = &kNoHomophones;
    }
    if (!controller_->SetCandidatesFromPrimary(parsed.codepoint, provider) ||
        !controller_->OpenCandidates()) {
        session_.MarkNoMatch();
        coordinator_->MarkNoMatch(generation);
        if (!controller_->EnterNoMatch() || !EnsureOverlay() || !RenderNoMatch()) {
            CancelSessionLocked(false);
            return false;
        }
        return true;
    }
    session_.MarkCandidates();
    coordinator_->MarkCandidates(generation, static_cast<uint64_t>(esp_timer_get_time() / 1000));
    if (!EnsureOverlay() || !RenderCandidates()) {
        CancelSessionLocked(false);
        return false;
    }
    return true;
#endif
}

void StrokeOrderView::DrawTianGrid(lv_layer_t* layer, int x, int y, int size, lv_color_t color) {
    DrawLine(layer, x, y, x + size - 1, y, color, 1);
    DrawLine(layer, x, y + size - 1, x + size - 1, y + size - 1, color, 1);
    DrawLine(layer, x, y, x, y + size - 1, color, 1);
    DrawLine(layer, x + size - 1, y, x + size - 1, y + size - 1, color, 1);
    const int mid_x = x + size / 2;
    const int mid_y = y + size / 2;
    DrawLine(layer, mid_x, y, mid_x, y + size - 1, color, 1);
    DrawLine(layer, x, mid_y, x + size - 1, mid_y, color, 1);
}

void StrokeOrderView::DrawGlyphOutlines(lv_layer_t* layer, const CachedGlyph& glyph, int x, int y,
                                        int size, lv_color_t color, int width) {
    for (const auto& stroke : glyph.strokes) {
        DrawStrokeOutline(layer, stroke, x, y, size, color, width);
    }
}

void StrokeOrderView::DrawStrokeOutline(lv_layer_t* layer, const CachedStroke& stroke, int x, int y,
                                        int size, lv_color_t color, int width) {
    if (stroke.outline.size() < 2) {
        return;
    }
    const int inner = size - (2 * kGridInset);
    const int origin_x = x + kGridInset;
    const int origin_y = y + kGridInset;
    for (size_t i = 1; i < stroke.outline.size(); ++i) {
        DrawLine(layer, StrokeOrderLayout::Scale(stroke.outline[i - 1].x, origin_x, inner),
                 StrokeOrderLayout::Scale(stroke.outline[i - 1].y, origin_y, inner),
                 StrokeOrderLayout::Scale(stroke.outline[i].x, origin_x, inner),
                 StrokeOrderLayout::Scale(stroke.outline[i].y, origin_y, inner), color, width);
    }
}

void StrokeOrderView::DrawStartMarker(lv_layer_t* layer, const CachedStroke& stroke, int x, int y,
                                      int size, lv_color_t color) {
    if (stroke.median.empty()) {
        return;
    }
    const int inner = size - (2 * kGridInset);
    const int origin_x = x + kGridInset;
    const int origin_y = y + kGridInset;
    const int cx = StrokeOrderLayout::Scale(stroke.median[0].x, origin_x, inner);
    const int cy = StrokeOrderLayout::Scale(stroke.median[0].y, origin_y, inner);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_width = 0;
    lv_area_t area;
    area.x1 = cx - kMarkerRadius;
    area.y1 = cy - kMarkerRadius;
    area.x2 = cx + kMarkerRadius;
    area.y2 = cy + kMarkerRadius;
    lv_draw_rect(layer, &dsc, &area);
}

void StrokeOrderView::RedrawCanvas() {
    if (canvas_ == nullptr || !lv_obj_is_valid(canvas_) || controller_ == nullptr ||
        display_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return;
    }
    const int tw = StrokeOrderLayout::kTianSize;
    lv_canvas_fill_bg(canvas_, theme->background_color(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    DrawTianGrid(&layer, 0, 0, tw, MixLight(theme->text_color(), theme->background_color()));
    const uint16_t strokes = static_cast<uint16_t>(current_glyph_.strokes.size());
    if (strokes == 0) {
        lv_canvas_finish_layer(canvas_, &layer);
        return;
    }
    const uint16_t current = controller_->current_stroke();
    const uint16_t completed = controller_->completed_stroke_count();
    const auto state = controller_->state();
    const lv_color_t reference = MixLight(theme->text_color(), theme->background_color());
    const lv_color_t done = theme->text_color();
    const lv_color_t start_cue = lv_color_hex(0xFF0000);
    for (uint16_t i = 0; i < strokes; ++i) {
        DrawStrokeOutline(&layer, current_glyph_.strokes[i], 0, 0, tw, reference, kOutlineWidth);
    }
    uint16_t done_count = completed;
    if (state == StrokeOrderUiState::Animating || state == StrokeOrderUiState::Paused) {
        if (!controller_->in_gap()) {
            done_count = current;
        }
    }
    if (done_count > strokes) {
        done_count = strokes;
    }
    for (uint16_t i = 0; i < done_count; ++i) {
        DrawStrokeOutline(&layer, current_glyph_.strokes[i], 0, 0, tw, done, kOutlineWidth + 1);
    }
    if ((state == StrokeOrderUiState::Animating || state == StrokeOrderUiState::Paused) &&
        current < strokes && !controller_->in_gap()) {
        // The cue is the only active-stroke drawing; the full contour snaps
        // into the completed pass after its hold. Never draw a partial median.
        DrawStartMarker(&layer, current_glyph_.strokes[current], 0, 0, tw, start_cue);
    }
    lv_canvas_finish_layer(canvas_, &layer);
}

void StrokeOrderView::UpdateControlLabels() {
    if (controller_ == nullptr || control_buttons_[0] == nullptr ||
        !lv_obj_is_valid(control_buttons_[0])) {
        return;
    }
    lv_obj_t* label = lv_obj_get_child(control_buttons_[0], 0);
    if (label == nullptr) {
        return;
    }
    if (controller_->state() == StrokeOrderUiState::Paused) {
        lv_label_set_text(label, ">");
    } else {
        lv_label_set_text(label, "II");
    }
}

bool StrokeOrderView::HandleControlLocked(uint32_t index) {
    if (controller_ == nullptr || coordinator_ == nullptr || !lifecycle_.CanHandleOverlayAction() ||
        index >= StrokeOrderLayout::kControlCount) {
        return false;
    }
    if (index != 4 && Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    static constexpr StrokeOrderUiAction actions[] = {
        StrokeOrderUiAction::PauseContinue, StrokeOrderUiAction::Step, StrokeOrderUiAction::Replay,
        StrokeOrderUiAction::Back, StrokeOrderUiAction::Exit};
    auto action = actions[index];
    const auto voice = session_.phase();
    if (index == 2 && controller_->state() == StrokeOrderUiState::Error &&
        (voice == StrokeOrderVoicePhase::Candidates ||
         voice == StrokeOrderVoicePhase::LocalPlayback)) {
        action = StrokeOrderUiAction::RetryLoad;
    } else if (index == 2 && (voice == StrokeOrderVoicePhase::NoMatch ||
                              voice == StrokeOrderVoicePhase::TimedOut ||
                              voice == StrokeOrderVoicePhase::Error)) {
        action = StrokeOrderUiAction::RetryVoice;
    }
    const uint64_t generation = presented_generation_;
    if (!StrokeOrderApplyUiAction(*coordinator_, *controller_, session_, anim_clock_, generation,
                                  action, static_cast<uint64_t>(esp_timer_get_time()))) {
        return false;
    }
    // Rendering may call out to Application on failure; do not hold coordinator
    // mutex across it. A fence-first rejected action never reaches these effects.
    // Replay uses the existing canvas: its admitted BeginPlayback token survives
    // ordinary SyncAnimTimer, and this callback redraws cue0 before returning.
    if (action == StrokeOrderUiAction::Exit) {
        StopAnimTimer();
    } else if (action != StrokeOrderUiAction::RetryVoice) {
        HandleStatePresentationLocked();
        RedrawCanvas();
        UpdateControlLabels();
    }
    if (action == StrokeOrderUiAction::Exit) {
        Application::GetInstance().RequestAbortStrokeRound(generation,
                                                           StrokeAbortReason::UserClose);
    } else if (action == StrokeOrderUiAction::RetryVoice) {
        Application::GetInstance().RequestStartStrokeRound(generation);
    }
    return true;
}

void StrokeOrderView::HandleStatePresentationLocked() {
    if (controller_ == nullptr) {
        return;
    }
    const auto state = controller_->state();
    if (state == StrokeOrderUiState::Hidden) {
        lifecycle_.CloseOverlay();
        DestroyOverlay();
        ReevaluateEntryLocked();
        return;
    }
    if (!lifecycle_.CanHandleOverlayAction() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        RequestAbortLocked(StrokeAbortReason::UnexpectedState);
        return;
    }
    if (state == StrokeOrderUiState::Candidates) {
        if (!showing_candidates_ && (!EnsureOverlay() || !RenderCandidates())) {
            RequestAbortLocked(StrokeAbortReason::UnexpectedState);
        }
        return;
    }
    if (state == StrokeOrderUiState::Error) {
        if (!EnsureOverlay() || !RenderErrorPage()) {
            RequestAbortLocked(StrokeAbortReason::UnexpectedState);
        }
        return;
    }
    if (canvas_ == nullptr || !lv_obj_is_valid(canvas_) || current_glyph_.strokes.empty()) {
        if (!EnsureOverlay() || !RenderAnimationPage()) {
            RequestAbortLocked(StrokeAbortReason::UnexpectedState);
        }
        return;
    }
    if (!SyncAnimTimer()) {
        RequestAbortLocked(StrokeAbortReason::UnexpectedState);
    }
}

void StrokeOrderView::EntryClicked(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    if (self == nullptr || self->controller_ == nullptr) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
        !self->lifecycle_.CanShowEntry()) {
        self->HideEntryLocked();
        return;
    }
    DisarmClick(lv_event_get_target_obj(event));
    self->HideEntryLocked();
    Application::GetInstance().RequestStartStrokeRound();
}

void StrokeOrderView::CandidateClicked(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (self == nullptr || self->controller_ == nullptr || target == nullptr ||
        !self->lifecycle_.CanHandleOverlayAction() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    if (!self->session_.IsCurrentGeneration(self->presented_generation_) ||
        self->coordinator_ == nullptr ||
        self->session_.phase() != StrokeOrderVoicePhase::Candidates ||
        self->controller_->state() != StrokeOrderUiState::Candidates) {
        return;
    }
    const uint32_t display = IndexFromUserData(target);
    if (display >= self->candidate_glyphs_.size()) {
        return;
    }
    const uint32_t select_index = self->candidate_select_ids_[display];
    const uint64_t generation = self->presented_generation_;
    if (StrokeOrderApplyUiAction(*self->coordinator_, *self->controller_, self->session_,
                                 self->anim_clock_, generation, StrokeOrderUiAction::Candidate,
                                 static_cast<uint64_t>(esp_timer_get_time()), select_index)) {
        // A transient coordinator try-lock miss must leave the button usable.
        // Disarm only after admission, before rendering can delete this target.
        DisarmClick(target);
        self->HandleStatePresentationLocked();
    }
}

void StrokeOrderView::ControlClicked(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (self == nullptr || self->controller_ == nullptr || target == nullptr ||
        !self->lifecycle_.CanHandleOverlayAction()) {
        return;
    }
    const uint32_t index = IndexFromUserData(target);
    if (index >= StrokeOrderLayout::kControlCount) {
        return;
    }
    if (self->HandleControlLocked(index) && index == 4) {
        // Exit stops animation but leaves deletion to the main-task abort.
        // Benign lock contention must not permanently disable Exit.
        DisarmClick(target);
    }
}

void StrokeOrderView::OverlayDeleted(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (self == nullptr) {
        return;
    }
    self->StopAnimTimer();
    self->overlay_visible_.store(false, std::memory_order_release);
    if (self->overlay_ == target) {
        self->overlay_ = nullptr;
    }
    self->canvas_ = nullptr;
    self->showing_candidates_ = false;
    self->candidate_glyphs_.clear();
    self->current_glyph_ = CachedGlyph{};
    if (!self->deleting_overlay_) {
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
        self->CancelPrepareLocked();
#endif
        self->lifecycle_.NotifyExternalDelete();
        self->lifecycle_.SetListenHold(false);
        if (self->controller_ != nullptr) {
            self->controller_->Exit();
            self->controller_->ClearEntryHitRect();
        }
        self->RequestAbortLocked(StrokeAbortReason::SurfaceDeleted);
    }
    for (uint32_t i = 0; i < StrokeOrderLayout::kControlCount; ++i) {
        self->control_buttons_[i] = nullptr;
    }
}

void StrokeOrderView::EntryDeleted(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    if (self == nullptr) {
        return;
    }
    self->entry_ = nullptr;
#if CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500
    self->CancelPrepareLocked();
#endif
    self->lifecycle_.NotifyExternalDelete();
    self->lifecycle_.SetListenHold(false);
    self->StopAnimTimer();
    if (self->overlay_ != nullptr && lv_obj_is_valid(self->overlay_)) {
        lv_obj_add_flag(self->overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(self->overlay_, LV_OBJ_FLAG_CLICKABLE);
    }
    if (self->controller_ != nullptr) {
        self->controller_->Exit();
        self->controller_->ClearEntryHitRect();
    }
    self->RequestAbortLocked(StrokeAbortReason::SurfaceDeleted);
}

void StrokeOrderView::CandidateDraw(lv_event_t* event) {
    auto* self = static_cast<StrokeOrderView*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target_obj(event);
    if (self == nullptr || target == nullptr) {
        return;
    }
    const uint32_t index = IndexFromUserData(target);
    if (index >= self->candidate_glyphs_.size()) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(event);
    if (layer == nullptr || self->display_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(self->display_->GetTheme());
    if (theme == nullptr) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);
    const int w = coords.x2 - coords.x1 + 1;
    const int h = coords.y2 - coords.y1 + 1;
    const int size = w < h ? w : h;
    const int x = coords.x1 + (w - size) / 2;
    const int y = coords.y1 + (h - size) / 2;
    self->DrawGlyphOutlines(layer, self->candidate_glyphs_[index], x, y, size, theme->text_color(),
                            kOutlineWidth);
}

void StrokeOrderView::AnimTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<StrokeOrderView*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->controller_ == nullptr || self->anim_timer_ != timer) {
        return;
    }
    if (self->overlay_ == nullptr || !lv_obj_is_valid(self->overlay_)) {
        return;
    }
    const auto state = self->controller_->state();
    if (state != StrokeOrderUiState::Animating) {
        lv_timer_pause(timer);
        return;
    }
    if (self->coordinator_ == nullptr ||
        !StrokeOrderApplyAnimationTick(*self->coordinator_, *self->controller_, self->session_,
                                       self->anim_clock_, self->presented_generation_,
                                       static_cast<uint64_t>(esp_timer_get_time()))) {
        return;
    }
    self->RedrawCanvas();
    self->UpdateControlLabels();
    if (self->controller_->state() != StrokeOrderUiState::Animating) {
        lv_timer_pause(timer);
    }
}

#endif  // HAVE_LVGL
