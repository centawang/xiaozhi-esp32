#pragma once

#include "stroke_order/stroke_order_alloc.h"
#include "stroke_order/stroke_order_candidates.h"
#include "stroke_order/stroke_order_store.h"

#include <cstddef>
#include <cstdint>

/**
 * Bounded read-only parser for stroke_pinyin.bin (SPY1).
 *
 * On-disk little-endian layout, format_version=1:
 *
 *   Header (32 bytes)
 *     u8  magic[4]           "SPY1"
 *     u16 format_version     1
 *     u16 reserved           0
 *     u32 char_count
 *     u32 group_count
 *     u32 char_index_offset  32
 *     u32 group_index_offset
 *     u32 header_crc32       CRC-32/ISO-HDLC of bytes 0..23
 *     u32 body_crc32         CRC-32/ISO-HDLC of bytes 32..end
 *
 *   Char index[char_count] (12 bytes, strictly increasing codepoint)
 *     u32 codepoint
 *     u16 rank
 *     u8  reading_count      1..8, first is primary
 *     u8  reserved           0
 *     u32 readings_offset    u16 group_id[reading_count]
 *
 *   Group index[group_count] (8 bytes)
 *     u16 member_count       1..32, sorted by rank then codepoint
 *     u16 reserved           0
 *     u32 members_offset
 *       member: u32 codepoint, u16 rank, u16 reserved
 *
 * Payload layout is canonical: all reading arrays are tightly packed in char
 * index order immediately after both indexes, then all member arrays are
 * tightly packed in group id order through end-of-file. Aliasing, overlap,
 * gaps, and trailing bytes are rejected. Character/group membership and rank
 * must be reciprocal in both directions.
 *
 * Bind() makes a bounded owned copy and validates header/body CRC plus
 * offset/length overflow. A failed bind leaves previous state unchanged.
 * Device storage is group id + rank; pinyin strings are never stored.
 */
class StrokeOrderPinyinIndex {
public:
    static constexpr uint16_t kFormatVersion = 1;
    static constexpr uint32_t kHeaderSize = 32;
    static constexpr uint32_t kCharIndexEntrySize = 12;
    static constexpr uint32_t kGroupIndexEntrySize = 8;
    static constexpr uint32_t kReadingSize = 2;
    static constexpr uint32_t kMemberSize = 8;
    static constexpr uint32_t kMaxCharacters = 1024;
    static constexpr uint32_t kMaxGroups = 1024;
    static constexpr uint8_t kMaxReadingsPerCharacter = 8;
    static constexpr uint16_t kMaxGroupMembers = 32;
    static constexpr uint32_t kMaxFileBytes = 65536;
    static constexpr uint32_t kMinTargetCodepoint = StrokeOrderStore::kMinTargetCodepoint;
    static constexpr uint32_t kMaxTargetCodepoint = StrokeOrderStore::kMaxTargetCodepoint;

    StrokeOrderPinyinIndex() = default;
    StrokeOrderPinyinIndex(const StrokeOrderPinyinIndex&) = delete;
    StrokeOrderPinyinIndex& operator=(const StrokeOrderPinyinIndex&) = delete;

    bool Bind(const uint8_t* data, size_t size);
    void Unbind();
    bool is_bound() const { return bound_; }
    uint32_t character_count() const { return char_count_; }
    uint32_t group_count() const { return group_count_; }
    bool Contains(uint32_t codepoint) const;
    bool GetRank(uint32_t codepoint, uint16_t* rank) const;
    uint32_t AppendHomophones(uint32_t primary, uint32_t* out, uint32_t cap) const;
    uint32_t AppendTopRanked(uint32_t* out, uint32_t cap) const;

#if defined(STROKE_ORDER_TESTING)
    static uint32_t TestOnlyCrc32(const uint8_t* data, size_t length);
    static bool TestOnlyAddU32(uint32_t a, uint32_t b, uint32_t* out);
    static bool TestOnlyMulU32(uint32_t a, uint32_t b, uint32_t* out);
    bool TestOnlyBindView(const uint8_t* data, size_t size);
#endif

private:
    bool BindView(const uint8_t* data, size_t size);
    bool FindChar(uint32_t codepoint, uint32_t* index) const;
    bool ReadCharEntry(uint32_t index, uint32_t* codepoint, uint16_t* rank, uint8_t* reading_count,
                       uint32_t* readings_offset) const;
    bool ReadGroupEntry(uint32_t group_id, uint16_t* member_count, uint32_t* members_offset) const;

    StrokeOrderOwnedBlob owned_blob_;
    size_t owned_size_ = 0;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool bound_ = false;
    uint32_t char_count_ = 0;
    uint32_t group_count_ = 0;
    uint32_t char_index_offset_ = 0;
    uint32_t group_index_offset_ = 0;
};

class StrokeOrderPinyinProvider : public StrokeOrderCandidateProvider {
public:
    explicit StrokeOrderPinyinProvider(const StrokeOrderPinyinIndex* index) : index_(index) {}

    uint32_t AppendHomophones(uint32_t primary, uint32_t* out, uint32_t cap) const override {
        if (index_ == nullptr || !index_->is_bound()) {
            return 0;
        }
        return index_->AppendHomophones(primary, out, cap);
    }

    uint32_t AppendTopRanked(uint32_t* out, uint32_t cap) const {
        if (index_ == nullptr || !index_->is_bound() || out == nullptr || cap == 0) {
            return 0;
        }
        return index_->AppendTopRanked(out, cap);
    }

private:
    const StrokeOrderPinyinIndex* index_ = nullptr;
};
