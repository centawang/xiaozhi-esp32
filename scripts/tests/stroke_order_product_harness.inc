// Prefix for extracted production View preparation methods; no full LVGL claim.
#include <cassert>
#include <cstdio>
#define ESP_LOGI(tag, ...) do { std::printf(__VA_ARGS__); std::puts(""); } while (0)
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_session.h"
#include "stroke_order/stroke_order_worker.h"
#include "stroke_order/stroke_round_coordinator.h"

struct lv_timer_t {
    void* user_data;
};
void* lv_timer_get_user_data(lv_timer_t* timer) { return timer->user_data; }
int64_t esp_timer_get_time() { return 100000; }
constexpr int kDeviceStateIdle = 0;
struct Application {
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    int GetDeviceState() { return kDeviceStateIdle; }
};
struct Assets {
    uint64_t generation = 7;
    static Assets& GetInstance() {
        static Assets assets;
        return assets;
    }
    uint64_t StrokeAssetsGeneration() const { return generation; }
};
struct StrokeOrderView {
    StrokeOrderController controller;
    StrokeRoundCoordinator coordinator;
    StrokeOrderController* controller_ = &controller;
    StrokeRoundCoordinator* coordinator_ = &coordinator;
    StrokeOrderSession session_;
    StrokeOrderLifecycle lifecycle_;
    std::shared_ptr<StrokeOrderWorker> prepare_worker_{new StrokeOrderWorker};
    lv_timer_t timer{this};
    lv_timer_t* prepare_timer_ = &timer;
    StrokeOrderWorker::Result prepare_result_;
    uint64_t assets_generation_ = 7;
    uint64_t presented_generation_ = 0;
    std::shared_ptr<const StrokeOrderBundleOwner> pending_bundle_;
    uint16_t bundle_drain_polls_ = 0;
    bool preparing_bundle_ = false;
    int rendered = 0, entry_updates = 0, loading = 0;
    bool entry_visible = false;
    void DestroyOverlay() {}
    void HideEntryLocked() { entry_visible = false; }
    bool HasPointerIndev() const { return true; }
    bool EnsureOverlay() { return true; }
    bool RenderNoMatch() { return true; }
    bool RenderCandidates() {
        ++rendered;
        return true;
    }
    bool RenderStatusPage(const char* text, bool) {
        if (std::string(text) == "Loading")
            ++loading;
        return true;
    }
    void ReevaluateEntryLocked() {
        ++entry_updates;
        entry_visible = controller.is_ready() && lifecycle_.CanShowEntry() &&
                        !coordinator.IsRoundActive();
    }
    void CancelSessionLocked(bool);
    void InvalidateVisualsLocked(bool);
    void RequestAbortLocked(StrokeAbortReason) { coordinator.PublishCurrentCancelFence(); }
    void CancelPrepareLocked(bool cancel_bundle = false);
    bool PrepareCandidatesLocked(uint64_t generation, uint32_t primary);
    static void PrepareTimerCb(lv_timer_t* timer);
};

struct Owner final : StrokeOrderBundleOwner {
    std::vector<uint8_t> files[8];
    explicit Owner(const std::string& root) {
        const char* names[] = {"stroke_cat.bin", "stroke_pinyin.bin", "so00.bin", "so01.bin",
                               "so02.bin",       "so03.bin",          "so04.bin", "so05.bin"};
        for (int i = 0; i < 8; ++i) {
            std::ifstream file(root + "/" + names[i], std::ios::binary);
            assert(file.good());
            files[i] = std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
        }
    }
    Blob Catalog() const override { return {files[0].data(), files[0].size()}; }
    Blob Pinyin() const override { return {files[1].data(), files[1].size()}; }
    size_t ShardCount() const override { return 6; }
    Blob Shard(size_t i) const override { return {files[i + 2].data(), files[i + 2].size()}; }
};

// PRODUCTION_METHODS_INSERTED_HERE

int main(int argc, char** argv) {
    assert(argc == 2);
    auto owner = std::make_shared<Owner>(argv[1]);
    StrokeOrderView view;
    view.lifecycle_.Initialize();
    // Replay the observed 69s sleep before 93s bundle completion. This executes
    // production visual invalidation, worker completion and View publication.
    view.pending_bundle_ = owner;
    view.preparing_bundle_ = true;
    StrokeOrderView::PrepareTimerCb(&view.timer);
    view.prepare_worker_->before_prepare_context = &view;
    view.prepare_worker_->before_prepare = [](void* context) {
        auto& current = *static_cast<StrokeOrderView*>(context);
        current.lifecycle_.SetPowerSave(true);
        current.InvalidateVisualsLocked(true);
        assert(!current.entry_visible);
    };
    assert(view.prepare_worker_->RunOne());
    view.prepare_worker_->before_prepare = nullptr;
    StrokeOrderView::PrepareTimerCb(&view.timer);
    assert(view.controller.is_ready() && !view.entry_visible);
    view.lifecycle_.SetPowerSave(false);
    view.ReevaluateEntryLocked();
    assert(view.entry_visible && view.lifecycle_.CanShowEntry());
    view.controller.Unbind();
    assert(view.prepare_worker_->Suspend());
    view.entry_updates = 0;
    auto poll = [&] { StrokeOrderView::PrepareTimerCb(&view.timer); };
    auto bundle = [&] {
        view.coordinator.AbortRound(view.coordinator.CurrentGeneration());
        view.pending_bundle_ = owner;
        view.preparing_bundle_ = true;
        view.bundle_drain_polls_ = 0;
        poll();
        assert(!view.controller.is_ready());
        assert(view.prepare_worker_->RunOne());
        poll();
        assert(view.controller.is_ready());
        assert(view.lifecycle_.assets_ready() && view.entry_visible);
    };
    bundle();
    assert(view.entry_updates == 1);
    auto round = [&] {
        view.session_.Cancel();
        auto generation = view.coordinator.BeginRound(100);
        assert(view.coordinator.MarkLocalCandidates(generation, 100));
        assert(view.session_.BeginLocalCandidates(generation));
        view.presented_generation_ = generation;
        assert(view.PrepareCandidatesLocked(generation, 0x6566));
        assert(view.controller.state() == StrokeOrderUiState::Connecting);
        assert(view.prepare_worker_->RunOne());
        return generation;
    };
    round();
    assert(view.loading == 1 && view.rendered == 0);
    poll();
    assert(view.rendered == 1 && view.controller.state() == StrokeOrderUiState::Candidates);
    assert(view.controller.SelectCandidate(0));
    assert(view.controller.loaded_codepoint() == 0x6566);
    poll();  // duplicate callback cannot re-render or reset playback
    assert(view.rendered == 1 && view.controller.state() == StrokeOrderUiState::Animating);
    assert(view.controller.BackToCandidates());
    auto generation = round();
    assert(view.coordinator.PublishCancelFence(generation));
    poll();
    assert(view.rendered == 1);  // cancel before callback
    generation = round();
    view.coordinator.AbortRound(generation);  // timeout/new normal round retirement
    poll();
    assert(view.rendered == 1);
    round();
    view.session_.Cancel();  // deleted surface/new View session
    poll();
    assert(view.rendered == 1);
    round();
    ++Assets::GetInstance().generation;  // asset replacement before callback
    poll();
    assert(view.rendered == 1 && !view.controller.is_ready());
    assert(view.prepare_worker_->Suspend());
    view.assets_generation_ = Assets::GetInstance().generation;
    bundle();
    assert(view.entry_updates == 3);
    round();
    view.CancelPrepareLocked();  // pending completion invalidated by View cancellation
    poll();
    assert(view.rendered == 1);
    // Failed structural admission, stale asset generation and stale Commit
    // tickets must never publish readiness/entry capability.
    for (int fault = 0; fault < 3; ++fault) {
        StrokeOrderView rejected;
        rejected.lifecycle_.Initialize();
        rejected.assets_generation_ = Assets::GetInstance().generation;
        auto input = std::make_shared<Owner>(argv[1]);
        if (fault == 0)
            input->files[0][0] ^= 1;
        rejected.pending_bundle_ = input;
        rejected.preparing_bundle_ = true;
        StrokeOrderView::PrepareTimerCb(&rejected.timer);
        assert(rejected.prepare_worker_->RunOne());
        if (fault == 1)
            ++Assets::GetInstance().generation;
        if (fault == 2)
            assert(rejected.prepare_worker_->source().Suspend());
        StrokeOrderView::PrepareTimerCb(&rejected.timer);
        assert(!rejected.controller.is_ready() && !rejected.entry_visible &&
               !rejected.lifecycle_.CanShowEntry());
        rejected.prepare_worker_->Stop();
        assert(rejected.prepare_worker_->Suspend());
    }
    view.controller.Unbind();
    view.prepare_worker_->Stop();
    assert(view.prepare_worker_->Suspend());
}
