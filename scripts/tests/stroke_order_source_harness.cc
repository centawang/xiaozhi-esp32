#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_pinyin.h"
#include "stroke_order/stroke_order_source_adapter.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Profile = StrokeOrderProfile;
using Adapter = StrokeOrderSourceAdapter;
using Blob = stroke_order_v2::Blob;

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL " << message << '\n';
        std::exit(1);
    }
}
std::vector<uint8_t> Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), path.c_str());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
void Put32(uint8_t* p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = n >> (8 * i);
}
void Refresh(std::vector<uint8_t>& b) {
    Put32(b.data() + 44, stroke_order_v2::Crc32(b.data() + 64, b.size() - 64));
    Put32(b.data() + 48, 0);
    Put32(b.data() + 48, stroke_order_v2::Crc32(b.data(), 64));
}
struct Owner final : StrokeOrderBundleOwner {
    std::vector<uint8_t> cat, py;
    std::vector<std::vector<uint8_t>> shards;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable bool gate = false, entered = false, proceed = false;
    explicit Owner(const std::string& path, bool v2) {
        cat = Read(path + "/stroke_cat.bin");
        py = Read(path + (v2 ? "/stroke_pinyin.bin" : "/stroke_pinyin.spy1"));
        for (unsigned i = 0; i < (v2 ? U32(cat.data() + 28) : 8U); ++i)
            shards.push_back(Read(path + "/so0" + std::to_string(i) + (v2 ? ".bin" : ".sob1")));
    }
    Blob Catalog() const override {
        std::unique_lock<std::mutex> lock(mutex);
        if (gate) {
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return proceed; });
        }
        return {cat.data(), cat.size()};
    }
    Blob Pinyin() const override { return {py.data(), py.size()}; }
    size_t ShardCount() const override { return shards.size(); }
    Blob Shard(size_t i) const override { return {shards[i].data(), shards[i].size()}; }
    uint32_t AtRank(uint16_t rank, bool v2) const {
        const size_t offset = v2 ? 64 : 32;
        const size_t count = v2 ? 3500 : 2000;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* e = cat.data() + offset + i * 12;
            if (U16(e + 4) == rank)
                return U32(e);
        }
        return 0;
    }
};

void Bind(Adapter& source, Profile profile, std::shared_ptr<Owner> owner) {
    const uint64_t base = source.generation();
    Adapter::Prepared ticket;
    Check(source.PrepareBundle(base, profile, owner, &ticket), "prepare complete bundle");
    Check(source.generation() == base, "prepare does not publish");
    Check(source.Commit(ticket), "commit validated bundle");
    Check(source.generation() > base, "commit monotonic generation");
}

void Candidates(Adapter& source, StrokeOrderController& controller, uint32_t primary) {
    uint32_t cps[7] = {primary};
    const auto gen = source.generation();
    const uint32_t extras = source.Homophones(gen, primary, cps + 1, 99);
    Check(extras <= 6, "homophones bounded six even overlarge capacity");
    const size_t n = std::min(size_t{6}, size_t{1 + extras});
    Check(source.PrepareGlyphs(gen, cps, n), "worker prepares candidate raw bytes");
    // Faults in both decoder and adapter allocations prove UI calls below do
    // not trigger corpus validation, decompression or raw-glyph allocation.
    stroke_order_v2::FailAllocationAfter(0);
    Adapter::FailAllocationAfter(0);
    Check(controller.SetCandidatesFromPrimary(primary), "unified provider candidates");
    Check(controller.candidate_count() == n, "candidate count/order preserved");
    Check(controller.OpenCandidates(), "open candidates");
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = 0;
        std::vector<StrokeOrderController::DecodedStroke> glyph;
        Check(controller.GetCandidate(i, &cp) && cp == cps[i], "primary then ranked homophones");
        Check(controller.CopyCandidateGlyph(i, &cp, &glyph) && !glyph.empty(),
              "UI copies owned raw");
    }
    Check(controller.SelectCandidate(0), "select prepared candidate");
    Check(controller.Pause() && controller.Pause(), "pause idempotent");
    Check(controller.StepForward(1000), "phase-local step");
    Check(controller.BackToCandidates() && controller.Exit(), "back and exit");
    Adapter::FailAllocationAfter(-1);
    stroke_order_v2::FailAllocationAfter(-1);
}

void ProfileTests(const std::string& path, bool v2) {
    auto owner = std::make_shared<Owner>(path, v2);
    std::weak_ptr<Owner> weak = owner;
    Adapter source;
    StrokeOrderController controller;
    StrokeOrderSource::Info info;
    Check(!source.GetInfo(&info) && !controller.BindSource(&source),
          "no capability before validation");
    Bind(source, v2 ? Profile::Level1_3500 : Profile::Legacy2000, owner);
    Check(source.GetInfo(&info) && info.characters == (v2 ? 3500U : 2000U) &&
              info.shards == (v2 ? 6U : 8U),
          "dynamic profile counts");
    Check(controller.BindSource(&source) && controller.is_ready(), "controller accepts source");
    for (uint16_t rank : {1, 2000, 2001, 3500}) {
        if (!v2 && rank > 2000)
            continue;
        uint32_t cp = owner->AtRank(rank, v2);
        uint16_t actual = 0;
        Check(
            source.Find(info.generation, cp, &actual) && actual == rank && controller.Contains(cp),
            "official rank boundary");
        Check(source.PrepareGlyphs(info.generation, &cp, 1), "prepare rank boundary");
        StrokeOrderSource::Borrow borrow, second;
        Check(source.Acquire(info.generation, cp, &borrow), "acquire boundary");
        Check(!source.Acquire(info.generation, cp, &second), "one borrow bound");
        StrokeOrderStore::Point point;
        Check(borrow.strokes[0].GetOutlinePoint(0, &point), "owned view usable");
        Check(!source.PrepareGlyphs(info.generation, &cp, 1), "cannot replace active glyph");
        Check(source.Release(borrow) && !source.Release(borrow), "exactly once release");
    }
    // Every descriptor's first/last rank, including all six real SOB2 boundaries.
    const size_t descriptor = v2 ? U32(owner->cat.data() + 36) : U32(owner->cat.data() + 16);
    for (size_t i = 0; i < info.shards; ++i) {
        const uint8_t* d = owner->cat.data() + descriptor + i * 40;
        const uint16_t first = v2 ? U16(d + 24) : U32(d + 28);
        const uint16_t last = v2 ? U16(d + 26) : U32(d + 32);
        for (uint16_t rank : {first, last}) {
            const uint32_t cp = owner->AtRank(rank, v2);
            Check(cp && source.PrepareGlyphs(info.generation, &cp, 1), "shard first/last decoded");
        }
    }
    // Exercise every actual entry through the adapter, not only its readers.
    // Batches stay at six, so memory never scales with the full corpus.
    {
        StrokeOrderPinyinIndex legacy_pinyin;
        stroke_order_v2::Pinyin modern_pinyin;
        Check(v2 ? modern_pinyin.Bind(owner->Pinyin())
                 : legacy_pinyin.Bind(owner->py.data(), owner->py.size()),
              "reference pinyin");
        for (uint32_t start = 0; start < info.characters; start += 6) {
            const size_t count = std::min(uint32_t{6}, info.characters - start);
            uint32_t cps[6] = {};
            for (size_t i = 0; i < count; ++i)
                cps[i] = U32(owner->cat.data() + (v2 ? 64 : 32) + (start + i) * 12);
            Check(source.PrepareGlyphs(info.generation, cps, count),
                  "all glyphs prepare in bounded batches");
            for (size_t i = 0; i < count; ++i) {
                uint32_t expected[6] = {}, actual[6] = {};
                const size_t n = v2 ? modern_pinyin.Homophones(cps[i], expected, 6)
                                    : legacy_pinyin.AppendHomophones(cps[i], expected, 6);
                Check(source.Homophones(info.generation, cps[i], actual, 6) == n &&
                          std::equal(expected, expected + n, actual),
                      "all homophones preserve reader ordering");
                uint16_t rank = 0;
                Check(source.Find(info.generation, cps[i], &rank) &&
                          rank == U16(owner->cat.data() + (v2 ? 64 : 32) + (start + i) * 12 + 4),
                      "all codepoints map to correct rank");
                StrokeOrderSource::Borrow each;
                Check(source.Acquire(info.generation, cps[i], &each) && each.codepoint == cps[i] &&
                          each.stroke_count > 0,
                      "all glyphs owned borrow");
                for (uint16_t s = 0; s < each.stroke_count; ++s) {
                    StrokeOrderStore::Point first, last;
                    const auto& stroke = each.strokes[s];
                    Check(stroke.GetOutlinePoint(0, &first) &&
                              stroke.GetOutlinePoint(stroke.outline_count() - 1, &last) &&
                              first.x == last.x && first.y == last.y,
                          "all outlines closed in owned view");
                    Check(stroke.GetMedianPoint(stroke.median_count() - 1, &last),
                          "all medians accessible");
                }
                Check(source.Release(each), "all glyph borrows released");
            }
        }
    }
    Candidates(source, controller, owner->AtRank(1, v2));
    if (v2) {
        stroke_order_v2::Pinyin pinyin;
        Check(pinyin.Bind(owner->Pinyin()) && pinyin.ReadingCount(0x6566) == 10,
              "Dun ten readings");
        Candidates(source, controller, 0x6566);
    }
    // Failed cache preparation leaves previous owned cache usable.
    const uint32_t cp = owner->AtRank(1, v2);
    Check(source.PrepareGlyphs(info.generation, &cp, 1), "initial cache");
    Adapter::FailAllocationAfter(0);
    Check(!source.PrepareGlyphs(info.generation, &cp, 1), "cache allocation failure");
    Adapter::FailAllocationAfter(-1);
    if (v2) {
        for (int n = 0; n < 3; ++n) {
            stroke_order_v2::FailAllocationAfter(n);
            Check(!source.PrepareGlyphs(info.generation, &cp, 1),
                  "decode allocation failure rollback");
        }
        stroke_order_v2::FailAllocationAfter(-1);
    }
    StrokeOrderSource::Borrow borrow;
    Check(source.Acquire(info.generation, cp, &borrow), "cache retained after allocation failures");
    owner.reset();
    Check(!weak.expired(), "source owns input storage lease");
    Check(!source.Suspend(), "suspend refuses active borrow");
    Check(!source.GetInfo(&info) && !controller.is_ready(),
          "suspend closes capability immediately");
    StrokeOrderSource::Borrow late;
    Check(!source.Acquire(borrow.generation, cp, &late), "suspend blocks old acquire");
    StrokeOrderStore::Point point;
    Check(borrow.strokes[0].GetMedianPoint(0, &point), "active raw remains alive during drain");
    Check(!weak.expired(), "mapping pinned until drain");
    Check(source.Release(borrow) && source.Suspend(), "release then safe unmap");
    Check(weak.expired(), "all readers and owner cleared before safe unmap");
    controller.Unbind();
    Check(!source.Release(borrow), "late release rejected after unmap");
    std::cout << "PASS profile " << (v2 ? 3500 : 2000) << '\n';
}

void RebindTests(const std::string& old_path, const std::string& new_path) {
    auto old = std::make_shared<Owner>(old_path, false);
    auto fresh = std::make_shared<Owner>(new_path, true);
    Adapter source;
    StrokeOrderController controller;
    Bind(source, Profile::Legacy2000, old);
    Check(controller.BindSource(&source), "bind legacy controller");
    Check(!controller.Contains(fresh->AtRank(2001, true)) &&
              !controller.Contains(fresh->AtRank(3500, true)),
          "legacy excludes ranks 2001/3500");
    const auto base = source.generation();
    Adapter::Prepared ticket;
    const uint32_t cp = old->AtRank(1, false);
    Check(source.PrepareGlyphs(base, &cp, 1), "old cache");
    StrokeOrderSource::Borrow borrow;
    Check(source.Acquire(base, cp, &borrow), "old borrow");
    Check(source.PrepareBundle(base, Profile::Level1_3500, fresh, &ticket),
          "prepare alongside borrow");
    Check(!source.Commit(ticket), "replacement refuses active borrow");
    Check(source.Release(borrow) && source.Commit(ticket), "replacement after borrow drain");
    Check(!controller.is_ready() && !controller.Contains(cp), "controller old generation fenced");
    Check(!source.Acquire(base, cp, &borrow), "old acquire fenced");
    Check(!source.Commit(ticket), "old commit fenced");
    Check(controller.BindSource(&source), "controller rebind new generation");
    const auto gen = source.generation();
    Check(source.PrepareGlyphs(gen, &cp, 1), "new cache");
    StrokeOrderSource::Borrow current;
    Check(source.Acquire(gen, cp, &current), "new acquire");
    Check(!source.Release(borrow), "old release cannot clear new borrow");
    Check(!source.Acquire(gen, cp, &borrow), "new borrow still active");
    Check(source.Release(current), "new release");

    Adapter::Prepared earlier, later;
    Check(source.PrepareBundle(gen, Profile::Level1_3500, fresh, &earlier),
          "stage first same-base job");
    Check(source.PrepareBundle(gen, Profile::Legacy2000, old, &later),
          "stage replacement same-base job");
    Check(!source.Commit(earlier) && source.generation() == gen,
          "late same-base commit ticket rejected");
    // A failed following prepare clears the staged slot, not the live generation.
    auto bad = std::make_shared<Owner>(new_path, true);
    bad->py[8] ^= 1;
    Refresh(bad->py);  // Valid SPY2 of a different corpus, not a mere bad CRC.
    Check(!source.PrepareBundle(gen, Profile::Level1_3500, bad, &ticket), "mixed corpus rejected");
    Check(!source.Commit(later) && !source.Commit(ticket) && source.generation() == gen &&
              controller.is_ready(),
          "failed rebind rollback keeps old published generation");
    Check(!source.PrepareBundle(gen, Profile::Legacy2000, fresh, &ticket), "no fallback v2 to v1");
    Check(!source.PrepareBundle(gen, Profile::Level1_3500, old, &ticket), "no fallback v1 to v2");
    Adapter::FailAllocationAfter(0);
    Check(!source.PrepareBundle(gen, Profile::Level1_3500, fresh, &ticket),
          "state allocation failure");
    Adapter::FailAllocationAfter(-1);
    for (int n = 0; n < 18; ++n) {
        stroke_order_v2::FailAllocationAfter(n);
        Check(!source.PrepareBundle(gen, Profile::Level1_3500, fresh, &ticket),
              "full validation allocation failure");
    }
    stroke_order_v2::FailAllocationAfter(-1);
    Check(source.generation() == gen && controller.is_ready(),
          "all failures preserve old generation");
    Candidates(source, controller, 0x6566);

    // Deterministic thread interleaving: hold the worker inside the input owner
    // after admission, then suspend. No sleeps or timing assumptions.
    auto gated = std::make_shared<Owner>(new_path, true);
    gated->gate = true;
    bool result = true;
    Adapter::Prepared worker_ticket;
    std::thread worker(
        [&] { result = source.PrepareBundle(gen, Profile::Level1_3500, gated, &worker_ticket); });
    {
        std::unique_lock<std::mutex> lock(gated->mutex);
        gated->cv.wait(lock, [&] { return gated->entered; });
    }
    Check(!source.Suspend(), "suspend refuses in-flight worker");
    Check(!source.Commit(ticket) && !source.Acquire(gen, cp, &current),
          "worker suspend fences late work");
    {
        std::lock_guard<std::mutex> lock(gated->mutex);
        gated->proceed = true;
        gated->cv.notify_all();
    }
    worker.join();
    Check(!result && source.Suspend(), "late worker discarded before unmap");
    controller.Unbind();
    Bind(source, Profile::Legacy2000, old);
    Check(!source.Release(current), "ancient release cannot affect restored profile");
    Check(controller.BindSource(&source), "restore legacy profile");
    Candidates(source, controller, cp);
    controller.Unbind();
    Check(source.Unbind(), "final unbind");
    std::cout << "PASS rollback generation controlled-interleave\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "malicious") {
        auto owner = std::make_shared<Owner>(argv[2], true);
        std::vector<Blob> blobs;
        for (const auto& shard : owner->shards)
            blobs.push_back({shard.data(), shard.size()});
        Check(stroke_order_v2::ValidateBundle(owner->Catalog(), owner->Pinyin(), blobs.data(),
                  blobs.size(), stroke_order_v2::ValidationMode::Structural),
              "structural admits checksum-valid opaque glyph payload");
        Check(!stroke_order_v2::ValidateBundle(owner->Catalog(), owner->Pinyin(), blobs.data(),
                  blobs.size(), stroke_order_v2::ValidationMode::Deep), "deep rejects bad glyph");
        Check(!stroke_order_v2::ValidateBundle(owner->Catalog(), owner->Pinyin(), blobs.data(),
                  blobs.size()), "default remains deep");
        Adapter source;
        Bind(source, Profile::Level1_3500, owner);
        const uint32_t good = owner->AtRank(2, true), bad = owner->AtRank(1, true);
        const auto gen = source.generation();
        Check(source.PrepareGlyphs(gen, &good, 1), "good glyph cached");
        const uint32_t batch[] = {good, bad};
        Check(!source.PrepareGlyphs(gen, batch, 2), "bad glyph fails whole replacement batch");
        StrokeOrderSource::Borrow borrow;
        Check(!source.Acquire(gen, bad, &borrow), "bad glyph never published");
        Check(source.Acquire(gen, good, &borrow), "previous cache preserved");
        StrokeOrderStore::Point point;
        Check(borrow.strokes[0].GetMedianPoint(0, &point), "preserved raw view usable");
        Check(source.Release(borrow) && source.Suspend(), "malicious test drain");
        std::cout << "PASS structural/deep/on-demand malicious rollback\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "prepare-perf") {
        auto owner = std::make_shared<Owner>(argv[2], true);
        Adapter source;
        stroke_order_v2::ResetWorkCounters();
        const auto start = std::chrono::steady_clock::now();
        Bind(source, Profile::Level1_3500, owner);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto work = stroke_order_v2::GetWorkCounters();
        std::cout << "prepare elapsed_us=" << elapsed << " decodes=" << work.decodes
                  << " owned_allocations=" << work.owned_allocations << std::endl;
        Check(source.Suspend(), "perf drain");
        Check(work.decodes == 0, "bundle prepare must not decode any glyph");
        Check(work.owned_allocations <= 2 * owner->ShardCount() + 6,
              "bundle prepare allocation count must depend only on shard count");
        Check(elapsed < 2000000, "host prepare under two seconds");
        return 0;
    }
    Check(argc == 3, "usage: source-harness prototype_2000 prototype_3500");
    Check(StrokeOrderProfileContract(Profile::Legacy2000, 2000, 8), "legacy contract");
    Check(!StrokeOrderProfileContract(Profile::Legacy2000, 1999, 8) &&
              !StrokeOrderProfileContract(Profile::Legacy2000, 2000, 7),
          "legacy exact contract");
    Check(StrokeOrderProfileContract(Profile::Level1_3500, 3500, 1) &&
              StrokeOrderProfileContract(Profile::Level1_3500, 3500, 32) &&
              !StrokeOrderProfileContract(Profile::Level1_3500, 3500, 0) &&
              !StrokeOrderProfileContract(Profile::Level1_3500, 3500, 33) &&
              !StrokeOrderProfileContract(Profile::Level1_3500, 3499, 6),
          "level1 bounded contract");
    ProfileTests(argv[1], false);
    ProfileTests(argv[2], true);
    RebindTests(argv[1], argv[2]);
    std::cout << "PASS W4b\n";
}
