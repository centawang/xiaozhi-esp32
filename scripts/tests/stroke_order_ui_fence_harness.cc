#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_ui_action.h"

#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

using Action = StrokeOrderUiAction;
using Phase = StrokeRoundCoordinator::Phase;

// UI-task snapshots around each admitted timer, Pause settlement or Step.
// These are state-machine presentation opportunities, not panel flush receipts.
struct Snapshot {
    uint16_t stroke, completed;
    uint32_t progress;
    bool gap, finished;

    static Snapshot Capture(const StrokeOrderController& c) {
        return {c.current_stroke(), c.completed_stroke_count(), c.current_progress_permille(),
                c.in_gap(), c.state() == StrokeOrderUiState::Completed};
    }
    unsigned phase() const { return 2u * stroke + (gap || finished ? 1u : 0u); }
};

uint32_t ProgressForMs(const StrokeOrderController& controller, uint32_t ms) {
    assert(controller.stroke_count() != 0);
    return static_cast<uint32_t>(uint64_t{ms} * 1000 / 150);
}

void AssertAdjacent(const Snapshot& before, const Snapshot& after, uint32_t duration) {
    assert(duration != 0);
    const uint64_t max_progress =
        (uint64_t{1000} * StrokeOrderController::kMaxAnimationAdvanceMs + duration - 1) / duration;
    assert(after.stroke >= before.stroke && after.stroke <= before.stroke + 1u);
    assert(after.completed >= before.completed && after.completed <= before.completed + 1u);
    assert(after.phase() >= before.phase() && after.phase() <= before.phase() + 1u);
    if (!before.gap && !before.finished) {
        assert(after.stroke == before.stroke);
        assert(after.progress >= before.progress);
        assert(after.progress - before.progress <= max_progress);
    } else if (after.stroke != before.stroke) {
        assert(!after.gap && !after.finished && after.progress == 0);
    }
}

struct Fixture {
    StrokeRoundCoordinator coordinator;
    StrokeOrderController controller;
    StrokeOrderSession session;
    StrokeOrderAnimationClock clock;
    uint64_t generation;
    int abort_requests = 0;
    bool clickable = true;

    explicit Fixture(const std::vector<uint8_t>& blob, uint32_t codepoint = 0x4EBA) {
        assert(controller.BindStore(blob.data(), blob.size()));
        const uint32_t candidates[] = {codepoint};
        assert(controller.SetCandidates(candidates, 1));
        assert(controller.OpenCandidates());
        generation = coordinator.BeginRound(1);
        assert(coordinator.MarkLocalCandidates(generation, 1));
        assert(session.BeginLocalCandidates(generation));
    }
    bool ApplyUs(Action action, uint64_t now_us) {
        const auto before = Snapshot::Capture(controller);
        const bool admitted = StrokeOrderApplyUiAction(coordinator, controller, session, clock,
                                                       generation, action, now_us);
        if (admitted && (action == Action::PauseContinue || action == Action::Step)) {
            AssertAdjacent(before, Snapshot::Capture(controller),
                           StrokeOrderController::kStartCueDurationMs);
        }
        return admitted;
    }
    bool Apply(Action action, uint64_t now = 1000) { return ApplyUs(action, now * 1000U); }
    bool TickUs(uint64_t now_us) {
        const auto before = Snapshot::Capture(controller);
        const bool admitted = StrokeOrderApplyAnimationTick(coordinator, controller, session, clock,
                                                            generation, now_us);
        if (admitted) {
            AssertAdjacent(before, Snapshot::Capture(controller),
                           StrokeOrderController::kStartCueDurationMs);
        }
        return admitted;
    }
    // Mirror the view's post-admission-only disarm/abort effects. Python source
    // checks below couple these assertions to the actual LVGL callback order.
    bool Click(Action action) {
        if (!Apply(action)) {
            return false;
        }
        if (action == Action::Candidate || action == Action::Exit) {
            clickable = false;
        }
        if (action == Action::Exit) {
            ++abort_requests;
        }
        return true;
    }
};

// Deterministic real coordinator contention, not a fake failed-admission flag.
// The holder never accesses controller/session/clock, which remain UI-task owned.
template <typename Attempt>
void WithCoordinatorHeld(Fixture& f, Attempt&& attempt) {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::thread holder([&]() {
        assert(f.coordinator.TryUiAction(f.generation, StrokeRoundCoordinator::UiTransition::None,
                                         0, [&]() {
                                             std::unique_lock<std::mutex> lock(mutex);
                                             entered = true;
                                             cv.notify_all();
                                             cv.wait(lock, [&]() { return release; });
                                             return true;
                                         }));
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return entered; });
    }
    attempt();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    }
    holder.join();
}

// Production-shaped startup regression: Candidate -> RenderAnimationPage's
// StopAnimTimer/Sync -> delayed first admitted timer -> one mutable canvas redraw.
// The initial invalidation may not have flushed before that timer runs.
void TestFirstCallback(const std::vector<uint8_t>& blob, uint64_t delay, uint64_t cue = 150000) {
    constexpr uint64_t start = 1000000;
    Fixture f(blob);
    assert(f.ApplyUs(Action::Candidate, start));
    // Mirror RenderAnimationPage -> StopAnimTimer(false): preserve the token.
    f.clock.Sync(f.controller.state(), start);
    auto canvas = Snapshot::Capture(f.controller);  // Cue drawn, not yet flushed.
    assert(f.TickUs(start + delay));
    canvas = Snapshot::Capture(f.controller);  // Mutable-buffer redraw before refresh.
    const auto panel_model = canvas;           // Only now does the fake refresh copy it.
    const auto first = Snapshot::Capture(f.controller);
    std::cerr << "first callback delay_us=" << delay << " stroke=" << first.stroke
              << " progress=" << first.progress << " done=" << first.completed
              << " gap=" << first.gap << '\n';
    assert(first.stroke == 0 && first.progress == 0 && first.completed == 0 && !first.gap &&
           !first.finished && "first admitted callback must leave exact cue0 unchanged");
    assert(panel_model.stroke == 0 && panel_model.progress == 0 && panel_model.completed == 0 &&
           !panel_model.gap);
    assert(f.TickUs(start + delay + cue));
    const auto second = Snapshot::Capture(f.controller);
    assert(second.stroke == 0 && second.progress == 1000 && second.completed == 1 && second.gap);
    std::cout << "stroke_order_first_callback: PASS\n";
}

// Multiple admitted presentation turns, with explicit phase boundaries in setup.
uint64_t AdvanceUs(Fixture& f, uint64_t now, uint64_t elapsed) {
    while (elapsed != 0) {
        const uint64_t step = elapsed > 33000 ? 33000 : elapsed;
        now += step;
        elapsed -= step;
        assert(f.TickUs(now));
    }
    return now;
}

// Independent cue/gap oracle. Uses only product constants, not the production
// duration helper or accumulated whole-glyph credit (boundary surplus is LOST).
uint64_t VerifyPlayback(const std::vector<uint8_t>& blob, uint32_t cp, unsigned count,
                        uint64_t interval_us) {
    constexpr uint64_t start = 1000000;
    Fixture f(blob, cp);
    assert(f.ApplyUs(Action::Candidate, start));
    assert(f.controller.stroke_count() == count);
    std::vector<bool> cue(count, false), gap(count, false);
    cue[0] = true;  // Candidate's synchronous redraw, before the first Tick.
    uint64_t elapsed = 0, previous = 0, remainder = 0;
    unsigned frame = 0, phase = 0, phase_ms = 0;
    while (f.controller.state() == StrokeOrderUiState::Animating) {
        ++frame;
        elapsed = interval_us ? uint64_t{frame} * interval_us : (uint64_t{frame} * 1000000 + 5) / 6;
        const auto before = Snapshot::Capture(f.controller);
        assert(f.TickUs(start + elapsed));
        const auto after = Snapshot::Capture(f.controller);
        const uint64_t delta = elapsed - previous + remainder;
        const auto credit = delta / 1000 > 160 ? 160 : delta / 1000;
        remainder = delta % 1000;
        previous = elapsed;
        phase_ms += frame == 1 ? 0 : credit;  // First admitted callback only arms.
        if (frame == 1)
            remainder = 0;
        if (phase_ms >= (phase % 2 ? 160u : 150u)) {
            ++phase;
            phase_ms = 0;  // Never carry overflow to the new phase.
        }
        assert(after.phase() == phase);
        assert(after.finished == (phase == 2 * count - 1));
        assert(after.stroke == (after.finished ? count - 1 : phase / 2));
        assert(after.gap == (!after.finished && phase % 2 == 1));
        assert(after.completed == (phase + 1) / 2);
        assert(after.progress == (phase % 2 ? 1000u : 1000u * phase_ms / 150u));
        if (after.completed > before.completed) {
            assert(cue[before.stroke] && "start-cue snapshot before every completion");
            assert(after.stroke == before.stroke && "snap only the current stroke");
        }
        if (after.gap)
            gap[after.stroke] = true;
        if (after.stroke > before.stroke) {
            assert(gap[before.stroke] && "non-final stroke requires a gap snapshot");
            assert(after.progress == 0 && "new stroke must present cue at zero elapsed");
            cue[after.stroke] = true;
        }
        if (!after.finished) {
            assert(f.TickUs(start + elapsed));
            assert(f.TickUs(start + elapsed - 1));  // backward/same never spends debt
            const auto same = Snapshot::Capture(f.controller);
            assert(same.phase() == after.phase() && same.progress == after.progress &&
                   same.completed == after.completed);
        }
        assert(frame <= count * 20);
    }
    for (unsigned i = 0; i < count; ++i)
        assert(cue[i] && (i == count - 1 || gap[i]));
    const uint64_t quantum = interval_us ? (interval_us > 160000 ? 160000 : interval_us) : 160000;
    const uint64_t cue_frames = (150000 + quantum - 1) / quantum;
    const uint64_t gap_frames = (160000 + quantum - 1) / quantum;
    assert(frame == 1 + count * cue_frames + (count - 1) * gap_frames);
    assert(!f.clock.running() && f.clock.remainder_us() == 0);
    assert(f.ApplyUs(Action::Replay, start + elapsed));
    assert(f.controller.current_stroke() == 0 && f.controller.current_progress_permille() == 0);
    std::cout << "glyph=" << cp << " strokes=" << count << " cue_ms=150 interval_us=" << interval_us
              << " frames=" << frame << " completion_us=" << elapsed << '\n';
    return elapsed;
}

void TestCueMatrix(const std::vector<uint8_t>& smoke, const std::vector<uint8_t>& shard,
                   const std::vector<uint8_t>& synthetic) {
    static_assert(StrokeOrderController::kTimerPeriodMs == 33);
    static_assert(StrokeOrderController::kMaxAnimationAdvanceMs == 160);
    static_assert(StrokeOrderController::kGapMs == 160);
    static_assert(StrokeOrderController::kStartCueDurationMs == 150);
    StrokeOrderController empty;
    assert(empty.stroke_count() == 0 && empty.current_progress_permille() == 0);
    empty.Tick(UINT32_MAX);
    assert(empty.state() == StrokeOrderUiState::Hidden && !empty.Replay());
    for (uint64_t interval :
         {uint64_t{33000}, uint64_t{50000}, uint64_t{100000}, uint64_t{150000}, uint64_t{160000},
          uint64_t{0}, uint64_t{200000}, uint64_t{5000000}, uint64_t{5000000000000}}) {
        VerifyPlayback(smoke, 0x4E00, 1, interval);
        VerifyPlayback(smoke, 0x4EBA, 2, interval);
        VerifyPlayback(smoke, 0x53E3, 3, interval);
        const auto shun = VerifyPlayback(shard, 0x987A, 9, interval);
        if (interval == 0)
            assert(shun == 3000000);
        if (interval == 33000)
            assert(shun == 2838000);
        if (interval == 200000)
            assert(shun == 3600000);
        for (unsigned count = 1; count <= 48; ++count)
            VerifyPlayback(synthetic, 0x4E00 + count, count, interval);
    }
    std::cout << "stroke_order_cue_matrix: PASS\n";
}

void TestDelayedSequentialRegression(const std::vector<uint8_t>& blob) {
    constexpr uint64_t start = 1000000;
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        AdvanceUs(f, start, 50250);
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 50));
        if (mode != 0)
            WithCoordinatorHeld(f, [&]() {
                assert(!f.TickUs(start + 1000000));
                assert(f.clock.last_tick_us() == start + 50250 && f.clock.remainder_us() == 250);
            });
        assert(mode == 2 ? f.ApplyUs(Action::PauseContinue, start + 5000600)
                         : f.TickUs(start + 5000600));
        assert(f.controller.current_stroke() == 0 && f.controller.completed_stroke_count() == 1);
        assert(f.controller.in_gap() && f.clock.remainder_us() == 600);
        std::cerr << "mode=" << mode << " done=0->1\n";
        auto now = start + 5000600;
        if (mode == 2)
            assert(f.ApplyUs(Action::PauseContinue, now += 10000000));
        assert(f.TickUs(now));  // no debt after either timer or Pause settlement
        now = AdvanceUs(f, now, 159399);
        assert(f.controller.in_gap());
        assert(f.TickUs(++now));  // all 160ms of the gap must still be credited
        assert(f.controller.current_stroke() == 1 && f.controller.current_progress_permille() == 0);
    }
    std::cout << "stroke_order_delayed_sequential: PASS\n";
}

void TestSequentialMatrix(const std::vector<uint8_t>& blob) {
    constexpr uint64_t start = 1000000;
    // Both boundary overflows must be discarded, even near a boundary.
    for (uint64_t near_ms : {uint64_t{0}, uint64_t{1}, uint64_t{149}}) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        auto now = AdvanceUs(f, start, near_ms * 1000);
        assert(f.TickUs(now += 5000000));
        assert(f.controller.in_gap() && f.controller.current_stroke() == 0);
        assert(f.TickUs(now));
        now = AdvanceUs(f, now, 159000);
        assert(f.controller.in_gap());
        assert(f.TickUs(now += 5000000));
        assert(f.controller.current_stroke() == 1 && !f.controller.in_gap());
        assert(f.controller.current_progress_permille() == 0);
        assert(f.TickUs(now));
        assert(f.controller.current_progress_permille() == 0);
        now = AdvanceUs(f, now, 149000);
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 149));
        assert(f.TickUs(now += 1000));
        assert(f.controller.state() == StrokeOrderUiState::Completed);
    }
    {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        assert(f.TickUs(UINT64_MAX));
        assert(f.controller.in_gap() && f.controller.current_stroke() == 0);
        assert(f.clock.last_tick_us() == UINT64_MAX && f.clock.remainder_us() == 615);
        assert(f.TickUs(UINT64_MAX));
        assert(f.TickUs(UINT64_MAX - 1));
        assert(f.controller.in_gap() && f.controller.current_stroke() == 0 &&
               f.clock.remainder_us() == 615);
    }
    std::cout << "stroke_order_sequential_matrix: PASS\n";
}

void TestPauseClock(const std::vector<uint8_t>& blob) {
    constexpr uint64_t start = 1000000, resume = 10000000;
    {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        AdvanceUs(f, start, 50250);
        WithCoordinatorHeld(f, [&]() {
            assert(!f.TickUs(start + 80500));
            assert(f.clock.last_tick_us() == start + 50250 && f.clock.remainder_us() == 250);
        });
        assert(f.ApplyUs(Action::PauseContinue, start + 100600));
        assert(f.controller.state() == StrokeOrderUiState::Paused && !f.controller.in_gap());
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 100));
        assert(!f.clock.running() && f.clock.last_tick_us() == 0 && f.clock.remainder_us() == 600);
        f.clock.Sync(f.controller.state(), start + 900000);
        assert(!f.TickUs(start + 2000000));
        assert(f.ApplyUs(Action::PauseContinue, resume));
        f.clock.Sync(f.controller.state(), resume + 10000);
        assert(f.clock.last_tick_us() == resume && f.clock.remainder_us() == 600);
        auto now = AdvanceUs(f, resume, 48399);
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 148));
        assert(f.clock.remainder_us() == 999);
        assert(f.TickUs(++now));
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 149));
        assert(f.TickUs(now += 1000));
        assert(f.controller.current_stroke() == 0 && f.controller.in_gap());
        now = AdvanceUs(f, now, 159999);
        assert(f.controller.in_gap());
        assert(f.TickUs(++now));
        assert(f.controller.current_stroke() == 1 && f.controller.current_progress_permille() == 0);
        now = AdvanceUs(f, now, 149999);
        assert(f.controller.state() == StrokeOrderUiState::Animating);
        assert(f.TickUs(++now));
        assert(f.controller.state() == StrokeOrderUiState::Completed);
        assert(!f.clock.running() && f.clock.remainder_us() == 0);
    }
    // Pause at/near both boundaries, after a real miss. Never enter two phases.
    for (bool in_gap : {false, true}) {
        for (uint64_t advance : {uint64_t{0}, uint64_t{10000}, uint64_t{5000000}}) {
            Fixture f(blob);
            assert(f.ApplyUs(Action::Candidate, start));
            assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
            auto now = start;
            if (in_gap)
                now = AdvanceUs(f, now, 150000);
            now = AdvanceUs(f, now, in_gap ? 150125 : 140125);
            WithCoordinatorHeld(f, [&]() { assert(!f.TickUs(now + 1000)); });
            assert(f.ApplyUs(Action::PauseContinue, now + advance));
            assert(f.controller.state() == StrokeOrderUiState::Paused);
            const bool crossed = advance >= 10000;
            assert(f.controller.current_stroke() == (in_gap && crossed ? 1 : 0));
            assert(f.controller.in_gap() == (in_gap != crossed));
            assert(f.clock.remainder_us() == 125);
            assert(f.ApplyUs(Action::PauseContinue, resume));
            if (crossed && in_gap) {
                assert(f.controller.current_progress_permille() == 0);
                AdvanceUs(f, resume, 149874);
                assert(f.controller.current_progress_permille() ==
                       ProgressForMs(f.controller, 149));
            } else if (crossed) {
                AdvanceUs(f, resume, 159874);
                assert(f.controller.in_gap() && f.controller.current_stroke() == 0);
                assert(f.TickUs(resume + 159875));
                assert(f.controller.current_stroke() == 1 &&
                       f.controller.current_progress_permille() == 0);
            }
            assert(f.ApplyUs(Action::Replay, resume + 200000));
            assert(f.clock.last_tick_us() == resume + 200000 && f.clock.remainder_us() == 0);
            assert(f.controller.current_stroke() == 0 &&
                   f.controller.current_progress_permille() == 0);
        }
    }
    for (uint32_t cp : {0x4E00u, 0x4EBAu}) {
        for (uint64_t elapsed : {uint64_t{5000000}, uint64_t{5000999}, UINT64_MAX - start}) {
            Fixture f(blob, cp);
            assert(f.ApplyUs(Action::Candidate, start));
            assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
            auto now = start;
            if (cp == 0x4EBA) {
                now = AdvanceUs(f, now, 150000);
                now = AdvanceUs(f, now, 160000);
            }
            now = AdvanceUs(f, now, 149125);
            assert(f.controller.current_stroke() + 1u == f.controller.stroke_count());
            WithCoordinatorHeld(f, [&]() { assert(!f.TickUs(now + 1000)); });
            assert(f.ApplyUs(Action::PauseContinue, start + elapsed));
            assert(f.controller.state() == StrokeOrderUiState::Completed);
            assert(f.controller.current_progress_permille() == 1000);
            assert(!f.clock.running() && f.clock.last_tick_us() == 0 &&
                   f.clock.remainder_us() == 0);
            assert(!f.ApplyUs(Action::PauseContinue, start + elapsed));
        }
    }
    {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        AdvanceUs(f, start, 50333);
        WithCoordinatorHeld(f, [&]() {
            assert(!f.TickUs(start + 70000));
            assert(!f.ApplyUs(Action::PauseContinue, start + 80777));
            assert(f.clickable && f.controller.state() == StrokeOrderUiState::Animating);
            assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 50));
            assert(f.clock.last_tick_us() == start + 50333 && f.clock.remainder_us() == 333);
        });
        assert(f.ApplyUs(Action::PauseContinue, start + 100888));
        assert(f.controller.state() == StrokeOrderUiState::Paused);
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 100) &&
               f.clock.remainder_us() == 888);
        assert(f.coordinator.PublishCancelFence(f.generation));
        assert(!f.ApplyUs(Action::PauseContinue, resume));
        assert(f.controller.state() == StrokeOrderUiState::Paused && f.clock.remainder_us() == 888);
    }
    for (bool cancel : {false, true}) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start));  // Fresh epoch arm-only turn, before timed setup.
        AdvanceUs(f, start, 50333);
        WithCoordinatorHeld(f, [&]() { assert(!f.TickUs(start + 70000)); });
        if (cancel)
            assert(f.coordinator.PublishCancelFence(f.generation));
        else
            f.session.Cancel();
        assert(!f.ApplyUs(Action::PauseContinue, start + 2000000));
        assert(!f.TickUs(start + 2000000));
        assert(f.controller.state() == StrokeOrderUiState::Animating);
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 50));
        assert(f.clock.running() && f.clock.last_tick_us() == start + 50333 &&
               f.clock.remainder_us() == 333);
        assert(f.clickable && f.abort_requests == 0);
    }
    {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, 0));
        assert(f.TickUs(0));  // Fresh epoch arm-only turn, before timed setup.
        assert(f.TickUs(999));
        assert(f.controller.current_progress_permille() == 0 && f.clock.remainder_us() == 999);
        assert(f.TickUs(999));
        assert(f.TickUs(998));
        assert(f.clock.last_tick_us() == 999 && f.clock.remainder_us() == 999);
        assert(f.ApplyUs(Action::PauseContinue, 999));
        assert(f.ApplyUs(Action::PauseContinue, resume));
        assert(f.TickUs(resume + 1));
        assert(f.controller.current_progress_permille() == ProgressForMs(f.controller, 1) &&
               f.clock.remainder_us() == 0);
        assert(f.ApplyUs(Action::Step, resume + 1000));
        assert(f.controller.completed_stroke_count() == 1 && f.controller.in_gap() &&
               f.controller.current_stroke() == 0 &&
               f.controller.current_progress_permille() == 1000);
        assert(!f.ApplyUs(Action::Step, resume + 2000));
        assert(f.ApplyUs(Action::Step, resume + 121000));
        assert(f.controller.state() == StrokeOrderUiState::Paused && !f.controller.in_gap() &&
               f.controller.current_stroke() == 1 && f.controller.current_progress_permille() == 0);
        assert(f.ApplyUs(Action::Step, resume + 241000));
        assert(f.controller.state() == StrokeOrderUiState::Completed);
        assert(f.ApplyUs(Action::Back, resume + 121000));
        assert(!f.clock.running() && f.clock.remainder_us() == 0);
    }
    std::cout << "stroke_order_pause_clock: PASS\n";
}

// Exact production-shaped red regression: Candidate itself presents cue0.
// Every subsequent boundary must alternate full stroke/gap and the NEXT cue0.
void TestStartCueSnapRegression(const std::vector<uint8_t>& shard) {
    constexpr uint64_t start = 1000000;
    Fixture f(shard, 0x987A);
    assert(f.ApplyUs(Action::Candidate, start));
    assert(f.controller.loaded_codepoint() == 0x987A && f.controller.stroke_count() == 9);
    assert(f.controller.current_stroke() == 0 && f.controller.current_progress_permille() == 0);
    bool cue[9] = {true};
    for (unsigned callback = 1; callback <= 18; ++callback) {
        const auto before = Snapshot::Capture(f.controller);
        assert(f.TickUs(start + uint64_t{callback} * 160000));
        const auto after = Snapshot::Capture(f.controller);
        if (callback == 1) {
            assert(after.stroke == 0 && after.progress == 0 && after.completed == 0 && !after.gap &&
                   !after.finished);
            continue;
        }
        const unsigned frame = callback - 1;
        std::cerr << "shun snap frame=" << frame << " stroke=" << after.stroke
                  << " progress=" << after.progress << " done=" << after.completed << '\n';
        assert(after.stroke <= before.stroke + 1u && after.completed <= before.completed + 1u);
        if (frame % 2 == 1) {
            assert(after.stroke == frame / 2 && after.progress == 1000 &&
                   after.completed == frame / 2 + 1 &&
                   "after one start-cue interval the current stroke should snap complete");
            assert(cue[after.stroke] && "start-cue snapshot before every completion");
            assert(after.gap == (frame != 17) && after.finished == (frame == 17));
        } else {
            assert(before.gap && !after.gap && !after.finished);
            assert(after.stroke == frame / 2 && after.progress == 0 &&
                   after.completed == frame / 2 && "gap must enter the next start cue at zero");
            cue[after.stroke] = true;
        }
        assert(after.phase() == before.phase() + 1u && "exactly one boundary per 160ms update");
    }
    for (bool shown : cue)
        assert(shown);
    std::cout << "stroke_order_start_cue_snap: PASS\n";
}

// Exact production/controller/coordinator/session Step regression. Run each
// entry phase independently so the old implementation fails BOTH red cases.
void TestStepRegression(const std::vector<uint8_t>& blob, bool from_gap) {
    for (bool paused : {false, true}) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, 1000000));
        assert(f.TickUs(1000000));  // Fresh epoch arm-only turn, before timed setup.
        if (from_gap)
            assert(f.TickUs(1150000));
        if (paused)
            assert(f.ApplyUs(Action::PauseContinue, from_gap ? 1150000 : 1000000));
        assert(f.ApplyUs(Action::Step, 2000000));
        const auto after = Snapshot::Capture(f.controller);
        std::cerr << "step from_gap=" << from_gap << " paused=" << paused
                  << " stroke=" << after.stroke << " progress=" << after.progress
                  << " done=" << after.completed << " gap=" << after.gap
                  << " finished=" << after.finished << '\n';
        if (from_gap) {
            assert(after.stroke == 1 && after.progress == 0 && after.completed == 1 && !after.gap &&
                   !after.finished && "gap-Step presents next cue, NOT its completion");
        } else {
            assert(after.stroke == 0 && after.progress == 1000 && after.completed == 1 &&
                   after.gap && !after.finished && "cue-Step presents same stroke full/gap");
        }
        assert(f.controller.state() == StrokeOrderUiState::Paused);
        assert(f.coordinator.CurrentPhase() == Phase::LocalPlayback);
        assert(f.session.phase() == StrokeOrderVoicePhase::LocalPlayback);
        assert(f.session.IsCurrentGeneration(f.generation));
        assert(!f.clock.running() && f.clock.last_tick_us() == 0 && f.clock.remainder_us() == 0);
    }
    std::cout << (from_gap ? "stroke_order_step_gap: PASS\n" : "stroke_order_step_cue: PASS\n");
}

// Rejected actions must preserve the entire observable UI/session/clock state.
struct StepSnapshot {
    Snapshot ui;
    StrokeOrderUiState state;
    StrokeOrderVoicePhase voice;
    Phase phase;
    bool current, running, pending;
    uint64_t generation, last_tick;
    uint32_t remainder;

    static StepSnapshot Capture(Fixture& f) {
        return {Snapshot::Capture(f.controller),
                f.controller.state(),
                f.session.phase(),
                f.coordinator.CurrentPhase(),
                f.session.IsCurrentGeneration(f.generation),
                f.clock.running(),
                f.clock.first_frame_pending(),
                f.coordinator.CurrentGeneration(),
                f.clock.last_tick_us(),
                f.clock.remainder_us()};
    }
    void AssertUnchanged(Fixture& f, bool coordinator_locked = false) const {
        const auto after = Snapshot::Capture(f.controller);
        assert(after.stroke == ui.stroke && after.completed == ui.completed &&
               after.progress == ui.progress && after.gap == ui.gap &&
               after.finished == ui.finished);
        assert(f.controller.state() == state && f.session.phase() == voice);
        assert(f.session.IsCurrentGeneration(f.generation) == current);
        assert(f.clock.first_frame_pending() == pending && f.clock.running() == running &&
               f.clock.last_tick_us() == last_tick && f.clock.remainder_us() == remainder);
        if (!coordinator_locked) {
            assert(f.coordinator.CurrentPhase() == phase);
            assert(f.coordinator.CurrentGeneration() == generation);
        }
    }
};

void AssertCueZero(const Fixture& f) {
    const auto ui = Snapshot::Capture(f.controller);
    assert(ui.stroke == 0 && ui.progress == 0 && ui.completed == 0 && !ui.gap && !ui.finished);
}

void TestStartupGate(const std::vector<uint8_t>& blob) {
    constexpr uint64_t start = 1000000;
    // Every fresh playback path must issue exactly one token, on either a new
    // canvas or the existing Replay canvas. Retry runs in BOTH allowed phases.
    for (int entry = 0; entry < 7; ++entry) {
        Fixture f(blob);
        uint64_t now = start;
        assert(f.ApplyUs(Action::Candidate, now));
        if (entry != 0) {
            assert(f.TickUs(now));
            assert(f.TickUs(now += 33999));
            assert(!f.clock.first_frame_pending() && f.clock.remainder_us() == 999);
            if (entry == 1) {
                assert(f.ApplyUs(Action::Back, now));
                assert(!f.clock.first_frame_pending() && !f.clock.running());
                const uint32_t candidates[] = {0x53E3};  // Back -> different selection.
                assert(f.controller.SetCandidates(candidates, 1));
                assert(f.ApplyUs(Action::Candidate, now));
                assert(f.controller.loaded_codepoint() == 0x53E3);
            } else if (entry <= 4) {
                if (entry == 3)
                    assert(f.ApplyUs(Action::PauseContinue, now));
                if (entry == 4) {
                    while (f.controller.state() == StrokeOrderUiState::Animating)
                        assert(f.TickUs(now += 160000));
                    assert(!f.clock.first_frame_pending() && !f.clock.running());
                }
                assert(f.ApplyUs(Action::Replay, now));
            } else {
                if (entry == 6) {
                    assert(f.ApplyUs(Action::Back, now));
                    // Retain a selected codepoint while modelling load failure
                    // before the session/coordinator enter LocalPlayback.
                    assert(f.controller.SelectCandidate(0));
                }
                assert(f.controller.EnterError());
                f.clock.Sync(f.controller.state(), now);  // Error page stops timer.
                assert(!f.clock.first_frame_pending() && !f.clock.running());
                assert(f.ApplyUs(Action::RetryLoad, now));
            }
        }
        AssertCueZero(f);
        assert(f.clock.first_frame_pending() && f.clock.running());
        assert(f.clock.last_tick_us() == now && f.clock.remainder_us() == 0);
        // RenderAnimationPage stops the timer with reset_clock=false; Replay
        // takes the existing-canvas path. Neither ordinary Sync may rearm it.
        f.clock.Sync(f.controller.state(), now + 90000);
        assert(f.clock.first_frame_pending() && f.clock.last_tick_us() == now);
        assert(f.TickUs(now += 5000000));
        AssertCueZero(f);
        assert(!f.clock.first_frame_pending() && f.clock.last_tick_us() == now &&
               f.clock.remainder_us() == 0);
        f.clock.Sync(f.controller.state(), now + 10000);
        assert(!f.clock.first_frame_pending() && f.clock.last_tick_us() == now);
        assert(f.TickUs(now) && f.TickUs(now - 1));
        AssertCueZero(f);
        assert(f.TickUs(now += 150000));
        assert(f.controller.in_gap() && f.controller.completed_stroke_count() == 1);
        assert(f.TickUs(now += 160000));
        assert(!f.controller.in_gap() && f.controller.current_stroke() == 1 &&
               f.controller.current_progress_permille() == 0);
        // No additional arm turn at later cues.
        assert(f.TickUs(now += 150000));
        assert(f.controller.completed_stroke_count() == 2);
    }
    for (bool pause_first : {false, true}) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        auto now = start + 5000000;
        auto before = StepSnapshot::Capture(f);
        WithCoordinatorHeld(f, [&]() {
            assert(!f.TickUs(now));
            assert(!f.ApplyUs(Action::PauseContinue, now));
            before.AssertUnchanged(f, true);
        });
        before.AssertUnchanged(f);
        if (pause_first) {
            assert(f.ApplyUs(Action::PauseContinue, now));
            AssertCueZero(f);
            assert(f.controller.state() == StrokeOrderUiState::Paused);
            assert(f.clock.first_frame_pending() && !f.clock.running() &&
                   f.clock.remainder_us() == 0);
            before = StepSnapshot::Capture(f);
            assert(!f.TickUs(now + 60000000));
            before.AssertUnchanged(f);
            assert(f.ApplyUs(Action::PauseContinue, now += 60000000));
            assert(f.clock.first_frame_pending() && f.clock.last_tick_us() == now);
        }
        // Same timestamp retry after real miss still only arms. Post-resume
        // callback may itself be late; paused or pre-arm time is never credited.
        assert(f.TickUs(pause_first ? now += 5000000 : now));
        AssertCueZero(f);
        assert(!f.clock.first_frame_pending() && f.clock.last_tick_us() == now);
        assert(f.TickUs(now) && f.TickUs(now - 1));
        AssertCueZero(f);
        assert(f.TickUs(now += 149999));
        assert(f.controller.completed_stroke_count() == 0 && f.clock.remainder_us() == 999);
        assert(f.ApplyUs(Action::PauseContinue, now));
        assert(f.ApplyUs(Action::PauseContinue, now += 60000000));
        assert(!f.clock.first_frame_pending() && f.clock.remainder_us() == 999);
        f.clock.Sync(f.controller.state(), now + 10000);
        assert(f.TickUs(now += 1));
        assert(f.controller.in_gap() && f.controller.completed_stroke_count() == 1 &&
               f.clock.remainder_us() == 0);
    }
    // Every rejection preserves the pending bit AND all clock/controller data.
    for (int rejection = 0; rejection < 4; ++rejection) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        if (rejection == 0)
            assert(f.coordinator.PublishCancelFence(f.generation));
        if (rejection == 1)
            f.session.Cancel();
        if (rejection == 2)
            assert(f.coordinator.BeginRound(2) != f.generation);
        if (rejection == 3)
            assert(f.controller.Pause());  // invalid state for automatic admission
        const auto before = StepSnapshot::Capture(f);
        assert(!f.TickUs(start + 5000000));
        before.AssertUnchanged(f);
        if (rejection != 3) {
            assert(!f.ApplyUs(Action::Replay, start + 5000000));
            assert(!f.ApplyUs(Action::Step, start + 5000000));
            before.AssertUnchanged(f);
        }
    }
    for (Action action : {Action::Step, Action::Back, Action::Exit}) {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        const auto before = StepSnapshot::Capture(f);
        WithCoordinatorHeld(f, [&]() {
            assert(!f.ApplyUs(action, start + 5000000));
            before.AssertUnchanged(f, true);
        });
        assert(f.ApplyUs(action, start + 5000000));
        assert(!f.clock.first_frame_pending() && !f.clock.running() && f.clock.remainder_us() == 0);
        if (action == Action::Step) {
            assert(f.controller.in_gap() && f.controller.completed_stroke_count() == 1);
            assert(f.ApplyUs(Action::PauseContinue, start + 6000000));
            assert(!f.clock.first_frame_pending());
            assert(f.TickUs(start + 6160000));  // Resume is not a fresh epoch.
            assert(f.controller.current_stroke() == 1 && !f.controller.in_gap());
            assert(f.ApplyUs(Action::Replay, start + 6160000));
            assert(f.clock.first_frame_pending());
        }
    }
    {
        Fixture f(blob, 0x4E00);  // One-stroke completion also requires two turns.
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.TickUs(start + 5000000));
        AssertCueZero(f);
        assert(f.TickUs(start + 5160000));
        assert(f.controller.state() == StrokeOrderUiState::Completed && !f.controller.in_gap());
        assert(!f.clock.first_frame_pending() && !f.clock.running());
    }
    {
        Fixture f(blob);
        assert(f.controller.EnterError());  // no selected glyph to retry
        const auto before = StepSnapshot::Capture(f);
        assert(!f.ApplyUs(Action::RetryLoad, start));
        before.AssertUnchanged(f);
        assert(!f.clock.first_frame_pending() && !f.clock.running());
        f.session.MarkNoMatch();
        assert(f.ApplyUs(Action::RetryVoice, start));
        assert(!f.clock.first_frame_pending() && !f.clock.running());
    }
    {
        Fixture f(blob);
        assert(f.ApplyUs(Action::Candidate, start));
        f.clock.Reset();  // View StopAnimTimer default on delete/session teardown.
        f.session.Cancel();
        assert(f.controller.Exit());
        const auto generation = f.coordinator.BeginRound(2000);
        assert(generation != f.generation && f.session.BeginConnecting(generation));
        assert(!f.clock.first_frame_pending() && !f.clock.running());
        assert(!f.TickUs(start + 5000000));
        assert(!f.clock.first_frame_pending());
    }
    std::cout << "stroke_order_startup_gate: PASS\n";
}

void VerifyManualSteps(const std::vector<uint8_t>& blob, uint32_t cp, unsigned count) {
    for (bool animating : {false, true}) {
        Fixture f(blob, cp);
        uint64_t now = 1000000;
        assert(f.ApplyUs(Action::Candidate, now));
        assert(f.TickUs(now));  // Fresh epoch arm-only turn, before timed setup.
        assert(f.controller.stroke_count() == count);
        // A fractional active clock must also be discarded by every accepted Step.
        assert(f.TickUs(now += 33999));
        if (!animating)
            assert(f.ApplyUs(Action::PauseContinue, now));
        std::vector<bool> cue(count, false), gap(count, false);
        cue[0] = true;
        for (unsigned phase = 0; phase < 2 * count - 1; ++phase) {
            const auto before = Snapshot::Capture(f.controller);
            assert(before.phase() == phase);
            assert(f.controller.state() ==
                   (animating ? StrokeOrderUiState::Animating : StrokeOrderUiState::Paused));
            assert(f.ApplyUs(Action::Step, now));
            const auto after = Snapshot::Capture(f.controller);
            AssertAdjacent(before, after, 150);
            assert(after.phase() == phase + 1);
            assert(after.finished == (phase + 1 == 2 * count - 1));
            if (before.gap) {
                assert(gap[before.stroke]);
                assert(after.stroke == before.stroke + 1 && !after.gap && !after.finished &&
                       after.progress == 0 && after.completed == before.completed);
                cue[after.stroke] = true;
            } else {
                assert(cue[before.stroke]);
                assert(after.stroke == before.stroke && after.progress == 1000 &&
                       after.completed == before.completed + 1 && after.gap == !after.finished);
                if (after.gap)
                    gap[after.stroke] = true;
            }
            assert(f.controller.state() ==
                   (after.finished ? StrokeOrderUiState::Completed : StrokeOrderUiState::Paused));
            assert(f.coordinator.CurrentPhase() == Phase::LocalPlayback &&
                   f.session.phase() == StrokeOrderVoicePhase::LocalPlayback &&
                   f.session.IsCurrentGeneration(f.generation));
            assert(!f.clock.running() && f.clock.last_tick_us() == 0 &&
                   f.clock.remainder_us() == 0);
            const auto stopped = StepSnapshot::Capture(f);
            for (uint64_t rejected : {now - 1000, now, now + 119000}) {
                assert(!f.ApplyUs(Action::Step, rejected));
                stopped.AssertUnchanged(f);
            }
            assert(!f.TickUs(now + 5000000));
            stopped.AssertUnchanged(f);
            now += 120000;  // exact debounce boundary, not 121ms
            if (animating && !after.finished)
                assert(f.ApplyUs(Action::PauseContinue, now));
        }
        for (unsigned i = 0; i < count; ++i)
            assert(cue[i] && (i + 1 == count || gap[i]));
        assert(!f.ApplyUs(Action::Step, now + 60000000));
        // Replay clears debounce as well as timing; a same-time fresh Step works.
        assert(f.ApplyUs(Action::Replay, now - 120000));
        assert(f.ApplyUs(Action::Step, now - 120000));
    }
}

void TestStepMatrix(const std::vector<uint8_t>& smoke, const std::vector<uint8_t>& shard,
                    const std::vector<uint8_t>& synthetic) {
    static_assert(StrokeOrderController::kStepDebounceMs == 120);
    VerifyManualSteps(smoke, 0x4EBA, 2);
    VerifyManualSteps(shard, 0x987A, 9);
    for (unsigned count = 1; count <= 48; ++count)
        VerifyManualSteps(synthetic, 0x4E00 + count, count);
    for (bool from_gap : {false, true}) {
        for (bool paused : {false, true}) {
            for (int rejection = 0; rejection < 4; ++rejection) {
                Fixture f(smoke);
                uint64_t now = 1000000;
                assert(f.ApplyUs(Action::Candidate, now));
                assert(f.TickUs(now));  // Fresh epoch arm-only turn, before timed setup.
                if (from_gap)
                    assert(f.TickUs(now += 150000));
                assert(f.TickUs(now += 33999));
                if (paused)
                    assert(f.ApplyUs(Action::PauseContinue, now));
                if (rejection == 1)
                    assert(f.coordinator.PublishCancelFence(f.generation));
                if (rejection == 2)
                    f.session.Cancel();
                if (rejection == 3)
                    assert(f.coordinator.BeginRound(now / 1000) != f.generation);
                const auto before = StepSnapshot::Capture(f);
                if (rejection == 0) {
                    WithCoordinatorHeld(f, [&]() {
                        assert(!f.ApplyUs(Action::Step, now));
                        before.AssertUnchanged(f, true);
                    });
                    before.AssertUnchanged(f);
                    // Rejected Step must not consume the debounce timestamp.
                    assert(f.ApplyUs(Action::Step, now));
                    AssertAdjacent(before.ui, Snapshot::Capture(f.controller), 150);
                    assert(f.coordinator.PublishCancelFence(f.generation));
                    const auto fenced = StepSnapshot::Capture(f);
                    assert(!f.ApplyUs(Action::Step, now + 120000));
                    fenced.AssertUnchanged(f);
                } else {
                    assert(!f.ApplyUs(Action::Step, now + 5000000));
                    before.AssertUnchanged(f);
                }
            }
            // Near-boundary elapsed and pending timer debt must NOT spill into
            // the phase Step enters. Resume must time that fresh phase from zero.
            Fixture f(smoke);
            uint64_t now = 1000000;
            assert(f.ApplyUs(Action::Candidate, now));
            assert(f.TickUs(now));  // Fresh epoch arm-only turn, before timed setup.
            if (from_gap)
                assert(f.TickUs(now += 150000));
            assert(f.TickUs(now += from_gap ? 159999 : 149999));
            if (paused)
                assert(f.ApplyUs(Action::PauseContinue, now));
            WithCoordinatorHeld(f, [&]() { assert(!f.TickUs(now + 5000000)); });
            assert(f.ApplyUs(Action::Step, now += 6000000));
            const auto after = Snapshot::Capture(f.controller);
            assert(after.stroke == (from_gap ? 1 : 0) && after.gap == !from_gap);
            assert(after.progress == (from_gap ? 0 : 1000) && after.completed == 1);
            assert(!f.clock.running() && f.clock.remainder_us() == 0);
            assert(f.ApplyUs(Action::PauseContinue, now += 60000000));
            assert(f.TickUs(now));
            assert(f.TickUs(now += from_gap ? 149000 : 159000));
            assert(f.controller.current_stroke() == after.stroke &&
                   f.controller.in_gap() == after.gap);
            assert(f.TickUs(now += 1000));
            assert(f.controller.state() ==
                   (from_gap ? StrokeOrderUiState::Completed : StrokeOrderUiState::Animating));
            assert(!f.controller.in_gap() && f.controller.current_stroke() == 1 &&
                   f.controller.current_progress_permille() == (from_gap ? 1000 : 0));
        }
    }
    std::cout << "stroke_order_step_phase_matrix: PASS\n";
}

int main(int argc, char** argv) {
    assert(argc == 4 || argc == 5);
    std::ifstream shard_file(argv[2], std::ios::binary);
    const std::vector<uint8_t> shard((std::istreambuf_iterator<char>(shard_file)), {});
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), {});
    std::ifstream synthetic_file(argv[3], std::ios::binary);
    const std::vector<uint8_t> synthetic((std::istreambuf_iterator<char>(synthetic_file)), {});
    if (argc == 5) {
        const std::string mode(argv[4]);
        if (mode == "first-160ms" || mode == "first-5s") {
            TestFirstCallback(blob, mode == "first-160ms" ? 160000 : 5000000);
            return 0;
        }
        assert(mode == "step-cue" || mode == "step-gap");
        TestStepRegression(blob, mode == "step-gap");
        return 0;
    }
    for (uint64_t delay : {33000u, 149000u, 150000u, 160000u, 5000000u})
        for (uint64_t cue : {150000u, 160000u})
            TestFirstCallback(blob, delay, cue);
    TestStartupGate(blob);
    TestStepRegression(blob, false);
    TestStepRegression(blob, true);
    TestStepMatrix(blob, shard, synthetic);
    TestStartCueSnapRegression(shard);
    TestCueMatrix(blob, shard, synthetic);
    TestDelayedSequentialRegression(blob);
    TestSequentialMatrix(blob);
    TestPauseClock(blob);
    for (auto action : {Action::Candidate, Action::PauseContinue, Action::Step, Action::Replay,
                        Action::Back, Action::Exit}) {
        for (bool paused : {false, true}) {
            Fixture f(blob);
            if (action != Action::Candidate) {
                assert(f.Apply(Action::Candidate));
                f.controller.Tick(250);
                if (paused) {
                    assert(f.Apply(Action::PauseContinue));
                }
            }
            const auto ui = f.controller.state();
            const auto voice = f.session.phase();
            const auto phase = f.coordinator.CurrentPhase();
            const auto active = f.controller.current_stroke();
            const auto progress = f.controller.current_progress_permille();
            assert(f.coordinator.PublishCancelFence(f.generation));
            // Deferred abort has NOT run. Fence-first must be mutation-free.
            assert(!f.Click(action));
            assert(f.clickable && f.abort_requests == 0);
            assert(f.controller.state() == ui && f.session.phase() == voice);
            assert(f.coordinator.CurrentPhase() == phase);
            assert(f.controller.current_stroke() == active &&
                   f.controller.current_progress_permille() == progress);
            // Timer ticks use the same admission; no advancement after a fence.
            assert(!f.coordinator.TryUiAction(f.generation,
                                              StrokeRoundCoordinator::UiTransition::None, 0, [&]() {
                                                  f.controller.Tick(1000);
                                                  assert(false);
                                                  return true;
                                              }));
        }
    }
    {
        Fixture f(blob);
        assert(f.Apply(Action::Candidate));
        assert(f.coordinator.CurrentPhase() == Phase::LocalPlayback);
        assert(f.Apply(Action::PauseContinue));
        assert(f.controller.state() == StrokeOrderUiState::Paused);
        assert(f.Apply(Action::PauseContinue));
        assert(f.controller.state() == StrokeOrderUiState::Animating);
        assert(f.Apply(Action::Step, 1000));
        assert(!f.Apply(Action::Step, 1001));  // debounce, no queue
        assert(f.Apply(Action::Replay));
        assert(f.Apply(Action::Replay));  // replay is idempotent
        assert(f.controller.current_progress_permille() == 0);
        assert(f.Apply(Action::Back));
        assert(f.coordinator.CurrentPhase() == Phase::Candidates);
        assert(f.Apply(Action::Candidate));
        assert(f.Click(Action::Exit));
        assert(f.abort_requests == 1 && !f.clickable);
        assert(!f.Click(Action::Exit));
        assert(f.abort_requests == 1);
    }
    {
        Fixture f(blob);
        assert(f.coordinator.BeginRound(2) != f.generation);
        assert(!f.Click(Action::Candidate));
        assert(f.clickable && f.abort_requests == 0);
        assert(f.controller.state() == StrokeOrderUiState::Candidates);
    }
    {
        Fixture f(blob);
        assert(f.Apply(Action::Candidate));
        f.session.Cancel();  // exact presentation session must also match
        assert(!f.Click(Action::Exit));
        assert(f.abort_requests == 0);
    }
    {
        Fixture f(blob);
        assert(f.controller.EnterError());
        assert(f.Apply(Action::Back));
        assert(f.coordinator.CurrentPhase() == Phase::Candidates);
        assert(f.controller.SelectCandidate(0));  // retain glyph for retry
        assert(f.controller.EnterError());
        assert(f.Apply(Action::RetryLoad));
        assert(f.session.phase() == StrokeOrderVoicePhase::LocalPlayback);
        assert(f.coordinator.CurrentPhase() == Phase::LocalPlayback);
        assert(f.Apply(Action::Back));
        assert(f.controller.EnterError());
        assert(f.coordinator.PublishCancelFence(f.generation));
        assert(!f.Apply(Action::RetryLoad));
        assert(!f.Apply(Action::Back));
        assert(f.controller.state() == StrokeOrderUiState::Error);
    }
    // Speech timeout retires the round; the view closes instead of rendering
    // unusable generation-0 controls. A fresh SO entry is the only retry route.
    {
        Fixture f(blob);
        StrokeOrderLifecycle lifecycle;
        lifecycle.Initialize();
        lifecycle.SetAssetsReady(true);
        lifecycle.SetPointerReady(true);
        lifecycle.SetDeviceIdle(true);
        assert(lifecycle.TryOpenOverlay());
        lifecycle.SetListenHold(true);
        lifecycle.SetDeviceIdle(false);
        assert(f.coordinator.AbortRound(f.generation).matched);
        lifecycle.SetDeviceIdle(true);
        f.session.Cancel();
        lifecycle.SetListenHold(false);
        assert(f.controller.Exit());
        lifecycle.CloseOverlay();
        assert(!lifecycle.overlay_open() && !lifecycle.CanHandleOverlayAction());
        assert(lifecycle.CanShowEntry() && lifecycle.CanStartVoice());
        assert(f.controller.state() == StrokeOrderUiState::Hidden);
        assert(!f.Click(Action::Exit) && !f.Click(Action::RetryVoice));
        assert(f.abort_requests == 0);
        assert(!StrokeOrderApplyUiAction(f.coordinator, f.controller, f.session, f.clock, 0,
                                         Action::RetryVoice, 1000000));
        const auto fresh = f.coordinator.BeginRound(2000);
        assert(fresh != f.generation && f.session.BeginConnecting(fresh));
        assert(lifecycle.TryOpenOverlay());
        assert(!f.Click(Action::Exit));  // an old click cannot abort the fresh retry
    }
    // Deterministically hold the coordinator on another thread without a fence.
    // Every rejected click stays armed and succeeds after benign contention.
    for (auto action : {Action::Candidate, Action::PauseContinue, Action::Step, Action::Replay,
                        Action::Back, Action::Exit}) {
        Fixture f(blob);
        if (action != Action::Candidate) {
            assert(f.Apply(Action::Candidate));
            f.controller.Tick(250);
        }
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, release = false;
        std::thread holder([&]() {
            assert(f.coordinator.TryUiAction(f.generation,
                                             StrokeRoundCoordinator::UiTransition::None, 0, [&]() {
                                                 std::unique_lock<std::mutex> lock(mutex);
                                                 entered = true;
                                                 cv.notify_all();
                                                 cv.wait(lock, [&]() { return release; });
                                                 return true;
                                             }));
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() { return entered; });
        }
        const auto ui = f.controller.state();
        const auto progress = f.controller.current_progress_permille();
        assert(!f.Click(action));
        assert(f.clickable && f.abort_requests == 0 && f.controller.state() == ui);
        assert(f.controller.current_progress_permille() == progress);
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
            cv.notify_all();
        }
        holder.join();
        assert(f.Click(action));
    }
    // An admitted action owns the coordinator lock through controller + session
    // mutation + coordinator phase transition. A concurrent fence comes after.
    {
        Fixture f(blob);
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, fence_started = false, release = false;
        std::thread action([&]() {
            assert(f.coordinator.TryUiAction(
                f.generation, StrokeRoundCoordinator::UiTransition::Playback, 0, [&]() {
                    std::unique_lock<std::mutex> lock(mutex);
                    entered = true;
                    cv.notify_all();
                    cv.wait(lock, [&]() { return release; });
                    assert(f.controller.SelectCandidate(0));
                    f.session.MarkLocalPlayback();
                    return true;
                }));
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() { return entered; });
        }
        std::thread fence([&]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                fence_started = true;
                cv.notify_all();
            }
            assert(f.coordinator.PublishCancelFence(f.generation));
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() { return fence_started; });
            release = true;
            cv.notify_all();
        }
        action.join();
        fence.join();
        assert(f.coordinator.CurrentPhase() == Phase::LocalPlayback);
        assert(f.session.phase() == StrokeOrderVoicePhase::LocalPlayback);
        assert(f.controller.state() == StrokeOrderUiState::Animating);
        assert(!f.Click(Action::Exit));
        assert(f.abort_requests == 0);
    }
    std::cout << "stroke_order_ui_fence_harness: PASS\n";
}
