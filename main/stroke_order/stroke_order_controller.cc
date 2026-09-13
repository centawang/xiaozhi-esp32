#include "stroke_order/stroke_order_controller.h"

#include "stroke_order/stroke_order_pinyin.h"

#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_log.h>
static const char* TAG = "StrokeOrder";
#endif

namespace {

constexpr uint32_t kPermille = 1000;

class ScopedShardView {
public:
    ScopedShardView(StrokeOrderShardSource* source, const char* name) : source_(source) {
        if (source_ != nullptr) {
            acquired_ = source_->AcquireShard(name, &data_, &size_);
        }
    }

    ~ScopedShardView() {
        if (acquired_) {
            source_->ReleaseShard();
        }
    }

    ScopedShardView(const ScopedShardView&) = delete;
    ScopedShardView& operator=(const ScopedShardView&) = delete;

    bool valid() const { return acquired_ && data_ != nullptr; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    StrokeOrderShardSource* source_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool acquired_ = false;
};

}  // namespace

StrokeOrderController& StrokeOrderController::GetInstance() {
    static StrokeOrderController instance;
    return instance;
}

StrokeOrderController::StrokeOrderController() = default;

bool StrokeOrderController::BindStore(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearDataLocked();
    if (data == nullptr || size == 0 || size > kMaxOwnedBlobBytes) {
        return false;
    }

#if defined(ESP_PLATFORM)
    StrokeOrderOwnedBlob copy = StrokeOrderAllocateOwned(size, TAG);
#else
    StrokeOrderOwnedBlob copy = StrokeOrderAllocateOwned(size, "StrokeOrder");
#endif
    if (copy == nullptr) {
#if defined(ESP_PLATFORM)
        ESP_LOGE(TAG, "BindStore hid SO: PSRAM owned copy failed size=%u",
                 static_cast<unsigned>(size));
#endif
        return false;
    }
    std::memcpy(copy.get(), data, size);
    StrokeOrderStore validated;
    if (!validated.Bind(copy.get(), size) || validated.character_count() == 0) {
        return false;
    }
    owned_blob_ = std::move(copy);
    owned_blob_size_ = size;
    if (!store_.Bind(owned_blob_.get(), owned_blob_size_)) {
        ClearDataLocked();
        return false;
    }
    return true;
}

bool StrokeOrderController::BindCatalog(const uint8_t* data, size_t size,
                                        StrokeOrderShardSource* source) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearDataLocked();
    if (data == nullptr || source == nullptr || size == 0 ||
        size > StrokeOrderCatalog::kMaxFileBytes) {
        return false;
    }
#if defined(ESP_PLATFORM)
    StrokeOrderOwnedBlob copy = StrokeOrderAllocateOwned(size, TAG);
#else
    StrokeOrderOwnedBlob copy = StrokeOrderAllocateOwned(size, "StrokeOrder");
#endif
    if (copy == nullptr) {
#if defined(ESP_PLATFORM)
        ESP_LOGE(TAG, "SCB1 catalog copy failed closed size=%u", static_cast<unsigned>(size));
#endif
        return false;
    }
    std::memcpy(copy.get(), data, size);
    catalog_blob_ = std::move(copy);
    catalog_blob_size_ = size;
    shard_source_ = source;
    if (!catalog_.Bind(catalog_blob_.get(), catalog_blob_size_) ||
        catalog_.character_count() != kRuntimeCharacterCount ||
        catalog_.shard_count() != kRuntimeShardCount || !ValidateCatalogShardsLocked()) {
        ClearDataLocked();
        return false;
    }
#if defined(ESP_PLATFORM)
    ESP_LOGI(TAG, "SCB1 ready chars=%u shards=%u catalog=%u shard_total=%u",
             static_cast<unsigned>(catalog_.character_count()),
             static_cast<unsigned>(catalog_.shard_count()), static_cast<unsigned>(size),
             static_cast<unsigned>(catalog_.total_shard_bytes()));
#endif
    return true;
}

void StrokeOrderController::Unbind() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearDataLocked();
}

bool StrokeOrderController::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadyLocked();
}

bool StrokeOrderController::MatchesPinyinIndex(const StrokeOrderPinyinIndex& pinyin_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!catalog_.is_bound() || !pinyin_index.is_bound() ||
        catalog_.character_count() != pinyin_index.character_count()) {
        return false;
    }
    for (uint32_t i = 0; i < catalog_.character_count(); ++i) {
        StrokeOrderCatalog::Entry entry;
        uint16_t pinyin_rank = 0;
        if (!catalog_.GetEntry(i, &entry) || !pinyin_index.GetRank(entry.codepoint, &pinyin_rank) ||
            pinyin_rank != entry.rank) {
            return false;
        }
    }
    return true;
}

bool StrokeOrderController::Contains(uint32_t codepoint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ContainsLocked(codepoint);
}

bool StrokeOrderController::SetCandidates(const uint32_t* codepoints, uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidate_count_ = 0;
    if (!ReadyLocked() || codepoints == nullptr || count == 0) {
        return false;
    }
    const uint32_t bounded_count = count < kMaxCandidateInputs ? count : kMaxCandidateInputs;
    for (uint32_t i = 0; i < bounded_count && candidate_count_ < StrokeOrderLayout::kMaxCandidates;
         ++i) {
        FilterCandidateLocked(codepoints[i]);
    }
    return candidate_count_ > 0;
}

bool StrokeOrderController::SetCandidatesFromPrimary(uint32_t primary,
                                                     const StrokeOrderCandidateProvider* provider) {
    uint32_t ordered[kMaxCandidateInputs] = {};
    uint32_t count = 0;
    ordered[count++] = primary;
    if (provider != nullptr) {
        uint32_t extra[kMaxCandidateInputs] = {};
        const uint32_t cap = kMaxCandidateInputs - count;
        uint32_t written = provider->AppendHomophones(primary, extra, cap);
        if (written > cap) {
            written = cap;
        }
        for (uint32_t i = 0; i < written && count < kMaxCandidateInputs; ++i) {
            ordered[count++] = extra[i];
        }
    }
    return SetCandidates(ordered, count);
}

bool StrokeOrderController::EnterConnecting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked()) {
        return false;
    }
    CancelPlaybackLocked();
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::Connecting;
    return true;
}

bool StrokeOrderController::EnterAwaitingSpeech() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked()) {
        return false;
    }
    CancelPlaybackLocked();
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::AwaitingSpeech;
    return true;
}

bool StrokeOrderController::EnterNoMatch() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::NoMatch;
    return true;
}

bool StrokeOrderController::EnterTimedOut() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::TimedOut;
    return true;
}

bool StrokeOrderController::EnterError() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::Error;
    return true;
}

bool StrokeOrderController::OpenCandidates() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked() || candidate_count_ == 0) {
        return false;
    }
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::Candidates;
    return true;
}

bool StrokeOrderController::SelectCandidate(uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != StrokeOrderUiState::Candidates) {
        return false;
    }
    if (index >= candidate_count_) {
        return false;
    }
    const uint32_t codepoint = candidates_[index];
    state_ = StrokeOrderUiState::Loading;
    if (!LoadSelectedLocked(codepoint)) {
        state_ = StrokeOrderUiState::Error;
        return false;
    }
    ResetPlaybackLocked();
    state_ = StrokeOrderUiState::Animating;
    return true;
}

bool StrokeOrderController::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Paused) {
        return true;
    }
    if (state_ != StrokeOrderUiState::Animating) {
        return false;
    }
    state_ = StrokeOrderUiState::Paused;
    return true;
}

bool StrokeOrderController::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Animating) {
        return true;
    }
    if (state_ != StrokeOrderUiState::Paused) {
        return false;
    }
    state_ = StrokeOrderUiState::Animating;
    return true;
}

bool StrokeOrderController::Replay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != StrokeOrderUiState::Animating && state_ != StrokeOrderUiState::Paused &&
        state_ != StrokeOrderUiState::Completed) {
        return false;
    }
    if (loaded_glyph_.empty()) {
        return false;
    }
    ResetPlaybackLocked();
    state_ = StrokeOrderUiState::Animating;
    return true;
}

bool StrokeOrderController::StepForward(uint64_t monotonic_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != StrokeOrderUiState::Animating && state_ != StrokeOrderUiState::Paused) {
        return false;
    }
    if (has_last_step_ms_ &&
        (monotonic_ms < last_step_ms_ || monotonic_ms - last_step_ms_ < kStepDebounceMs)) {
        return false;
    }
    // Invalid indices/final gaps cannot be produced by playback. Reject them
    // without consuming debounce or wrapping an index into a different stroke.
    if (loaded_glyph_.empty() || current_stroke_ >= loaded_glyph_.size() ||
        (in_gap_ && current_stroke_ + 1U >= loaded_glyph_.size())) {
        return false;
    }
    has_last_step_ms_ = true;
    last_step_ms_ = monotonic_ms;
    gap_elapsed_ms_ = 0;
    if (in_gap_) {
        // Finish ONLY the gap: the next start cue needs its own presentation.
        in_gap_ = false;
        current_stroke_ = static_cast<uint16_t>(current_stroke_ + 1);
        stroke_elapsed_ms_ = 0;
        state_ = StrokeOrderUiState::Paused;
        return true;
    }
    // Finish ONLY this cue: keep its full contour/gap visible until another
    // accepted action or resumed timer turn. The final cue has no trailing gap.
    stroke_elapsed_ms_ = StrokeDurationLocked(current_stroke_);
    if (current_stroke_ + 1U >= loaded_glyph_.size()) {
        state_ = StrokeOrderUiState::Completed;
        return true;
    }
    in_gap_ = true;
    state_ = StrokeOrderUiState::Paused;
    return true;
}

bool StrokeOrderController::BackToCandidates() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Hidden) {
        return false;
    }
    if (!ReadyLocked() || candidate_count_ == 0) {
        CancelPlaybackLocked();
        state_ = StrokeOrderUiState::Hidden;
        return false;
    }
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::Candidates;
    return true;
}

bool StrokeOrderController::Exit() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    state_ = StrokeOrderUiState::Hidden;
    return true;
}

bool StrokeOrderController::RetryLoad() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != StrokeOrderUiState::Error) {
        return false;
    }
    if (selected_codepoint_ == 0) {
        return false;
    }
    state_ = StrokeOrderUiState::Loading;
    if (!LoadSelectedLocked(selected_codepoint_)) {
        state_ = StrokeOrderUiState::Error;
        return false;
    }
    ResetPlaybackLocked();
    state_ = StrokeOrderUiState::Animating;
    return true;
}

void StrokeOrderController::Tick(uint32_t dt_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != StrokeOrderUiState::Animating) {
        return;
    }
    if (loaded_glyph_.empty()) {
        state_ = StrokeOrderUiState::Error;
        return;
    }

    // Only settle the phase present when this call began. Even a saturated
    // sample cannot consume any of the newly entered phase: every stroke's
    // start cue and completed contour/gap get separate presentation turns.
    const uint32_t credit = dt_ms > kMaxAnimationAdvanceMs ? kMaxAnimationAdvanceMs : dt_ms;
    if (credit == 0) {
        return;
    }
    if (in_gap_) {
        const uint32_t need = kGapMs - gap_elapsed_ms_;
        if (credit < need) {
            gap_elapsed_ms_ += credit;
            return;
        }
        in_gap_ = false;
        gap_elapsed_ms_ = 0;
        current_stroke_ = static_cast<uint16_t>(current_stroke_ + 1);
        stroke_elapsed_ms_ = 0;
        return;  // Present the next start cue at zero; discard gap overflow.
    }

    const uint32_t duration = StrokeDurationLocked(current_stroke_);
    if (duration == 0) {
        state_ = StrokeOrderUiState::Error;
        return;
    }
    const uint32_t need = duration - stroke_elapsed_ms_;
    if (credit < need) {
        stroke_elapsed_ms_ += credit;
        return;
    }
    stroke_elapsed_ms_ = duration;
    if (current_stroke_ + 1U >= loaded_glyph_.size()) {
        state_ = StrokeOrderUiState::Completed;
        return;
    }
    in_gap_ = true;
    gap_elapsed_ms_ = 0;
    // Present this full stroke. No cue overflow is credited to the gap.
}

void StrokeOrderController::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearDataLocked();
    entry_x_ = 0;
    entry_y_ = 0;
    entry_w_ = 0;
    entry_h_ = 0;
}

StrokeOrderUiState StrokeOrderController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool StrokeOrderController::IsExclusiveTouch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return OverlayOpenLocked();
}

bool StrokeOrderController::ShouldConsumePointer(int x, int y) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (OverlayOpenLocked()) {
        return true;
    }
    return PointInEntryLocked(x, y);
}

void StrokeOrderController::SetEntryHitRect(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    entry_x_ = x;
    entry_y_ = y;
    entry_w_ = w;
    entry_h_ = h;
}

void StrokeOrderController::ClearEntryHitRect() {
    std::lock_guard<std::mutex> lock(mutex_);
    entry_x_ = 0;
    entry_y_ = 0;
    entry_w_ = 0;
    entry_h_ = 0;
}

uint32_t StrokeOrderController::candidate_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return candidate_count_;
}

bool StrokeOrderController::GetCandidate(uint32_t index, uint32_t* codepoint) const {
    if (codepoint == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= candidate_count_) {
        return false;
    }
    *codepoint = candidates_[index];
    return true;
}

uint32_t StrokeOrderController::loaded_codepoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_codepoint_;
}

uint16_t StrokeOrderController::stroke_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint16_t>(loaded_glyph_.size());
}

uint16_t StrokeOrderController::current_stroke() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_stroke_;
}

uint16_t StrokeOrderController::completed_stroke_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Completed) {
        return static_cast<uint16_t>(loaded_glyph_.size());
    }
    if (in_gap_) {
        return static_cast<uint16_t>(current_stroke_ + 1);
    }
    return current_stroke_;
}

uint32_t StrokeOrderController::current_progress_permille() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Completed) {
        return kPermille;
    }
    if (in_gap_) {
        return kPermille;
    }
    const uint32_t duration = StrokeDurationLocked(current_stroke_);
    if (duration == 0) {
        return 0;
    }
    uint64_t permille = (uint64_t{stroke_elapsed_ms_} * kPermille) / duration;
    if (permille > kPermille) {
        permille = kPermille;
    }
    return static_cast<uint32_t>(permille);
}

bool StrokeOrderController::in_gap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_gap_;
}

#if defined(STROKE_ORDER_TESTING)
bool StrokeOrderController::GetStroke(uint16_t index, StrokeOrderStore::StrokeView* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.GetStroke(index, out);
}
#endif

bool StrokeOrderController::CopyLoadedGlyph(std::vector<DecodedStroke>* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *out = loaded_glyph_;
    return !out->empty();
}

bool StrokeOrderController::CopyCandidateGlyph(uint32_t index, uint32_t* codepoint,
                                               std::vector<DecodedStroke>* out) const {
    if (codepoint == nullptr || out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= candidate_count_) {
        return false;
    }
    const uint32_t candidate = candidates_[index];
    if (catalog_.is_bound()) {
        if (!LoadCatalogGlyphLocked(candidate, out)) {
            return false;
        }
    } else {
        StrokeOrderStore snapshot;
        if (owned_blob_ == nullptr || owned_blob_size_ == 0 ||
            !snapshot.Bind(owned_blob_.get(), owned_blob_size_) ||
            !snapshot.LoadCharacter(candidate) || !CopyGlyphLocked(&snapshot, out)) {
            return false;
        }
    }
    *codepoint = candidate;
    return true;
}

bool StrokeOrderController::CopyGlyphLocked(const StrokeOrderStore* store,
                                            std::vector<DecodedStroke>* out) {
    if (store == nullptr || out == nullptr || !store->has_character() ||
        store->stroke_count() == 0) {
        return false;
    }
    std::vector<DecodedStroke> local;
    local.resize(store->stroke_count());
    for (uint16_t s = 0; s < store->stroke_count(); ++s) {
        StrokeOrderStore::StrokeView stroke;
        if (!store->GetStroke(s, &stroke)) {
            return false;
        }
        local[s].outline.resize(stroke.outline_count());
        local[s].median.resize(stroke.median_count());
        for (uint16_t p = 0; p < stroke.outline_count(); ++p) {
            StrokeOrderStore::Point point;
            if (!stroke.GetOutlinePoint(p, &point)) {
                return false;
            }
            local[s].outline[p] = DecodedPoint{point.x, point.y};
        }
        for (uint16_t p = 0; p < stroke.median_count(); ++p) {
            StrokeOrderStore::Point point;
            if (!stroke.GetMedianPoint(p, &point)) {
                return false;
            }
            local[s].median[p] = DecodedPoint{point.x, point.y};
        }
    }
    *out = std::move(local);
    return !out->empty();
}

bool StrokeOrderController::ReadyLocked() const {
    return (catalog_.is_bound() && shard_source_ != nullptr && catalog_.character_count() > 0) ||
           (store_.is_bound() && store_.character_count() > 0);
}

bool StrokeOrderController::ContainsLocked(uint32_t codepoint) const {
    return catalog_.is_bound() ? catalog_.Contains(codepoint) : store_.Contains(codepoint);
}

bool StrokeOrderController::FilterCandidateLocked(uint32_t codepoint) {
    if (candidate_count_ >= StrokeOrderLayout::kMaxCandidates ||
        codepoint < StrokeOrderStore::kMinTargetCodepoint ||
        codepoint > StrokeOrderStore::kMaxTargetCodepoint || !ContainsLocked(codepoint)) {
        return false;
    }
    for (uint32_t j = 0; j < candidate_count_; ++j) {
        if (candidates_[j] == codepoint) {
            return false;
        }
    }
    std::vector<DecodedStroke> decoded;
    if (catalog_.is_bound()) {
        if (!LoadCatalogGlyphLocked(codepoint, &decoded)) {
            return false;
        }
    } else {
        StrokeOrderStore snapshot;
        if (owned_blob_ == nullptr || owned_blob_size_ == 0 ||
            !snapshot.Bind(owned_blob_.get(), owned_blob_size_) ||
            !snapshot.LoadCharacter(codepoint) || !CopyGlyphLocked(&snapshot, &decoded)) {
            return false;
        }
    }
    candidates_[candidate_count_] = codepoint;
    candidate_count_ += 1;
    return true;
}

bool StrokeOrderController::LoadSelectedLocked(uint32_t codepoint) {
    selected_codepoint_ = codepoint;
    std::vector<DecodedStroke> decoded;
    if (catalog_.is_bound()) {
        if (!LoadCatalogGlyphLocked(codepoint, &decoded)) {
            return false;
        }
    } else {
        if (!store_.Contains(codepoint) || !store_.LoadCharacter(codepoint) ||
            !CopyGlyphLocked(&store_, &decoded)) {
            return false;
        }
    }
    loaded_glyph_ = std::move(decoded);
    loaded_codepoint_ = codepoint;
#if defined(ESP_PLATFORM)
    size_t decoded_bytes = 0;
    for (const auto& stroke : loaded_glyph_) {
        decoded_bytes += stroke.outline.size() * sizeof(DecodedPoint);
        decoded_bytes += stroke.median.size() * sizeof(DecodedPoint);
    }
    ESP_LOGI(TAG, "glyph U+%04X decoded=%u bytes strokes=%u retained_shard_bytes=0",
             static_cast<unsigned>(codepoint), static_cast<unsigned>(decoded_bytes),
             static_cast<unsigned>(loaded_glyph_.size()));
#endif
    return !loaded_glyph_.empty();
}

bool StrokeOrderController::LoadCatalogGlyphLocked(uint32_t codepoint,
                                                   std::vector<DecodedStroke>* out) const {
    if (out == nullptr || shard_source_ == nullptr || !catalog_.is_bound()) {
        return false;
    }
    StrokeOrderCatalog::Entry entry;
    StrokeOrderCatalog::Shard shard;
    if (!catalog_.Find(codepoint, &entry) || !catalog_.GetShard(entry.shard, &shard)) {
        return false;
    }
    ScopedShardView view(shard_source_, shard.name);
    if (!view.valid() || view.size() != shard.size) {
        return false;
    }
    StrokeOrderStore transient;
    uint32_t actual = 0;
    return transient.Bind(view.data(), view.size()) &&
           transient.character_count() == shard.character_count &&
           transient.GetCodepointAt(entry.local_index, &actual) && actual == codepoint &&
           transient.LoadCharacter(codepoint) && CopyGlyphLocked(&transient, out);
}

bool StrokeOrderController::ValidateCatalogShardsLocked() const {
    if (!catalog_.is_bound() || shard_source_ == nullptr) {
        return false;
    }
    for (uint16_t shard_index = 0; shard_index < catalog_.shard_count(); ++shard_index) {
        StrokeOrderCatalog::Shard shard;
        if (!catalog_.GetShard(shard_index, &shard)) {
            return false;
        }
        ScopedShardView view(shard_source_, shard.name);
        if (!view.valid() || view.size() != shard.size ||
            StrokeOrderCatalog::CalculateCrc32(view.data(), view.size()) != shard.crc32) {
            return false;
        }
        StrokeOrderStore transient;
        if (!transient.Bind(view.data(), view.size()) ||
            transient.character_count() != shard.character_count) {
            return false;
        }
        for (uint32_t local = 0; local < transient.character_count(); ++local) {
            uint32_t codepoint = 0;
            StrokeOrderCatalog::Entry entry;
            if (!transient.GetCodepointAt(local, &codepoint) || !catalog_.Find(codepoint, &entry) ||
                entry.shard != shard_index || entry.local_index != local ||
                !transient.LoadCharacter(codepoint) || transient.stroke_count() == 0) {
                return false;
            }
        }
    }
    return true;
}

void StrokeOrderController::ClearDataLocked() {
    CancelPlaybackLocked();
    store_.Unbind();
    catalog_.Unbind();
    shard_source_ = nullptr;
    owned_blob_.reset();
    owned_blob_size_ = 0;
    catalog_blob_.reset();
    catalog_blob_size_ = 0;
    loaded_glyph_.clear();
    loaded_codepoint_ = 0;
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::Hidden;
}

void StrokeOrderController::ResetPlaybackLocked() {
    current_stroke_ = 0;
    stroke_elapsed_ms_ = 0;
    gap_elapsed_ms_ = 0;
    in_gap_ = false;
    has_last_step_ms_ = false;
    last_step_ms_ = 0;
}

void StrokeOrderController::CancelPlaybackLocked() { ResetPlaybackLocked(); }

uint32_t StrokeOrderController::StrokeDurationLocked(uint16_t index) const {
    return index < loaded_glyph_.size() ? kStartCueDurationMs : 0;
}

bool StrokeOrderController::OverlayOpenLocked() const {
    return state_ != StrokeOrderUiState::Hidden;
}

bool StrokeOrderController::PointInEntryLocked(int x, int y) const {
    if (entry_w_ <= 0 || entry_h_ <= 0) {
        return false;
    }
    return x >= entry_x_ && x < entry_x_ + entry_w_ && y >= entry_y_ && y < entry_y_ + entry_h_;
}
