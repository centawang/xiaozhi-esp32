#include "stroke_order/stroke_order_source_adapter.h"

#include "stroke_order/stroke_order_catalog.h"
#include "stroke_order/stroke_order_pinyin.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>

namespace {
#if defined(STROKE_ORDER_TESTING)
thread_local int allocation_budget = -1;
#endif
bool AllocationAllowed() {
#if defined(STROKE_ORDER_TESTING)
    if (allocation_budget == 0)
        return false;
    if (allocation_budget > 0)
        --allocation_budget;
#endif
    return true;
}
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

struct StrokeOrderSourceAdapter::State {
    std::shared_ptr<const StrokeOrderBundleOwner> owner;
    Info info;
    stroke_order_v2::Blob shards[32];
    StrokeOrderCatalog legacy_catalog;
    StrokeOrderPinyinIndex legacy_pinyin;
    stroke_order_v2::Catalog catalog;
    stroke_order_v2::Pinyin pinyin;
    stroke_order_v2::Shard readers[32];

    bool Validate(StrokeOrderProfile profile) {
        const size_t count = owner->ShardCount();
        if (count == 0 || count > 32)
            return false;
        const auto cat = owner->Catalog();
        const auto py = owner->Pinyin();
        for (size_t i = 0; i < count; ++i) {
            shards[i] = owner->Shard(i);
            if (!shards[i].data || !shards[i].size)
                return false;
        }
        info.profile = profile;
        info.shards = static_cast<uint32_t>(count);
        if (profile == StrokeOrderProfile::Level1_3500) {
            if (!stroke_order_v2::ValidateBundle(cat, py, shards, count,
                                                   stroke_order_v2::ValidationMode::Structural) || !catalog.Bind(cat) ||
                !pinyin.Bind(py, catalog.corpus()) || !pinyin.Matches(catalog))
                return false;
            info.characters = pinyin.count();
            if (catalog.shard_count() != count ||
                !StrokeOrderProfileContract(profile, info.characters, info.shards))
                return false;
            for (size_t i = 0; i < count; ++i) {
                if (!readers[i].Bind(shards[i], catalog.corpus()))
                    return false;
            }
            return true;
        }
        if (profile != StrokeOrderProfile::Legacy2000 || !legacy_catalog.Bind(cat.data, cat.size) ||
            !legacy_pinyin.Bind(py.data, py.size))
            return false;
        info.characters = legacy_catalog.character_count();
        if (legacy_catalog.shard_count() != count ||
            legacy_pinyin.character_count() != info.characters ||
            !StrokeOrderProfileContract(profile, info.characters, info.shards))
            return false;
        for (size_t i = 0; i < count; ++i) {
            StrokeOrderCatalog::Shard descriptor;
            StrokeOrderStore store;
            if (!legacy_catalog.GetShard(i, &descriptor) || shards[i].size != descriptor.size ||
                StrokeOrderCatalog::CalculateCrc32(shards[i].data, shards[i].size) !=
                    descriptor.crc32 ||
                !store.Bind(shards[i].data, shards[i].size) ||
                store.character_count() != descriptor.character_count)
                return false;
            for (uint32_t local = 0; local < store.character_count(); ++local) {
                uint32_t cp;
                uint16_t rank;
                StrokeOrderCatalog::Entry entry;
                if (!store.GetCodepointAt(local, &cp) || !legacy_catalog.Find(cp, &entry) ||
                    entry.shard != i || entry.local_index != local ||
                    !legacy_pinyin.GetRank(cp, &rank) || rank != entry.rank ||
                    !store.LoadCharacter(cp))
                    return false;
            }
        }
        return true;
    }

    bool Find(uint32_t cp, stroke_order_v2::CatalogEntry* out) const {
        if (info.profile == StrokeOrderProfile::Legacy2000) {
            StrokeOrderCatalog::Entry e;
            if (!legacy_catalog.Find(cp, &e))
                return false;
            *out = {e.codepoint, e.rank, e.shard, e.local_index};
            return true;
        }
        uint32_t lo = 0, hi = info.characters;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            stroke_order_v2::CatalogEntry e;
            if (!catalog.GetEntry(mid, &e))
                return false;
            if (e.codepoint < cp)
                lo = mid + 1;
            else if (e.codepoint > cp)
                hi = mid;
            else {
                *out = e;
                return true;
            }
        }
        return false;
    }

    bool Decode(uint32_t cp, stroke_order_v2::Glyph* glyph) const {
        stroke_order_v2::CatalogEntry e;
        if (!Find(cp, &e))
            return false;
        if (info.profile == StrokeOrderProfile::Level1_3500)
            return readers[e.shard].Decode(e.local, glyph);
        // Full SOB1 validation above establishes these canonical index bounds.
        const auto blob = shards[e.shard];
        const uint8_t* index =
            blob.data + StrokeOrderStore::kHeaderSize + e.local * StrokeOrderStore::kIndexEntrySize;
        const uint32_t offset = U32(index + 4), size = U32(index + 8), crc = U32(index + 12);
        if (!AllocationAllowed())
            return false;
        auto raw = StrokeOrderAllocateOwned(size, "StrokeSource");
        if (!raw)
            return false;
        std::memcpy(raw.get(), blob.data + offset, size);
        if (!StrokeOrderStore::ParseRawRecord(raw.get(), size, cp, crc, &glyph->stroke_count,
                                              glyph->strokes))
            return false;
        glyph->raw = std::move(raw);
        glyph->size = size;
        glyph->codepoint = cp;
        return true;
    }
};

struct StrokeOrderSourceAdapter::Cache {
    stroke_order_v2::Glyph glyphs[6];
    size_t count = 0;
};

StrokeOrderSourceAdapter::StrokeOrderSourceAdapter() = default;
StrokeOrderSourceAdapter::~StrokeOrderSourceAdapter() {
    // Owner lifetime is explicit: destroy only after a successful Suspend.
    assert(!worker_ && active_.token == 0 && !state_ && !staged_ && !cache_ && !accepting_);
}

bool StrokeOrderSourceAdapter::PrepareBundle(uint64_t base, StrokeOrderProfile profile,
                                             std::shared_ptr<const StrokeOrderBundleOwner> owner,
                                             Prepared* out) {
    uint64_t token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owner || !out || base != generation_ || worker_ || (suspending_ && state_) ||
            next_preparation_ == std::numeric_limits<uint64_t>::max())
            return false;
        staged_.reset();
        staged_base_ = 0;
        staged_token_ = 0;
        token = ++next_preparation_;
        suspending_ = false;
        worker_ = true;
    }
    std::unique_ptr<State> local(AllocationAllowed() ? new (std::nothrow) State : nullptr);
    bool valid = false;
    if (local) {
        local->owner = std::move(owner);
        valid = local->Validate(profile);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Destroy local readers/lease BEFORE clearing the worker fence: Suspend
    // must never report safe-to-unmap while worker temporaries remain alive.
    if (!valid || base != generation_) {
        local.reset();
        owner.reset();
        worker_ = false;
        return false;
    }
    staged_ = std::move(local);
    staged_base_ = base;
    staged_token_ = token;
    *out = {base, token, this};
    worker_ = false;
    return true;
}

bool StrokeOrderSourceAdapter::Commit(const Prepared& prepared) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prepared.owner != this || prepared.base_generation != generation_ || !staged_ ||
        staged_base_ != prepared.base_generation || staged_token_ != prepared.token || worker_ ||
        active_.token || generation_ == std::numeric_limits<uint64_t>::max())
        return false;
    state_ = std::move(staged_);
    cache_.reset();
    staged_base_ = 0;
    staged_token_ = 0;
    state_->info.generation = ++generation_;
    accepting_ = true;
    suspending_ = false;
    return true;
}

bool StrokeOrderSourceAdapter::PrepareGlyphs(uint64_t generation, const uint32_t* cps,
                                             size_t count) {
    // Copy request before worker validation, never retain a caller array.
    uint32_t request[6] = {};
    State* state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!AvailableLocked(generation) || worker_ || active_.token || !cps || count == 0 ||
            count > 6)
            return false;
        for (size_t i = 0; i < count; ++i) {
            request[i] = cps[i];
            for (size_t j = 0; j < i; ++j)
                if (request[i] == request[j])
                    return false;
        }
        worker_ = true;
        state = state_.get();
    }
    std::unique_ptr<Cache> local(AllocationAllowed() ? new (std::nothrow) Cache : nullptr);
    bool valid = local != nullptr;
    for (size_t i = 0; valid && i < count; ++i)
        valid = state->Decode(request[i], &local->glyphs[i]);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid || !AvailableLocked(generation) || active_.token) {
        local.reset();
        worker_ = false;
        return false;
    }
    local->count = count;
    cache_ = std::move(local);
    worker_ = false;
    return true;
}

bool StrokeOrderSourceAdapter::Suspend() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    if (!suspending_) {
        suspending_ = true;
        // Never wrap: exhaustion permanently prevents subsequent Commit.
        if (generation_ != std::numeric_limits<uint64_t>::max())
            ++generation_;
    }
    if (worker_ || active_.token)
        return false;
    cache_.reset();
    staged_.reset();
    state_.reset();
    staged_base_ = 0;
    staged_token_ = 0;
    return true;
}

uint64_t StrokeOrderSourceAdapter::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool StrokeOrderSourceAdapter::AvailableLocked(uint64_t generation) const {
    return accepting_ && state_ && generation == generation_;
}

bool StrokeOrderSourceAdapter::GetInfo(Info* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !AvailableLocked(generation_))
        return false;
    *out = state_->info;
    return true;
}

bool StrokeOrderSourceAdapter::Find(uint64_t generation, uint32_t cp, uint16_t* rank) const {
    std::lock_guard<std::mutex> lock(mutex_);
    stroke_order_v2::CatalogEntry entry;
    if (!rank || !AvailableLocked(generation) || !state_->Find(cp, &entry))
        return false;
    *rank = entry.rank;
    return true;
}

uint32_t StrokeOrderSourceAdapter::Homophones(uint64_t generation, uint32_t cp, uint32_t* out,
                                              uint32_t capacity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !AvailableLocked(generation))
        return 0;
    capacity = std::min(capacity, uint32_t{6});
    if (state_->info.profile == StrokeOrderProfile::Legacy2000)
        return state_->legacy_pinyin.AppendHomophones(cp, out, capacity);
    return state_->pinyin.Homophones(cp, out, capacity);
}

bool StrokeOrderSourceAdapter::Acquire(uint64_t generation, uint32_t cp, Borrow* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !AvailableLocked(generation) || active_.token || !cache_ ||
        next_token_ == std::numeric_limits<uint64_t>::max())
        return false;
    for (size_t i = 0; i < cache_->count; ++i) {
        const auto& glyph = cache_->glyphs[i];
        if (glyph.codepoint != cp)
            continue;
        active_ = {glyph.strokes, glyph.stroke_count, cp, generation, ++next_token_, this};
        *out = active_;
        return true;
    }
    return false;
}

bool StrokeOrderSourceAdapter::Release(const Borrow& borrow) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.token || borrow.owner != this || borrow.generation != active_.generation ||
        borrow.token != active_.token)
        return false;
    active_ = {};
    return true;
}

#if defined(STROKE_ORDER_TESTING)
void StrokeOrderSourceAdapter::FailAllocationAfter(int count) { allocation_budget = count; }
#endif
