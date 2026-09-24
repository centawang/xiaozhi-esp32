#pragma once

#include "stroke_order/stroke_order_source.h"
#include "stroke_order/stroke_order_v2.h"

#include <memory>
#include <mutex>

// Owning lease for immutable input spans (e.g. a pinned Assets mapping). Source
// retains this owner for every reader. The lease implementation, not a naked
// Blob caller, owns the storage and must prevent unmap/mutation while retained.
class StrokeOrderBundleOwner {
public:
    using Blob = stroke_order_v2::Blob;
    virtual ~StrokeOrderBundleOwner() = default;
    virtual Blob Catalog() const = 0;
    virtual Blob Pinyin() const = 0;
    virtual size_t ShardCount() const = 0;
    // Catalog descriptor order; v1 names must already be resolved by the owner.
    virtual Blob Shard(size_t index) const = 0;
};

// W4b opt-in adapter: no LVGL, tasks, queues, Application or Assets singleton.
// PrepareBundle and PrepareGlyphs are WORKER-ONLY (no display lock/callback).
// All other methods are bounded metadata/owned-view operations. One worker,
// one staged bundle, six cached glyphs, one outstanding borrow at most.
class StrokeOrderSourceAdapter final : public StrokeOrderSource {
public:
    StrokeOrderSourceAdapter();
    ~StrokeOrderSourceAdapter() override;
    StrokeOrderSourceAdapter(const StrokeOrderSourceAdapter&) = delete;
    StrokeOrderSourceAdapter& operator=(const StrokeOrderSourceAdapter&) = delete;

    struct Prepared {
        uint64_t base_generation = 0;
        uint64_t token = 0;
        const StrokeOrderSourceAdapter* owner = nullptr;
    };
    // Failed prepare/commit preserves the published generation. A unique
    // preparation ticket also fences delayed Commit calls within the SAME base
    // generation. Failure never changes the caller's output ticket.
    bool PrepareBundle(uint64_t base_generation, StrokeOrderProfile profile,
                       std::shared_ptr<const StrokeOrderBundleOwner> owner, Prepared* out);
    bool Commit(const Prepared& prepared);
    bool PrepareGlyphs(uint64_t generation, const uint32_t* codepoints, size_t count);

    // First call closes admission and invalidates queued work. Nonblocking:
    // false means a worker or borrow remains; keep mapping pinned and retry.
    // true means all staged/published readers and owner leases are gone.
    bool Suspend();
    bool Unbind() { return Suspend(); }
    uint64_t generation() const;
    bool GetInfo(Info* out) const override;
    bool Find(uint64_t generation, uint32_t codepoint, uint16_t* rank) const override;
    uint32_t Homophones(uint64_t generation, uint32_t codepoint, uint32_t* out,
                        uint32_t capacity) const override;
    bool Acquire(uint64_t generation, uint32_t codepoint, Borrow* out) override;
    bool Release(const Borrow& borrow) override;

#if defined(STROKE_ORDER_TESTING)
    // Thread-local adapter-owned allocation failures; reader hook is separate.
    static void FailAllocationAfter(int successful_allocations);
#endif

private:
    struct State;
    struct Cache;
    bool AvailableLocked(uint64_t generation) const;
    mutable std::mutex mutex_;
    std::unique_ptr<State> state_;
    std::unique_ptr<State> staged_;
    std::unique_ptr<Cache> cache_;
    uint64_t generation_ = 1;
    uint64_t staged_base_ = 0;
    uint64_t staged_token_ = 0;
    uint64_t next_preparation_ = 0;
    uint64_t next_token_ = 0;
    Borrow active_;
    bool accepting_ = false;
    bool suspending_ = false;
    bool worker_ = false;
};
