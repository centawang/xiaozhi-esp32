#pragma once

#include <cstdint>

/** A coherent 32-bit FT6336/LVGL pointer sample: pressed + x + y. */
class StrokeOrderTouchSnapshot {
public:
    struct Value {
        bool pressed = false;
        uint16_t x = 0;
        uint16_t y = 0;
    };

    static constexpr uint16_t kMaxX = 0x7FFF;
    static constexpr uint16_t kMaxY = 0xFFFF;

    static constexpr uint32_t Encode(bool pressed, uint16_t x, uint16_t y) {
        const uint32_t bounded_x = x > kMaxX ? kMaxX : x;
        return (pressed ? kPressedMask : 0U) | (bounded_x << kXShift) | y;
    }

    static constexpr Value Decode(uint32_t packed) {
        return Value{(packed & kPressedMask) != 0,
                     static_cast<uint16_t>((packed >> kXShift) & kMaxX),
                     static_cast<uint16_t>(packed & kMaxY)};
    }

private:
    static constexpr uint32_t kPressedMask = 0x80000000U;
    static constexpr uint32_t kXShift = 16;
};

/**
 * Tracks one physical touch sequence. It is used by the CoreS3 poll task, so a
 * press/release can produce at most one legacy short-tap action.
 */
class StrokeOrderTouchSequence {
public:
    static constexpr uint64_t kShortPressThresholdMs = 500;

    bool Update(bool pressed, uint64_t now_ms, bool consume_on_press, bool exclusive_on_release) {
        if (pressed) {
            if (!pressed_) {
                pressed_ = true;
                consumed_ = consume_on_press;
                acted_ = false;
                started_ms_ = now_ms;
            }
            return false;
        }

        if (!pressed_) {
            return false;
        }

        pressed_ = false;
        const bool monotonic = now_ms >= started_ms_;
        const uint64_t duration = monotonic ? now_ms - started_ms_ : kShortPressThresholdMs;
        if (!acted_ && !consumed_ && !exclusive_on_release && duration < kShortPressThresholdMs) {
            acted_ = true;
            return true;
        }
        return false;
    }

    bool pressed() const { return pressed_; }
    bool consumed() const { return consumed_; }

private:
    bool pressed_ = false;
    bool consumed_ = false;
    bool acted_ = false;
    uint64_t started_ms_ = 0;
};
