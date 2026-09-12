#include "stroke_order/stroke_order_lifecycle.h"
#include "stroke_order/stroke_order_touch_input.h"

#include <atomic>
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
    StrokeOrderLifecycle lifecycle;
    lifecycle.Initialize();
    lifecycle.SetAssetsReady(true);
    lifecycle.SetPointerReady(true);
    lifecycle.SetDeviceIdle(true);
    Expect(lifecycle.CanShowEntry() && lifecycle.CanStartVoice(),
           "valid idle lifecycle shows entry and allows voice");
    lifecycle.SetVoiceTransportReady(false);
    Expect(lifecycle.CanShowEntry() && !lifecycle.CanStartVoice(),
           "MQTT/local fallback still shows entry when stroke voice is unavailable");
    Expect(lifecycle.TryOpenOverlay(), "local candidate overlay can open without stroke voice");
    lifecycle.CloseOverlay();
    lifecycle.SetVoiceTransportReady(true);
    Expect(lifecycle.CanShowEntry() && lifecycle.CanStartVoice(),
           "a correlated transport can re-enable remote STT");
    Expect(lifecycle.TryOpenOverlay(), "idle click opens overlay");
    Expect(lifecycle.overlay_open(), "overlay tracked open");

    // click -> sleep: sleep wins in the same serialized lifecycle and a stale
    // click cannot reopen until wake re-evaluates all gates.
    lifecycle.SetPowerSave(true);
    Expect(!lifecycle.overlay_open(), "sleep cancels overlay");
    Expect(!lifecycle.CanShowEntry(), "sleep hides entry");
    Expect(!lifecycle.TryOpenOverlay(), "late click rejected during sleep");
    lifecycle.SetPowerSave(false);
    Expect(lifecycle.CanShowEntry(), "idle wake restores valid entry");

    // click -> state transition and high-priority interruption.
    Expect(lifecycle.TryOpenOverlay(), "overlay reopens after wake");
    lifecycle.SetDeviceIdle(false);
    Expect(!lifecycle.overlay_open(), "non-idle state cancels overlay");
    Expect(!lifecycle.TryOpenOverlay(), "non-idle late click rejected");
    lifecycle.SetDeviceIdle(true);
    Expect(lifecycle.TryOpenOverlay(), "idle transition allows fresh click");
    lifecycle.Interrupt();
    Expect(!lifecycle.overlay_open(), "interrupt cancels overlay");
    Expect(!lifecycle.TryOpenOverlay(), "interrupted lifecycle rejects late click");
    lifecycle.SetDeviceIdle(true);
    Expect(lifecycle.CanShowEntry(), "fresh idle boundary clears interruption");

    // old asset -> suspend/unmap -> deleted/corrupt replacement. The lifecycle
    // remains closed until a validated rebind explicitly marks assets ready.
    Expect(lifecycle.TryOpenOverlay(), "old asset overlay opens");
    lifecycle.SuspendAssets();
    Expect(!lifecycle.assets_ready(), "asset suspension clears readiness");
    Expect(!lifecycle.TryOpenOverlay(), "late old-asset click rejected");
    lifecycle.SetAssetsReady(false);
    lifecycle.RebuildSurface();
    Expect(!lifecycle.CanShowEntry(), "deleted or corrupt replacement stays hidden");
    lifecycle.SetAssetsReady(true);
    Expect(lifecycle.CanShowEntry(), "validated replacement restores entry");

    // External LVGL deletion and shutdown both invalidate late operations.
    Expect(lifecycle.TryOpenOverlay(), "overlay opens before external delete");
    lifecycle.NotifyExternalDelete();
    Expect(!lifecycle.TryOpenOverlay(), "external delete invalidates late action");
    lifecycle.RebuildSurface();
    Expect(lifecycle.CanShowEntry(), "explicit rebuild restores surface");
    Expect(lifecycle.TryOpenOverlay(), "overlay opens before shutdown");
    lifecycle.Shutdown();
    Expect(!lifecycle.overlay_open(), "shutdown cancels overlay");
    Expect(!lifecycle.TryOpenOverlay(), "shutdown rejects late click");
    lifecycle.Initialize();
    lifecycle.SetAssetsReady(true);
    lifecycle.SetPointerReady(true);
    lifecycle.SetDeviceIdle(true);
    Expect(lifecycle.CanShowEntry(), "explicit attach rebuilds after shutdown");

    // Connecting/Listening may keep an already-open overlay only while listen-hold is set.
    Expect(lifecycle.TryOpenOverlay(), "overlay opens before listen hold");
    lifecycle.SetListenHold(true);
    lifecycle.SetDeviceIdle(false);
    Expect(lifecycle.overlay_open(), "listen hold keeps overlay during non-idle");
    Expect(lifecycle.CanHandleOverlayAction(), "cancel still allowed during listen hold");
    Expect(!lifecycle.CanShowEntry(), "entry stays hidden while overlay is held");
    lifecycle.SetListenHold(false);
    Expect(lifecycle.overlay_open(), "clearing hold does not itself close overlay");
    lifecycle.SetDeviceIdle(false);
    Expect(!lifecycle.overlay_open(), "non-idle without hold closes overlay");
    lifecycle.SetDeviceIdle(true);
    Expect(lifecycle.CanShowEntry(), "idle after hold restore entry");

    // One acquire-load observes a coherent packed tuple, including movement
    // while pressed and release retaining the last point.
    std::atomic<uint32_t> packed{StrokeOrderTouchSnapshot::Encode(false, 0, 0)};
    packed.store(StrokeOrderTouchSnapshot::Encode(true, 31, 47), std::memory_order_release);
    auto sample = StrokeOrderTouchSnapshot::Decode(packed.load(std::memory_order_acquire));
    Expect(sample.pressed && sample.x == 31 && sample.y == 47, "decode pressed sample");
    packed.store(StrokeOrderTouchSnapshot::Encode(true, 222, 111), std::memory_order_release);
    sample = StrokeOrderTouchSnapshot::Decode(packed.load(std::memory_order_acquire));
    Expect(sample.pressed && sample.x == 222 && sample.y == 111, "coherent moved sample");
    packed.store(StrokeOrderTouchSnapshot::Encode(false, sample.x, sample.y),
                 std::memory_order_release);
    sample = StrokeOrderTouchSnapshot::Decode(packed.load(std::memory_order_acquire));
    Expect(!sample.pressed && sample.x == 222 && sample.y == 111, "coherent release sample");
    sample =
        StrokeOrderTouchSnapshot::Decode(StrokeOrderTouchSnapshot::Encode(true, 0xFFFF, 0xFFFF));
    Expect(
        sample.x == StrokeOrderTouchSnapshot::kMaxX && sample.y == StrokeOrderTouchSnapshot::kMaxY,
        "snapshot bounds x and preserves y");

    StrokeOrderTouchSequence sequence;
    Expect(!sequence.Update(true, 100, false, false), "press edge has no legacy action");
    Expect(!sequence.Update(true, 120, true, false), "move cannot change press consumption");
    Expect(sequence.Update(false, 200, false, false), "short release triggers once");
    Expect(!sequence.Update(false, 201, false, false), "duplicate release does not retrigger");
    Expect(!sequence.Update(true, 300, true, false), "consumed press begins");
    Expect(!sequence.Update(false, 350, false, false), "consumed release is suppressed");
    Expect(!sequence.Update(true, 400, false, false), "second plain press begins");
    Expect(!sequence.Update(false, 950, false, false), "long release is suppressed");
    Expect(!sequence.Update(true, 1000, false, false), "third plain press begins");
    Expect(!sequence.Update(false, 1050, false, true), "overlay opened before release suppresses");

    if (failures != 0) {
        std::cerr << "stroke_order_lifecycle_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_order_lifecycle_harness: PASS\n";
    return 0;
}
