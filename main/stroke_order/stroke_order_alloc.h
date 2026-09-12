#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#endif

/**
 * Bounded owned copies for SOB1/SPY1. Device copies go to PSRAM-capable memory
 * and fail closed instead of silently consuming internal DRAM.
 */
struct StrokeOrderBlobDeleter {
    void operator()(uint8_t* ptr) const noexcept {
        if (ptr == nullptr) {
            return;
        }
#if defined(ESP_PLATFORM)
        heap_caps_free(ptr);
#else
        delete[] ptr;
#endif
    }
};

using StrokeOrderOwnedBlob = std::unique_ptr<uint8_t, StrokeOrderBlobDeleter>;

inline StrokeOrderOwnedBlob StrokeOrderAllocateOwned(size_t size, const char* tag) {
    if (size == 0) {
        return StrokeOrderOwnedBlob();
    }
#if defined(ESP_PLATFORM)
    uint8_t* ptr =
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ptr == nullptr) {
        ESP_LOGE(tag,
                 "owned copy fail-closed size=%u caps=SPIRAM|8BIT spiram_total=%u spiram_free=%u "
                 "internal_free=%u (no DRAM fallback)",
                 static_cast<unsigned>(size),
                 static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        return StrokeOrderOwnedBlob();
    }
    ESP_LOGI(tag, "owned copy ptr=%p size=%u caps=SPIRAM|8BIT ptr_in_psram=%d spiram_free=%u", ptr,
             static_cast<unsigned>(size), esp_ptr_external_ram(ptr) ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return StrokeOrderOwnedBlob(ptr);
#else
    (void)tag;
    uint8_t* ptr = new (std::nothrow) uint8_t[size];
    return StrokeOrderOwnedBlob(ptr);
#endif
}
