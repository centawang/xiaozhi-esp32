#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Bounded SCB1 catalog for the version-1 sharded SOB1 corpus.
 *
 * The catalog is a small caller-owned byte view. It maps each Basic-CJK
 * codepoint and official rank to one short-named SOB1 shard/local index. Bind
 * validates canonical offsets, CRCs, unique names/codepoints/ranks, continuous
 * ranks, complete per-shard local indexes, and rank ranges. No shard bytes are
 * retained by this class.
 */
class StrokeOrderCatalog {
public:
    static constexpr uint16_t kFormatVersion = 1;
    static constexpr uint32_t kHeaderSize = 32;
    static constexpr uint32_t kEntrySize = 12;
    static constexpr uint32_t kShardSize = 40;
    static constexpr uint32_t kMaxCharacters = 2048;
    static constexpr uint16_t kMaxShards = 16;
    static constexpr uint32_t kMaxFileBytes = 65536;
    static constexpr uint32_t kMaxShardBytes = 1048576;
    static constexpr uint32_t kShardNameBytes = 16;

    struct Entry {
        uint32_t codepoint = 0;
        uint16_t rank = 0;
        uint8_t shard = 0;
        uint16_t local_index = 0;
    };

    struct Shard {
        char name[kShardNameBytes] = {};
        uint32_t size = 0;
        uint32_t crc32 = 0;
        uint32_t character_count = 0;
        uint32_t first_rank = 0;
        uint32_t last_rank = 0;
    };

    bool Bind(const uint8_t* data, size_t size);
    void Unbind();
    bool is_bound() const { return bound_; }
    uint32_t character_count() const { return character_count_; }
    uint16_t shard_count() const { return shard_count_; }
    uint32_t total_shard_bytes() const { return total_shard_bytes_; }
    bool Contains(uint32_t codepoint) const;
    bool Find(uint32_t codepoint, Entry* out) const;
    bool GetEntry(uint32_t index, Entry* out) const;
    bool GetShard(uint16_t index, Shard* out) const;

    static uint32_t CalculateCrc32(const uint8_t* data, size_t length);

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool bound_ = false;
    uint16_t shard_count_ = 0;
    uint32_t character_count_ = 0;
    uint32_t entry_offset_ = 0;
    uint32_t shard_offset_ = 0;
    uint32_t total_shard_bytes_ = 0;
};
