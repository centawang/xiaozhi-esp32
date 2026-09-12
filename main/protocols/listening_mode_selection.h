#ifndef LISTENING_MODE_SELECTION_H
#define LISTENING_MODE_SELECTION_H

#include <cstdint>

// Narrow, host-testable mapping used by Application::HandleStartListeningRequest.
// Integer values match protocol.h ListeningMode so Application can static_cast.
// Generation 0 is ordinary press-to-talk / explicit StartListening (ManualStop).
// A nonzero stroke generation uses AutoStop/VAD so the server can emit final STT
// without waiting for listen/stop. Does not change GetDefaultListeningMode.
enum class QueuedStartListeningMode : int {
    AutoStop = 0,
    ManualStop = 1,
    Realtime = 2,
};

inline QueuedStartListeningMode ListeningModeForStartGeneration(uint64_t generation) {
    return generation == 0 ? QueuedStartListeningMode::ManualStop
                           : QueuedStartListeningMode::AutoStop;
}

#endif  // LISTENING_MODE_SELECTION_H
