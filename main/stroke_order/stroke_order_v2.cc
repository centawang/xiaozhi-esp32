#include "stroke_order/stroke_order_v2.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

extern "C" {
#include "heatshrink_decoder.h"
}
#if HEATSHRINK_VERSION_MAJOR != 0 || HEATSHRINK_VERSION_MINOR != 4 || HEATSHRINK_VERSION_PATCH != 1
#error "SOB2 requires the resolved heatshrink 0.4.1"
#endif
#if !HEATSHRINK_DYNAMIC_ALLOC
#error "SOB2 requires heatshrink dynamic allocation for fixed 10/4 parameters"
#endif

namespace stroke_order_v2 {
namespace {
constexpr uint32_t kCharacters = 3500;
constexpr uint32_t kRawMax = 16384;
constexpr uint32_t kTransformedMax = 16388;
// Device admission is intentionally stricter than the host 18437-byte codec cap.
constexpr uint32_t kStoredMax = 16384;
#if defined(STROKE_ORDER_TESTING)
thread_local int allocation_budget = -1;
thread_local WorkCounters work_counters;
#endif
bool AllocationAllowed() {
#if defined(STROKE_ORDER_TESTING)
    if (allocation_budget == 0)
        return false;
    if (allocation_budget > 0)
        --allocation_budget;
#endif
    return true;
}
StrokeOrderOwnedBlob Allocate(size_t size) {
    if (!AllocationAllowed())
        return {};
#if defined(STROKE_ORDER_TESTING)
    ++work_counters.owned_allocations;
#endif
    return StrokeOrderAllocateOwned(size, "StrokeV2");
}
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = v & 255;
    p[1] = v >> 8;
}
bool Codepoint(uint32_t cp) { return cp >= 0x4e00 && cp <= 0x9fff; }
bool Zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i])
            return false;
    return true;
}
// Syntax/length validator only, not a second decompressor. The actual bytes
// are produced exclusively by the unchanged resolved heatshrink decoder.
bool StrictTokens(Blob input, size_t expected) {
    size_t bit = 0, produced = 0;
    auto bits = [&](size_t n, uint32_t* value) {
        if (n > input.size * 8 - bit)
            return false;
        *value = 0;
        while (n--) {
            *value = (*value << 1) | ((input.data[bit / 8] >> (7 - bit % 8)) & 1);
            ++bit;
        }
        return true;
    };
    while (produced < expected) {
        uint32_t tag, value, length;
        if (!bits(1, &tag))
            return false;
        if (tag) {
            if (!bits(8, &value))
                return false;
            ++produced;
        } else {
            if (!bits(10, &value) || !bits(4, &length))
                return false;
            ++length;
            if (length > expected - produced)
                return false;
            produced += length;
        }
    }
    uint32_t padding;
    size_t remaining = input.size * 8 - bit;
    return remaining < 8 && bits(remaining, &padding) && padding == 0;
}
uint32_t Crc32Masked(const uint8_t* data, size_t size, size_t zero_at) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= i >= zero_at && i - zero_at < 4 ? 0 : data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}
Record ReadRecord(const uint8_t* p) {
    return {U32(p),      U32(p + 4),  U32(p + 8),  U32(p + 12),
            U32(p + 16), U32(p + 20), U32(p + 24), U16(p + 28)};
}
}  // namespace

#if defined(STROKE_ORDER_TESTING)
void FailAllocationAfter(int count) { allocation_budget = count; }
void ResetWorkCounters() { work_counters = {}; }
WorkCounters GetWorkCounters() { return work_counters; }
#endif
uint32_t Crc32(const uint8_t* data, size_t size) { return Crc32Masked(data, size, size); }
bool OpenEnvelope(Blob blob, const char* magic, size_t maximum, Envelope* out,
                  const uint8_t* corpus) {
    if (!blob.data || !out || !magic || blob.size < 64 || blob.size > maximum)
        return false;
    if (std::memcmp(magic, "SOB2", 4) && std::memcmp(magic, "SCB2", 4) &&
        std::memcmp(magic, "SPY2", 4))
        return false;
    const uint8_t* p = blob.data;
    if (std::memcmp(p, magic, 4) || U16(p + 4) != 2 || U16(p + 6) != 3500 || Zero(p + 8, 16) ||
        (corpus && std::memcmp(corpus, p + 8, 16)) || U32(p + 32) != 64 ||
        U32(p + 40) != blob.size || !Zero(p + 56, 8))
        return false;
    // Compute the masked header CRC without copying a header onto the task stack.
    if (Crc32Masked(p, 64, 48) != U32(p + 48) || Crc32(p + 64, blob.size - 64) != U32(p + 44))
        return false;
    *out = {p + 8, U32(p + 24), U32(p + 28), U32(p + 36), U32(p + 52)};
    return true;
}
bool DecodeHeatshrink(Blob stored, uint8_t* output, size_t expected) {
    if (!stored.data || !output || !stored.size || stored.size > kStoredMax || !expected ||
        expected > kTransformedMax || !StrictTokens(stored, expected) || !AllocationAllowed())
        return false;
    std::unique_ptr<heatshrink_decoder, decltype(&heatshrink_decoder_free)> decoder(
        heatshrink_decoder_alloc(256, 10, 4), heatshrink_decoder_free);
    if (!decoder)
        return false;
    size_t consumed = 0, produced = 0;
    // Output never exceeds its exact capacity. Once full, poll into one byte to
    // reject an unexpected output token; do not rely on finish's permissive EOF.
    for (size_t steps = 0; steps < 100000; ++steps) {
        size_t sunk = 0, polled = 0;
        if (consumed < stored.size &&
            heatshrink_decoder_sink(decoder.get(), const_cast<uint8_t*>(stored.data) + consumed,
                                    stored.size - consumed, &sunk) < 0)
            return false;
        consumed += sunk;
        uint8_t excess;
        if (heatshrink_decoder_poll(decoder.get(),
                                    produced < expected ? output + produced : &excess,
                                    produced < expected ? expected - produced : 1, &polled) < 0)
            return false;
        if (polled > expected - produced)
            return false;
        produced += polled;
        auto finished = heatshrink_decoder_finish(decoder.get());
        if (finished < 0)
            return false;
        if (consumed == stored.size && finished == HSDR_FINISH_DONE)
            return produced == expected;
        if (!sunk && !polled)
            return false;
    }
    return false;
}
bool DecodeDzz1(Blob input, uint8_t* out, size_t raw_size) {
    if (!input.data || !out || input.size < 12 || input.size > kTransformedMax || raw_size < 8 ||
        raw_size > kRawMax || input.size > raw_size + 4 || std::memcmp(input.data, "DZZ1", 4))
        return false;
    const uint8_t* p = input.data;
    uint16_t strokes = U16(p + 8);
    if (!Codepoint(U32(p + 4)) || strokes < 1 || strokes > 48 || U16(p + 10))
        return false;
    std::memcpy(out, p + 4, 8);
    size_t read = 12, write = 8;
    for (uint16_t stroke = 0; stroke < strokes; ++stroke) {
        if (input.size - read < 4)
            return false;
        uint16_t outline = U16(p + read), median = U16(p + read + 2);
        if (outline < 4 || outline > 256 || median < 2 || median > 64 ||
            4 + 4 * size_t(outline + median) > raw_size - write)
            return false;
        std::memcpy(out + write, p + read, 4);
        read += 4;
        write += 4;
        for (int path = 0; path < 2; ++path) {
            int previous[2] = {0, 0};
            const size_t path_start = write;
            uint16_t count = path ? median : outline;
            for (uint16_t point = 0; point < count; ++point) {
                for (int axis = 0; axis < 2; ++axis) {
                    if (read == input.size)
                        return false;
                    unsigned value = p[read++];
                    if (value & 128) {
                        if (read == input.size)
                            return false;
                        unsigned high = p[read++];
                        if (high < 1 || high > 16)
                            return false;
                        value = (value & 127) | (high << 7);
                    }
                    if (value > 2048)
                        return false;
                    int delta = (value & 1) ? -int(value / 2) - 1 : int(value / 2);
                    int coord = previous[axis] + delta;
                    if (coord < 0 || coord > 1024)
                        return false;
                    Put16(out + write, coord);
                    write += 2;
                    previous[axis] = coord;
                }
            }
            if (!path && std::memcmp(out + path_start, out + write - 4, 4))
                return false;
        }
    }
    return read == input.size && write == raw_size;
}
bool Shard::Bind(Blob blob, const uint8_t* corpus) {
    Envelope h;
    if (!OpenEnvelope(blob, "SOB2", 1048576, &h, corpus) || !h.count || h.count > kCharacters ||
        !h.aux || h.aux > kCharacters || h.count > kCharacters + 1 - h.aux ||
        h.flags != 0x040a0101 || h.offset2 != 64 + 32 * h.count || h.offset2 > blob.size)
        return false;
    auto seen = Allocate((0xa000 - 0x4e00 + 7) / 8);
    if (!seen)
        return false;
    std::memset(seen.get(), 0, (0xa000 - 0x4e00 + 7) / 8);
    size_t cursor = h.offset2;
    for (uint32_t i = 0; i < h.count; ++i) {
        const auto* entry = blob.data + 64 + 32 * i;
        Record r = ReadRecord(entry);
        if (!Codepoint(r.codepoint) || r.rank != h.aux + i || U16(entry + 30) ||
            r.offset != cursor || !r.stored || r.stored > kStoredMax ||
            r.stored > blob.size - cursor || r.raw < 8 || r.raw > kRawMax || r.transformed < 12 ||
            r.transformed > kTransformedMax || r.transformed > r.raw + 4)
            return false;
        uint32_t bit = r.codepoint - 0x4e00;
        if (seen.get()[bit / 8] & (1u << (bit % 8)))
            return false;
        seen.get()[bit / 8] |= 1u << (bit % 8);
        if (Crc32(blob.data + cursor, r.stored) != r.stored_crc)
            return false;
        cursor += r.stored;
    }
    if (cursor != blob.size)
        return false;
    blob_ = blob;
    header_ = h;
    return true;
}
bool Shard::GetRecord(uint32_t local, Record* out) const {
    if (!blob_.data || !out || local >= header_.count)
        return false;
    *out = ReadRecord(blob_.data + 64 + 32 * local);
    return true;
}
bool Shard::Decode(uint32_t local, Glyph* out) const {
#if defined(STROKE_ORDER_TESTING)
    ++work_counters.decodes;
#endif
    Record r;
    if (!out || !GetRecord(local, &r))
        return false;
    auto transformed = Allocate(r.transformed);
    Glyph next;
    next.raw = Allocate(r.raw);
    if (!transformed || !next.raw || Crc32(blob_.data + r.offset, r.stored) != r.stored_crc ||
        !DecodeHeatshrink({blob_.data + r.offset, r.stored}, transformed.get(), r.transformed) ||
        !DecodeDzz1({transformed.get(), r.transformed}, next.raw.get(), r.raw) ||
        !StrokeOrderStore::ParseRawRecord(next.raw.get(), r.raw, r.codepoint, r.raw_crc,
                                          &next.stroke_count, next.strokes))
        return false;
    next.size = r.raw;
    next.codepoint = r.codepoint;
    *out = std::move(next);
    return true;
}
bool Catalog::Bind(Blob blob) {
    Envelope h;
    if (!OpenEnvelope(blob, "SCB2", 65536, &h) || h.count != kCharacters || !h.aux || h.aux > 32 ||
        h.flags || h.offset2 != 64 + 12 * kCharacters || h.offset2 + 40 * h.aux != blob.size)
        return false;
    uint32_t first = 1;
    for (uint32_t s = 0; s < h.aux; ++s) {
        const auto* d = blob.data + h.offset2 + s * 40;
        char name[16] = {'s', 'o', char('0' + s / 10), char('0' + s % 10), '.', 'b', 'i', 'n'};
        uint32_t count = U16(d + 28), last = U16(d + 26);
        if (std::memcmp(name, d, 16) || !Zero(d + 30, 10) || U32(d + 16) <= 64 ||
            U32(d + 16) > 1048576 || U16(d + 24) != first || !count || last != first + count - 1 ||
            last > kCharacters)
            return false;
        first = last + 1;
    }
    if (first != kCharacters + 1)
        return false;
    auto ranks = Allocate(kCharacters + 1);
    if (!ranks)
        return false;
    std::memset(ranks.get(), 0, kCharacters + 1);
    uint32_t previous = 0;
    for (uint32_t i = 0; i < h.count; ++i) {
        const auto* e = blob.data + 64 + 12 * i;
        uint32_t cp = U32(e), rank = U16(e + 4), shard = U16(e + 6), local = U16(e + 8);
        if (!Codepoint(cp) || cp <= previous || !rank || rank > kCharacters || ranks.get()[rank] ||
            shard >= h.aux || U16(e + 10))
            return false;
        const auto* d = blob.data + h.offset2 + 40 * shard;
        if (local >= U16(d + 28) || rank != U16(d + 24) + local)
            return false;
        ranks.get()[rank] = 1;
        previous = cp;
    }
    blob_ = blob;
    header_ = h;
    return true;
}
bool Catalog::GetEntry(uint32_t index, CatalogEntry* out) const {
    if (!blob_.data || !out || index >= header_.count)
        return false;
    const auto* e = blob_.data + 64 + 12 * index;
    *out = {U32(e), U16(e + 4), U16(e + 6), U16(e + 8)};
    return true;
}
bool Catalog::ValidateShards(const Blob* shards, size_t count, ValidationMode mode) const {
    if (!blob_.data || !shards || count != header_.aux)
        return false;
    // Validate all shards once, not once per catalog character.
    for (size_t s = 0; s < count; ++s) {
        const auto* d = blob_.data + header_.offset2 + 40 * s;
        Shard shard;
        if (!shards[s].data || shards[s].size != U32(d + 16) ||
            Crc32(shards[s].data, shards[s].size) != U32(d + 20) ||
            !shard.Bind(shards[s], header_.corpus) || shard.count() != U16(d + 28) ||
            shard.first_rank() != U16(d + 24))
            return false;
        if (mode == ValidationMode::Deep) {
            Glyph glyph;
            for (uint32_t i = 0; i < shard.count(); ++i)
                if (!shard.Decode(i, &glyph))
                    return false;
        }
    }
    for (uint32_t i = 0; i < header_.count; ++i) {
        CatalogEntry e;
        GetEntry(i, &e);
        // Validated immutable shard tables are bounded by their descriptors.
        const auto* r = shards[e.shard].data + 64 + 32 * e.local;
        if (U32(r) != e.codepoint || U16(r + 28) != e.rank)
            return false;
    }
    return true;
}
bool Pinyin::Bind(Blob blob, const uint8_t* corpus) {
    Envelope h;
    if (!OpenEnvelope(blob, "SPY2", 524288, &h, corpus) || !h.count || h.count > kCharacters ||
        !h.aux || h.aux > 4096 || h.flags < h.count || h.flags > h.count * 16 || h.flags < h.aux ||
        h.flags > h.aux * 64 || h.flags > 56000)
        return false;
    uint32_t co = 64 + 8 * h.count, edges = co + 4 * (h.count + 1);
    uint32_t go = edges + 2 * h.flags, members = go + 4 * (h.aux + 1);
    if (h.offset2 != co || members + 2 * h.flags != blob.size)
        return false;
    const auto* p = blob.data;
    if (U32(p + co) || U32(p + go) || U32(p + co + 4 * h.count) != h.flags ||
        U32(p + go + 4 * h.aux) != h.flags)
        return false;
    auto ranks = Allocate(h.count + 1);
    auto matched = Allocate((h.flags + 7) / 8);
    if (!ranks || !matched)
        return false;
    std::memset(ranks.get(), 0, h.count + 1);
    std::memset(matched.get(), 0, (h.flags + 7) / 8);
    uint32_t previous = 0;
    for (uint32_t c = 0; c < h.count; ++c) {
        const auto* ch = p + 64 + 8 * c;
        uint32_t cp = U32(ch), rank = U16(ch + 4);
        if (!Codepoint(cp) || cp <= previous || !rank || rank > h.count || ranks.get()[rank] ||
            U16(ch + 6))
            return false;
        ranks.get()[rank] = 1;
        previous = cp;
        uint32_t a = U32(p + co + 4 * c), b = U32(p + co + 4 * (c + 1));
        if (a >= b || b > h.flags || b - a > 16)
            return false;
        for (uint32_t e = a; e < b; ++e) {
            uint16_t gid = U16(p + edges + 2 * e);
            if (gid >= h.aux)
                return false;
            for (uint32_t j = a; j < e; ++j)
                if (U16(p + edges + 2 * j) == gid)
                    return false;
        }
    }
    for (uint32_t g = 0; g < h.aux; ++g) {
        uint32_t a = U32(p + go + 4 * g), b = U32(p + go + 4 * (g + 1));
        if (a >= b || b > h.flags || b - a > 64)
            return false;
        uint16_t prev_rank = 0;
        for (uint32_t m = a; m < b; ++m) {
            uint16_t c = U16(p + members + 2 * m);
            if (c >= h.count)
                return false;
            uint16_t rank = U16(p + 64 + 8 * c + 4);
            if (rank <= prev_rank)
                return false;
            prev_rank = rank;
            uint32_t start = U32(p + co + 4 * c), end = U32(p + co + 4 * (c + 1));
            bool found = false;
            for (uint32_t e = start; e < end; ++e) {
                if (U16(p + edges + 2 * e) != g)
                    continue;
                if (matched.get()[e / 8] & (1u << (e % 8)))
                    return false;
                matched.get()[e / 8] |= 1u << (e % 8);
                found = true;
                break;
            }
            if (!found)
                return false;
        }
    }
    // Equal E, no repeated reverse edges, and every reverse edge matched a
    // distinct forward edge: a bijection. Work <=16*E, storage O(N+E).
    blob_ = blob;
    header_ = h;
    char_csr_ = co;
    edges_ = edges;
    group_csr_ = go;
    members_ = members;
    return true;
}
bool Pinyin::Matches(const Catalog& catalog) const {
    if (!blob_.data || header_.count != kCharacters || !catalog.corpus() ||
        std::memcmp(header_.corpus, catalog.corpus(), 16))
        return false;
    for (uint32_t i = 0; i < header_.count; ++i) {
        CatalogEntry e;
        if (!catalog.GetEntry(i, &e) || e.codepoint != U32(blob_.data + 64 + 8 * i) ||
            e.rank != U16(blob_.data + 68 + 8 * i))
            return false;
    }
    return true;
}
uint32_t Pinyin::Find(uint32_t cp) const {
    uint32_t lo = 0, hi = header_.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (U32(blob_.data + 64 + 8 * mid) < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < header_.count && U32(blob_.data + 64 + 8 * lo) == cp ? lo : header_.count;
}
uint32_t Pinyin::ReadingCount(uint32_t cp) const {
    uint32_t c = Find(cp);
    if (c == header_.count)
        return 0;
    return U32(blob_.data + char_csr_ + 4 * (c + 1)) - U32(blob_.data + char_csr_ + 4 * c);
}
uint32_t Pinyin::GroupSize(uint32_t g) const {
    if (g >= header_.aux)
        return 0;
    return U32(blob_.data + group_csr_ + 4 * (g + 1)) - U32(blob_.data + group_csr_ + 4 * g);
}
size_t Pinyin::Homophones(uint32_t cp, uint32_t* output, size_t capacity) const {
    if (!output || !capacity)
        return 0;
    capacity = std::min(capacity, size_t(6));
    uint32_t c = Find(cp);
    if (c == header_.count)
        return 0;
    uint16_t best_rank[6] = {};
    size_t used = 0;
    const auto* p = blob_.data;
    uint32_t start = U32(p + char_csr_ + 4 * c), end = U32(p + char_csr_ + 4 * (c + 1));
    for (uint32_t edge = start; edge < end; ++edge) {
        uint16_t group = U16(p + edges_ + 2 * edge);
        uint32_t a = U32(p + group_csr_ + 4 * group), b = U32(p + group_csr_ + 4 * (group + 1));
        for (uint32_t m = a; m < b; ++m) {
            uint16_t member = U16(p + members_ + 2 * m);
            if (member == c)
                continue;
            uint16_t rank = U16(p + 68 + 8 * member);
            size_t pos = 0;
            while (pos < used && best_rank[pos] < rank)
                ++pos;
            if ((pos < used && best_rank[pos] == rank) || pos == capacity)
                continue;
            size_t last = std::min(used, capacity - 1);
            for (size_t j = last; j > pos; --j) {
                best_rank[j] = best_rank[j - 1];
                output[j] = output[j - 1];
            }
            best_rank[pos] = rank;
            output[pos] = U32(p + 64 + 8 * member);
            used = std::min(used + 1, capacity);
        }
    }
    return used;
}
bool ValidateBundle(Blob catalog, Blob pinyin, const Blob* shards, size_t count,
                    ValidationMode mode) {
    Catalog cat;
    Pinyin py;
    return cat.Bind(catalog) && py.Bind(pinyin, cat.corpus()) && py.Matches(cat) &&
           cat.ValidateShards(shards, count, mode);
}
}  // namespace stroke_order_v2
