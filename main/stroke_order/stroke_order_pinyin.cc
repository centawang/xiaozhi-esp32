#include "stroke_order/stroke_order_pinyin.h"

#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_log.h>
static const char* TAG = "StrokePinyin";
#define SPY_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
#define TAG "StrokePinyin"
#define SPY_LOGW(fmt, ...)
#endif

namespace {

constexpr uint8_t kMagicBytes[4] = {'S', 'P', 'Y', '1'};

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
    return codepoint >= StrokeOrderPinyinIndex::kMinTargetCodepoint &&
           codepoint <= StrokeOrderPinyinIndex::kMaxTargetCodepoint;
}

}  // namespace

bool StrokeOrderPinyinIndex::Bind(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxFileBytes) {
        return false;
    }
    StrokeOrderOwnedBlob copy = StrokeOrderAllocateOwned(size, TAG);
    if (copy == nullptr) {
        return false;
    }
    std::memcpy(copy.get(), data, size);
    const uint8_t* previous_data = data_;
    size_t previous_size = size_;
    bool previous_bound = bound_;
    uint32_t previous_chars = char_count_;
    uint32_t previous_groups = group_count_;
    uint32_t previous_char_off = char_index_offset_;
    uint32_t previous_group_off = group_index_offset_;
    if (!BindView(copy.get(), size)) {
        data_ = previous_data;
        size_ = previous_size;
        bound_ = previous_bound;
        char_count_ = previous_chars;
        group_count_ = previous_groups;
        char_index_offset_ = previous_char_off;
        group_index_offset_ = previous_group_off;
        return false;
    }
    owned_blob_ = std::move(copy);
    owned_size_ = size;
    data_ = owned_blob_.get();
    size_ = owned_size_;
    return true;
}

#if defined(STROKE_ORDER_TESTING)
bool StrokeOrderPinyinIndex::TestOnlyBindView(const uint8_t* data, size_t size) {
    return BindView(data, size);
}

uint32_t StrokeOrderPinyinIndex::TestOnlyCrc32(const uint8_t* data, size_t length) {
    return Crc32(data, length);
}

bool StrokeOrderPinyinIndex::TestOnlyAddU32(uint32_t a, uint32_t b, uint32_t* out) {
    return AddU32(a, b, out);
}

bool StrokeOrderPinyinIndex::TestOnlyMulU32(uint32_t a, uint32_t b, uint32_t* out) {
    return MulU32(a, b, out);
}
#endif

void StrokeOrderPinyinIndex::Unbind() {
    bound_ = false;
    data_ = nullptr;
    size_ = 0;
    char_count_ = 0;
    group_count_ = 0;
    char_index_offset_ = 0;
    group_index_offset_ = 0;
    owned_blob_.reset();
    owned_size_ = 0;
}

bool StrokeOrderPinyinIndex::BindView(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kHeaderSize || size > kMaxFileBytes) {
        return false;
    }
    if (data[0] != kMagicBytes[0] || data[1] != kMagicBytes[1] || data[2] != kMagicBytes[2] ||
        data[3] != kMagicBytes[3]) {
        return false;
    }
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint32_t char_count = 0;
    uint32_t group_count = 0;
    uint32_t char_index_offset = 0;
    uint32_t group_index_offset = 0;
    uint32_t header_crc = 0;
    uint32_t body_crc = 0;
    if (!ReadU16(data, size, 4, &version) || !ReadU16(data, size, 6, &reserved) ||
        !ReadU32(data, size, 8, &char_count) || !ReadU32(data, size, 12, &group_count) ||
        !ReadU32(data, size, 16, &char_index_offset) ||
        !ReadU32(data, size, 20, &group_index_offset) || !ReadU32(data, size, 24, &header_crc) ||
        !ReadU32(data, size, 28, &body_crc)) {
        return false;
    }
    if (version != kFormatVersion || reserved != 0) {
        return false;
    }
    if (char_count == 0 || char_count > kMaxCharacters || group_count == 0 ||
        group_count > kMaxGroups) {
        return false;
    }
    if (char_index_offset != kHeaderSize) {
        return false;
    }
    if (Crc32(data, 24) != header_crc) {
        return false;
    }
    if (Crc32(data + kHeaderSize, size - kHeaderSize) != body_crc) {
        return false;
    }

    uint32_t char_index_bytes = 0;
    uint32_t group_index_bytes = 0;
    uint32_t expected_group_offset = 0;
    uint32_t index_end = 0;
    if (!MulU32(char_count, kCharIndexEntrySize, &char_index_bytes) ||
        !MulU32(group_count, kGroupIndexEntrySize, &group_index_bytes) ||
        !AddU32(char_index_offset, char_index_bytes, &expected_group_offset) ||
        !AddU32(group_index_offset, group_index_bytes, &index_end)) {
        return false;
    }
    if (group_index_offset != expected_group_offset || index_end > size) {
        return false;
    }

    uint32_t prev_cp = 0;
    uint32_t payload_cursor = index_end;
    uint16_t seen_ranks[kMaxCharacters] = {};
    for (uint32_t i = 0; i < char_count; ++i) {
        uint32_t entry = 0;
        if (!MulU32(i, kCharIndexEntrySize, &entry) || !AddU32(char_index_offset, entry, &entry)) {
            return false;
        }
        uint32_t codepoint = 0;
        uint16_t rank = 0;
        uint32_t readings_offset = 0;
        if (!ReadU32(data, size, entry, &codepoint) || !ReadU16(data, size, entry + 4, &rank) ||
            !ReadU32(data, size, entry + 8, &readings_offset)) {
            return false;
        }
        const uint8_t reading_count = data[entry + 6];
        const uint8_t reserved_u8 = data[entry + 7];
        if (reserved_u8 != 0 || rank == 0 || reading_count == 0 ||
            reading_count > kMaxReadingsPerCharacter || !ValidCodepoint(codepoint)) {
            return false;
        }
        for (uint32_t prev = 0; prev < i; ++prev) {
            if (seen_ranks[prev] == rank) {
                return false;
            }
        }
        seen_ranks[i] = rank;
        if (i > 0 && codepoint <= prev_cp) {
            return false;
        }
        uint32_t reading_bytes = 0;
        uint32_t reading_end = 0;
        if (!MulU32(reading_count, kReadingSize, &reading_bytes) ||
            !AddU32(readings_offset, reading_bytes, &reading_end) ||
            readings_offset != payload_cursor ||
            !RangeInSize(readings_offset, reading_bytes, size)) {
            return false;
        }
        payload_cursor = reading_end;
        uint16_t seen_groups[kMaxReadingsPerCharacter] = {};
        for (uint8_t r = 0; r < reading_count; ++r) {
            uint16_t gid = 0;
            if (!ReadU16(data, size, readings_offset + static_cast<uint32_t>(r) * kReadingSize,
                         &gid)) {
                return false;
            }
            if (gid >= group_count) {
                return false;
            }
            for (uint8_t prev = 0; prev < r; ++prev) {
                if (seen_groups[prev] == gid) {
                    return false;
                }
            }
            seen_groups[r] = gid;
        }
        prev_cp = codepoint;
    }

    for (uint32_t g = 0; g < group_count; ++g) {
        uint32_t entry = 0;
        if (!MulU32(g, kGroupIndexEntrySize, &entry) ||
            !AddU32(group_index_offset, entry, &entry)) {
            return false;
        }
        uint16_t member_count = 0;
        uint16_t reserved_u16 = 0;
        uint32_t members_offset = 0;
        if (!ReadU16(data, size, entry, &member_count) ||
            !ReadU16(data, size, entry + 2, &reserved_u16) ||
            !ReadU32(data, size, entry + 4, &members_offset)) {
            return false;
        }
        if (reserved_u16 != 0 || member_count == 0 || member_count > kMaxGroupMembers) {
            return false;
        }
        uint32_t member_bytes = 0;
        uint32_t member_end = 0;
        if (!MulU32(member_count, kMemberSize, &member_bytes) ||
            !AddU32(members_offset, member_bytes, &member_end) ||
            members_offset != payload_cursor || !RangeInSize(members_offset, member_bytes, size)) {
            return false;
        }
        payload_cursor = member_end;
        uint32_t prev_rank = 0;
        uint32_t prev_member_cp = 0;
        bool have_prev = false;
        for (uint16_t m = 0; m < member_count; ++m) {
            uint32_t m_off = 0;
            uint32_t m_bytes = 0;
            if (!MulU32(m, kMemberSize, &m_bytes) || !AddU32(members_offset, m_bytes, &m_off)) {
                return false;
            }
            uint32_t codepoint = 0;
            uint16_t rank = 0;
            uint16_t reserved_member = 0;
            if (!ReadU32(data, size, m_off, &codepoint) || !ReadU16(data, size, m_off + 4, &rank) ||
                !ReadU16(data, size, m_off + 6, &reserved_member)) {
                return false;
            }
            if (reserved_member != 0 || rank == 0 || !ValidCodepoint(codepoint)) {
                return false;
            }
            if (have_prev &&
                (rank < prev_rank || (rank == prev_rank && codepoint <= prev_member_cp))) {
                return false;
            }
            have_prev = true;
            prev_rank = rank;
            prev_member_cp = codepoint;
        }
    }
    if (payload_cursor != size) {
        return false;
    }

    // Validate both sides of the character <-> group relation without dynamic
    // allocation. Counts are capped above, so these scans are deterministic and
    // bounded by 2048 * 8 * 32 plus 2048 * 32 * log2(2048).
    for (uint32_t i = 0; i < char_count; ++i) {
        uint32_t char_entry_delta = 0;
        uint32_t char_entry = 0;
        if (!MulU32(i, kCharIndexEntrySize, &char_entry_delta) ||
            !AddU32(char_index_offset, char_entry_delta, &char_entry)) {
            return false;
        }
        uint32_t char_cp = 0;
        uint16_t char_rank = 0;
        uint32_t readings_offset = 0;
        if (!ReadU32(data, size, char_entry, &char_cp) ||
            !ReadU16(data, size, char_entry + 4, &char_rank) ||
            !ReadU32(data, size, char_entry + 8, &readings_offset)) {
            return false;
        }
        const uint8_t reading_count = data[char_entry + 6];
        for (uint8_t r = 0; r < reading_count; ++r) {
            uint16_t group_id = 0;
            if (!ReadU16(data, size, readings_offset + static_cast<uint32_t>(r) * kReadingSize,
                         &group_id)) {
                return false;
            }
            uint32_t group_delta = 0;
            uint32_t group_entry = 0;
            uint16_t member_count = 0;
            uint32_t members_offset = 0;
            if (!MulU32(group_id, kGroupIndexEntrySize, &group_delta) ||
                !AddU32(group_index_offset, group_delta, &group_entry) ||
                !ReadU16(data, size, group_entry, &member_count) ||
                !ReadU32(data, size, group_entry + 4, &members_offset)) {
                return false;
            }
            bool found = false;
            for (uint16_t m = 0; m < member_count; ++m) {
                uint32_t member_delta = 0;
                uint32_t member_offset = 0;
                uint32_t member_cp = 0;
                uint16_t member_rank = 0;
                if (!MulU32(m, kMemberSize, &member_delta) ||
                    !AddU32(members_offset, member_delta, &member_offset) ||
                    !ReadU32(data, size, member_offset, &member_cp) ||
                    !ReadU16(data, size, member_offset + 4, &member_rank)) {
                    return false;
                }
                if (member_cp == char_cp) {
                    if (member_rank != char_rank) {
                        return false;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
    }

    for (uint32_t group_id = 0; group_id < group_count; ++group_id) {
        uint32_t group_delta = 0;
        uint32_t group_entry = 0;
        uint16_t member_count = 0;
        uint32_t members_offset = 0;
        if (!MulU32(group_id, kGroupIndexEntrySize, &group_delta) ||
            !AddU32(group_index_offset, group_delta, &group_entry) ||
            !ReadU16(data, size, group_entry, &member_count) ||
            !ReadU32(data, size, group_entry + 4, &members_offset)) {
            return false;
        }
        for (uint16_t m = 0; m < member_count; ++m) {
            uint32_t member_delta = 0;
            uint32_t member_offset = 0;
            uint32_t member_cp = 0;
            uint16_t member_rank = 0;
            if (!MulU32(m, kMemberSize, &member_delta) ||
                !AddU32(members_offset, member_delta, &member_offset) ||
                !ReadU32(data, size, member_offset, &member_cp) ||
                !ReadU16(data, size, member_offset + 4, &member_rank)) {
                return false;
            }

            uint32_t lo = 0;
            uint32_t hi = char_count;
            bool owner_found = false;
            uint8_t owner_reading_count = 0;
            uint32_t owner_readings_offset = 0;
            while (lo < hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                uint32_t owner_delta = 0;
                uint32_t owner_entry = 0;
                uint32_t owner_cp = 0;
                uint16_t owner_rank = 0;
                if (!MulU32(mid, kCharIndexEntrySize, &owner_delta) ||
                    !AddU32(char_index_offset, owner_delta, &owner_entry) ||
                    !ReadU32(data, size, owner_entry, &owner_cp) ||
                    !ReadU16(data, size, owner_entry + 4, &owner_rank) ||
                    !ReadU32(data, size, owner_entry + 8, &owner_readings_offset)) {
                    return false;
                }
                if (owner_cp == member_cp) {
                    if (owner_rank != member_rank) {
                        return false;
                    }
                    owner_reading_count = data[owner_entry + 6];
                    owner_found = true;
                    break;
                }
                if (owner_cp < member_cp) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (!owner_found) {
                return false;
            }
            bool reciprocal = false;
            for (uint8_t r = 0; r < owner_reading_count; ++r) {
                uint16_t owner_group = 0;
                if (!ReadU16(data, size,
                             owner_readings_offset + static_cast<uint32_t>(r) * kReadingSize,
                             &owner_group)) {
                    return false;
                }
                if (owner_group == group_id) {
                    reciprocal = true;
                    break;
                }
            }
            if (!reciprocal) {
                return false;
            }
        }
    }

    data_ = data;
    size_ = size;
    bound_ = true;
    char_count_ = char_count;
    group_count_ = group_count;
    char_index_offset_ = char_index_offset;
    group_index_offset_ = group_index_offset;
    return true;
}

bool StrokeOrderPinyinIndex::FindChar(uint32_t codepoint, uint32_t* index) const {
    if (!bound_ || index == nullptr || char_count_ == 0) {
        return false;
    }
    uint32_t lo = 0;
    uint32_t hi = char_count_;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        uint32_t mid_cp = 0;
        uint16_t rank = 0;
        uint8_t readings = 0;
        uint32_t readings_offset = 0;
        if (!ReadCharEntry(mid, &mid_cp, &rank, &readings, &readings_offset)) {
            return false;
        }
        if (mid_cp == codepoint) {
            *index = mid;
            return true;
        }
        if (mid_cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool StrokeOrderPinyinIndex::ReadCharEntry(uint32_t index, uint32_t* codepoint, uint16_t* rank,
                                           uint8_t* reading_count,
                                           uint32_t* readings_offset) const {
    if (!bound_ || index >= char_count_ || data_ == nullptr) {
        return false;
    }
    uint32_t entry = 0;
    if (!MulU32(index, kCharIndexEntrySize, &entry) || !AddU32(char_index_offset_, entry, &entry)) {
        return false;
    }
    uint32_t cp = 0;
    uint16_t rk = 0;
    uint32_t off = 0;
    if (!ReadU32(data_, size_, entry, &cp) || !ReadU16(data_, size_, entry + 4, &rk) ||
        !ReadU32(data_, size_, entry + 8, &off)) {
        return false;
    }
    if (codepoint != nullptr) {
        *codepoint = cp;
    }
    if (rank != nullptr) {
        *rank = rk;
    }
    if (reading_count != nullptr) {
        *reading_count = data_[entry + 6];
    }
    if (readings_offset != nullptr) {
        *readings_offset = off;
    }
    return true;
}

bool StrokeOrderPinyinIndex::ReadGroupEntry(uint32_t group_id, uint16_t* member_count,
                                            uint32_t* members_offset) const {
    if (!bound_ || group_id >= group_count_) {
        return false;
    }
    uint32_t entry = 0;
    if (!MulU32(group_id, kGroupIndexEntrySize, &entry) ||
        !AddU32(group_index_offset_, entry, &entry)) {
        return false;
    }
    uint16_t count = 0;
    uint32_t off = 0;
    if (!ReadU16(data_, size_, entry, &count) || !ReadU32(data_, size_, entry + 4, &off)) {
        return false;
    }
    if (member_count != nullptr) {
        *member_count = count;
    }
    if (members_offset != nullptr) {
        *members_offset = off;
    }
    return true;
}

bool StrokeOrderPinyinIndex::Contains(uint32_t codepoint) const {
    uint32_t index = 0;
    return FindChar(codepoint, &index);
}

bool StrokeOrderPinyinIndex::GetRank(uint32_t codepoint, uint16_t* rank) const {
    uint32_t index = 0;
    if (!FindChar(codepoint, &index)) {
        return false;
    }
    uint32_t cp = 0;
    uint8_t readings = 0;
    uint32_t off = 0;
    return ReadCharEntry(index, &cp, rank, &readings, &off);
}

uint32_t StrokeOrderPinyinIndex::AppendHomophones(uint32_t primary, uint32_t* out,
                                                  uint32_t cap) const {
    if (!bound_ || out == nullptr || cap == 0) {
        return 0;
    }
    uint32_t index = 0;
    if (!FindChar(primary, &index)) {
        return 0;
    }
    uint32_t cp = 0;
    uint16_t rank = 0;
    uint8_t reading_count = 0;
    uint32_t readings_offset = 0;
    if (!ReadCharEntry(index, &cp, &rank, &reading_count, &readings_offset) || reading_count == 0 ||
        reading_count > kMaxReadingsPerCharacter) {
        return 0;
    }

    uint32_t candidates_cp[kMaxReadingsPerCharacter * kMaxGroupMembers] = {};
    uint16_t candidates_rank[kMaxReadingsPerCharacter * kMaxGroupMembers] = {};
    uint32_t candidate_count = 0;
    const uint32_t candidate_cap =
        static_cast<uint32_t>(sizeof(candidates_cp) / sizeof(candidates_cp[0]));

    for (uint8_t r = 0; r < reading_count; ++r) {
        uint16_t gid = 0;
        if (!ReadU16(data_, size_, readings_offset + static_cast<uint32_t>(r) * kReadingSize,
                     &gid) ||
            gid >= group_count_) {
            return 0;
        }
        uint16_t member_count = 0;
        uint32_t members_offset = 0;
        if (!ReadGroupEntry(gid, &member_count, &members_offset) || member_count == 0 ||
            member_count > kMaxGroupMembers) {
            return 0;
        }
        for (uint16_t m = 0; m < member_count && candidate_count < candidate_cap; ++m) {
            uint32_t m_off = 0;
            uint32_t m_bytes = 0;
            if (!MulU32(m, kMemberSize, &m_bytes) || !AddU32(members_offset, m_bytes, &m_off)) {
                return 0;
            }
            uint32_t member_cp = 0;
            uint16_t member_rank = 0;
            if (!ReadU32(data_, size_, m_off, &member_cp) ||
                !ReadU16(data_, size_, m_off + 4, &member_rank)) {
                return 0;
            }
            if (member_cp == primary) {
                continue;
            }
            bool duplicate = false;
            for (uint32_t i = 0; i < candidate_count; ++i) {
                if (candidates_cp[i] == member_cp) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            candidates_cp[candidate_count] = member_cp;
            candidates_rank[candidate_count] = member_rank;
            candidate_count += 1;
        }
    }

    for (uint32_t i = 1; i < candidate_count; ++i) {
        const uint32_t cp_i = candidates_cp[i];
        const uint16_t rank_i = candidates_rank[i];
        uint32_t j = i;
        while (j > 0 && (candidates_rank[j - 1] > rank_i ||
                         (candidates_rank[j - 1] == rank_i && candidates_cp[j - 1] > cp_i))) {
            candidates_cp[j] = candidates_cp[j - 1];
            candidates_rank[j] = candidates_rank[j - 1];
            j -= 1;
        }
        candidates_cp[j] = cp_i;
        candidates_rank[j] = rank_i;
    }

    const uint32_t written = candidate_count < cap ? candidate_count : cap;
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = candidates_cp[i];
    }
    return written;
}

uint32_t StrokeOrderPinyinIndex::AppendTopRanked(uint32_t* out, uint32_t cap) const {
    if (!bound_ || out == nullptr || cap == 0) {
        return 0;
    }
    uint32_t best_cp[StrokeOrderStore::kMaxCharacters > 6 ? 6 : 1] = {};
    uint16_t best_rank[6] = {};
    uint32_t best_count = 0;
    const uint32_t keep = cap < 6 ? cap : 6;
    for (uint32_t i = 0; i < char_count_; ++i) {
        uint32_t cp = 0;
        uint16_t rank = 0;
        uint8_t readings = 0;
        uint32_t off = 0;
        if (!ReadCharEntry(i, &cp, &rank, &readings, &off)) {
            return 0;
        }
        uint32_t insert = best_count;
        while (insert > 0 && (best_rank[insert - 1] > rank ||
                              (best_rank[insert - 1] == rank && best_cp[insert - 1] > cp))) {
            insert -= 1;
        }
        if (best_count < keep) {
            for (uint32_t j = best_count; j > insert; --j) {
                best_cp[j] = best_cp[j - 1];
                best_rank[j] = best_rank[j - 1];
            }
            best_cp[insert] = cp;
            best_rank[insert] = rank;
            best_count += 1;
        } else if (insert < keep) {
            for (uint32_t j = keep - 1; j > insert; --j) {
                best_cp[j] = best_cp[j - 1];
                best_rank[j] = best_rank[j - 1];
            }
            best_cp[insert] = cp;
            best_rank[insert] = rank;
        }
    }
    const uint32_t written = best_count < cap ? best_count : cap;
    for (uint32_t i = 0; i < written; ++i) {
        out[i] = best_cp[i];
    }
    return written;
}
