#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_session.h"
#include "stroke_order/stroke_order_worker.h"
#include "stroke_order/stroke_round_coordinator.h"

#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

namespace {
struct Owner final : StrokeOrderBundleOwner {
    std::vector<uint8_t> bytes[8];
    explicit Owner(const std::string& path) {
        const char* names[] = {"stroke_cat.bin", "stroke_pinyin.bin", "so00.bin", "so01.bin",
                               "so02.bin",       "so03.bin",          "so04.bin", "so05.bin"};
        for (size_t i = 0; i < 8; ++i) {
            std::ifstream file(path + "/" + names[i], std::ios::binary);
            assert(file.good());
            bytes[i] = std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
        }
    }
    Blob Catalog() const override { return {bytes[0].data(), bytes[0].size()}; }
    Blob Pinyin() const override { return {bytes[1].data(), bytes[1].size()}; }
    size_t ShardCount() const override { return 6; }
    Blob Shard(size_t i) const override { return {bytes[i + 2].data(), bytes[i + 2].size()}; }
};
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    static void Wait(void* arg) {
        auto& self = *static_cast<Gate*>(arg);
        std::unique_lock<std::mutex> lock(self.mutex);
        self.entered = true;
        self.cv.notify_all();
        self.cv.wait(lock, [&] { return self.release; });
    }
    void Entered() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    }
};
void Bind(StrokeOrderWorker& worker, StrokeOrderController& controller,
          const std::shared_ptr<Owner>& owner, uint64_t generation) {
    assert(worker.SubmitBundle(owner, generation));
    assert(worker.RunOne());
    StrokeOrderWorker::Result r;
    assert(worker.Take(&r) && r.ok && r.assets == generation);
    assert(worker.source().Commit(r.prepared));
    assert(!worker.source().Commit(r.prepared));
    assert(controller.BindSource(&worker.source()));
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    auto owner = std::make_shared<Owner>(argv[1]);
    StrokeOrderWorker worker;
    StrokeOrderController controller;
    // Delayed bundle: pending slot cannot accumulate, Suspend vetoes unmap.
    Gate bundle_gate;
    worker.before_prepare = Gate::Wait;
    worker.before_prepare_context = &bundle_gate;
    assert(worker.SubmitBundle(owner, 1));
    assert(!worker.SubmitBundle(owner, 1));
    std::thread first([&] { assert(worker.RunOne()); });
    bundle_gate.Entered();
    assert(!controller.is_ready());
    assert(!worker.Suspend());
    assert(!worker.SubmitBundle(owner, 2));
    bundle_gate.Release();
    first.join();
    worker.before_prepare = nullptr;
    assert(worker.Suspend());
    StrokeOrderWorker::Result result;
    assert(!worker.Take(&result));
    Bind(worker, controller, owner, 2);
    std::cout << "PASS delayed bundle replacement suspend\n";

    uint32_t cps[6] = {};
    uint32_t count = controller.PlanSourceCandidates(0x6566, cps, 6);  // 敦: real polyphony
    assert(count > 1 && count <= 6 && cps[0] == 0x6566);
    assert(!controller.SetCandidates(cps, count));  // metadata never decompresses
    assert(worker.SubmitGlyphs(2, 1, cps, count));
    assert(worker.RunOne() && worker.Take(&result) && result.ok);
    assert(controller.SetCandidates(result.cps, result.count));
    assert(controller.OpenCandidates());
    assert(controller.SelectCandidate(0));
    assert(controller.loaded_codepoint() == 0x6566 && controller.stroke_count() > 0);
    std::vector<StrokeOrderController::DecodedStroke> copy;
    assert(controller.CopyLoadedGlyph(&copy) && !copy.empty());
    std::cout << "PASS real3500 metadata candidates select Load\n";

    // Delayed glyph, latest of 100 requests wins. No worker or completion queue.
    Gate glyph_gate;
    worker.before_prepare = Gate::Wait;
    worker.before_prepare_context = &glyph_gate;
    assert(worker.SubmitGlyphs(2, 2, cps, count));
    std::thread glyph([&] { assert(worker.RunOne()); });
    glyph_gate.Entered();
    worker.CancelRound();
    count = controller.PlanSourceCandidates(0, cps, 6);
    assert(count == 6);
    for (uint64_t round = 3; round <= 102; ++round)
        assert(worker.SubmitGlyphs(2, round, cps, count));
    glyph_gate.Release();
    glyph.join();
    worker.before_prepare = nullptr;
    assert(!worker.Take(&result));
    assert(worker.RunOne() && worker.Take(&result) && result.ok && result.round == 102);
    assert(!worker.RunOne() && !worker.Take(&result));
    worker.CancelRound();
    assert(!worker.Take(&result));
    std::cout << "PASS delayed glyph cancel single-slot coalesce100 late duplicate\n";

    // Exact round/cancel fence: a late UI result cannot mutate Controller.
    StrokeRoundCoordinator rounds;
    const auto round = rounds.BeginRound(10);
    assert(rounds.MarkLocalCandidates(round, 10));
    assert(rounds.PublishCancelFence(round));
    bool mutated = false;
    assert(!rounds.TryUiAction(round, StrokeRoundCoordinator::UiTransition::None, 0, [&] {
        mutated = true;
        return true;
    }));
    assert(!mutated);
    auto next = rounds.BeginRound(20);
    assert(next != round);
    assert(!rounds.TryUiAction(round, StrokeRoundCoordinator::UiTransition::None, 0, [&] {
        mutated = true;
        return true;
    }));

    // Active raw borrow keeps owner alive; false means caller must NOT unmap.
    StrokeOrderSource::Borrow borrow;
    assert(worker.source().Acquire(worker.source().generation(), cps[0], &borrow));
    controller.Unbind();
    std::weak_ptr<Owner> lease = owner;
    owner.reset();
    bool unmapped = false;
    if (worker.Suspend())
        unmapped = true;
    assert(!unmapped && !lease.expired() && borrow.strokes);
    assert(worker.source().Release(borrow));
    assert(!worker.source().Release(borrow));
    if (worker.Suspend())
        unmapped = true;
    assert(unmapped && lease.expired());
    std::cout << "PASS active borrow unmap only after true owner release\n";

    owner = std::make_shared<Owner>(argv[1]);
    Bind(worker, controller, owner, 3);
    for (uint64_t i = 1; i <= 100; ++i) {
        count = controller.PlanSourceCandidates(0x4E00, cps, 6);
        assert(count && worker.SubmitGlyphs(3, i, cps, count));
        assert(worker.RunOne() && worker.Take(&result) && result.ok);
        assert(controller.SetCandidates(result.cps, result.count));
        assert(controller.OpenCandidates() && controller.SelectCandidate(0));
        assert(controller.BackToCandidates() && controller.Exit());
        worker.CancelRound();
        assert(!worker.RunOne() && !worker.Take(&result));
    }
    // Suspend after a glyph is running, then stop: no retained callback/owner.
    Gate stop_gate;
    worker.before_prepare = Gate::Wait;
    worker.before_prepare_context = &stop_gate;
    assert(worker.SubmitGlyphs(3, 101, cps, count));
    std::thread last([&] { worker.RunOne(); });
    stop_gate.Entered();
    controller.Unbind();
    assert(!worker.Suspend());
    worker.Stop();
    stop_gate.Release();
    last.join();
    assert(worker.stopped() && worker.Suspend() && !worker.Take(&result));
    assert(!worker.SubmitBundle(owner, 4));
    std::cout << "PASS sessions100 bounded stop delayed glyph\nPASS W7b1 worker\n";
}
