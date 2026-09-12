#include "audio/audio_stream_generation.h"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    AudioStreamGenerationGate gate;
    Expect(gate.capture == 0 && gate.playback == 0, "fresh gate starts at generation 0");
    const uint32_t encode_gen = gate.capture;
    const uint32_t decode_gen = gate.playback;
    Expect(gate.AcceptCapture(encode_gen) && gate.AcceptPlayback(decode_gen),
           "in-flight work matches the live generation");

    std::deque<AudioStreamInFlightWork> encode_queue;
    std::deque<AudioStreamInFlightWork> decode_queue;
    encode_queue.push_back(AudioStreamInFlightWork{encode_gen, true});
    decode_queue.push_back(AudioStreamInFlightWork{decode_gen, false});
    Expect(AudioStreamCanRequeue(encode_queue.front(), gate),
           "encode work can requeue before reset");
    Expect(AudioStreamCanRequeue(decode_queue.front(), gate),
           "decode work can requeue before reset");

    gate.BumpStreaming();
    Expect(gate.capture == 1 && gate.playback == 1, "ResetStreamingState bumps both generations");
    Expect(!gate.AcceptCapture(encode_gen) && !gate.AcceptPlayback(decode_gen),
           "old in-flight encode/decode cannot rejoin after reset");
    AudioStreamDropStale(&encode_queue, gate);
    AudioStreamDropStale(&decode_queue, gate);
    Expect(encode_queue.empty() && decode_queue.empty(),
           "stale encode/decode queues are dropped rather than refilled");

    const uint32_t next_encode = gate.capture;
    Expect(AudioStreamCanRequeue(AudioStreamInFlightWork{next_encode, true}, gate),
           "fresh capture generation can encode after reset");
    Expect(AudioStreamGenerationGate::Next(0xFFFFFFFFu) == 1,
           "generation bump never wraps to the ordinary 0 token");

    if (failures != 0) {
        std::cerr << "audio_stream_generation_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio_stream_generation_harness: PASS\n";
    return 0;
}
