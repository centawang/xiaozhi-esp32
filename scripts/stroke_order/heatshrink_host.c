// Host-only bridge to the unchanged repository heatshrink 0.4.1.
// No IDF headers, network, or persistent codec state.
#include <stddef.h>
#include <stdint.h>

#include "heatshrink_decoder.h"
#include "heatshrink_encoder.h"

#if HEATSHRINK_VERSION_MAJOR != 0 || HEATSHRINK_VERSION_MINOR != 4 || HEATSHRINK_VERSION_PATCH != 1
#error "The v2 host profile requires heatshrink 0.4.1"
#endif

int stroke_hs_encode(const uint8_t* input, size_t size, uint8_t* output, size_t capacity) {
    if (!input || !output || size == 0 || size > 16388 || capacity > 18438) {
        return -1;
    }
    heatshrink_encoder* encoder = heatshrink_encoder_alloc(10, 4);
    if (!encoder) {
        return -1;
    }
    size_t consumed = 0, produced = 0;
    int result = -1;
    // Bound work even if a library regression stops making progress.
    for (size_t steps = 0; steps < 100000; ++steps) {
        size_t count = 0;
        if (consumed < size) {
            if (heatshrink_encoder_sink(encoder, (uint8_t*)input + consumed, size - consumed,
                                        &count) < 0) {
                break;
            }
            consumed += count;
        }
        HSE_finish_res finish =
            consumed == size ? heatshrink_encoder_finish(encoder) : HSER_FINISH_MORE;
        if (finish < 0 || produced == capacity) {
            break;
        }
        HSE_poll_res poll =
            heatshrink_encoder_poll(encoder, output + produced, capacity - produced, &count);
        if (poll < 0) {
            break;
        }
        produced += count;
        if (consumed == size && finish == HSER_FINISH_DONE && poll == HSER_POLL_EMPTY) {
            result = (int)produced;
            break;
        }
    }
    heatshrink_encoder_free(encoder);
    return result;
}

// Reference cross-check only. Strict termination is enforced by the Python
// bitstream parser; heatshrink_finish alone accepts some truncated streams.
int stroke_hs_decode(const uint8_t* input, size_t size, uint8_t* output, size_t capacity) {
    if (!input || !output || size == 0 || size > 18437 || capacity == 0 || capacity > 16389) {
        return -1;
    }
    heatshrink_decoder* decoder = heatshrink_decoder_alloc(256, 10, 4);
    if (!decoder) {
        return -1;
    }
    size_t consumed = 0, produced = 0;
    int result = -1;
    for (size_t steps = 0; steps < 100000; ++steps) {
        size_t count = 0;
        if (consumed < size) {
            if (heatshrink_decoder_sink(decoder, (uint8_t*)input + consumed, size - consumed,
                                        &count) < 0) {
                break;
            }
            consumed += count;
        }
        if (produced == capacity) {
            break;
        }
        if (heatshrink_decoder_poll(decoder, output + produced, capacity - produced, &count) < 0) {
            break;
        }
        produced += count;
        if (consumed == size && heatshrink_decoder_finish(decoder) == HSDR_FINISH_DONE) {
            result = (int)produced;
            break;
        }
    }
    heatshrink_decoder_free(decoder);
    return result;
}
