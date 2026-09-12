#pragma once

#include <cstdint>

/**
 * CoreS3 320x240 layout constants for the local 笔划 prototype.
 * Pure geometry; no LVGL. Host tests lock minimum touch targets.
 */
struct StrokeOrderLayout {
    static constexpr int kScreenWidth = 320;
    static constexpr int kScreenHeight = 240;
    static constexpr int kMinTouchTarget = 44;
    static constexpr uint32_t kMaxCandidates = 6;
    static constexpr int kGridColumns = 3;
    static constexpr int kGridRows = 2;
    static constexpr int kPad = 8;
    static constexpr int kGap = 8;
    static constexpr int kTitleHeight = 52;
    static constexpr int kTianSize = 200;
    static constexpr int kTianX = 8;
    static constexpr int kTianY = 20;
    static constexpr int kControlCount = 5;
    static constexpr int kControlWidth = 88;
    static constexpr int kControlHeight = 44;
    static constexpr int kControlX = 224;
    static constexpr int kControlGap = 2;
    static constexpr int kCoordMax = 1024;

    static bool CandidateCell(uint32_t index, int* x, int* y, int* w, int* h);
    static bool ControlCell(uint32_t index, int* x, int* y, int* w, int* h);
    static bool EntryRect(int* x, int* y, int* w, int* h);
    static bool TianRect(int* x, int* y, int* w, int* h);
    static int Scale(uint16_t value, int origin, int size);
};

inline bool StrokeOrderLayout::CandidateCell(uint32_t index, int* x, int* y, int* w, int* h) {
    if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    if (index >= kMaxCandidates) {
        return false;
    }
    const int col = static_cast<int>(index % static_cast<uint32_t>(kGridColumns));
    const int row = static_cast<int>(index / static_cast<uint32_t>(kGridColumns));
    const int grid_x = kPad;
    const int grid_y = kTitleHeight + kPad;
    const int grid_w = kScreenWidth - (2 * kPad);
    const int grid_h = kScreenHeight - kTitleHeight - (2 * kPad);
    const int cell_w = (grid_w - ((kGridColumns - 1) * kGap)) / kGridColumns;
    const int cell_h = (grid_h - ((kGridRows - 1) * kGap)) / kGridRows;
    if (cell_w < kMinTouchTarget || cell_h < kMinTouchTarget) {
        return false;
    }
    *w = cell_w;
    *h = cell_h;
    *x = grid_x + col * (cell_w + kGap);
    *y = grid_y + row * (cell_h + kGap);
    return true;
}

inline bool StrokeOrderLayout::ControlCell(uint32_t index, int* x, int* y, int* w, int* h) {
    if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    if (index >= static_cast<uint32_t>(kControlCount)) {
        return false;
    }
    *w = kControlWidth;
    *h = kControlHeight;
    *x = kControlX;
    *y = kPad + static_cast<int>(index) * (kControlHeight + kControlGap);
    return (*y + *h) <= kScreenHeight && *w >= kMinTouchTarget && *h >= kMinTouchTarget;
}

inline bool StrokeOrderLayout::EntryRect(int* x, int* y, int* w, int* h) {
    if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    *w = 52;
    *h = 44;
    *x = kPad;
    *y = kScreenHeight - *h - kPad;
    return *w >= kMinTouchTarget && *h >= kMinTouchTarget;
}

inline bool StrokeOrderLayout::TianRect(int* x, int* y, int* w, int* h) {
    if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    *x = kTianX;
    *y = kTianY;
    *w = kTianSize;
    *h = kTianSize;
    return true;
}

inline int StrokeOrderLayout::Scale(uint16_t value, int origin, int size) {
    if (size <= 0) {
        return origin;
    }
    return origin + (static_cast<int>(value) * size) / kCoordMax;
}
