#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_layout.h"
#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_store.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<uint8_t> ReadBinary(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

bool MedianDelta(const StrokeOrderStore::StrokeView& stroke, int32_t* dx, int32_t* dy) {
    StrokeOrderStore::Point first;
    StrokeOrderStore::Point last;
    if (!stroke.GetMedianPoint(0, &first) ||
        !stroke.GetMedianPoint(static_cast<uint16_t>(stroke.median_count() - 1), &last)) {
        return false;
    }
    *dx = static_cast<int32_t>(last.x) - static_cast<int32_t>(first.x);
    *dy = static_cast<int32_t>(last.y) - static_cast<int32_t>(first.y);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: stroke_order_controller_harness <smoke.bin>\n";
        return 2;
    }
    const auto blob = ReadBinary(argv[1]);
    Expect(!blob.empty(), "read smoke blob");

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    Expect(StrokeOrderLayout::CandidateCell(0, &x, &y, &w, &h) && w >= 44 && h >= 44,
           "candidate cell 0 touch target");
    Expect(StrokeOrderLayout::CandidateCell(5, &x, &y, &w, &h) && w >= 44 && h >= 44,
           "candidate cell 5 touch target");
    Expect(!StrokeOrderLayout::CandidateCell(6, &x, &y, &w, &h), "candidate cell 6 rejected");
    Expect(StrokeOrderLayout::ControlCell(0, &x, &y, &w, &h) && w >= 44 && h >= 44,
           "control 0 touch target");
    Expect(StrokeOrderLayout::ControlCell(4, &x, &y, &w, &h) && w >= 44 && h >= 44,
           "control 4 touch target");
    Expect(StrokeOrderLayout::EntryRect(&x, &y, &w, &h) && w >= 44 && h >= 44,
           "entry touch target");
    Expect(StrokeOrderLayout::TianRect(&x, &y, &w, &h) && w == 200 && h == 200, "tian 200x200");

    StrokeOrderController controller;
    auto mmap_blob = blob;
    Expect(controller.BindStore(mmap_blob.data(), mmap_blob.size()), "bind smoke store");
    Expect(controller.is_ready(), "controller ready");
    mmap_blob.assign(mmap_blob.size(), 0);
    mmap_blob.clear();
    mmap_blob.shrink_to_fit();
    Expect(controller.is_ready() && controller.candidate_count() == 0,
           "controller owns blob after source mmap disappears and does not list whole library");
    Expect(!controller.OpenCandidates(), "open candidates without SetCandidates rejected");
    const uint32_t missing = 0x5929;  // 天, not in smoke store
    Expect(!controller.SetCandidates(&missing, 1), "unsupported codepoint rejected");
    const uint32_t duplicates[] = {0x4E00, 0x4E00, 0x4EBA, 0x53E3, 0x4E00, 0x5929, 0x4EBA, 0x53E3};
    Expect(controller.SetCandidates(duplicates, 8), "set filtered candidates");
    Expect(controller.candidate_count() == 3,
           "dedup, store filter, and cap keep three smoke chars");
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t third = 0;
    Expect(controller.GetCandidate(0, &first) && first == 0x4E00, "candidate 一");
    Expect(controller.GetCandidate(1, &second) && second == 0x4EBA, "candidate 人");
    Expect(controller.GetCandidate(2, &third) && third == 0x53E3, "candidate 口");
    Expect(!controller.GetCandidate(3, &first), "candidate overflow rejected");
    std::vector<StrokeOrderController::DecodedStroke> candidate_copy;
    uint32_t candidate_codepoint = 0;
    Expect(controller.CopyCandidateGlyph(1, &candidate_codepoint, &candidate_copy) &&
               candidate_codepoint == 0x4EBA && candidate_copy.size() == 2,
           "candidate glyph copied from controller-owned blob");

    Expect(!controller.IsExclusiveTouch(), "hidden is not exclusive");
    controller.SetEntryHitRect(8, 188, 52, 44);
    Expect(controller.ShouldConsumePointer(10, 190), "entry hit consumes");
    Expect(!controller.ShouldConsumePointer(200, 100), "empty hit does not consume");

    Expect(controller.OpenCandidates(), "open candidates");
    Expect(controller.state() == StrokeOrderUiState::Candidates, "candidates state");
    Expect(controller.IsExclusiveTouch(), "overlay exclusive");
    Expect(controller.ShouldConsumePointer(200, 100), "overlay consumes empty taps");
    Expect(!controller.SelectCandidate(9), "invalid candidate index");
    Expect(controller.state() == StrokeOrderUiState::Candidates, "invalid select keeps candidates");

    Expect(controller.SelectCandidate(0), "select 一");
    Expect(controller.state() == StrokeOrderUiState::Animating, "animating after select");
    Expect(controller.loaded_codepoint() == 0x4E00, "loaded 一");
    Expect(controller.stroke_count() == 1, "一 has one stroke");

    std::vector<StrokeOrderController::DecodedStroke> copied;
    Expect(controller.CopyLoadedGlyph(&copied) && copied.size() == 1, "copy 一 glyph");
    Expect(copied[0].median.size() >= 2, "copied 一 median");

    StrokeOrderStore::StrokeView stroke;
    Expect(controller.GetStroke(0, &stroke), "get 一 stroke");
    int32_t dx = 0;
    int32_t dy = 0;
    Expect(MedianDelta(stroke, &dx, &dy), "一 median ends");
    Expect(dx > 200, "一 median moves right");
    const int32_t ady = dy < 0 ? -dy : dy;
    Expect(ady < dx, "一 is more horizontal than vertical");

    Expect(controller.Pause(), "pause");
    Expect(controller.state() == StrokeOrderUiState::Paused, "paused");
    Expect(controller.Pause(), "pause idempotent");
    Expect(controller.Resume(), "resume");
    Expect(controller.state() == StrokeOrderUiState::Animating, "animating after resume");
    Expect(controller.Resume(), "resume idempotent");

    for (int i = 0; i < 80; ++i) {
        controller.Tick(StrokeOrderController::kTickMs);
    }
    Expect(controller.state() == StrokeOrderUiState::Completed, "一 completes");
    Expect(controller.Replay(), "replay");
    Expect(controller.state() == StrokeOrderUiState::Animating, "replay animates");
    Expect(controller.current_stroke() == 0, "replay resets stroke");

    Expect(controller.StepForward(1000), "step");
    Expect(controller.state() == StrokeOrderUiState::Completed, "step finishes 一");
    Expect(!controller.StepForward(1001), "step ignored when completed");
    Expect(controller.Replay(), "replay after complete");
    Expect(controller.StepForward(4000), "step debounce start");
    Expect(!controller.StepForward(4000), "completed state rejects repeated step");
    Expect(controller.state() == StrokeOrderUiState::Completed, "一 step still complete");

    Expect(controller.BackToCandidates(), "back to candidates");
    Expect(controller.state() == StrokeOrderUiState::Candidates, "candidates after back");
    Expect(controller.SelectCandidate(1), "select 人");
    Expect(controller.stroke_count() == 2, "人 has two strokes");
    Expect(controller.GetStroke(0, &stroke) && MedianDelta(stroke, &dx, &dy), "人 stroke 0");
    Expect(dx < 0 && dy > 0, "人 first stroke down-left in y-down");
    Expect(controller.GetStroke(1, &stroke) && MedianDelta(stroke, &dx, &dy), "人 stroke 1");
    Expect(dx > 0 && dy > 0, "人 second stroke down-right in y-down");
    Expect(controller.StepForward(0), "人 step first stroke at monotonic zero");
    Expect(controller.state() == StrokeOrderUiState::Paused, "人 paused after step");
    Expect(!controller.StepForward(0), "zero timestamp still debounces");
    Expect(!controller.StepForward(StrokeOrderController::kStepDebounceMs - 1),
           "paused step remains debounced before threshold");
    Expect(controller.StepForward(StrokeOrderController::kStepDebounceMs + 1),
           "paused step works after real monotonic wait");
    Expect(controller.state() == StrokeOrderUiState::Completed, "人 completed by steps");

    Expect(controller.Exit(), "exit");
    Expect(controller.state() == StrokeOrderUiState::Hidden, "hidden after exit");
    Expect(controller.Exit(), "exit idempotent");
    Expect(!controller.IsExclusiveTouch(), "exit releases exclusive touch");
    Expect(!controller.Pause(), "pause ignored when hidden");
    Expect(!controller.Resume(), "resume ignored when hidden");
    Expect(!controller.Replay(), "replay ignored when hidden");
    Expect(!controller.StepForward(9000), "step ignored when hidden");
    Expect(!controller.SelectCandidate(0), "select ignored when hidden");
    Expect(!controller.RetryLoad(), "retry ignored when hidden");
    Expect(!controller.BackToCandidates(), "back ignored when hidden");

    Expect(controller.OpenCandidates(), "reopen");
    Expect(controller.SelectCandidate(2), "select 口");
    Expect(controller.stroke_count() == 3, "口 has three strokes");
    Expect(controller.CopyLoadedGlyph(&copied) && copied.size() == 3, "copy 口 glyph");
    const uint16_t first_median = static_cast<uint16_t>(copied[0].median.size());
    StrokeOrderLifecycle asset_lifecycle;
    asset_lifecycle.Initialize();
    asset_lifecycle.SetAssetsReady(true);
    asset_lifecycle.SetPointerReady(true);
    asset_lifecycle.SetDeviceIdle(true);
    Expect(asset_lifecycle.TryOpenOverlay(), "old asset session opens");
    asset_lifecycle.SuspendAssets();
    controller.Unbind();
    Expect(controller.state() == StrokeOrderUiState::Hidden, "unbind hides");
    Expect(!controller.OpenCandidates() && !asset_lifecycle.TryOpenOverlay(),
           "late action after unmap is rejected");
    Expect(!controller.GetStroke(0, &stroke), "GetStroke invalid after unbind");
    Expect(copied.size() == 3 && copied[0].median.size() == first_median,
           "copied glyph survives unbind");
    std::vector<uint8_t> corrupt(blob.size(), 0);
    Expect(!controller.BindStore(corrupt.data(), corrupt.size()), "corrupt replacement rejected");
    asset_lifecycle.SetAssetsReady(false);
    asset_lifecycle.RebuildSurface();
    Expect(
        !controller.is_ready() && !controller.OpenCandidates() && !asset_lifecycle.CanShowEntry(),
        "corrupt replacement cannot retain old readiness or entry");
    uint8_t dummy = 0;
    Expect(!controller.BindStore(&dummy, StrokeOrderController::kMaxOwnedBlobBytes + 1),
           "oversized blob rejected before copy");
    Expect(controller.BindStore(blob.data(), blob.size()), "rebind after unbind");
    asset_lifecycle.SetAssetsReady(true);
    Expect(asset_lifecycle.CanShowEntry(), "validated rebind restores entry gate");
    Expect(controller.SetCandidatesFromPrimary(0x4E00, nullptr), "primary 一 only");
    Expect(controller.candidate_count() == 1, "null provider does not invent homophones");
    uint32_t only = 0;
    Expect(controller.GetCandidate(0, &only) && only == 0x4E00, "recognized char first");
    Expect(!controller.SetCandidatesFromPrimary(0x5929, nullptr), "unsupported primary rejected");
    const uint32_t smoke[3] = {0x4E00, 0x4EBA, 0x53E3};
    Expect(controller.SetCandidates(smoke, 3), "restore smoke candidates");
    Expect(controller.OpenCandidates(), "open after rebind");
    Expect(controller.SelectCandidate(2), "select 口 after rebind");
    controller.Shutdown();
    Expect(controller.state() == StrokeOrderUiState::Hidden, "shutdown hides");
    controller.Shutdown();
    Expect(controller.state() == StrokeOrderUiState::Hidden, "shutdown idempotent");

    if (failures != 0) {
        std::cerr << "stroke_order_controller_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_order_controller_harness: PASS\n";
    return 0;
}
