#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Bounded read-only parser for stroke_order.bin (SOB1).
 *
 * On-disk little-endian layout, format_version=1:
 *
 *   Header (32 bytes)
 *     u8  magic[4]          "SOB1"
 *     u16 format_version    1
 *     u16 coord_max         1024
 *     u32 char_count
 *     u32 index_offset      32
 *     u32 data_offset       32 + char_count * 16
 *     u32 data_size
 *     u32 header_crc32      CRC-32/ISO-HDLC of bytes 0..23
 *     u32 index_crc32       CRC-32/ISO-HDLC of the complete index
 *
 *   Index[char_count] (16 bytes each, strictly increasing codepoint)
 *     u32 codepoint
 *     u32 offset            absolute file offset of the character record
 *     u32 length
 *     u32 crc32             CRC of the character record bytes
 *
 *   Character record
 *     u32 codepoint
 *     u16 stroke_count
 *     u16 reserved          0
 *     Stroke[stroke_count]
 *       u16 outline_count
 *       u16 median_count
 *       Point outline[outline_count]   closed, first==last, y-down 0..1024
 *       Point median[median_count]     ordered reveal path, y-down 0..1024
 *       Point = { u16 x, u16 y }, encoded little-endian
 *
 * Bind() validates the header and complete index, including the index CRC.
 * LoadCharacter() checksums and parses one record. A failed Bind/Load leaves
 * any previous successful bind/character state unchanged. The store never
 * allocates. StrokeView retains byte views into the caller-owned blob and only
 * exposes explicitly decoded Point values; encoded bytes are never exposed as
 * Point pointers.
 *
 * These constexpr limits must stay in lockstep with
 * scripts/stroke_order/constants.py.
 */
class StrokeOrderStore {
public:
    static constexpr uint16_t kFormatVersion = 1;
    static constexpr uint16_t kCoordMax = 1024;
    static constexpr uint32_t kHeaderSize = 32;
    static constexpr uint32_t kIndexEntrySize = 16;
    static constexpr uint32_t kCharacterRecordHeaderSize = 8;
    static constexpr uint32_t kStrokeHeaderSize = 4;
    static constexpr uint32_t kPointSize = 4;
    static constexpr uint32_t kMaxCharacters = 1024;
    static constexpr uint16_t kMaxStrokesPerCharacter = 48;
    static constexpr uint16_t kMaxOutlinePointsPerStroke = 256;
    static constexpr uint16_t kMaxMedianPointsPerStroke = 64;
    static constexpr uint32_t kMaxCharacterBytes = 16384;
    static constexpr uint32_t kMaxFileBytes = 1048576;
    static constexpr uint16_t kMinStrokesPerCharacter = 1;
    static constexpr uint16_t kMinOutlinePointsPerStroke = 4;
    static constexpr uint16_t kMinMedianPointsPerStroke = 2;
    static constexpr uint32_t kMinTargetCodepoint = 0x4E00;
    static constexpr uint32_t kMaxTargetCodepoint = 0x9FFF;

    struct Point {
        uint16_t x = 0;
        uint16_t y = 0;
    };

    class StrokeView {
    public:
        StrokeView() = default;

        uint16_t outline_count() const { return outline_count_; }
        uint16_t median_count() const { return median_count_; }
        bool GetOutlinePoint(uint16_t index, Point* out) const;
        bool GetMedianPoint(uint16_t index, Point* out) const;

    private:
        friend class StrokeOrderStore;

        StrokeView(const uint8_t* outline_bytes, uint16_t outline_count,
                   const uint8_t* median_bytes, uint16_t median_count)
            : outline_bytes_(outline_bytes),
              median_bytes_(median_bytes),
              outline_count_(outline_count),
              median_count_(median_count) {}

        static bool DecodePoint(const uint8_t* encoded, uint16_t count, uint16_t index, Point* out);

        const uint8_t* outline_bytes_ = nullptr;
        const uint8_t* median_bytes_ = nullptr;
        uint16_t outline_count_ = 0;
        uint16_t median_count_ = 0;
    };

    StrokeOrderStore() = default;
    StrokeOrderStore(const StrokeOrderStore&) = delete;
    StrokeOrderStore& operator=(const StrokeOrderStore&) = delete;

    bool Bind(const uint8_t* data, size_t size);
    void Unbind();
    bool is_bound() const { return bound_; }
    uint32_t character_count() const { return char_count_; }
    bool Contains(uint32_t codepoint) const;
    bool GetCodepointAt(uint32_t index, uint32_t* codepoint) const;

    bool LoadCharacter(uint32_t codepoint);
    bool has_character() const { return has_character_; }
    uint32_t loaded_codepoint() const { return loaded_codepoint_; }
    uint16_t stroke_count() const { return stroke_count_; }
    bool GetStroke(uint16_t index, StrokeView* out) const;

#if defined(STROKE_ORDER_TESTING)
    static uint32_t TestOnlyCrc32(const uint8_t* data, size_t length);
#endif

private:
    bool FindIndexEntry(uint32_t codepoint, uint32_t* offset, uint32_t* length,
                        uint32_t* crc) const;
    static bool ParseCharacter(const uint8_t* data, size_t size, uint32_t expected_cp,
                               uint32_t offset, uint32_t length, uint32_t expected_crc,
                               uint16_t* stroke_count, StrokeView* strokes);

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool bound_ = false;
    uint32_t char_count_ = 0;
    uint32_t index_offset_ = 0;
    uint32_t data_offset_ = 0;
    uint32_t data_size_ = 0;

    bool has_character_ = false;
    uint32_t loaded_codepoint_ = 0;
    uint16_t stroke_count_ = 0;
    StrokeView strokes_[kMaxStrokesPerCharacter];
};

static_assert(sizeof(uint8_t) == 1, "SOB1 requires 8-bit bytes");
static_assert(sizeof(uint16_t) == 2, "SOB1 requires 16-bit uint16_t");
static_assert(sizeof(uint32_t) == 4, "SOB1 requires 32-bit uint32_t");
static_assert(StrokeOrderStore::kPointSize == 4, "SOB1 point encoding is exactly four bytes");
