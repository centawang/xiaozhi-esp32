#pragma once

#include "stroke_order/stroke_order_alloc.h"
#include "stroke_order/stroke_order_store.h"

#include <cstddef>
#include <cstdint>

// Read-only v2 primitives used by product preparation and host admission.
// Borrowed bytes must remain immutable/alive until all readers are discarded.
// Call on a worker, never under the LVGL lock. Instances are not thread-safe.
// Failed Bind/Decode preserves prior published state. Primitive decode outputs
// are caller-owned scratch and unspecified on failure. No fallback to v1.
namespace stroke_order_v2 {
enum class ValidationMode { Structural, Deep };

struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};
struct Envelope {
    const uint8_t* corpus = nullptr;
    uint32_t count = 0, aux = 0, offset2 = 0, flags = 0;
};
uint32_t Crc32(const uint8_t* data, size_t size);
bool OpenEnvelope(Blob blob, const char* magic, size_t maximum, Envelope* out,
                  const uint8_t* corpus = nullptr);
// Output is unpublished scratch on failure. The caller supplies exact bounded capacity.
bool DecodeHeatshrink(Blob stored, uint8_t* output, size_t transformed_size);
bool DecodeDzz1(Blob transformed, uint8_t* output, size_t raw_size);

struct Glyph {
    StrokeOrderOwnedBlob raw;
    size_t size = 0;
    uint32_t codepoint = 0;
    uint16_t stroke_count = 0;
    StrokeOrderStore::StrokeView strokes[StrokeOrderStore::kMaxStrokesPerCharacter];
};
struct Record {
    uint32_t codepoint = 0, offset = 0, stored = 0, transformed = 0, raw = 0;
    uint32_t stored_crc = 0, raw_crc = 0;
    uint16_t rank = 0;
};
class Shard {
public:
    // Bind validates the complete index and stored CRCs; Decode validates the
    // compressed stream, raw CRC and shared geometry semantics for one record.
    bool Bind(Blob blob, const uint8_t* corpus = nullptr);
    bool GetRecord(uint32_t local, Record* out) const;
    bool Decode(uint32_t local, Glyph* out) const;
    uint32_t count() const { return header_.count; }
    uint32_t first_rank() const { return header_.aux; }

private:
    Blob blob_;
    Envelope header_;
};
struct CatalogEntry {
    uint32_t codepoint = 0;
    uint16_t rank = 0, shard = 0, local = 0;
};
class Catalog {
public:
    bool Bind(Blob blob);
    bool GetEntry(uint32_t index, CatalogEntry* out) const;
    // Both modes check complete descriptors, stored CRCs/corpus and mappings.
    // Deep additionally decodes every glyph (host/build admission).
    bool ValidateShards(const Blob* shards, size_t count,
                        ValidationMode mode = ValidationMode::Deep) const;
    const uint8_t* corpus() const { return header_.corpus; }
    uint32_t shard_count() const { return header_.aux; }

private:
    Blob blob_;
    Envelope header_;
};
class Pinyin {
public:
    bool Bind(Blob blob, const uint8_t* corpus = nullptr);
    bool Matches(const Catalog& catalog) const;
    // Streaming Top-6, excludes the input character, rank ordered and deduplicated.
    size_t Homophones(uint32_t codepoint, uint32_t* output, size_t capacity = 6) const;
    uint32_t count() const { return header_.count; }
    uint32_t group_count() const { return header_.aux; }
    uint32_t ReadingCount(uint32_t codepoint) const;
    uint32_t GroupSize(uint32_t group) const;

private:
    uint32_t Find(uint32_t codepoint) const;
    Blob blob_;
    Envelope header_;
    uint32_t char_csr_ = 0, edges_ = 0, group_csr_ = 0, members_ = 0;
};
// Structural admits an immutable container, NOT its compressed/raw geometry.
// It checks every envelope/index/stored CRC and complete catalog/SPY2 mapping.
// Deep also validates every glyph; keep it for host/build product admission.
// On-demand Shard::Decode is strict regardless of bundle validation mode.
// Default stays Deep so existing admission callers cannot silently weaken.
bool ValidateBundle(Blob catalog, Blob pinyin, const Blob* shards, size_t count,
                    ValidationMode mode = ValidationMode::Deep);

#if defined(STROKE_ORDER_TESTING)
// Thread-local allocation fault injection for this module only. -1 disables.
void FailAllocationAfter(int successful_allocations);
struct WorkCounters { size_t decodes = 0, owned_allocations = 0; };
void ResetWorkCounters();
WorkCounters GetWorkCounters();
#endif
}  // namespace stroke_order_v2
