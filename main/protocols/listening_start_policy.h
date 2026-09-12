#ifndef LISTENING_START_POLICY_H
#define LISTENING_START_POLICY_H

#include <cstdint>

/**
 * Host-testable policy for the side effects that start microphone streaming.
 *
 * Application owns the actual protocol/audio/UI operations and evaluates this
 * result only after preserving their production order. A nonzero generation is
 * a stroke round; generation zero is an ordinary listening request.
 */
struct ListeningStartResult {
    bool send_start_succeeded = false;
    bool voice_processing_enabled = false;
    bool audio_processor_running = false;
};

enum class ListeningStartDisposition : uint8_t {
    Proceed = 0,
    RecoverOrdinary,
    AbortStroke,
    AbandonFencedStroke,
};

inline constexpr ListeningStartDisposition EvaluateListeningStartResult(
    uint64_t generation, bool stroke_cancel_fenced, const ListeningStartResult& result) {
    if (generation != 0 && stroke_cancel_fenced) {
        return ListeningStartDisposition::AbandonFencedStroke;
    }
    if (!result.send_start_succeeded || !result.voice_processing_enabled ||
        !result.audio_processor_running) {
        return generation == 0 ? ListeningStartDisposition::RecoverOrdinary
                               : ListeningStartDisposition::AbortStroke;
    }
    return ListeningStartDisposition::Proceed;
}

#endif  // LISTENING_START_POLICY_H
