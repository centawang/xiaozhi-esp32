#pragma once

#include "stroke_order/stroke_order_store.h"

#include <cstddef>
#include <cstdint>

// Corpus admission is a profile contract, not a Controller/UI constant.
enum class StrokeOrderProfile : uint8_t { Legacy2000, Level1_3500 };

inline bool StrokeOrderProfileContract(StrokeOrderProfile profile, uint32_t characters,
                                       uint32_t shards) {
    switch (profile) {
        case StrokeOrderProfile::Legacy2000:
            return characters == 2000 && shards == 8;
        case StrokeOrderProfile::Level1_3500:
            return characters == 3500 && shards >= 1 && shards <= 32;
    }
    return false;
}

// Read-only, generation-fenced UI boundary. Acquire NEVER validates a corpus or
// decompresses. A worker must prepare the <=6 requested glyphs before UI use.
// Source must outlive the Controller binding and all borrows. A borrow's views
// refer to source-owned raw bytes, never mmap bytes, until matching Release.
class StrokeOrderSource {
public:
    struct Info {
        StrokeOrderProfile profile = StrokeOrderProfile::Legacy2000;
        uint32_t characters = 0;
        uint32_t shards = 0;
        uint64_t generation = 0;
    };
    struct Borrow {
        const StrokeOrderStore::StrokeView* strokes = nullptr;
        uint16_t stroke_count = 0;
        uint32_t codepoint = 0;
        uint64_t generation = 0;
        uint64_t token = 0;
        const StrokeOrderSource* owner = nullptr;
    };
    virtual ~StrokeOrderSource() = default;
    virtual bool GetInfo(Info* out) const = 0;
    virtual bool Find(uint64_t generation, uint32_t codepoint, uint16_t* rank) const = 0;
    virtual uint32_t Homophones(uint64_t generation, uint32_t codepoint, uint32_t* out,
                                uint32_t capacity) const = 0;
    virtual bool Acquire(uint64_t generation, uint32_t codepoint, Borrow* out) = 0;
    virtual bool Release(const Borrow& borrow) = 0;
};
