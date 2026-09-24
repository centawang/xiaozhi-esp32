#pragma once

#include "stroke_order/stroke_order_source_adapter.h"

// Portable single-consumer mailbox. The product task calls RunOne; UI only
// submits/polls metadata. No callbacks retain a View, no dynamically sized queue.
class StrokeOrderWorker {
public:
    enum class Kind { None, Bundle, Glyphs };
    struct Result {
        Kind kind = Kind::None;
        uint64_t ticket = 0;
        uint64_t assets = 0;
        uint64_t round = 0;
        uint64_t source = 0;
        uint32_t cps[6] = {};
        size_t count = 0;
        bool ok = false;
        StrokeOrderSourceAdapter::Prepared prepared;
    };
    ~StrokeOrderWorker();
    bool SubmitBundle(std::shared_ptr<const StrokeOrderBundleOwner> owner, uint64_t assets);
    bool SubmitGlyphs(uint64_t assets, uint64_t round, const uint32_t* cps, size_t count);
    void CancelRound();
    // Nonblocking; false forbids unmap. Controller must be unbound first.
    bool Suspend();
    void Stop();
    bool stopped() const;
    bool RunOne();  // WORKER ONLY; never under display/main/audio locks.
    bool Take(Result* result);
    StrokeOrderSourceAdapter& source() { return source_; }
#if defined(STROKE_ORDER_TESTING)
    // Set before starting the consumer; deterministic delayed-job harness only.
    void (*before_prepare)(void*) = nullptr;
    void* before_prepare_context = nullptr;
#endif

private:
    struct Command {
        Result result;
        std::shared_ptr<const StrokeOrderBundleOwner> owner;
    };
    mutable std::mutex mutex_;
    StrokeOrderSourceAdapter source_;
    Command pending_;
    Result done_;
    uint64_t ticket_ = 0;
    bool active_ = false;
    Kind active_kind_ = Kind::None;
    bool closed_ = true;
    bool stop_ = false;
};
