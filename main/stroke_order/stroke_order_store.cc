#include "stroke_order/stroke_order_store.h"

#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_log.h>
static const char* TAG = "StrokeOrder";
#define STROKE_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
#define STROKE_LOGW(fmt, ...)
#endif

namespace {

constexpr uint8_t kMagicBytes[4] = {'S', 'O', 'B', '1'};

bool AddU32(uint32_t a, uint32_t b, uint32_t* out) {
    const uint64_t sum = static_cast<uint64_t>(a) + b;
    if (sum > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<uint32_t>(sum);
    return true;
}

bool MulU32(uint32_t a, uint32_t b, uint32_t* out) {
    const uint64_t product = static_cast<uint64_t>(a) * b;
    if (product > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<uint32_t>(product);
    return true;
}

bool RangeInSize(uint32_t offset, uint32_t length, size_t size) {
    uint32_t end = 0;
    if (!AddU32(offset, length, &end)) {
        return false;
    }
    return static_cast<uint64_t>(end) <= size;
}

bool ReadU16(const uint8_t* data, size_t size, uint32_t offset, uint16_t* out) {
    if (data == nullptr || out == nullptr || !RangeInSize(offset, 2, size)) {
        return false;
    }
    *out = static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    return true;
}

bool ReadU32(const uint8_t* data, size_t size, uint32_t offset, uint32_t* out) {
    if (data == nullptr || out == nullptr || !RangeInSize(offset, 4, size)) {
        return false;
    }
    *out = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

uint32_t Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t lowest = crc & 1u;
            crc >>= 1;
            if (lowest != 0) {
                crc ^= 0xEDB88320u;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool ValidCodepoint(uint32_t codepoint) {
    return codepoint >= StrokeOrderStore::kMinTargetCodepoint &&
           codepoint <= StrokeOrderStore::kMaxTargetCodepoint;
}

bool ReadPoint(const uint8_t* data, size_t size, uint32_t offset, StrokeOrderStore::Point* out) {
    if (out == nullptr || !ReadU16(data, size, offset, &out->x)) {
        return false;
    }
    uint32_t y_offset = 0;
    return AddU32(offset, 2, &y_offset) && ReadU16(data, size, y_offset, &out->y);
}

}  // namespace

bool StrokeOrderStore::StrokeView::GetOutlinePoint(uint16_t index, Point* out) const {
    return DecodePoint(outline_bytes_, outline_count_, index, out);
}

bool StrokeOrderStore::StrokeView::GetMedianPoint(uint16_t index, Point* out) const {
    return DecodePoint(median_bytes_, median_count_, index, out);
}

bool StrokeOrderStore::StrokeView::DecodePoint(const uint8_t* encoded, uint16_t count,
                                               uint16_t index, Point* out) {
    if (encoded == nullptr || out == nullptr || index >= count) {
        return false;
    }
    const size_t offset = static_cast<size_t>(index) * kPointSize;
    Point decoded;
    decoded.x = static_cast<uint16_t>(encoded[offset]) |
                static_cast<uint16_t>(static_cast<uint16_t>(encoded[offset + 1]) << 8);
    decoded.y = static_cast<uint16_t>(encoded[offset + 2]) |
                static_cast<uint16_t>(static_cast<uint16_t>(encoded[offset + 3]) << 8);
    *out = decoded;
    return true;
}

bool StrokeOrderStore::Bind(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kHeaderSize || size > kMaxFileBytes) {
        return false;
    }
    if (std::memcmp(data, kMagicBytes, 4) != 0) {
        return false;
    }

    uint16_t version = 0;
    uint16_t coord_max = 0;
    uint32_t char_count = 0;
    uint32_t index_offset = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t header_crc = 0;
    uint32_t index_crc = 0;
    if (!ReadU16(data, size, 4, &version) || !ReadU16(data, size, 6, &coord_max) ||
        !ReadU32(data, size, 8, &char_count) || !ReadU32(data, size, 12, &index_offset) ||
        !ReadU32(data, size, 16, &data_offset) || !ReadU32(data, size, 20, &data_size) ||
        !ReadU32(data, size, 24, &header_crc) || !ReadU32(data, size, 28, &index_crc)) {
        return false;
    }
    if (version != kFormatVersion || coord_max != kCoordMax || char_count > kMaxCharacters ||
        index_offset != kHeaderSize) {
        return false;
    }
    if (Crc32(data, 24) != header_crc) {
        STROKE_LOGW("header checksum mismatch");
        return false;
    }

    uint32_t index_bytes = 0;
    uint32_t expected_data_offset = 0;
    uint32_t data_end = 0;
    if (!MulU32(char_count, kIndexEntrySize, &index_bytes) ||
        !AddU32(index_offset, index_bytes, &expected_data_offset) ||
        expected_data_offset != data_offset || !RangeInSize(index_offset, index_bytes, size) ||
        Crc32(data + index_offset, index_bytes) != index_crc ||
        !AddU32(data_offset, data_size, &data_end) || static_cast<uint64_t>(data_end) != size) {
        return false;
    }

    uint32_t cursor = data_offset;
    uint32_t prev_cp = 0;
    bool have_prev = false;
    for (uint32_t i = 0; i < char_count; ++i) {
        uint32_t entry_offset = 0;
        uint32_t codepoint = 0;
        uint32_t rec_offset = 0;
        uint32_t rec_length = 0;
        uint32_t rec_crc = 0;
        uint32_t rec_offset_pos = 0;
        uint32_t rec_length_pos = 0;
        uint32_t rec_crc_pos = 0;
        if (!MulU32(i, kIndexEntrySize, &entry_offset) ||
            !AddU32(index_offset, entry_offset, &entry_offset) ||
            !AddU32(entry_offset, 4, &rec_offset_pos) ||
            !AddU32(entry_offset, 8, &rec_length_pos) || !AddU32(entry_offset, 12, &rec_crc_pos) ||
            !ReadU32(data, size, entry_offset, &codepoint) ||
            !ReadU32(data, size, rec_offset_pos, &rec_offset) ||
            !ReadU32(data, size, rec_length_pos, &rec_length) ||
            !ReadU32(data, size, rec_crc_pos, &rec_crc)) {
            return false;
        }
        uint32_t rec_end = 0;
        if (!ValidCodepoint(codepoint) || (have_prev && codepoint <= prev_cp) ||
            rec_length < kCharacterRecordHeaderSize || rec_length > kMaxCharacterBytes ||
            rec_offset != cursor || !AddU32(rec_offset, rec_length, &rec_end) ||
            rec_end > data_end || rec_offset < data_offset) {
            STROKE_LOGW("rejected malicious index entry");
            return false;
        }
        cursor = rec_end;
        prev_cp = codepoint;
        have_prev = true;
        (void)rec_crc;
    }
    if (cursor != data_end) {
        return false;
    }

    data_ = data;
    size_ = size;
    bound_ = true;
    char_count_ = char_count;
    index_offset_ = index_offset;
    data_offset_ = data_offset;
    data_size_ = data_size;
    has_character_ = false;
    loaded_codepoint_ = 0;
    stroke_count_ = 0;
    return true;
}

void StrokeOrderStore::Unbind() {
    data_ = nullptr;
    size_ = 0;
    bound_ = false;
    char_count_ = 0;
    index_offset_ = 0;
    data_offset_ = 0;
    data_size_ = 0;
    has_character_ = false;
    loaded_codepoint_ = 0;
    stroke_count_ = 0;
}

bool StrokeOrderStore::Contains(uint32_t codepoint) const {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    return FindIndexEntry(codepoint, &offset, &length, &crc);
}

bool StrokeOrderStore::GetCodepointAt(uint32_t index, uint32_t* codepoint) const {
    if (!bound_ || codepoint == nullptr || index >= char_count_) {
        return false;
    }
    uint32_t entry_offset = 0;
    if (!MulU32(index, kIndexEntrySize, &entry_offset) ||
        !AddU32(index_offset_, entry_offset, &entry_offset) ||
        !ReadU32(data_, size_, entry_offset, codepoint)) {
        return false;
    }
    return true;
}

bool StrokeOrderStore::LoadCharacter(uint32_t codepoint) {
    if (!bound_) {
        return false;
    }
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    if (!FindIndexEntry(codepoint, &offset, &length, &crc)) {
        return false;
    }

    StrokeView local_strokes[kMaxStrokesPerCharacter];
    uint16_t local_count = 0;
    if (!RangeInSize(offset, length, size_) ||
        !ParseRawRecord(data_ + offset, length, codepoint, crc, &local_count, local_strokes)) {
        return false;
    }

    has_character_ = true;
    loaded_codepoint_ = codepoint;
    stroke_count_ = local_count;
    for (uint16_t i = 0; i < local_count; ++i) {
        strokes_[i] = local_strokes[i];
    }
    return true;
}

bool StrokeOrderStore::GetStroke(uint16_t index, StrokeView* out) const {
    if (!has_character_ || out == nullptr || index >= stroke_count_) {
        return false;
    }
    *out = strokes_[index];
    return true;
}

bool StrokeOrderStore::FindIndexEntry(uint32_t codepoint, uint32_t* offset, uint32_t* length,
                                      uint32_t* crc) const {
    if (!bound_ || offset == nullptr || length == nullptr || crc == nullptr || char_count_ == 0) {
        return false;
    }
    uint32_t lo = 0;
    uint32_t hi = char_count_;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        uint32_t entry_offset = 0;
        uint32_t found = 0;
        if (!MulU32(mid, kIndexEntrySize, &entry_offset) ||
            !AddU32(index_offset_, entry_offset, &entry_offset) ||
            !ReadU32(data_, size_, entry_offset, &found)) {
            return false;
        }
        if (found == codepoint) {
            uint32_t rec_offset_pos = 0;
            uint32_t rec_length_pos = 0;
            uint32_t rec_crc_pos = 0;
            if (!AddU32(entry_offset, 4, &rec_offset_pos) ||
                !AddU32(entry_offset, 8, &rec_length_pos) ||
                !AddU32(entry_offset, 12, &rec_crc_pos) ||
                !ReadU32(data_, size_, rec_offset_pos, offset) ||
                !ReadU32(data_, size_, rec_length_pos, length) ||
                !ReadU32(data_, size_, rec_crc_pos, crc)) {
                return false;
            }
            return true;
        }
        if (found < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool StrokeOrderStore::ParseRawRecord(const uint8_t* record, uint32_t length, uint32_t expected_cp,
                                      uint32_t expected_crc, uint16_t* stroke_count,
                                      StrokeView* strokes) {
    if (record == nullptr || stroke_count == nullptr || strokes == nullptr ||
        !ValidCodepoint(expected_cp) || length < kCharacterRecordHeaderSize ||
        length > kMaxCharacterBytes) {
        return false;
    }
    if (Crc32(record, length) != expected_crc) {
        STROKE_LOGW("character checksum mismatch");
        return false;
    }

    uint32_t rec_cp = 0;
    uint16_t count = 0;
    uint16_t reserved = 0;
    if (!ReadU32(record, length, 0, &rec_cp) || !ReadU16(record, length, 4, &count) ||
        !ReadU16(record, length, 6, &reserved) || rec_cp != expected_cp || reserved != 0 ||
        count < kMinStrokesPerCharacter || count > kMaxStrokesPerCharacter) {
        return false;
    }

    uint32_t pos = kCharacterRecordHeaderSize;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t outline_count = 0;
        uint16_t median_count = 0;
        uint32_t median_count_pos = 0;
        if (!AddU32(pos, 2, &median_count_pos) || !ReadU16(record, length, pos, &outline_count) ||
            !ReadU16(record, length, median_count_pos, &median_count)) {
            return false;
        }
        uint32_t after_header = 0;
        uint32_t outline_bytes = 0;
        uint32_t median_bytes = 0;
        uint32_t outline_end = 0;
        uint32_t median_end = 0;
        if (!AddU32(pos, kStrokeHeaderSize, &after_header) ||
            outline_count < kMinOutlinePointsPerStroke ||
            outline_count > kMaxOutlinePointsPerStroke ||
            median_count < kMinMedianPointsPerStroke || median_count > kMaxMedianPointsPerStroke ||
            !MulU32(outline_count, kPointSize, &outline_bytes) ||
            !MulU32(median_count, kPointSize, &median_bytes) ||
            !AddU32(after_header, outline_bytes, &outline_end) ||
            !AddU32(outline_end, median_bytes, &median_end) || median_end > length) {
            return false;
        }

        Point first;
        Point last;
        if (!ReadPoint(record, length, after_header, &first)) {
            return false;
        }
        uint32_t last_offset_delta = 0;
        uint32_t last_offset = 0;
        if (!MulU32(static_cast<uint32_t>(outline_count - 1), kPointSize, &last_offset_delta) ||
            !AddU32(after_header, last_offset_delta, &last_offset) ||
            !ReadPoint(record, length, last_offset, &last) || first.x != last.x ||
            first.y != last.y) {
            return false;
        }

        for (uint16_t p = 0; p < outline_count; ++p) {
            uint32_t point_delta = 0;
            uint32_t point_offset = 0;
            Point point;
            if (!MulU32(p, kPointSize, &point_delta) ||
                !AddU32(after_header, point_delta, &point_offset) ||
                !ReadPoint(record, length, point_offset, &point) || point.x > kCoordMax ||
                point.y > kCoordMax) {
                return false;
            }
        }
        for (uint16_t p = 0; p < median_count; ++p) {
            uint32_t point_delta = 0;
            uint32_t point_offset = 0;
            Point point;
            if (!MulU32(p, kPointSize, &point_delta) ||
                !AddU32(outline_end, point_delta, &point_offset) ||
                !ReadPoint(record, length, point_offset, &point) || point.x > kCoordMax ||
                point.y > kCoordMax) {
                return false;
            }
        }
        strokes[i] =
            StrokeView(record + after_header, outline_count, record + outline_end, median_count);
        pos = median_end;
    }
    if (pos != length) {
        return false;
    }
    *stroke_count = count;
    return true;
}

#if defined(STROKE_ORDER_TESTING)
uint32_t StrokeOrderStore::TestOnlyCrc32(const uint8_t* data, size_t length) {
    return Crc32(data, length);
}
#endif
