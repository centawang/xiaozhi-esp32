#include "audio/audio_stream_generation.h"
#include "protocols/listening_mode_selection.h"
#include "protocols/listening_start_policy.h"
#include "stroke_order/stroke_round_coordinator.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

enum class FakeCancelSource : uint8_t {
    ToggleChat = 0,
    StartListening,
    StopListening,
    WakeWord,
    Reboot,
    Alert,
    AssetSuspend,
    PowerSave,
    SurfaceDeleted,
    ChannelClosed,
    NetworkDisconnected,
    NetworkError,
    ResetProtocol,
    ReplaceRound,
};

StrokeAbortReason ReasonFor(FakeCancelSource source) {
    switch (source) {
        case FakeCancelSource::Alert:
            return StrokeAbortReason::Alert;
        case FakeCancelSource::AssetSuspend:
            return StrokeAbortReason::AssetSuspend;
        case FakeCancelSource::PowerSave:
            return StrokeAbortReason::PowerSave;
        case FakeCancelSource::SurfaceDeleted:
            return StrokeAbortReason::SurfaceDeleted;
        case FakeCancelSource::Reboot:
            return StrokeAbortReason::Reboot;
        case FakeCancelSource::ChannelClosed:
        case FakeCancelSource::NetworkDisconnected:
        case FakeCancelSource::NetworkError:
        case FakeCancelSource::ResetProtocol:
            return StrokeAbortReason::ChannelClosed;
        case FakeCancelSource::ReplaceRound:
            return StrokeAbortReason::ReplacedByNewStroke;
        default:
            return StrokeAbortReason::NewNormalSession;
    }
}

struct FakeChannelCloseInfo {
    std::string session_id;
    uint64_t open_attempt_id = 0;
};

struct FakeAsrServer {
    QueuedStartListeningMode mode = QueuedStartListeningMode::ManualStop;
    bool started = false;
    bool stop_seen = false;
    bool final_stt = false;
    std::string utterance;

    void Start(QueuedStartListeningMode listen_mode) {
        mode = listen_mode;
        started = true;
        stop_seen = false;
        final_stt = false;
        utterance.clear();
    }

    void Stop() {
        stop_seen = true;
        started = false;
        if (mode == QueuedStartListeningMode::ManualStop && !utterance.empty()) {
            final_stt = true;
        }
    }

    bool VadEnd(const std::string& text) {
        utterance = text;
        if (mode == QueuedStartListeningMode::AutoStop ||
            mode == QueuedStartListeningMode::Realtime) {
            final_stt = true;
            return true;
        }
        return false;
    }
};

struct FakeProtocol {
    bool opened = false;
    int opens = 0;
    int closes = 0;
    int stops = 0;
    int start_listen = 0;
    bool send_start_ok = true;
    QueuedStartListeningMode last_listen_mode = QueuedStartListeningMode::ManualStop;
    FakeAsrServer server;
    std::string session_id;
    uint64_t next_open_attempt_id = 0;
    uint64_t active_open_attempt_id = 0;
    bool supports_correlated_open = true;
    bool supports_stroke_voice = true;
    std::function<void(const FakeChannelCloseInfo&)> on_close;

    bool SendStartListening(QueuedStartListeningMode mode) {
        ++start_listen;
        last_listen_mode = mode;
        if (!send_start_ok) {
            return false;
        }
        server.Start(mode);
        return true;
    }

    void SendStopListening() {
        ++stops;
        server.Stop();
    }

    bool SupportsCorrelatedSessionOpen() const { return supports_correlated_open; }
    bool SupportsStrokeVoiceRouting() const { return supports_stroke_voice; }

    uint64_t ReserveAudioChannelOpenAttempt() {
        return supports_correlated_open ? ++next_open_attempt_id : 0;
    }

    bool Open(const std::string& id, uint64_t open_attempt_id, std::string* opened_id = nullptr,
              const std::function<void()>& during_open = {},
              const std::function<void()>& after_open_before_bind = {}) {
        ++opens;
        opened = true;
        session_id = id;
        active_open_attempt_id = open_attempt_id;
        if (during_open) {
            during_open();
        }
        if (opened_id != nullptr) {
            *opened_id = id;
        }
        if (after_open_before_bind) {
            after_open_before_bind();
        }
        return true;
    }

    void Close() {
        const FakeChannelCloseInfo close{session_id, active_open_attempt_id};
        if (opened) {
            ++closes;
        }
        opened = false;
        session_id.clear();
        active_open_attempt_id = 0;
        server.started = false;
        if (on_close) {
            on_close(close);
        }
    }
};

struct FakeAudioService {
    AudioStreamGenerationGate gate;
    std::deque<AudioStreamInFlightWork> encode_inflight;
    std::deque<AudioStreamInFlightWork> decode_inflight;
    bool enable_result = true;
    bool expose_running_after_enable = true;
    bool microphone_enabled = false;
    bool processor_running = false;
    int enable_attempts = 0;
    int resets = 0;

    uint32_t CaptureEncode() const { return gate.capture; }
    uint32_t CaptureDecode() const { return gate.playback; }

    bool EnableVoiceProcessing(bool enable) {
        if (!enable) {
            microphone_enabled = false;
            processor_running = false;
            return true;
        }
        ++enable_attempts;
        if (!enable_result) {
            microphone_enabled = false;
            processor_running = false;
            return false;
        }
        processor_running = expose_running_after_enable;
        microphone_enabled = processor_running;
        return true;
    }

    bool IsAudioProcessorRunning() const { return processor_running; }

    bool RequeueEncode(uint32_t generation) {
        const AudioStreamInFlightWork work{generation, true};
        if (!AudioStreamCanRequeue(work, gate)) {
            return false;
        }
        encode_inflight.push_back(work);
        return true;
    }

    bool RequeueDecode(uint32_t generation) {
        const AudioStreamInFlightWork work{generation, false};
        if (!AudioStreamCanRequeue(work, gate)) {
            return false;
        }
        decode_inflight.push_back(work);
        return true;
    }

    void ResetStreamingState() {
        EnableVoiceProcessing(false);
        gate.BumpStreaming();
        AudioStreamDropStale(&encode_inflight, gate);
        AudioStreamDropStale(&decode_inflight, gate);
        ++resets;
    }
};

class FakeApplication {
public:
    FakeApplication() {
        protocol.on_close = [this](const FakeChannelCloseInfo& info) {
            const uint64_t opening_generation = MatchStrokeOpenAttempt(info.open_attempt_id);
            const bool valid = StrokeRoundCoordinator::ValidateSessionId(info.session_id.data(),
                                                                         info.session_id.size());
            const auto close = coordinator.CaptureChannelClose(info.session_id.data(),
                                                               info.session_id.size(), valid);
            const uint64_t closing_generation =
                opening_generation != 0
                    ? opening_generation
                    : (info.open_attempt_id == 0 &&
                               close.decision ==
                                   StrokeRoundCoordinator::ChannelCloseDecision::AbortStroke
                           ? close.generation
                           : 0);
            if (closing_generation != 0 && coordinator.PublishCancelFence(closing_generation)) {
                // Production close callbacks only fence and schedule main-task
                // cleanup. They never mutate UI/application state directly.
                scheduled_aborts.emplace_back(closing_generation, StrokeAbortReason::ChannelClosed);
                ++stroke_close_events;
            } else if (close.decision == StrokeRoundCoordinator::ChannelCloseDecision::Normal &&
                       coordinator.CommitNormalChannelClose(close)) {
                ++normal_close_events;
            } else {
                ++ignored_close_events;
            }
        };
    }

    uint64_t Begin(uint64_t now_ms) {
        const uint64_t current = coordinator.CurrentGeneration();
        if (current != 0) {
            AbortOnMain(current, StrokeAbortReason::ReplacedByNewStroke);
        }
        if (protocol.opened) {
            coordinator.CloseCurrentChannel();
            protocol.Close();
        }
        audio.ResetStreamingState();
        idle = true;
        connecting = false;
        overlay_page = "Connect";
        return coordinator.BeginRound(now_ms);
    }

    bool StrokeVoiceAvailable() const {
        return protocol.SupportsCorrelatedSessionOpen() && protocol.SupportsStrokeVoiceRouting();
    }

    void BindStrokeOpenAttempt(uint64_t generation, uint64_t open_attempt_id) {
        stroke_open_attempt_generation = generation;
        stroke_open_attempt_id = open_attempt_id;
    }

    uint64_t MatchStrokeOpenAttempt(uint64_t open_attempt_id) const {
        if (open_attempt_id == 0 || open_attempt_id != stroke_open_attempt_id) {
            return 0;
        }
        return stroke_open_attempt_generation;
    }

    void ClearStrokeOpenAttempt(uint64_t generation) {
        if (generation == 0 || generation != stroke_open_attempt_generation) {
            return;
        }
        stroke_open_attempt_id = 0;
        stroke_open_attempt_generation = 0;
    }

    bool BeginLocalCandidates(uint64_t generation, uint64_t now_ms) {
        if (StrokeVoiceAvailable()) {
            return false;
        }
        if (protocol.opened) {
            return false;
        }
        return coordinator.MarkLocalCandidates(generation, now_ms);
    }

    // Production-shaped async cancel: publish the coordinator fence and a
    // pending event only. Do not AbortRound on the calling thread.
    void PublishCancelEvent(uint64_t generation, FakeCancelSource source) {
        coordinator.PublishCancelFence(generation);
        if (source == FakeCancelSource::ReplaceRound) {
            replacement_pending = true;
            replacement_generation = generation;
            return;
        }
        abort_pending = true;
        abort_pending_generation = generation;
        abort_pending_reason = ReasonFor(source);
    }

    void DispatchAsyncCancel(uint64_t generation, FakeCancelSource source) {
        if (source == FakeCancelSource::ChannelClosed) {
            protocol.Close();
            return;
        }
        PublishCancelEvent(generation, source);
    }

    void RunScheduledAborts() {
        auto pending = std::move(scheduled_aborts);
        scheduled_aborts.clear();
        for (const auto& abort : pending) {
            AbortOnMain(abort.first, abort.second);
        }
    }

    uint64_t RunReplacement(uint64_t now_ms) {
        if (!replacement_pending) {
            return 0;
        }
        const uint64_t expected_generation = replacement_generation;
        replacement_pending = false;
        replacement_generation = 0;
        const uint64_t current = coordinator.CurrentGeneration();
        if (expected_generation != current &&
            !(current == 0 && coordinator.IsLatestGeneration(expected_generation))) {
            return 0;
        }
        if (current != 0) {
            AbortOnMain(current, StrokeAbortReason::ReplacedByNewStroke);
        }
        return coordinator.BeginRound(now_ms);
    }

    bool IsAbortPending(uint64_t generation) const {
        return abort_pending && generation != 0 && abort_pending_generation == generation;
    }

    bool AbortOnMain(uint64_t generation, StrokeAbortReason reason) {
        (void)reason;
        abort_pending = false;
        ClearStrokeOpenAttempt(generation);
        const auto result = coordinator.AbortRound(generation);
        audio.ResetStreamingState();
        if (protocol.opened) {
            protocol.SendStopListening();
            protocol.Close();
        }
        listening_generation = 0;
        listening_mode_set = false;
        connecting = false;
        idle = true;
        wake_word_detection = true;
        audio.EnableVoiceProcessing(false);
        return result.matched;
    }

    bool ConsumeCancel(uint64_t generation) {
        if (generation == 0) {
            return false;
        }
        ClearStrokeOpenAttempt(generation);
        if (protocol.opened) {
            coordinator.CloseCurrentChannel();
            protocol.Close();
        }
        audio.ResetStreamingState();
        if (IsAbortPending(generation) || coordinator.HasCancelFence(generation)) {
            return AbortOnMain(generation, abort_pending_reason);
        }
        return AbortOnMain(generation, StrokeAbortReason::NewNormalSession);
    }

    // Test isolation only: never used by during_open. Production cancel still
    // publishes a pending fence and lets continuation/start consume it.
    void EnsureIdle() {
        const uint64_t current = coordinator.CurrentGeneration();
        if (current != 0) {
            (void)AbortOnMain(current, StrokeAbortReason::UserClose);
        }
        if (protocol.opened) {
            coordinator.CloseCurrentChannel();
            protocol.Close();
        }
        audio.ResetStreamingState();
        listening_generation = 0;
        listening_mode_set = false;
        connecting = false;
        idle = true;
        abort_pending = false;
        abort_pending_generation = 0;
        replacement_pending = false;
        replacement_generation = 0;
        scheduled_aborts.clear();
        stroke_open_attempt_id = 0;
        stroke_open_attempt_generation = 0;
        wake_word_detection = true;
        audio.EnableVoiceProcessing(false);
    }

    bool ContinueOpen(uint64_t generation, const std::string& session_id,
                      const std::function<void()>& during_open = {},
                      const std::function<void()>& after_open_before_bind = {}) {
        return ContinueOpenWithMode(generation, session_id,
                                    ListeningModeForStartGeneration(generation), during_open,
                                    after_open_before_bind);
    }

    bool ContinueOpenWithMode(uint64_t generation, const std::string& session_id,
                              QueuedStartListeningMode start_mode,
                              const std::function<void()>& during_open = {},
                              const std::function<void()>& after_open_before_bind = {}) {
        if (coordinator.HasCancelFence(generation) || !coordinator.CanContinueOpen(generation) ||
            IsAbortPending(generation) || !coordinator.MarkConnecting(generation)) {
            ConsumeCancel(generation);
            return false;
        }
        connecting = true;
        const uint64_t open_attempt_id = protocol.ReserveAudioChannelOpenAttempt();
        if (open_attempt_id == 0) {
            ConsumeCancel(generation);
            return false;
        }
        BindStrokeOpenAttempt(generation, open_attempt_id);
        std::string opened_session_id;
        if (!protocol.Open(session_id, open_attempt_id, &opened_session_id, during_open,
                           after_open_before_bind) ||
            !protocol.opened) {
            ConsumeCancel(generation);
            return false;
        }
        if (coordinator.HasCancelFence(generation) || !coordinator.CanContinueOpen(generation) ||
            IsAbortPending(generation)) {
            ConsumeCancel(generation);
            return false;
        }
        if (!coordinator.BindOpenedChannel(generation, opened_session_id)) {
            ConsumeCancel(generation);
            return false;
        }
        listening_generation = generation;
        listening_mode = start_mode;
        listening_mode_set = true;
        connecting = false;
        idle = false;
        wake_word_detection = false;
        return true;
    }

    bool RecoverOrdinaryListeningStartFailure() {
        audio.ResetStreamingState();
        if (protocol.opened) {
            coordinator.CloseCurrentChannel();
            protocol.Close();
        }
        listening_generation = 0;
        listening_mode_set = false;
        connecting = false;
        idle = true;
        wake_word_detection = true;
        return false;
    }

    bool ApplyListeningStartDisposition(ListeningStartDisposition disposition,
                                        uint64_t generation) {
        switch (disposition) {
            case ListeningStartDisposition::Proceed:
                return true;
            case ListeningStartDisposition::RecoverOrdinary:
                return RecoverOrdinaryListeningStartFailure();
            case ListeningStartDisposition::AbandonFencedStroke:
                ConsumeCancel(generation);
                return false;
            case ListeningStartDisposition::AbortStroke:
                coordinator.PublishCancelFence(generation);
                AbortOnMain(generation, StrokeAbortReason::StartFailed);
                return false;
        }
        return false;
    }

    bool StartListeningAudio(uint64_t now_ms = 1000) {
        if (listening_generation != 0 && (coordinator.HasCancelFence(listening_generation) ||
                                          IsAbortPending(listening_generation) ||
                                          !coordinator.CanStartListening(listening_generation))) {
            ConsumeCancel(listening_generation);
            return false;
        }
        if (listening_generation == 0 && !coordinator.CanStartListening(listening_generation)) {
            audio.ResetStreamingState();
            if (protocol.opened) {
                protocol.Close();
            }
            idle = true;
            return false;
        }
        ListeningStartResult start_result;
        start_result.send_start_succeeded = protocol.SendStartListening(listening_mode);
        if (!start_result.send_start_succeeded) {
            const auto disposition = EvaluateListeningStartResult(
                listening_generation, coordinator.HasCancelFence(listening_generation),
                start_result);
            return ApplyListeningStartDisposition(disposition, listening_generation);
        }
        start_result.voice_processing_enabled = audio.EnableVoiceProcessing(true);
        start_result.audio_processor_running = audio.IsAudioProcessorRunning();
        const auto disposition = EvaluateListeningStartResult(
            listening_generation, coordinator.HasCancelFence(listening_generation), start_result);
        if (disposition != ListeningStartDisposition::Proceed) {
            return ApplyListeningStartDisposition(disposition, listening_generation);
        }
        if (!coordinator.MarkListeningStarted(listening_generation, now_ms)) {
            if (listening_generation != 0) {
                coordinator.PublishCancelFence(listening_generation);
                AbortOnMain(listening_generation, StrokeAbortReason::StartFailed);
            } else {
                RecoverOrdinaryListeningStartFailure();
            }
            return false;
        }
        if (listening_generation != 0) {
            overlay_page = "Speak";
        }
        return true;
    }

    bool WakeRelisten() {
        send_queue_residue = 0;
        ListeningStartResult result;
        result.send_start_succeeded =
            protocol.SendStartListening(QueuedStartListeningMode::AutoStop);
        if (!result.send_start_succeeded) {
            return ApplyListeningStartDisposition(EvaluateListeningStartResult(0, false, result),
                                                  0);
        }
        ++decoder_resets;
        ++popup_sounds;
        wake_word_detection = true;
        return true;
    }

    bool SpeakUtterance(const std::string& session_id, const std::string& text) {
        if (!protocol.server.VadEnd(text)) {
            return false;
        }
        return CaptureAndCommitStrokeStt(session_id);
    }

    bool FinishStrokeAfterStt(uint64_t generation, uint64_t now_ms) {
        protocol.SendStopListening();
        ClearStrokeOpenAttempt(generation);
        if (!coordinator.RetireStrokeChannel(generation)) {
            return false;
        }
        audio.ResetStreamingState();
        listening_generation = 0;
        if (protocol.opened) {
            protocol.Close();
        }
        idle = true;
        connecting = false;
        return coordinator.MarkCandidates(generation, now_ms);
    }

    bool CaptureAndCommitStrokeStt(const std::string& session_id) {
        const auto route = coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                                    session_id.data(), session_id.size(), true);
        return coordinator.CommitStrokeStt(route);
    }

    bool DeliverNormalStt(const char* session_id, bool valid) {
        const size_t size = session_id == nullptr ? 0 : std::char_traits<char>::length(session_id);
        const auto route = coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                                    session_id, size, valid);
        if (!coordinator.RevalidateNormal(route)) {
            return false;
        }
        ++glyph_events;
        ++chat_events;
        return true;
    }

    bool DeliverNormal(StrokeRoundCoordinator::MessageKind kind, const char* session_id,
                       bool valid) {
        const size_t size = session_id == nullptr ? 0 : std::char_traits<char>::length(session_id);
        const auto route = coordinator.CaptureRoute(kind, session_id, size, valid);
        return coordinator.RevalidateNormal(route);
    }

    StrokeRoundCoordinator coordinator;
    FakeProtocol protocol;
    FakeAudioService audio;
    bool abort_pending = false;
    uint64_t abort_pending_generation = 0;
    StrokeAbortReason abort_pending_reason = StrokeAbortReason::UserClose;
    bool replacement_pending = false;
    uint64_t replacement_generation = 0;
    std::deque<std::pair<uint64_t, StrokeAbortReason>> scheduled_aborts;
    uint64_t stroke_open_attempt_id = 0;
    uint64_t stroke_open_attempt_generation = 0;
    uint64_t listening_generation = 0;
    bool listening_mode_set = false;
    QueuedStartListeningMode listening_mode = QueuedStartListeningMode::ManualStop;
    std::string overlay_page;
    bool idle = true;
    bool connecting = false;
    bool wake_word_detection = true;
    int send_queue_residue = 0;
    int decoder_resets = 0;
    int popup_sounds = 0;
    int glyph_events = 0;
    int chat_events = 0;
    int stroke_close_events = 0;
    int normal_close_events = 0;
    int ignored_close_events = 0;
};

}  // namespace

int main() {
    FakeApplication app;

    Expect(app.DeliverNormalStt(nullptr, false), "legacy ordinary STT without id passes");
    Expect(app.glyph_events == 1 && app.chat_events == 1,
           "ordinary STT reaches glyph and chat side effects");
    const auto queued_legacy =
        app.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Tts, nullptr, 0, false);

    const uint64_t stale_start = app.Begin(10);
    Expect(!app.coordinator.RevalidateNormal(queued_legacy),
           "a normal callback queued before stroke start cannot cross the fresh boundary");
    Expect(app.coordinator.MarkConnecting(stale_start), "stale-start round connecting");
    app.PublishCancelEvent(stale_start, FakeCancelSource::ToggleChat);
    Expect(app.coordinator.HasCancelFence(stale_start), "toggle publishes generation cancel fence");
    Expect(!app.ContinueOpen(stale_start, "stale-open"),
           "cancelled generation cannot continue open");
    Expect(!app.coordinator.CanContinueOpen(stale_start),
           "cancelled generation cannot become ordinary request");
    Expect(app.protocol.start_listen == 0 && !app.audio.microphone_enabled,
           "stale cancelled open never sends listen/start");

    const std::vector<FakeCancelSource> async_sources = {
        FakeCancelSource::ToggleChat,
        FakeCancelSource::StartListening,
        FakeCancelSource::StopListening,
        FakeCancelSource::WakeWord,
        FakeCancelSource::Reboot,
        FakeCancelSource::Alert,
        FakeCancelSource::AssetSuspend,
        FakeCancelSource::PowerSave,
        FakeCancelSource::SurfaceDeleted,
        FakeCancelSource::ChannelClosed,
        FakeCancelSource::NetworkDisconnected,
        FakeCancelSource::NetworkError,
        FakeCancelSource::ResetProtocol,
        FakeCancelSource::ReplaceRound,
    };
    uint64_t now = 20;
    int suffix = 0;
    for (auto source : async_sources) {
        const uint64_t generation = app.Begin(now++);
        const std::string id = "stroke-async-" + std::to_string(++suffix);
        const int opens_before = app.protocol.opens;
        bool started = app.ContinueOpen(generation, id, [&]() {
            app.DispatchAsyncCancel(generation, source);
            Expect(app.coordinator.IsCurrentGeneration(generation) &&
                       app.coordinator.HasCancelFence(generation),
                   "during_open only publishes the cancel fence; coordinator stays active");
        });
        Expect(!started && !app.protocol.opened && !app.audio.microphone_enabled &&
                   app.protocol.start_listen == 0,
               "async cancel during blocking open is consumed by continuation");
        Expect(app.protocol.opens == opens_before + 1, "open still returns a fresh channel");
        Expect(app.idle, "fence hit returns the device to idle");
        app.RunScheduledAborts();
    }

    const uint64_t between_open_and_bind = app.Begin(70);
    Expect(!app.ContinueOpen(
               between_open_and_bind, "stroke-open-bind", {},
               [&]() {
                   app.protocol.Close();
                   Expect(app.coordinator.HasCancelFence(between_open_and_bind),
                          "close after open completion matches the reserved attempt identity");
                   Expect(app.coordinator.IsCurrentGeneration(between_open_and_bind),
                          "open-to-bind close callback schedules rather than directly aborting");
               }),
           "open-to-bind close fence prevents channel binding");
    Expect(app.protocol.start_listen == 0 && !app.audio.microphone_enabled,
           "open-to-bind close cannot start voice capture");
    app.RunScheduledAborts();

    const std::vector<FakeCancelSource> final_gate_sources = {
        FakeCancelSource::ChannelClosed, FakeCancelSource::NetworkDisconnected,
        FakeCancelSource::NetworkError,  FakeCancelSource::ResetProtocol,
        FakeCancelSource::ReplaceRound,
    };
    now = 80;
    for (auto source : final_gate_sources) {
        const uint64_t post_open = app.Begin(now++);
        const std::string id = "stroke-final-gate-" + std::to_string(++suffix);
        Expect(app.ContinueOpen(post_open, id),
               "open succeeds before the asynchronous cancellation source arrives");
        Expect(app.listening_mode_set && !app.audio.microphone_enabled &&
                   app.protocol.start_listen == 0,
               "SetListeningMode queues STATE_CHANGED without starting the microphone");
        app.DispatchAsyncCancel(post_open, source);
        Expect(app.coordinator.IsCurrentGeneration(post_open) &&
                   app.coordinator.HasCancelFence(post_open),
               "async source fences the exact bound generation before main-task cleanup");
        if (source == FakeCancelSource::ChannelClosed) {
            Expect(!app.scheduled_aborts.empty(),
                   "channel-close callback only schedules abort after publishing its fence");
        }
        // Production Run handles STATE_CHANGED before MAIN_EVENT_SCHEDULE. The
        // final gate must reject before the scheduled close abort is drained.
        Expect(!app.StartListeningAudio() && !app.audio.microphone_enabled &&
                   app.protocol.start_listen == 0,
               "STATE_CHANGED-first final gate rejects every asynchronous fence");
        Expect(app.listening_generation == 0,
               "stale stroke start never degrades to generation-0 ordinary listening");
        if (source == FakeCancelSource::ReplaceRound) {
            const uint64_t replacement = app.RunReplacement(now + 1000);
            Expect(replacement != 0 && replacement != post_open,
                   "replacement event starts a fresh nonzero generation after the old gate aborts");
            Expect(!app.coordinator.CanStartListening(0),
                   "replacement never reinterprets the stale request as ordinary listening");
            app.AbortOnMain(replacement, StrokeAbortReason::UserClose);
        }
        app.RunScheduledAborts();
        Expect(app.idle && !app.protocol.opened,
               "post-open/start cancellation closes the fresh channel");
    }

    const uint64_t first = app.Begin(100);
    Expect(app.ContinueOpen(first, "stroke-1") && app.StartListeningAudio(),
           "start first stroke session");
    StrokeRoundCoordinator::RouteSnapshot captured_old;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool captured = false;
    bool release_network = false;
    bool old_committed = true;
    std::thread network_thread([&]() {
        captured_old = app.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt,
                                                    "stroke-1", 8, true);
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            captured = true;
        }
        barrier_cv.notify_one();
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return release_network; });
        lock.unlock();
        old_committed = app.coordinator.CommitStrokeStt(captured_old);
    });
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_cv.wait(lock, [&]() { return captured; });
    }
    Expect(captured_old.decision == StrokeRoundCoordinator::RouteDecision::InterceptStrokeStt,
           "production capture identifies matching stroke STT atomically");
    Expect(app.AbortOnMain(first, StrokeAbortReason::UserClose), "cancel captured first round");

    const uint64_t second = app.Begin(200);
    Expect(app.ContinueOpen(second, "stroke-2") && app.StartListeningAudio(),
           "start replacement stroke session");
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_network = true;
    }
    barrier_cv.notify_one();
    network_thread.join();
    Expect(!old_committed, "cancel then new begin cannot commit old captured STT");
    const auto stale_close = app.coordinator.CaptureChannelClose("stroke-1", 8, true);
    Expect(stale_close.decision == StrokeRoundCoordinator::ChannelCloseDecision::Ignore,
           "retired channel close cannot abort its replacement");
    const auto matching_close = app.coordinator.CaptureChannelClose("stroke-2", 8, true);
    Expect(matching_close.decision == StrokeRoundCoordinator::ChannelCloseDecision::AbortStroke &&
               app.coordinator.RevalidateStrokeChannelClose(matching_close),
           "matching active channel close is routed to unified abort");
    const auto unknown_close = app.coordinator.CaptureChannelClose("unknown-close", 13, true);
    Expect(unknown_close.decision == StrokeRoundCoordinator::ChannelCloseDecision::AbortStroke &&
               app.coordinator.RevalidateStrokeChannelClose(unknown_close),
           "unknown active close fails the stroke round closed");
    const auto late_old =
        app.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt, "stroke-1", 8, true);
    Expect(late_old.decision == StrokeRoundCoordinator::RouteDecision::Drop,
           "retired stroke STT is dropped, not assigned current generation");

    for (auto kind :
         {StrokeRoundCoordinator::MessageKind::Stt, StrokeRoundCoordinator::MessageKind::Tts,
          StrokeRoundCoordinator::MessageKind::Llm, StrokeRoundCoordinator::MessageKind::Audio}) {
        const auto missing = app.coordinator.CaptureRoute(kind, nullptr, 0, false);
        Expect(missing.decision == StrokeRoundCoordinator::RouteDecision::FailStroke,
               "missing session id fails active stroke closed");
        const auto wrong = app.coordinator.CaptureRoute(kind, "unknown", 7, true);
        Expect(wrong.decision == StrokeRoundCoordinator::RouteDecision::FailStroke,
               "wrong session id fails active stroke closed");
    }
    Expect(!StrokeRoundCoordinator::ValidateSessionId("bad id", 6),
           "session id format is bounded and validated");
    Expect(!StrokeRoundCoordinator::ValidateSessionId(
               std::string(StrokeRoundCoordinator::kMaxSessionIdBytes + 1, 'a').data(),
               StrokeRoundCoordinator::kMaxSessionIdBytes + 1),
           "oversized session id rejected");

    const auto accepted =
        app.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Stt, "stroke-2", 8, true);
    Expect(app.coordinator.CommitStrokeStt(accepted), "matching stroke STT commits once");
    Expect(!app.coordinator.CommitStrokeStt(accepted), "duplicate snapshot cannot commit twice");
    app.ClearStrokeOpenAttempt(second);
    Expect(app.coordinator.RetireStrokeChannel(second), "valid STT retires stroke channel");
    app.audio.ResetStreamingState();
    app.protocol.Close();
    Expect(app.ignored_close_events > 0,
           "close callback from an already-retired stroke channel is ignored");
    Expect(app.coordinator.MarkCandidates(second, 1000), "candidate phase begins");

    for (auto kind :
         {StrokeRoundCoordinator::MessageKind::Tts, StrokeRoundCoordinator::MessageKind::Llm,
          StrokeRoundCoordinator::MessageKind::Audio}) {
        const auto old_output = app.coordinator.CaptureRoute(kind, "stroke-2", 8, true);
        Expect(old_output.decision == StrokeRoundCoordinator::RouteDecision::Drop,
               "old stroke assistant/audio output drops");
    }
    Expect(app.coordinator.CheckTimeouts(60999).kind == StrokeRoundCoordinator::TimeoutKind::None,
           "candidate timeout does not fire early");
    const auto candidate_timeout = app.coordinator.CheckTimeouts(61000);
    Expect(candidate_timeout.kind == StrokeRoundCoordinator::TimeoutKind::Candidates &&
               candidate_timeout.generation == second,
           "candidate timeout fires at 60 seconds");
    Expect(app.AbortOnMain(second, StrokeAbortReason::CandidateTimeout),
           "candidate timeout exits the round");

    Expect(!app.coordinator.BindOpenedChannel(0, "stroke-2"),
           "normal channel cannot reuse retired stroke session id");
    Expect(app.coordinator.BindOpenedChannel(0, "normal-1"),
           "new normal channel binds a distinct session id");
    Expect(app.DeliverNormalStt("normal-1", true), "confirmed normal STT passes route gate");
    Expect(app.glyph_events == 2 && app.chat_events == 2,
           "confirmed normal STT preserves glyph/chat events");
    for (auto kind :
         {StrokeRoundCoordinator::MessageKind::Stt, StrokeRoundCoordinator::MessageKind::Tts,
          StrokeRoundCoordinator::MessageKind::Llm, StrokeRoundCoordinator::MessageKind::Audio}) {
        Expect(app.DeliverNormal(kind, "normal-1", true),
               "new normal session passes every routed message class");
    }
    const auto queued_normal =
        app.coordinator.CaptureRoute(StrokeRoundCoordinator::MessageKind::Llm, "normal-1", 8, true);

    app.coordinator.CloseCurrentChannel();
    const uint64_t speech_timeout_round = app.Begin(5000);
    Expect(!app.coordinator.RevalidateNormal(queued_normal),
           "queued normal assistant output cannot cross into a stroke round");
    Expect(app.ContinueOpen(speech_timeout_round, "stroke-timeout") && app.StartListeningAudio(),
           "start timeout round");
    Expect(app.coordinator.CheckTimeouts(4999).kind == StrokeRoundCoordinator::TimeoutKind::None,
           "a monotonic clock sample before the start never underflows into a timeout");
    Expect(app.coordinator.CheckTimeouts(15999).kind == StrokeRoundCoordinator::TimeoutKind::None,
           "15-second speech timeout not early");
    const auto speech_timeout = app.coordinator.CheckTimeouts(16000);
    Expect(speech_timeout.kind == StrokeRoundCoordinator::TimeoutKind::Speech,
           "15-second speech timeout fires");
    const uint32_t encode_gen = app.audio.CaptureEncode();
    const uint32_t decode_gen = app.audio.CaptureDecode();
    Expect(app.audio.RequeueEncode(encode_gen) && app.audio.RequeueDecode(decode_gen),
           "in-flight work can join before reset");
    Expect(app.AbortOnMain(speech_timeout_round, StrokeAbortReason::SpeechTimeout),
           "speech timeout uses abort path");
    Expect(app.audio.encode_inflight.empty() && app.audio.decode_inflight.empty() &&
               !app.audio.microphone_enabled,
           "abort drains in-flight encode/decode and stops microphone");
    Expect(!app.audio.RequeueEncode(encode_gen) && !app.audio.RequeueDecode(decode_gen),
           "ResetStreamingState rejects in-flight encode/decode from the old generation");

    FakeApplication mqtt_chat;
    mqtt_chat.protocol.supports_correlated_open = false;
    mqtt_chat.protocol.supports_stroke_voice = false;
    Expect(!mqtt_chat.StrokeVoiceAvailable(), "ordinary MQTT never advertises stroke voice");
    Expect(mqtt_chat.DeliverNormalStt(nullptr, false),
           "ordinary MQTT STT without a stroke round is unchanged");
    Expect(mqtt_chat.coordinator.BindOpenedChannel(0, "mqtt-normal-1"),
           "ordinary MQTT can bind a normal channel");
    Expect(mqtt_chat.DeliverNormalStt("mqtt-normal-1", true),
           "ordinary MQTT STT with a session id still reaches glyph/chat");
    mqtt_chat.EnsureIdle();

    FakeApplication mqtt;
    mqtt.protocol.supports_correlated_open = false;
    mqtt.protocol.supports_stroke_voice = false;
    Expect(!mqtt.StrokeVoiceAvailable(), "MQTT fail-closes stroke voice routing");
    const uint64_t mqtt_round = mqtt.Begin(9000);
    Expect(mqtt.BeginLocalCandidates(mqtt_round, 9000),
           "MQTT degrades to local candidates without opening audio");
    Expect(mqtt.protocol.opens == 0 && !mqtt.protocol.opened && mqtt.protocol.start_listen == 0 &&
               !mqtt.audio.microphone_enabled,
           "MQTT local fallback never opens a stroke voice channel or starts listening");
    Expect(mqtt.coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::Candidates,
           "MQTT local fallback lands on the candidate page");
    mqtt.EnsureIdle();

    FakeApplication ws;
    Expect(ws.StrokeVoiceAvailable(), "WebSocket advertises correlated stroke voice routing");
    const uint64_t ws_round = ws.Begin(9100);
    Expect(ws.ContinueOpen(ws_round, "ws-stroke") && ws.StartListeningAudio(),
           "WebSocket may enter remote STT AwaitingSpeech");
    Expect(ws.protocol.start_listen == 1 && ws.audio.microphone_enabled,
           "WebSocket stroke voice sends listen/start after the final gate");
    ws.EnsureIdle();

    std::string first_id;
    FakeApplication longevity;
    for (int i = 0; i < 100; ++i) {
        const uint64_t generation = longevity.Begin(30000 + static_cast<uint64_t>(i));
        const std::string id = "stroke-life-" + std::to_string(i);
        if (i == 0) {
            first_id = id;
        }
        Expect(longevity.ContinueOpen(generation, id) && longevity.StartListeningAudio(),
               "longevity round starts");
        Expect(longevity.AbortOnMain(generation, StrokeAbortReason::UserClose),
               "longevity round aborts");
    }
    Expect(!longevity.coordinator.IsRoundActive() && longevity.coordinator.CurrentGeneration() == 0,
           "100 aborted rounds leave no leftover active stroke");
    const uint64_t replay_round = longevity.Begin(40000);
    Expect(
        longevity.ContinueOpen(replay_round, "stroke-life-100") && longevity.StartListeningAudio(),
        "round 101 starts after 100 prior identities");
    Expect(longevity.coordinator.IsRoundActive(),
           "replay round stays active until fail-closed abort");
    const auto replay_stt = longevity.coordinator.CaptureRoute(
        StrokeRoundCoordinator::MessageKind::Stt, first_id.data(), first_id.size(), true);
    Expect(replay_stt.decision == StrokeRoundCoordinator::RouteDecision::FailStroke,
           "replaying round-1 STT after 16 retired slots fail-closes the current round");
    Expect(!longevity.coordinator.CommitStrokeStt(replay_stt),
           "retired-capacity overflow does not accept the old STT");
    for (auto kind :
         {StrokeRoundCoordinator::MessageKind::Tts, StrokeRoundCoordinator::MessageKind::Llm,
          StrokeRoundCoordinator::MessageKind::Audio}) {
        const auto replay =
            longevity.coordinator.CaptureRoute(kind, first_id.data(), first_id.size(), true);
        Expect(replay.decision == StrokeRoundCoordinator::RouteDecision::FailStroke,
               "replaying round-1 TTS/LLM/audio fail-closes rather than passing as normal");
    }
    Expect(longevity.AbortOnMain(replay_round, StrokeAbortReason::InvalidSessionIdentity),
           "fail-closed replay aborts the current round instead of accepting old traffic");
    longevity.EnsureIdle();
    Expect(!longevity.coordinator.IsRoundActive() &&
               longevity.coordinator.CurrentGeneration() == 0 && !longevity.protocol.opened,
           "fail-closed replay cleanup leaves no leftover active stroke");
    const auto after_abort = longevity.coordinator.CaptureRoute(
        StrokeRoundCoordinator::MessageKind::Stt, first_id.data(), first_id.size(), true);
    Expect(after_abort.decision == StrokeRoundCoordinator::RouteDecision::Drop,
           "after abort, overflowed round-1 STT drops and cannot bind as normal");

    for (int i = 0; i < 100; ++i) {
        const std::string id = "normal-long-run-" + std::to_string(i);
        Expect(!longevity.coordinator.IsRoundActive(),
               "normal bind loop starts with no leftover stroke");
        Expect(longevity.coordinator.BindOpenedChannel(0, id),
               "distinct normal identity remains bindable over long uptime");
        longevity.coordinator.CloseCurrentChannel();
    }
    Expect(!longevity.coordinator.IsRoundActive() && longevity.coordinator.CurrentGeneration() == 0,
           "normal bind loop does not leave an active stroke");

    FakeApplication mode_matrix;
    Expect(ListeningModeForStartGeneration(0) == QueuedStartListeningMode::ManualStop,
           "production helper keeps generation 0 ManualStop");
    Expect(ListeningModeForStartGeneration(9) == QueuedStartListeningMode::AutoStop,
           "production helper selects AutoStop for stroke generation");
    const uint64_t press_to_talk = 0;
    Expect(mode_matrix.coordinator.BindOpenedChannel(press_to_talk, "normal-manual-open"),
           "ordinary existing channel can bind");
    mode_matrix.listening_generation = press_to_talk;
    Expect(mode_matrix.StartListeningAudio() &&
               mode_matrix.protocol.last_listen_mode == QueuedStartListeningMode::ManualStop,
           "ordinary explicit StartListening on an open channel stays ManualStop");
    mode_matrix.EnsureIdle();

    FakeApplication circular;
    const uint64_t circular_gen = circular.Begin(7000);
    Expect(circular.overlay_page == "Connect", "SO click presents Connect before hello");
    Expect(circular.ContinueOpenWithMode(circular_gen, "stroke-manual-deadlock",
                                         QueuedStartListeningMode::ManualStop),
           "legacy ManualStop wiring binds without a test-only post-start mode overwrite");
    Expect(
        circular.coordinator.CheckTimeouts(22000).kind == StrokeRoundCoordinator::TimeoutKind::None,
        "connect/bind does not consume the 15-second speech wait");
    Expect(circular.StartListeningAudio(7000), "manual deadlock round starts listening");
    Expect(circular.protocol.last_listen_mode == QueuedStartListeningMode::ManualStop,
           "legacy wiring naturally sends ManualStop to the fake ASR server");
    Expect(circular.overlay_page == "Speak", "Speak appears only after listen/start");
    Expect(!circular.SpeakUtterance("stroke-manual-deadlock", "yi"),
           "manual ASR emits no final STT without listen/stop");
    Expect(!circular.protocol.server.final_stt, "manual VAD does not finalize");
    Expect(
        circular.coordinator.CheckTimeouts(21999).kind == StrokeRoundCoordinator::TimeoutKind::None,
        "manual deadlock does not timeout before 15s from MarkListeningStarted");
    const auto circular_timeout = circular.coordinator.CheckTimeouts(22000);
    Expect(circular_timeout.kind == StrokeRoundCoordinator::TimeoutKind::Speech &&
               circular_timeout.generation == circular_gen,
           "manual listen/stop circular wait times out at 15s from MarkListeningStarted");
    Expect(!circular.protocol.server.stop_seen, "FinishStrokeListening never sent stop before STT");
    circular.EnsureIdle();

    FakeApplication autostop;
    const uint64_t auto_gen = autostop.Begin(8000);
    Expect(autostop.overlay_page == "Connect", "AutoStop round starts on Connect");
    Expect(autostop.ContinueOpen(auto_gen, "stroke-autostop"), "AutoStop round binds");
    Expect(autostop.StartListeningAudio(8000) &&
               autostop.protocol.last_listen_mode == QueuedStartListeningMode::AutoStop,
           "stroke StartListeningAudio sends AutoStop");
    Expect(autostop.audio.enable_attempts == 1 && autostop.audio.IsAudioProcessorRunning(),
           "successful stroke start observes voice processing running before UI advance");
    Expect(autostop.overlay_page == "Speak", "successful start advances Connect to Speak");
    Expect(autostop.SpeakUtterance("stroke-autostop", "yi"),
           "AutoStop/VAD returns matching STT without listen/stop");
    Expect(autostop.FinishStrokeAfterStt(auto_gen, 8100),
           "matching AutoStop STT retires the channel and enters Candidates");
    Expect(autostop.coordinator.CurrentPhase() == StrokeRoundCoordinator::Phase::Candidates,
           "AutoStop matching STT lands on Candidates");
    autostop.EnsureIdle();

    FakeApplication send_fail;
    const uint64_t fail_gen = send_fail.Begin(8500);
    Expect(send_fail.ContinueOpen(fail_gen, "stroke-send-fail"), "send-fail round binds");
    send_fail.protocol.send_start_ok = false;
    const int send_fail_resets = send_fail.audio.resets;
    Expect(!send_fail.StartListeningAudio(8500), "stroke listen/start send failure is visible");
    Expect(send_fail.overlay_page != "Speak", "failed stroke listen/start never shows Speak");
    Expect(send_fail.protocol.start_listen == 1 && !send_fail.audio.microphone_enabled,
           "failed stroke send attempts listen/start but never enables the microphone");
    Expect(send_fail.idle && !send_fail.protocol.opened &&
               send_fail.audio.resets > send_fail_resets && !send_fail.coordinator.IsRoundActive(),
           "stroke send failure fences, aborts, drains, closes, and returns Idle");
    Expect(send_fail.coordinator.CheckTimeouts(23500).kind ==
               StrokeRoundCoordinator::TimeoutKind::None,
           "stroke send failure never arms the 15-second speech timeout");
    send_fail.EnsureIdle();

    FakeApplication stroke_enable_fail;
    const uint64_t enable_fail_gen = stroke_enable_fail.Begin(8550);
    Expect(stroke_enable_fail.ContinueOpen(enable_fail_gen, "stroke-enable-fail"),
           "voice-enable-fail stroke round binds");
    stroke_enable_fail.audio.enable_result = false;
    const int stroke_enable_fail_resets = stroke_enable_fail.audio.resets;
    Expect(!stroke_enable_fail.StartListeningAudio(8550),
           "stroke voice-processing enable failure is visible");
    Expect(stroke_enable_fail.protocol.start_listen == 1 &&
               stroke_enable_fail.audio.enable_attempts == 1 &&
               !stroke_enable_fail.audio.IsAudioProcessorRunning(),
           "stroke enable failure happens only after listen/start and is observable");
    Expect(stroke_enable_fail.overlay_page != "Speak" && stroke_enable_fail.idle &&
               !stroke_enable_fail.protocol.opened &&
               stroke_enable_fail.audio.resets > stroke_enable_fail_resets &&
               !stroke_enable_fail.coordinator.IsRoundActive(),
           "stroke enable failure never shows Speak and uses unified abort cleanup");
    Expect(stroke_enable_fail.coordinator.CheckTimeouts(23550).kind ==
               StrokeRoundCoordinator::TimeoutKind::None,
           "stroke enable failure never arms the 15-second speech timeout");
    stroke_enable_fail.EnsureIdle();

    FakeApplication stroke_not_running;
    const uint64_t not_running_gen = stroke_not_running.Begin(8575);
    Expect(stroke_not_running.ContinueOpen(not_running_gen, "stroke-not-running"),
           "processor-not-running stroke round binds");
    stroke_not_running.audio.expose_running_after_enable = false;
    Expect(!stroke_not_running.StartListeningAudio(8575),
           "engine not running after enable is a stroke start failure");
    Expect(stroke_not_running.overlay_page != "Speak" && stroke_not_running.idle &&
               !stroke_not_running.protocol.opened &&
               !stroke_not_running.coordinator.IsRoundActive(),
           "processor-running postcondition gates MarkListeningStarted and Speak");
    stroke_not_running.EnsureIdle();

    FakeApplication ordinary_send_fail;
    ordinary_send_fail.protocol.Open("normal-send-fail", 0);
    Expect(ordinary_send_fail.coordinator.BindOpenedChannel(0, "normal-send-fail"),
           "ordinary send-fail channel binds");
    ordinary_send_fail.listening_generation = 0;
    ordinary_send_fail.listening_mode = QueuedStartListeningMode::ManualStop;
    ordinary_send_fail.idle = false;
    ordinary_send_fail.wake_word_detection = false;
    ordinary_send_fail.protocol.send_start_ok = false;
    const int ordinary_send_resets = ordinary_send_fail.audio.resets;
    Expect(!ordinary_send_fail.StartListeningAudio(8600),
           "generation-0 listen/start send failure is visible");
    Expect(ordinary_send_fail.idle && ordinary_send_fail.wake_word_detection &&
               !ordinary_send_fail.protocol.opened &&
               ordinary_send_fail.audio.resets > ordinary_send_resets,
           "generation-0 send failure closes, drains, returns Idle, and restores wake word");
    ordinary_send_fail.EnsureIdle();

    FakeApplication ordinary_enable_fail;
    ordinary_enable_fail.protocol.Open("normal-enable-fail", 0);
    Expect(ordinary_enable_fail.coordinator.BindOpenedChannel(0, "normal-enable-fail"),
           "ordinary enable-fail channel binds");
    ordinary_enable_fail.listening_generation = 0;
    ordinary_enable_fail.listening_mode = QueuedStartListeningMode::ManualStop;
    ordinary_enable_fail.idle = false;
    ordinary_enable_fail.wake_word_detection = false;
    ordinary_enable_fail.audio.enable_result = false;
    const int ordinary_enable_resets = ordinary_enable_fail.audio.resets;
    Expect(!ordinary_enable_fail.StartListeningAudio(8625),
           "generation-0 voice-processing enable failure is visible");
    Expect(ordinary_enable_fail.protocol.start_listen == 1 &&
               ordinary_enable_fail.audio.enable_attempts == 1 && ordinary_enable_fail.idle &&
               ordinary_enable_fail.wake_word_detection && !ordinary_enable_fail.protocol.opened &&
               ordinary_enable_fail.audio.resets > ordinary_enable_resets,
           "generation-0 enable failure closes, drains, returns Idle, and restores wake word");
    ordinary_enable_fail.EnsureIdle();

    FakeApplication wake_relisten_fail;
    wake_relisten_fail.protocol.Open("normal-wake-relisten", 0);
    Expect(wake_relisten_fail.coordinator.BindOpenedChannel(0, "normal-wake-relisten"),
           "wake re-listen normal channel binds");
    wake_relisten_fail.listening_generation = 0;
    wake_relisten_fail.listening_mode = QueuedStartListeningMode::AutoStop;
    wake_relisten_fail.idle = false;
    wake_relisten_fail.wake_word_detection = false;
    wake_relisten_fail.send_queue_residue = 3;
    wake_relisten_fail.protocol.send_start_ok = false;
    const int wake_fail_resets = wake_relisten_fail.audio.resets;
    Expect(!wake_relisten_fail.WakeRelisten(), "wake re-listen send failure is visible");
    Expect(wake_relisten_fail.send_queue_residue == 0 && wake_relisten_fail.idle &&
               wake_relisten_fail.wake_word_detection && !wake_relisten_fail.protocol.opened &&
               wake_relisten_fail.audio.resets > wake_fail_resets,
           "wake re-listen send failure clears residue, closes, drains, and restores Idle wake");
    Expect(wake_relisten_fail.decoder_resets == 0 && wake_relisten_fail.popup_sounds == 0,
           "wake re-listen decoder and popup work occur only after successful send");
    wake_relisten_fail.EnsureIdle();

    FakeAudioService observable_enable;
    observable_enable.enable_result = false;
    Expect(!observable_enable.EnableVoiceProcessing(true) &&
               !observable_enable.IsAudioProcessorRunning(),
           "voice-processing enable false is observable and never reports running");
    observable_enable.enable_result = true;
    Expect(observable_enable.EnableVoiceProcessing(true) &&
               observable_enable.IsAudioProcessorRunning(),
           "successful voice-processing enable reports the running postcondition");
    Expect(observable_enable.EnableVoiceProcessing(false) &&
               !observable_enable.IsAudioProcessorRunning(),
           "voice-processing disable has successful stopped semantics");

    FakeApplication ordinary_modes;
    Expect(ListeningModeForStartGeneration(0) != QueuedStartListeningMode::Realtime &&
               ListeningModeForStartGeneration(0) != QueuedStartListeningMode::AutoStop,
           "ordinary queued StartListening stays ManualStop, not chat Auto/Realtime");
    ordinary_modes.EnsureIdle();

    FakeApplication mqtt_no_voice_repeat;
    mqtt_no_voice_repeat.protocol.supports_correlated_open = false;
    mqtt_no_voice_repeat.protocol.supports_stroke_voice = false;
    const uint64_t mqtt_voice = mqtt_no_voice_repeat.Begin(8600);
    Expect(mqtt_no_voice_repeat.BeginLocalCandidates(mqtt_voice, 8600),
           "MQTT still degrades to local candidates without voice");
    Expect(mqtt_no_voice_repeat.protocol.start_listen == 0 &&
               mqtt_no_voice_repeat.overlay_page != "Speak",
           "MQTT local fallback never shows Speak or sends listen/start");
    mqtt_no_voice_repeat.EnsureIdle();

    if (failures != 0) {
        std::cerr << "stroke_round_integration_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_round_integration_harness: PASS\n";
    return 0;
}
