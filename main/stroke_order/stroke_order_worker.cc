#include "stroke_order/stroke_order_worker.h"

#include <cassert>
#include <limits>

#if defined(ESP_PLATFORM)
#include <cinttypes>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
void LogBundle(const char* phase, uint64_t generation, bool success, int64_t elapsed_ms) {
    ESP_LOGI("StrokePrepare",
             "bundle %s stage=structural generation=%" PRIu64 " success=%d elapsed_ms=%" PRId64
             " reason=%s stack_hwm=%u psram_free=%u psram_min=%u internal_free=%u internal_min=%u",
             phase, generation, success, elapsed_ms,
             phase[0] == 's' ? "begin" : (success ? "prepared" : "invalid_or_stale"),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}
}  // namespace
#endif

StrokeOrderWorker::~StrokeOrderWorker() {
    assert(!active_);
    const bool drained = source_.Suspend();
    assert(drained);
    (void)drained;
}

bool StrokeOrderWorker::SubmitBundle(std::shared_ptr<const StrokeOrderBundleOwner> owner,
                                     uint64_t assets) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || active_ || pending_.result.kind != Kind::None || !owner || !assets ||
        ticket_ == std::numeric_limits<uint64_t>::max())
        return false;
    // Only reopen after successful drain, never supersede live readers.
    if (!source_.Suspend())
        return false;
    closed_ = false;
    done_ = {};
    pending_.result = {};
    pending_.result.kind = Kind::Bundle;
    pending_.result.ticket = ++ticket_;
    pending_.result.assets = assets;
    pending_.result.source = source_.generation();
    pending_.owner = std::move(owner);
    return true;
}

bool StrokeOrderWorker::SubmitGlyphs(uint64_t assets, uint64_t round, const uint32_t* cps,
                                     size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || stop_ || !round || !cps || !count || count > 6 ||
        pending_.result.kind == Kind::Bundle || ticket_ == std::numeric_limits<uint64_t>::max())
        return false;
    StrokeOrderSource::Info info;
    if (!source_.GetInfo(&info))
        return false;
    // Latest round replaces the single pending slot; active work is fenced.
    done_ = {};
    pending_ = {};
    auto& r = pending_.result;
    r.kind = Kind::Glyphs;
    r.ticket = ++ticket_;
    r.assets = assets;
    r.round = round;
    r.source = info.generation;
    r.count = count;
    for (size_t i = 0; i < count; ++i)
        r.cps[i] = cps[i];
    return true;
}

void StrokeOrderWorker::CancelRound() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Bundle readiness is independent of a voice round.
    if (pending_.result.kind == Kind::Bundle || done_.kind == Kind::Bundle ||
        active_kind_ == Kind::Bundle)
        return;
    if (ticket_ != std::numeric_limits<uint64_t>::max())
        ++ticket_;
    pending_ = {};
    done_ = {};
}

bool StrokeOrderWorker::Suspend() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    if (ticket_ != std::numeric_limits<uint64_t>::max())
        ++ticket_;
    pending_ = {};
    done_ = {};
    const bool drained = source_.Suspend();
    return drained && !active_;
}

void StrokeOrderWorker::Stop() {
    Suspend();
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
}

bool StrokeOrderWorker::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
}

bool StrokeOrderWorker::RunOne() {
    Command command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || closed_ || active_ || pending_.result.kind == Kind::None)
            return false;
        command = std::move(pending_);
        pending_ = {};
        active_ = true;
        active_kind_ = command.result.kind;
    }
#if defined(STROKE_ORDER_TESTING)
    if (before_prepare)
        before_prepare(before_prepare_context);
#endif
    auto& r = command.result;
#if defined(ESP_PLATFORM)
    const int64_t started = esp_timer_get_time();
    if (r.kind == Kind::Bundle)
        LogBundle("start", r.source, false, 0);
#endif
    if (r.kind == Kind::Bundle)
        r.ok = source_.PrepareBundle(r.source, StrokeOrderProfile::Level1_3500, command.owner,
                                     &r.prepared);
    else
        r.ok = source_.PrepareGlyphs(r.source, r.cps, r.count);
#if defined(ESP_PLATFORM)
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
    if (r.kind == Kind::Bundle) {
        LogBundle("end", r.source, r.ok, elapsed_ms);
    } else {
        ESP_LOGD("StrokePrepare", "glyphs generation=%" PRIu64 " success=%d elapsed_ms=%" PRId64,
                 r.source, r.ok, elapsed_ms);
    }
#endif
    // Drop worker-local input lease before publishing the drain fence.
    command.owner.reset();
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    active_kind_ = Kind::None;
    if (!closed_ && !stop_ && r.ticket == ticket_)
        done_ = r;
#if defined(ESP_PLATFORM)
    else if (r.kind == Kind::Bundle)
        ESP_LOGI("StrokePrepare", "bundle stage=completion ok=0 reason=worker_fence");
#endif
    if (closed_ || stop_)
        source_.Suspend();
    return true;
}

bool StrokeOrderWorker::Take(Result* result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result || closed_ || done_.kind == Kind::None)
        return false;
    *result = done_;
    done_ = {};
    return true;
}
