#pragma once

#include <cstdint>

/**
 * Narrow extra-candidate provider. This phase ships a no-op implementation so
 * the controller never invents homophones or consults an unlicensed dictionary.
 */
class StrokeOrderCandidateProvider {
public:
    virtual ~StrokeOrderCandidateProvider() = default;

    // Writes extra codepoints (not including `primary`) into `out`. Returns the
    // number written, which must be <= cap.
    virtual uint32_t AppendHomophones(uint32_t primary, uint32_t* out, uint32_t cap) const {
        (void)primary;
        (void)out;
        (void)cap;
        return 0;
    }
};

class StrokeOrderNullHomophoneProvider : public StrokeOrderCandidateProvider {};
