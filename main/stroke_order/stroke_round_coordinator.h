#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

enum class StrokeAbortReason : uint8_t {
    UserClose = 0,
    Alert,
    AssetSuspend,
    PowerSave,
    SurfaceDeleted,
    UnexpectedState,
    ChannelClosed,
    Reboot,
    SpeechTimeout,
    CandidateTimeout,
    NewNormalSession,
    ReplacedByNewStroke,
    MissingSessionIdentity,
    InvalidSessionIdentity,
    InvalidPayload,
    StartFailed,
};

/**
 * Transport-neutral gate for the local 笔划 voice round.
 *
 * All generation, phase, channel-session, and route decisions are published
 * under one mutex. Network callbacks capture a RouteSnapshot once and the main
 * task must commit/revalidate that exact snapshot before acting on it. A
 * service message is never assigned the generation that happens to be current
 * when the message arrives: its server session_id must match the channel that
 * was bound to that generation after OpenAudioChannel returned.
 */
class StrokeRoundCoordinator {
public:
    static constexpr size_t kMaxSessionIdBytes = 96;
    static constexpr size_t kRetiredSessionCount = 16;
    static constexpr uint64_t kSpeechTimeoutMs = 15000;
    static constexpr uint64_t kCandidateTimeoutMs = 60000;

    enum class Phase : uint8_t {
        Inactive = 0,
        Starting,
        Connecting,
        AwaitingSpeech,
        ProcessingStt,
        Candidates,
        NoMatch,
        TimedOut,
        Error,
        LocalPlayback,
    };

    enum class MessageKind : uint8_t {
        Stt = 0,
        Tts,
        Llm,
        Audio,
    };

    enum class RouteDecision : uint8_t {
        PassNormal = 0,
        InterceptStrokeStt,
        Drop,
        FailStroke,
    };

    enum class ChannelCloseDecision : uint8_t {
        Ignore = 0,
        Normal,
        AbortStroke,
    };

    enum class TimeoutKind : uint8_t {
        None = 0,
        Speech,
        Candidates,
    };

    struct RouteSnapshot {
        RouteDecision decision = RouteDecision::Drop;
        MessageKind kind = MessageKind::Stt;
        uint64_t route_epoch = 0;
        uint64_t generation = 0;
        bool session_id_valid = false;
        size_t session_id_size = 0;
        std::array<char, kMaxSessionIdBytes + 1> session_id{};
    };

    struct ChannelCloseSnapshot {
        ChannelCloseDecision decision = ChannelCloseDecision::Ignore;
        uint64_t route_epoch = 0;
        uint64_t generation = 0;
        bool session_id_valid = false;
        size_t session_id_size = 0;
        std::array<char, kMaxSessionIdBytes + 1> session_id{};
    };

    struct AbortResult {
        bool matched = false;
        bool had_stroke_channel = false;
        uint64_t generation = 0;
        std::string session_id;
    };

    struct TimeoutResult {
        TimeoutKind kind = TimeoutKind::None;
        uint64_t generation = 0;
    };

    StrokeRoundCoordinator() = default;
    StrokeRoundCoordinator(const StrokeRoundCoordinator&) = delete;
    StrokeRoundCoordinator& operator=(const StrokeRoundCoordinator&) = delete;

    static bool ValidateSessionId(const char* data, size_t size);

    uint64_t BeginRound(uint64_t now_ms);
    bool MarkConnecting(uint64_t generation);
    bool CanContinueOpen(uint64_t generation) const;
    bool BindOpenedChannel(uint64_t generation, std::string_view session_id);
    bool CanStartListening(uint64_t generation) const;
    bool MarkListeningStarted(uint64_t generation, uint64_t now_ms);
    bool MarkLocalCandidates(uint64_t generation, uint64_t now_ms);

    // Thread-safe cancel fence. Async cancel/replace sources publish this before
    // only setting an event/schedule bit. It does not mutate Application/LVGL.
    bool PublishCancelFence(uint64_t generation);
    uint64_t PublishCurrentCancelFence();
    bool HasCancelFence(uint64_t generation) const;

    RouteSnapshot CaptureRoute(MessageKind kind, const char* session_id, size_t session_id_size,
                               bool session_id_valid) const;
    bool CommitStrokeStt(const RouteSnapshot& snapshot);
    bool RevalidateNormal(const RouteSnapshot& snapshot) const;

    bool MarkCandidates(uint64_t generation, uint64_t now_ms);
    bool MarkNoMatch(uint64_t generation);
    bool MarkTimedOut(uint64_t generation);
    bool MarkError(uint64_t generation);
    bool MarkLocalPlayback(uint64_t generation);
    TimeoutResult CheckTimeouts(uint64_t now_ms) const;

    AbortResult AbortRound(uint64_t expected_generation);
    bool RetireStrokeChannel(uint64_t expected_generation);
    void CloseCurrentChannel();

    ChannelCloseSnapshot CaptureChannelClose(const char* session_id, size_t session_id_size,
                                             bool session_id_valid) const;
    bool CommitNormalChannelClose(const ChannelCloseSnapshot& snapshot);
    bool RevalidateStrokeChannelClose(const ChannelCloseSnapshot& snapshot) const;

    uint64_t CurrentGeneration() const;
    Phase CurrentPhase() const;
    bool IsCurrentGeneration(uint64_t generation) const;
    bool IsLatestGeneration(uint64_t generation) const;
    bool IsRoundActive() const;
    bool IsAwaitingSpeech(uint64_t generation) const;
    bool AllowsDeviceClass(uint64_t generation, uint8_t device_class) const;
    bool strict_routing() const;

private:
    enum class ChannelKind : uint8_t {
        None = 0,
        Normal,
        Stroke,
    };

    static bool IsActivePhase(Phase phase);
    static bool SnapshotSessionEquals(const RouteSnapshot& snapshot, const std::string& value);
    static bool SnapshotSessionEquals(const ChannelCloseSnapshot& snapshot,
                                      const std::string& value);
    static void CopySession(const char* data, size_t size, RouteSnapshot* snapshot);
    static void CopySession(const char* data, size_t size, ChannelCloseSnapshot* snapshot);

    bool IsRetiredLocked(std::string_view session_id) const;
    void RetireLocked(const std::string& session_id);
    void ClearCurrentChannelLocked();
    bool SnapshotStillCurrentLocked(const RouteSnapshot& snapshot) const;
    bool CancelFenceRaisedLocked(uint64_t generation) const;
    void ClearCancelFenceLocked();

    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    uint64_t route_epoch_ = 0;
    uint64_t cancel_fence_generation_ = 0;
    bool cancel_fence_raised_ = false;
    uint64_t speech_started_ms_ = 0;
    uint64_t candidates_started_ms_ = 0;
    Phase phase_ = Phase::Inactive;
    ChannelKind channel_kind_ = ChannelKind::None;
    uint64_t channel_generation_ = 0;
    std::string channel_session_id_;
    std::string stroke_session_id_;
    bool strict_routing_ = false;
    // Recent exact IDs prevent a just-closed stroke response from being bound
    // to a replacement channel without accumulating false positives over long
    // uptimes. Older IDs are still rejected whenever they do not equal the
    // currently bound channel.
    std::array<std::string, kRetiredSessionCount> retired_sessions_{};
    size_t next_retired_session_ = 0;
};
