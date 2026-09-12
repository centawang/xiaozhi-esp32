#include "stroke_order/stroke_order_controller.h"

#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_log.h>
static const char* TAG = "StrokeOrder";
#endif

namespace {

constexpr uint32_t kPermille = 1000;

uint32_t ApproxLength(StrokeOrderStore::Point a, StrokeOrderStore::Point b) {
    const int32_t dx = static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x);
    const int32_t dy = static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y);
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    if (adx > ady) {
        return static_cast<uint32_t>(adx + (ady / 2));
    }
    return static_cast<uint32_t>(ady + (adx / 2));
}

}  // namespace

StrokeOrderController& StrokeOrderController::GetInstance() {
    static StrokeOrderController instance;
    return instance;
}

StrokeOrderController::StrokeOrderController() = default;

bool StrokeOrderController::BindStore(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    store_.Unbind();
    owned_blob_.reset();
    owned_blob_size_ = 0;
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::Hidden;

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
        store_.Unbind();
        owned_blob_.reset();
        owned_blob_size_ = 0;
        candidate_count_ = 0;
        return false;
    }
    return true;
}

void StrokeOrderController::Unbind() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    store_.Unbind();
    owned_blob_.reset();
    owned_blob_size_ = 0;
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::Hidden;
}

bool StrokeOrderController::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.is_bound() && store_.character_count() > 0;
}

bool StrokeOrderController::Contains(uint32_t codepoint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.Contains(codepoint);
}

bool StrokeOrderController::SetCandidates(const uint32_t* codepoints, uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidate_count_ = 0;
    if (!store_.is_bound() || codepoints == nullptr || count == 0) {
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
    if (!store_.is_bound()) {
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
    if (!store_.is_bound()) {
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
    if (!store_.is_bound() || candidate_count_ == 0) {
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
    if (!store_.has_character()) {
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
    has_last_step_ms_ = true;
    last_step_ms_ = monotonic_ms;
    if (!store_.has_character() || store_.stroke_count() == 0) {
        return false;
    }
    if (in_gap_) {
        in_gap_ = false;
        gap_elapsed_ms_ = 0;
        current_stroke_ = static_cast<uint16_t>(current_stroke_ + 1);
        if (current_stroke_ >= store_.stroke_count()) {
            current_stroke_ = static_cast<uint16_t>(store_.stroke_count() - 1);
            stroke_elapsed_ms_ = StrokeDurationLocked(current_stroke_);
            state_ = StrokeOrderUiState::Completed;
            return true;
        }
    }
    stroke_elapsed_ms_ = StrokeDurationLocked(current_stroke_);
    if (current_stroke_ + 1 >= store_.stroke_count()) {
        state_ = StrokeOrderUiState::Completed;
        in_gap_ = false;
        return true;
    }
    in_gap_ = false;
    gap_elapsed_ms_ = 0;
    current_stroke_ = static_cast<uint16_t>(current_stroke_ + 1);
    stroke_elapsed_ms_ = 0;
    state_ = StrokeOrderUiState::Paused;
    return true;
}

bool StrokeOrderController::BackToCandidates() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Hidden) {
        return false;
    }
    if (!store_.is_bound() || candidate_count_ == 0) {
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
    if (!store_.has_character() || store_.stroke_count() == 0) {
        state_ = StrokeOrderUiState::Error;
        return;
    }
    if (in_gap_) {
        gap_elapsed_ms_ += dt_ms;
        if (gap_elapsed_ms_ < kGapMs) {
            return;
        }
        in_gap_ = false;
        gap_elapsed_ms_ = 0;
        current_stroke_ = static_cast<uint16_t>(current_stroke_ + 1);
        stroke_elapsed_ms_ = 0;
        if (current_stroke_ >= store_.stroke_count()) {
            current_stroke_ = static_cast<uint16_t>(store_.stroke_count() - 1);
            stroke_elapsed_ms_ = StrokeDurationLocked(current_stroke_);
            state_ = StrokeOrderUiState::Completed;
        }
        return;
    }
    stroke_elapsed_ms_ += dt_ms;
    const uint32_t duration = StrokeDurationLocked(current_stroke_);
    if (stroke_elapsed_ms_ < duration) {
        return;
    }
    stroke_elapsed_ms_ = duration;
    if (current_stroke_ + 1 >= store_.stroke_count()) {
        state_ = StrokeOrderUiState::Completed;
        return;
    }
    in_gap_ = true;
    gap_elapsed_ms_ = 0;
}

void StrokeOrderController::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelPlaybackLocked();
    store_.Unbind();
    owned_blob_.reset();
    owned_blob_size_ = 0;
    candidate_count_ = 0;
    selected_codepoint_ = 0;
    state_ = StrokeOrderUiState::Hidden;
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
    return store_.loaded_codepoint();
}

uint16_t StrokeOrderController::stroke_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.stroke_count();
}

uint16_t StrokeOrderController::current_stroke() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_stroke_;
}

uint16_t StrokeOrderController::completed_stroke_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == StrokeOrderUiState::Completed) {
        return store_.stroke_count();
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
    uint32_t permille = (stroke_elapsed_ms_ * kPermille) / duration;
    if (permille > kPermille) {
        permille = kPermille;
    }
    return permille;
}

bool StrokeOrderController::in_gap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_gap_;
}

bool StrokeOrderController::GetStroke(uint16_t index, StrokeOrderStore::StrokeView* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.GetStroke(index, out);
}

bool StrokeOrderController::CopyLoadedGlyph(std::vector<DecodedStroke>* out) const {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return CopyGlyphLocked(&store_, out);
}

bool StrokeOrderController::CopyCandidateGlyph(uint32_t index, uint32_t* codepoint,
                                               std::vector<DecodedStroke>* out) const {
    if (codepoint == nullptr || out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= candidate_count_ || owned_blob_ == nullptr || owned_blob_size_ == 0) {
        return false;
    }
    StrokeOrderStore snapshot;
    if (!snapshot.Bind(owned_blob_.get(), owned_blob_size_) ||
        !snapshot.LoadCharacter(candidates_[index]) || !CopyGlyphLocked(&snapshot, out)) {
        return false;
    }
    *codepoint = candidates_[index];
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

bool StrokeOrderController::FilterCandidateLocked(uint32_t codepoint) {
    if (candidate_count_ >= StrokeOrderLayout::kMaxCandidates) {
        return false;
    }
    if (codepoint < StrokeOrderStore::kMinTargetCodepoint ||
        codepoint > StrokeOrderStore::kMaxTargetCodepoint) {
        return false;
    }
    if (owned_blob_ == nullptr || owned_blob_size_ == 0 || !store_.Contains(codepoint)) {
        return false;
    }
    for (uint32_t j = 0; j < candidate_count_; ++j) {
        if (candidates_[j] == codepoint) {
            return false;
        }
    }
    StrokeOrderStore snapshot;
    if (!snapshot.Bind(owned_blob_.get(), owned_blob_size_) || !snapshot.LoadCharacter(codepoint) ||
        snapshot.stroke_count() == 0) {
        return false;
    }
    candidates_[candidate_count_] = codepoint;
    candidate_count_ += 1;
    return true;
}

bool StrokeOrderController::LoadSelectedLocked(uint32_t codepoint) {
    selected_codepoint_ = codepoint;
    if (!store_.Contains(codepoint) || !store_.LoadCharacter(codepoint)) {
        return false;
    }
    return store_.has_character() && store_.stroke_count() > 0;
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
    uint32_t length = MedianLengthLocked(index);
    if (length < kMinStrokeMs) {
        length = kMinStrokeMs;
    }
    if (length > kMaxStrokeMs) {
        length = kMaxStrokeMs;
    }
    return length;
}

uint32_t StrokeOrderController::MedianLengthLocked(uint16_t index) const {
    StrokeOrderStore::StrokeView stroke;
    if (!store_.GetStroke(index, &stroke) || stroke.median_count() < 2) {
        return kMinStrokeMs;
    }
    uint32_t length = 0;
    StrokeOrderStore::Point previous;
    if (!stroke.GetMedianPoint(0, &previous)) {
        return kMinStrokeMs;
    }
    for (uint16_t i = 1; i < stroke.median_count(); ++i) {
        StrokeOrderStore::Point point;
        if (!stroke.GetMedianPoint(i, &point)) {
            continue;
        }
        length += ApproxLength(previous, point);
        previous = point;
    }
    return length;
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
