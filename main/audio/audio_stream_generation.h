#pragma once

#include <cstdint>
#include <deque>

/**
 * Host-testable capture/playback generation gate used by AudioService.
 * In-flight encode/decode work may only rejoin a queue when its generation
 * still matches the current streaming generation after ResetStreamingState.
 */
struct AudioStreamGenerationGate {
    uint32_t capture = 0;
    uint32_t playback = 0;

    static uint32_t Next(uint32_t value) {
        const uint32_t next = value + 1U;
        return next == 0 ? 1U : next;
    }

    static bool Accept(uint32_t task_generation, uint32_t current_generation) {
        return task_generation == current_generation;
    }

    uint32_t BumpCapture() {
        capture = Next(capture);
        return capture;
    }

    uint32_t BumpPlayback() {
        playback = Next(playback);
        return playback;
    }

    void BumpStreaming() {
        BumpCapture();
        BumpPlayback();
    }

    bool AcceptCapture(uint32_t task_generation) const { return Accept(task_generation, capture); }

    bool AcceptPlayback(uint32_t task_generation) const {
        return Accept(task_generation, playback);
    }
};

struct AudioStreamInFlightWork {
    uint32_t generation = 0;
    bool encode = true;
};

inline bool AudioStreamCanRequeue(const AudioStreamInFlightWork& work,
                                  const AudioStreamGenerationGate& gate) {
    return work.encode ? gate.AcceptCapture(work.generation) : gate.AcceptPlayback(work.generation);
}

inline void AudioStreamDropStale(std::deque<AudioStreamInFlightWork>* queue,
                                 const AudioStreamGenerationGate& gate) {
    if (queue == nullptr) {
        return;
    }
    std::deque<AudioStreamInFlightWork> kept;
    while (!queue->empty()) {
        const AudioStreamInFlightWork work = queue->front();
        queue->pop_front();
        if (AudioStreamCanRequeue(work, gate)) {
            kept.push_back(work);
        }
    }
    *queue = std::move(kept);
}
