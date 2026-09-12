#pragma once

#include <cstdint>

/**
 * Main/LVGL-task presentation state for one local 笔划 round. Network routing
 * is deliberately owned by StrokeRoundCoordinator; this class never decides
 * whether an incoming server message belongs to a round.
 */
enum class StrokeOrderVoicePhase : uint8_t {
    Inactive = 0,
    Connecting,
    AwaitingSpeech,
    Candidates,
    NoMatch,
    TimedOut,
    Error,
    LocalPlayback,
};

class StrokeOrderSession {
public:
    uint64_t generation() const { return generation_; }
    StrokeOrderVoicePhase phase() const { return phase_; }
    bool inactive() const { return phase_ == StrokeOrderVoicePhase::Inactive; }
    bool connecting() const { return phase_ == StrokeOrderVoicePhase::Connecting; }
    bool awaiting_speech() const { return phase_ == StrokeOrderVoicePhase::AwaitingSpeech; }
    bool overlay_active() const { return !inactive(); }

    bool BeginConnecting(uint64_t generation) {
        if (generation == 0) {
            return false;
        }
        generation_ = generation;
        phase_ = StrokeOrderVoicePhase::Connecting;
        return true;
    }

    bool MarkListeningReady(uint64_t expected_generation) {
        if (expected_generation == 0 || expected_generation != generation_ ||
            phase_ != StrokeOrderVoicePhase::Connecting) {
            return false;
        }
        phase_ = StrokeOrderVoicePhase::AwaitingSpeech;
        return true;
    }

    bool BeginAwaitingSpeech(uint64_t generation) {
        if (generation == 0) {
            return false;
        }
        generation_ = generation;
        phase_ = StrokeOrderVoicePhase::AwaitingSpeech;
        return true;
    }

    bool BeginLocalCandidates(uint64_t generation) {
        if (generation == 0) {
            return false;
        }
        generation_ = generation;
        phase_ = StrokeOrderVoicePhase::Candidates;
        return true;
    }

    bool AcceptStt(uint64_t expected_generation) {
        if (expected_generation == 0 || expected_generation != generation_ ||
            phase_ != StrokeOrderVoicePhase::AwaitingSpeech) {
            return false;
        }
        return true;
    }

    void MarkCandidates() { phase_ = StrokeOrderVoicePhase::Candidates; }
    void MarkNoMatch() { phase_ = StrokeOrderVoicePhase::NoMatch; }
    void MarkTimedOut() { phase_ = StrokeOrderVoicePhase::TimedOut; }
    void MarkError() { phase_ = StrokeOrderVoicePhase::Error; }
    void MarkLocalPlayback() { phase_ = StrokeOrderVoicePhase::LocalPlayback; }

    bool IsCurrentGeneration(uint64_t candidate) const {
        return candidate != 0 && candidate == generation_ && !inactive();
    }

    void Cancel() {
        generation_ = 0;
        phase_ = StrokeOrderVoicePhase::Inactive;
    }

private:
    uint64_t generation_ = 0;
    StrokeOrderVoicePhase phase_ = StrokeOrderVoicePhase::Inactive;
};
