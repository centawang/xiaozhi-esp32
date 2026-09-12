#include "stroke_order/stroke_round_coordinator.h"

#include <algorithm>

namespace {

bool SessionByteAllowed(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
           value == ':' || value == '/' || value == '+' || value == '=' || value == '@';
}

bool ElapsedAtLeast(uint64_t now_ms, uint64_t started_ms, uint64_t duration_ms) {
    return now_ms >= started_ms && now_ms - started_ms >= duration_ms;
}

}  // namespace

bool StrokeRoundCoordinator::ValidateSessionId(const char* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxSessionIdBytes) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        if (!SessionByteAllowed(static_cast<unsigned char>(data[i]))) {
            return false;
        }
    }
    return true;
}

uint64_t StrokeRoundCoordinator::BeginRound(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (channel_kind_ != ChannelKind::None) {
        RetireLocked(channel_session_id_);
        ClearCurrentChannelLocked();
    }
    uint64_t next = generation_ + 1U;
    if (next == 0) {
        next = 1;
    }
    generation_ = next;
    phase_ = Phase::Starting;
    speech_started_ms_ = 0;
    candidates_started_ms_ = 0;
    (void)now_ms;
    stroke_session_id_.clear();
    strict_routing_ = true;
    ClearCancelFenceLocked();
    ++route_epoch_;
    return generation_;
}

bool StrokeRoundCoordinator::MarkConnecting(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || phase_ != Phase::Starting ||
        CancelFenceRaisedLocked(generation)) {
        return false;
    }
    phase_ = Phase::Connecting;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::CanContinueOpen(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0) {
        return !IsActivePhase(phase_);
    }
    if (CancelFenceRaisedLocked(generation)) {
        return false;
    }
    return generation == generation_ && (phase_ == Phase::Starting || phase_ == Phase::Connecting);
}

bool StrokeRoundCoordinator::BindOpenedChannel(uint64_t generation, std::string_view session_id) {
    const bool valid = ValidateSessionId(session_id.data(), session_id.size());
    std::lock_guard<std::mutex> lock(mutex_);

    if (generation == 0) {
        if (IsActivePhase(phase_)) {
            return false;
        }
        if (channel_kind_ == ChannelKind::Normal && valid && session_id == channel_session_id_) {
            return true;
        }
        if (strict_routing_ && (!valid || IsRetiredLocked(session_id))) {
            return false;
        }
        if (channel_kind_ != ChannelKind::None) {
            RetireLocked(channel_session_id_);
        }
        channel_kind_ = ChannelKind::Normal;
        channel_generation_ = 0;
        channel_session_id_.assign(valid ? session_id.data() : "", valid ? session_id.size() : 0);
        ++route_epoch_;
        return true;
    }

    if (generation != generation_ || (phase_ != Phase::Starting && phase_ != Phase::Connecting) ||
        CancelFenceRaisedLocked(generation) || !valid || IsRetiredLocked(session_id)) {
        return false;
    }
    if (channel_kind_ != ChannelKind::None) {
        RetireLocked(channel_session_id_);
    }
    channel_kind_ = ChannelKind::Stroke;
    channel_generation_ = generation;
    channel_session_id_.assign(session_id.data(), session_id.size());
    stroke_session_id_ = channel_session_id_;
    phase_ = Phase::AwaitingSpeech;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::CanStartListening(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0) {
        return !IsActivePhase(phase_) && channel_kind_ == ChannelKind::Normal &&
               (!strict_routing_ || !channel_session_id_.empty());
    }
    if (CancelFenceRaisedLocked(generation)) {
        return false;
    }
    return generation == generation_ && phase_ == Phase::AwaitingSpeech &&
           channel_kind_ == ChannelKind::Stroke && channel_generation_ == generation &&
           !stroke_session_id_.empty() && channel_session_id_ == stroke_session_id_;
}

bool StrokeRoundCoordinator::MarkListeningStarted(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0) {
        return !IsActivePhase(phase_) && channel_kind_ == ChannelKind::Normal &&
               (!strict_routing_ || !channel_session_id_.empty());
    }
    if (generation != generation_ || phase_ != Phase::AwaitingSpeech ||
        CancelFenceRaisedLocked(generation) || channel_kind_ != ChannelKind::Stroke ||
        channel_generation_ != generation || channel_session_id_ != stroke_session_id_) {
        return false;
    }
    speech_started_ms_ = now_ms;
    return true;
}

bool StrokeRoundCoordinator::MarkLocalCandidates(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || CancelFenceRaisedLocked(generation) ||
        (phase_ != Phase::Starting && phase_ != Phase::Connecting)) {
        return false;
    }
    phase_ = Phase::Candidates;
    candidates_started_ms_ = now_ms;
    speech_started_ms_ = 0;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::PublishCancelFence(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || !IsActivePhase(phase_)) {
        return false;
    }
    if (!CancelFenceRaisedLocked(generation)) {
        cancel_fence_raised_ = true;
        cancel_fence_generation_ = generation;
        ++route_epoch_;
    }
    return true;
}

uint64_t StrokeRoundCoordinator::PublishCurrentCancelFence() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsActivePhase(phase_) || generation_ == 0) {
        return 0;
    }
    if (!CancelFenceRaisedLocked(generation_)) {
        cancel_fence_raised_ = true;
        cancel_fence_generation_ = generation_;
        ++route_epoch_;
    }
    return generation_;
}

bool StrokeRoundCoordinator::HasCancelFence(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CancelFenceRaisedLocked(generation);
}

StrokeRoundCoordinator::RouteSnapshot StrokeRoundCoordinator::CaptureRoute(
    MessageKind kind, const char* session_id, size_t session_id_size, bool session_id_valid) const {
    RouteSnapshot snapshot;
    snapshot.kind = kind;
    snapshot.session_id_valid = session_id_valid && ValidateSessionId(session_id, session_id_size);
    if (snapshot.session_id_valid) {
        CopySession(session_id, session_id_size, &snapshot);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.route_epoch = route_epoch_;
    snapshot.generation = IsActivePhase(phase_) ? generation_ : 0;

    if (!snapshot.session_id_valid) {
        if (IsActivePhase(phase_)) {
            snapshot.decision = RouteDecision::FailStroke;
        } else {
            snapshot.decision = strict_routing_ ? RouteDecision::Drop : RouteDecision::PassNormal;
        }
        return snapshot;
    }

    const std::string_view incoming(snapshot.session_id.data(), snapshot.session_id_size);
    if (!stroke_session_id_.empty() && incoming == stroke_session_id_) {
        if (kind == MessageKind::Stt && phase_ == Phase::AwaitingSpeech &&
            channel_kind_ == ChannelKind::Stroke && channel_generation_ == generation_) {
            snapshot.decision = RouteDecision::InterceptStrokeStt;
        } else {
            snapshot.decision = RouteDecision::Drop;
        }
        return snapshot;
    }
    if (IsRetiredLocked(incoming)) {
        snapshot.decision = RouteDecision::Drop;
        return snapshot;
    }
    if (channel_kind_ == ChannelKind::Normal && incoming == channel_session_id_ &&
        !IsActivePhase(phase_)) {
        snapshot.decision = RouteDecision::PassNormal;
        snapshot.generation = 0;
        return snapshot;
    }
    if (IsActivePhase(phase_)) {
        snapshot.decision = RouteDecision::FailStroke;
        return snapshot;
    }
    snapshot.decision = strict_routing_ ? RouteDecision::Drop : RouteDecision::PassNormal;
    snapshot.generation = 0;
    return snapshot;
}

bool StrokeRoundCoordinator::CommitStrokeStt(const RouteSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.decision != RouteDecision::InterceptStrokeStt ||
        snapshot.kind != MessageKind::Stt || !SnapshotStillCurrentLocked(snapshot) ||
        snapshot.generation == 0 || snapshot.generation != generation_ ||
        phase_ != Phase::AwaitingSpeech || channel_kind_ != ChannelKind::Stroke ||
        channel_generation_ != generation_ ||
        !SnapshotSessionEquals(snapshot, stroke_session_id_)) {
        return false;
    }
    phase_ = Phase::ProcessingStt;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::RevalidateNormal(const RouteSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.decision != RouteDecision::PassNormal || snapshot.route_epoch != route_epoch_ ||
        IsActivePhase(phase_)) {
        return false;
    }
    if (!strict_routing_ && channel_kind_ == ChannelKind::None) {
        return true;
    }
    if (!snapshot.session_id_valid) {
        return !strict_routing_;
    }
    return channel_kind_ == ChannelKind::Normal &&
           SnapshotSessionEquals(snapshot, channel_session_id_);
}

bool StrokeRoundCoordinator::MarkCandidates(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ ||
        (phase_ != Phase::ProcessingStt && phase_ != Phase::LocalPlayback)) {
        return false;
    }
    phase_ = Phase::Candidates;
    candidates_started_ms_ = now_ms;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::MarkNoMatch(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || phase_ != Phase::ProcessingStt) {
        return false;
    }
    phase_ = Phase::NoMatch;
    candidates_started_ms_ = 0;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::MarkTimedOut(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || phase_ != Phase::AwaitingSpeech) {
        return false;
    }
    phase_ = Phase::TimedOut;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::MarkError(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || !IsActivePhase(phase_)) {
        return false;
    }
    phase_ = Phase::Error;
    candidates_started_ms_ = 0;
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::MarkLocalPlayback(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || phase_ != Phase::Candidates) {
        return false;
    }
    phase_ = Phase::LocalPlayback;
    candidates_started_ms_ = 0;
    ++route_epoch_;
    return true;
}

StrokeRoundCoordinator::TimeoutResult StrokeRoundCoordinator::CheckTimeouts(uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ == Phase::AwaitingSpeech && speech_started_ms_ != 0 &&
        ElapsedAtLeast(now_ms, speech_started_ms_, kSpeechTimeoutMs)) {
        return TimeoutResult{TimeoutKind::Speech, generation_};
    }
    if (phase_ == Phase::Candidates &&
        ElapsedAtLeast(now_ms, candidates_started_ms_, kCandidateTimeoutMs)) {
        return TimeoutResult{TimeoutKind::Candidates, generation_};
    }
    return {};
}

StrokeRoundCoordinator::AbortResult StrokeRoundCoordinator::AbortRound(
    uint64_t expected_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    AbortResult result;
    result.generation = generation_;
    if (expected_generation == 0 || expected_generation != generation_ || !IsActivePhase(phase_)) {
        return result;
    }
    result.matched = true;
    result.had_stroke_channel =
        channel_kind_ == ChannelKind::Stroke && channel_generation_ == expected_generation;
    result.session_id = stroke_session_id_;
    RetireLocked(stroke_session_id_);
    if (result.had_stroke_channel) {
        ClearCurrentChannelLocked();
    }
    stroke_session_id_.clear();
    phase_ = Phase::Inactive;
    speech_started_ms_ = 0;
    candidates_started_ms_ = 0;
    ClearCancelFenceLocked();
    ++route_epoch_;
    return result;
}

bool StrokeRoundCoordinator::RetireStrokeChannel(uint64_t expected_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_generation == 0 || expected_generation != generation_ || !IsActivePhase(phase_)) {
        return false;
    }
    RetireLocked(stroke_session_id_);
    if (channel_kind_ == ChannelKind::Stroke && channel_generation_ == expected_generation) {
        ClearCurrentChannelLocked();
    }
    ++route_epoch_;
    return true;
}

void StrokeRoundCoordinator::CloseCurrentChannel() {
    std::lock_guard<std::mutex> lock(mutex_);
    RetireLocked(channel_session_id_);
    ClearCurrentChannelLocked();
    ++route_epoch_;
}

StrokeRoundCoordinator::ChannelCloseSnapshot StrokeRoundCoordinator::CaptureChannelClose(
    const char* session_id, size_t session_id_size, bool session_id_valid) const {
    ChannelCloseSnapshot snapshot;
    snapshot.session_id_valid = session_id_valid && ValidateSessionId(session_id, session_id_size);
    if (snapshot.session_id_valid) {
        CopySession(session_id, session_id_size, &snapshot);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.route_epoch = route_epoch_;
    if (channel_kind_ == ChannelKind::Stroke) {
        const bool same_channel =
            snapshot.session_id_valid && SnapshotSessionEquals(snapshot, channel_session_id_);
        if (!snapshot.session_id_valid || same_channel) {
            snapshot.decision = ChannelCloseDecision::AbortStroke;
            snapshot.generation = channel_generation_;
        } else if (!IsRetiredLocked(
                       std::string_view(snapshot.session_id.data(), snapshot.session_id_size))) {
            // An unknown close while a stroke channel is active is not safe to ignore.
            snapshot.decision = ChannelCloseDecision::AbortStroke;
            snapshot.generation = channel_generation_;
        }
        return snapshot;
    }
    if (channel_kind_ == ChannelKind::Normal &&
        ((!strict_routing_ && !snapshot.session_id_valid) ||
         (snapshot.session_id_valid && SnapshotSessionEquals(snapshot, channel_session_id_)))) {
        snapshot.decision = ChannelCloseDecision::Normal;
    }
    return snapshot;
}

bool StrokeRoundCoordinator::CommitNormalChannelClose(const ChannelCloseSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.decision != ChannelCloseDecision::Normal || snapshot.route_epoch != route_epoch_ ||
        channel_kind_ != ChannelKind::Normal) {
        return false;
    }
    if (strict_routing_ &&
        (!snapshot.session_id_valid || !SnapshotSessionEquals(snapshot, channel_session_id_))) {
        return false;
    }
    RetireLocked(channel_session_id_);
    ClearCurrentChannelLocked();
    ++route_epoch_;
    return true;
}

bool StrokeRoundCoordinator::RevalidateStrokeChannelClose(
    const ChannelCloseSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.decision != ChannelCloseDecision::AbortStroke ||
        snapshot.route_epoch != route_epoch_ || snapshot.generation == 0 ||
        snapshot.generation != generation_ || channel_kind_ != ChannelKind::Stroke ||
        channel_generation_ != generation_) {
        return false;
    }
    if (!snapshot.session_id_valid) {
        return true;
    }
    if (SnapshotSessionEquals(snapshot, channel_session_id_)) {
        return true;
    }
    return !IsRetiredLocked(std::string_view(snapshot.session_id.data(), snapshot.session_id_size));
}

uint64_t StrokeRoundCoordinator::CurrentGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsActivePhase(phase_) ? generation_ : 0;
}

StrokeRoundCoordinator::Phase StrokeRoundCoordinator::CurrentPhase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_;
}

bool StrokeRoundCoordinator::IsCurrentGeneration(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ && IsActivePhase(phase_);
}

bool StrokeRoundCoordinator::IsLatestGeneration(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_;
}

bool StrokeRoundCoordinator::IsRoundActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsActivePhase(phase_);
}

bool StrokeRoundCoordinator::IsAwaitingSpeech(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ && phase_ == Phase::AwaitingSpeech;
}

bool StrokeRoundCoordinator::AllowsDeviceClass(uint64_t generation, uint8_t device_class) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || !IsActivePhase(phase_)) {
        return false;
    }
    // 0=Idle, 1=Connecting, 2=Listening, 3=Speaking, 4=Other.
    if (phase_ == Phase::Starting || phase_ == Phase::Connecting ||
        phase_ == Phase::AwaitingSpeech) {
        return device_class <= 2;
    }
    return device_class == 0;
}

bool StrokeRoundCoordinator::strict_routing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strict_routing_;
}

bool StrokeRoundCoordinator::IsActivePhase(Phase phase) { return phase != Phase::Inactive; }

bool StrokeRoundCoordinator::SnapshotSessionEquals(const RouteSnapshot& snapshot,
                                                   const std::string& value) {
    return snapshot.session_id_valid && snapshot.session_id_size == value.size() &&
           std::equal(snapshot.session_id.begin(),
                      snapshot.session_id.begin() + snapshot.session_id_size, value.begin());
}

bool StrokeRoundCoordinator::SnapshotSessionEquals(const ChannelCloseSnapshot& snapshot,
                                                   const std::string& value) {
    return snapshot.session_id_valid && snapshot.session_id_size == value.size() &&
           std::equal(snapshot.session_id.begin(),
                      snapshot.session_id.begin() + snapshot.session_id_size, value.begin());
}

void StrokeRoundCoordinator::CopySession(const char* data, size_t size, RouteSnapshot* snapshot) {
    if (snapshot == nullptr || data == nullptr || size > kMaxSessionIdBytes) {
        return;
    }
    std::copy(data, data + size, snapshot->session_id.begin());
    snapshot->session_id[size] = '\0';
    snapshot->session_id_size = size;
}

void StrokeRoundCoordinator::CopySession(const char* data, size_t size,
                                         ChannelCloseSnapshot* snapshot) {
    if (snapshot == nullptr || data == nullptr || size > kMaxSessionIdBytes) {
        return;
    }
    std::copy(data, data + size, snapshot->session_id.begin());
    snapshot->session_id[size] = '\0';
    snapshot->session_id_size = size;
}

bool StrokeRoundCoordinator::IsRetiredLocked(std::string_view session_id) const {
    if (session_id.empty()) {
        return false;
    }
    return std::any_of(retired_sessions_.begin(), retired_sessions_.end(),
                       [session_id](const std::string& retired) { return retired == session_id; });
}

void StrokeRoundCoordinator::RetireLocked(const std::string& session_id) {
    if (session_id.empty() || IsRetiredLocked(session_id)) {
        return;
    }
    retired_sessions_[next_retired_session_] = session_id;
    next_retired_session_ = (next_retired_session_ + 1U) % retired_sessions_.size();
}

void StrokeRoundCoordinator::ClearCurrentChannelLocked() {
    channel_kind_ = ChannelKind::None;
    channel_generation_ = 0;
    channel_session_id_.clear();
}

bool StrokeRoundCoordinator::SnapshotStillCurrentLocked(const RouteSnapshot& snapshot) const {
    return snapshot.route_epoch == route_epoch_ && snapshot.generation == generation_ &&
           !CancelFenceRaisedLocked(snapshot.generation);
}

bool StrokeRoundCoordinator::CancelFenceRaisedLocked(uint64_t generation) const {
    return cancel_fence_raised_ && generation != 0 && generation == generation_ &&
           generation == cancel_fence_generation_ && IsActivePhase(phase_);
}

void StrokeRoundCoordinator::ClearCancelFenceLocked() {
    cancel_fence_raised_ = false;
    cancel_fence_generation_ = 0;
}
