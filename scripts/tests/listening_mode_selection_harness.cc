#include "listening_mode_selection.h"

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
    Expect(ListeningModeForStartGeneration(0) == QueuedStartListeningMode::ManualStop,
           "ordinary explicit StartListening generation 0 stays ManualStop");
    Expect(ListeningModeForStartGeneration(1) == QueuedStartListeningMode::AutoStop,
           "stroke generation 1 uses AutoStop");
    Expect(ListeningModeForStartGeneration(7) == QueuedStartListeningMode::AutoStop,
           "stroke generation 7 uses AutoStop");
    Expect(ListeningModeForStartGeneration(UINT64_C(0xFFFFFFFFFFFFFFFF)) ==
               QueuedStartListeningMode::AutoStop,
           "max stroke generation uses AutoStop");
    Expect(ListeningModeForStartGeneration(0) != QueuedStartListeningMode::AutoStop,
           "generation 0 is never AutoStop");
    Expect(ListeningModeForStartGeneration(0) != QueuedStartListeningMode::Realtime,
           "queued start helper never selects Realtime");
    Expect(ListeningModeForStartGeneration(2) != QueuedStartListeningMode::Realtime,
           "stroke start helper never selects Realtime");
    Expect(ListeningModeForStartGeneration(2) != QueuedStartListeningMode::ManualStop,
           "stroke generation is not ManualStop");

    if (failures != 0) {
        std::cerr << "listening_mode_selection_harness: FAIL (" << failures << ")\n";
        return 1;
    }
    std::cout << "listening_mode_selection_harness: PASS\n";
    return 0;
}
