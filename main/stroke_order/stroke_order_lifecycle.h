#pragma once

/**
 * Small, platform-independent lifecycle gate shared by the production LVGL view
 * and host tests. Callers serialize access with the display/LVGL lock.
 */
class StrokeOrderLifecycle {
public:
    void Initialize() {
        initialized_ = true;
        shutdown_ = false;
        surface_ready_ = true;
        overlay_open_ = false;
        interrupted_ = false;
        voice_transport_ready_ = true;
    }

    void SetAssetsReady(bool ready) {
        assets_ready_ = ready;
        if (!ready) {
            overlay_open_ = false;
        }
    }

    void SuspendAssets() {
        assets_ready_ = false;
        overlay_open_ = false;
    }

    void SetPointerReady(bool ready) {
        pointer_ready_ = ready;
        if (!ready) {
            overlay_open_ = false;
        }
    }

    void SetVoiceTransportReady(bool ready) {
        voice_transport_ready_ = ready;
        // Local candidate demo remains usable when stroke voice is unavailable.
    }

    void SetDeviceIdle(bool idle) {
        device_idle_ = idle;
        if (idle) {
            interrupted_ = false;
        } else if (!listen_hold_) {
            overlay_open_ = false;
        }
    }

    void SetListenHold(bool hold) { listen_hold_ = hold; }

    void SetPowerSave(bool sleeping) {
        sleeping_ = sleeping;
        if (sleeping) {
            overlay_open_ = false;
        }
    }

    void Interrupt() {
        interrupted_ = true;
        overlay_open_ = false;
    }

    void NotifyExternalDelete() {
        surface_ready_ = false;
        overlay_open_ = false;
    }

    void RebuildSurface() {
        if (!shutdown_) {
            surface_ready_ = true;
        }
    }

    void CloseOverlay() { overlay_open_ = false; }

    bool TryOpenOverlay() {
        if (!CanShowEntry()) {
            return false;
        }
        overlay_open_ = true;
        return true;
    }

    void Shutdown() {
        shutdown_ = true;
        initialized_ = false;
        surface_ready_ = false;
        assets_ready_ = false;
        pointer_ready_ = false;
        device_idle_ = false;
        sleeping_ = false;
        interrupted_ = false;
        overlay_open_ = false;
        listen_hold_ = false;
        voice_transport_ready_ = false;
    }

    bool CanShowEntry() const {
        return initialized_ && !shutdown_ && surface_ready_ && assets_ready_ && pointer_ready_ &&
               device_idle_ && !sleeping_ && !interrupted_ && !overlay_open_;
    }

    bool CanStartVoice() const { return CanShowEntry() && voice_transport_ready_; }

    bool CanHandleOverlayAction() const {
        return initialized_ && !shutdown_ && surface_ready_ && assets_ready_ && pointer_ready_ &&
               !sleeping_ && !interrupted_ && overlay_open_ && (device_idle_ || listen_hold_);
    }

    bool overlay_open() const { return overlay_open_; }
    bool assets_ready() const { return assets_ready_; }
    bool sleeping() const { return sleeping_; }
    bool listen_hold() const { return listen_hold_; }

private:
    bool initialized_ = false;
    bool shutdown_ = false;
    bool surface_ready_ = false;
    bool assets_ready_ = false;
    bool pointer_ready_ = false;
    bool voice_transport_ready_ = true;
    bool device_idle_ = false;
    bool sleeping_ = false;
    bool interrupted_ = false;
    bool overlay_open_ = false;
    bool listen_hold_ = false;
};
