#pragma once

#include "stroke_order_controller.h"
#include "stroke_order_session.h"
#include "stroke_round_coordinator.h"

// Display/LVGL-lock owned active clock, shared by timer and Pause admission.
// Only successful admission may settle elapsed time. Sync never rebases a live
// clock; Paused suspends the baseline, NOT the fractional active millisecond.
class StrokeOrderAnimationClock {
public:
    void Reset() {
        running_ = false;
        first_frame_pending_ = false;
        last_tick_us_ = 0;
        remainder_us_ = 0;
    }

    // A fresh selection/replay/retry presents cue0 before any timer may credit
    // time. One bounded token survives page rebuild and pre-timer Pause/Resume.
    void BeginPlayback() {
        Reset();
        first_frame_pending_ = true;
    }

    // Timer-only, inside successful admission. Give a delayed first callback a
    // cue-only redraw turn: no Tick, no old elapsed/fraction debt. This is NOT a
    // panel flush acknowledgement; LVGL still owns asynchronous presentation.
    void OnTimer(StrokeOrderController& controller, uint64_t now_us) {
        if (first_frame_pending_) {
            first_frame_pending_ = false;
            running_ = true;
            last_tick_us_ = now_us;
            remainder_us_ = 0;
            return;
        }
        Settle(controller, now_us);
    }

    void Sync(StrokeOrderUiState state, uint64_t now_us) {
        if (state == StrokeOrderUiState::Animating) {
            if (!running_) {
                last_tick_us_ = now_us;
                running_ = true;
            }
        } else if (state == StrokeOrderUiState::Paused) {
            running_ = false;
            last_tick_us_ = 0;
        } else {
            Reset();
        }
    }

    // Caller holds the same exact-generation/session/cancel-fence admission as
    // the impending controller mutation. Credit only the starting cue/gap phase,
    // rebase to this sample, and discard older whole-ms backlog. Retain only the
    // sub-ms fraction: stalls slow playback, never queue catch-up or skip strokes.
    void Settle(StrokeOrderController& controller, uint64_t now_us) {
        // Pre-arm Pause credits zero and must not consume the timer-only token.
        if (first_frame_pending_) {
            return;
        }
        if (running_ && now_us >= last_tick_us_) {
            const uint64_t delta_us = now_us - last_tick_us_;
            const uint32_t fraction_us = static_cast<uint32_t>(delta_us % 1000U) + remainder_us_;
            const uint64_t elapsed_ms = delta_us / 1000U + fraction_us / 1000U;
            last_tick_us_ = now_us;
            remainder_us_ = fraction_us % 1000U;
            constexpr uint32_t quantum_ms = StrokeOrderController::kMaxAnimationAdvanceMs;
            controller.Tick(
                static_cast<uint32_t>(elapsed_ms > quantum_ms ? quantum_ms : elapsed_ms));
        }
        Sync(controller.state(), now_us);
    }

    bool first_frame_pending() const { return first_frame_pending_; }
    bool running() const { return running_; }
    uint64_t last_tick_us() const { return last_tick_us_; }
    uint32_t remainder_us() const { return remainder_us_; }

private:
    bool running_ = false;
    bool first_frame_pending_ = false;
    uint64_t last_tick_us_ = 0;
    uint32_t remainder_us_ = 0;
};

// Shared production timing seam: a benign try-lock miss or stale session leaves
// BOTH baseline and remainder intact. No coordinator calls inside the callback.
inline bool StrokeOrderApplyAnimationTick(StrokeRoundCoordinator& coordinator,
                                          StrokeOrderController& controller,
                                          StrokeOrderSession& session,
                                          StrokeOrderAnimationClock& clock, uint64_t presented,
                                          uint64_t now_us) {
    return coordinator.TryUiAction(presented, StrokeRoundCoordinator::UiTransition::None, 0, [&]() {
        if (!session.IsCurrentGeneration(presented) ||
            controller.state() != StrokeOrderUiState::Animating) {
            return false;
        }
        clock.OnTimer(controller, now_us);
        return true;
    });
}

enum class StrokeOrderUiAction : uint8_t {
    Candidate,
    PauseContinue,
    Step,
    Replay,
    Back,
    Exit,
    RetryLoad,
    RetryVoice,
};

// The display lock owns the controller and presentation session. The coordinator
// lock additionally fences asynchronous cancellation across the ENTIRE mutation.
inline bool StrokeOrderApplyUiAction(StrokeRoundCoordinator& coordinator,
                                     StrokeOrderController& controller, StrokeOrderSession& session,
                                     StrokeOrderAnimationClock& clock, uint64_t presented,
                                     StrokeOrderUiAction action, uint64_t now_us,
                                     uint32_t candidate = 0) {
    using Transition = StrokeRoundCoordinator::UiTransition;
    const bool entering_playback = action == StrokeOrderUiAction::Candidate ||
                                   (action == StrokeOrderUiAction::RetryLoad &&
                                    session.phase() == StrokeOrderVoicePhase::Candidates);
    const auto transition = entering_playback                     ? Transition::Playback
                            : action == StrokeOrderUiAction::Back ? Transition::Candidates
                                                                  : Transition::None;
    const uint64_t now_ms = now_us / 1000U;
    return coordinator.TryUiAction(presented, transition, now_ms, [&]() {
        if (!session.IsCurrentGeneration(presented)) {
            return false;
        }
        const bool applied = [&]() {
            switch (action) {
                case StrokeOrderUiAction::Candidate:
                    if (session.phase() != StrokeOrderVoicePhase::Candidates ||
                        !controller.SelectCandidate(candidate)) {
                        return false;
                    }
                    session.MarkLocalPlayback();
                    return true;
                case StrokeOrderUiAction::PauseContinue:
                    if (controller.state() == StrokeOrderUiState::Paused) {
                        return controller.Resume();
                    }
                    if (controller.state() != StrokeOrderUiState::Animating) {
                        return false;
                    }
                    // Settle at most one visual quantum (including after missed
                    // timer admissions) to ONLY the starting phase before Pause,
                    // under this SAME admission; discard any boundary overflow.
                    // Only the final stroke may complete the glyph: present it, never
                    // force Completed/Error back to Paused or report a rejection
                    // after having already advanced the controller.
                    clock.Settle(controller, now_us);
                    return controller.state() != StrokeOrderUiState::Animating ||
                           controller.Pause();
                case StrokeOrderUiAction::Step:
                    return controller.StepForward(now_ms);
                case StrokeOrderUiAction::Replay:
                    return controller.Replay();
                case StrokeOrderUiAction::Back:
                    if (!controller.BackToCandidates()) {
                        return false;
                    }
                    session.MarkCandidates();
                    return true;
                case StrokeOrderUiAction::Exit:
                    if (controller.state() == StrokeOrderUiState::Hidden) {
                        return false;
                    }
                    controller.Exit();
                    return true;
                case StrokeOrderUiAction::RetryLoad:
                    if (!controller.RetryLoad()) {
                        return false;
                    }
                    session.MarkLocalPlayback();
                    return true;
                case StrokeOrderUiAction::RetryVoice:
                    return session.phase() == StrokeOrderVoicePhase::NoMatch ||
                           session.phase() == StrokeOrderVoicePhase::TimedOut ||
                           session.phase() == StrokeOrderVoicePhase::Error;
            }
            return false;
        }();
        if (applied) {
            if (action == StrokeOrderUiAction::Candidate || action == StrokeOrderUiAction::Replay ||
                action == StrokeOrderUiAction::RetryLoad) {
                clock.BeginPlayback();
            } else if (action != StrokeOrderUiAction::PauseContinue) {
                clock.Reset();
            }
            clock.Sync(controller.state(), now_us);
        }
        return applied;
    });
}
