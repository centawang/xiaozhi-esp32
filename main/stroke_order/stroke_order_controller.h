#pragma once

#include "stroke_order/stroke_order_alloc.h"
#include "stroke_order/stroke_order_candidates.h"
#include "stroke_order/stroke_order_layout.h"
#include "stroke_order/stroke_order_store.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

enum class StrokeOrderUiState : uint8_t {
    Hidden = 0,
    Connecting,
    AwaitingSpeech,
    Candidates,
    Loading,
    Animating,
    Paused,
    Completed,
    Error,
    NoMatch,
    TimedOut,
};

/**
 * Local 笔划 session owner. No LVGL, no protocol, no DeviceStateMachine change.
 * BindStore makes a bounded owned copy of the SOB1 blob, so an Assets mmap may
 * be released immediately after BindStore returns. All store and animation
 * reads/writes take the controller mutex. Scale 0..1024 in the View.
 */
class StrokeOrderController {
public:
    struct DecodedPoint {
        uint16_t x = 0;
        uint16_t y = 0;
    };
    struct DecodedStroke {
        std::vector<DecodedPoint> outline;
        std::vector<DecodedPoint> median;
    };

    static constexpr uint32_t kMinStrokeMs = 280;
    static constexpr uint32_t kMaxStrokeMs = 900;
    static constexpr uint32_t kGapMs = 160;
    static constexpr uint32_t kStepDebounceMs = 120;
    static constexpr uint32_t kTickMs = 33;
    static constexpr uint32_t kMaxCandidateInputs = 24;
    static constexpr size_t kMaxOwnedBlobBytes = StrokeOrderStore::kMaxFileBytes;

    static StrokeOrderController& GetInstance();

    StrokeOrderController();
    StrokeOrderController(const StrokeOrderController&) = delete;
    StrokeOrderController& operator=(const StrokeOrderController&) = delete;

    bool BindStore(const uint8_t* data, size_t size);
    void Unbind();
    bool is_ready() const;

    bool Contains(uint32_t codepoint) const;
    bool SetCandidates(const uint32_t* codepoints, uint32_t count);
    bool SetCandidatesFromPrimary(uint32_t primary,
                                  const StrokeOrderCandidateProvider* provider = nullptr);
    bool EnterConnecting();
    bool EnterAwaitingSpeech();
    bool EnterNoMatch();
    bool EnterTimedOut();
    bool EnterError();
    bool OpenCandidates();
    bool SelectCandidate(uint32_t index);
    bool Pause();
    bool Resume();
    bool Replay();
    bool StepForward(uint64_t monotonic_ms);
    bool BackToCandidates();
    bool Exit();
    bool RetryLoad();
    void Tick(uint32_t dt_ms);
    void Shutdown();

    StrokeOrderUiState state() const;
    bool IsExclusiveTouch() const;
    bool ShouldConsumePointer(int x, int y) const;
    void SetEntryHitRect(int x, int y, int w, int h);
    void ClearEntryHitRect();

    uint32_t candidate_count() const;
    bool GetCandidate(uint32_t index, uint32_t* codepoint) const;
    uint32_t loaded_codepoint() const;
    uint16_t stroke_count() const;
    uint16_t current_stroke() const;
    uint16_t completed_stroke_count() const;
    uint32_t current_progress_permille() const;
    bool in_gap() const;
    // Host-test helper only. The StrokeView aliases blob bytes and is invalid
    // after Unbind/LoadCharacter. The LVGL renderer must use CopyLoadedGlyph().
    bool GetStroke(uint16_t index, StrokeOrderStore::StrokeView* out) const;
    bool CopyLoadedGlyph(std::vector<DecodedStroke>* out) const;
    bool CopyCandidateGlyph(uint32_t index, uint32_t* codepoint,
                            std::vector<DecodedStroke>* out) const;

private:
    bool FilterCandidateLocked(uint32_t codepoint);
    bool LoadSelectedLocked(uint32_t codepoint);
    static bool CopyGlyphLocked(const StrokeOrderStore* store, std::vector<DecodedStroke>* out);
    void ResetPlaybackLocked();
    void CancelPlaybackLocked();
    uint32_t StrokeDurationLocked(uint16_t index) const;
    uint32_t MedianLengthLocked(uint16_t index) const;
    bool OverlayOpenLocked() const;
    bool PointInEntryLocked(int x, int y) const;

    mutable std::mutex mutex_;
    StrokeOrderOwnedBlob owned_blob_;
    size_t owned_blob_size_ = 0;
    StrokeOrderStore store_;
    StrokeOrderUiState state_ = StrokeOrderUiState::Hidden;
    uint32_t candidates_[StrokeOrderLayout::kMaxCandidates] = {};
    uint32_t candidate_count_ = 0;
    uint32_t selected_codepoint_ = 0;
    uint16_t current_stroke_ = 0;
    uint32_t stroke_elapsed_ms_ = 0;
    uint32_t gap_elapsed_ms_ = 0;
    bool in_gap_ = false;
    bool has_last_step_ms_ = false;
    uint64_t last_step_ms_ = 0;
    int entry_x_ = 0;
    int entry_y_ = 0;
    int entry_w_ = 0;
    int entry_h_ = 0;
};
