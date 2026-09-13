#include "stroke_order/stroke_order_catalog.h"

#include <cstring>

namespace {

constexpr uint8_t kMagic[4] = {'S', 'C', 'B', '1'};

bool AddU32(uint32_t a, uint32_t b, uint32_t* out) {
    const uint64_t value = static_cast<uint64_t>(a) + b;
    if (out == nullptr || value > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool MulU32(uint32_t a, uint32_t b, uint32_t* out) {
    const uint64_t value = static_cast<uint64_t>(a) * b;
    if (out == nullptr || value > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool RangeInSize(uint32_t offset, uint32_t length, size_t size) {
    uint32_t end = 0;
    return AddU32(offset, length, &end) && static_cast<uint64_t>(end) <= size;
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

bool ValidName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    bool found_zero = false;
    for (uint32_t i = 0; i < StrokeOrderCatalog::kShardNameBytes; ++i) {
        const unsigned char value = static_cast<unsigned char>(name[i]);
        if (found_zero) {
            if (value != 0) {
                return false;
            }
            continue;
        }
        if (value == 0) {
            found_zero = true;
            continue;
        }
        const bool valid = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                           value == '_' || value == '-' || value == '.';
        if (!valid ||
            (i == 0 && !((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')))) {
            return false;
        }
    }
    return found_zero;
}

}  // namespace

uint32_t StrokeOrderCatalog::CalculateCrc32(const uint8_t* data, size_t length) {
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

bool StrokeOrderCatalog::Bind(const uint8_t* data, size_t size) {
    if (data == nullptr || size < kHeaderSize || size > kMaxFileBytes ||
        std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    uint16_t version = 0;
    uint16_t shard_count = 0;
    uint32_t character_count = 0;
    uint32_t entry_offset = 0;
    uint32_t shard_offset = 0;
    uint32_t total_shard_bytes = 0;
    uint32_t header_crc = 0;
    uint32_t body_crc = 0;
    if (!ReadU16(data, size, 4, &version) || !ReadU16(data, size, 6, &shard_count) ||
        !ReadU32(data, size, 8, &character_count) || !ReadU32(data, size, 12, &entry_offset) ||
        !ReadU32(data, size, 16, &shard_offset) || !ReadU32(data, size, 20, &total_shard_bytes) ||
        !ReadU32(data, size, 24, &header_crc) || !ReadU32(data, size, 28, &body_crc) ||
        version != kFormatVersion || shard_count == 0 || shard_count > kMaxShards ||
        character_count == 0 || character_count > kMaxCharacters || entry_offset != kHeaderSize ||
        CalculateCrc32(data, 24) != header_crc ||
        CalculateCrc32(data + kHeaderSize, size - kHeaderSize) != body_crc) {
        return false;
    }

    uint32_t entry_bytes = 0;
    uint32_t expected_shard_offset = 0;
    uint32_t shard_bytes = 0;
    uint32_t expected_size = 0;
    if (!MulU32(character_count, kEntrySize, &entry_bytes) ||
        !AddU32(entry_offset, entry_bytes, &expected_shard_offset) ||
        expected_shard_offset != shard_offset || !MulU32(shard_count, kShardSize, &shard_bytes) ||
        !AddU32(shard_offset, shard_bytes, &expected_size) || expected_size != size) {
        return false;
    }

    // Temporarily expose the structurally bounded view to the helpers below.
    data_ = data;
    size_ = size;
    shard_count_ = shard_count;
    character_count_ = character_count;
    entry_offset_ = entry_offset;
    shard_offset_ = shard_offset;
    total_shard_bytes_ = total_shard_bytes;
    bound_ = true;

    uint64_t shard_size_sum = 0;
    for (uint16_t i = 0; i < shard_count; ++i) {
        Shard shard;
        if (!GetShard(i, &shard) || !ValidName(shard.name) || shard.size == 0 ||
            shard.size > kMaxShardBytes || shard.character_count == 0 ||
            shard.character_count > kMaxCharacters || shard.first_rank == 0 ||
            shard.last_rank < shard.first_rank || shard.last_rank > character_count ||
            shard.last_rank - shard.first_rank + 1 != shard.character_count) {
            Unbind();
            return false;
        }
        for (uint16_t previous = 0; previous < i; ++previous) {
            Shard prior;
            if (!GetShard(previous, &prior) || std::strcmp(shard.name, prior.name) == 0) {
                Unbind();
                return false;
            }
        }
        shard_size_sum += shard.size;
    }
    if (shard_size_sum != total_shard_bytes) {
        Unbind();
        return false;
    }

    uint32_t previous_cp = 0;
    for (uint32_t i = 0; i < character_count; ++i) {
        Entry entry;
        if (!GetEntry(i, &entry) || entry.codepoint < 0x4E00 || entry.codepoint > 0x9FFF ||
            (i != 0 && entry.codepoint <= previous_cp) || entry.rank == 0 ||
            entry.rank > character_count || entry.shard >= shard_count) {
            Unbind();
            return false;
        }
        Shard shard;
        if (!GetShard(entry.shard, &shard) || entry.rank < shard.first_rank ||
            entry.rank > shard.last_rank || entry.local_index >= shard.character_count) {
            Unbind();
            return false;
        }
        for (uint32_t previous = 0; previous < i; ++previous) {
            Entry other;
            if (!GetEntry(previous, &other) || other.rank == entry.rank ||
                (other.shard == entry.shard && other.local_index == entry.local_index)) {
                Unbind();
                return false;
            }
        }
        previous_cp = entry.codepoint;
    }

    // The uniqueness/range checks above imply completeness because there are
    // exactly character_count ranks and sum(shard.character_count) entries.
    uint32_t shard_char_sum = 0;
    for (uint16_t i = 0; i < shard_count; ++i) {
        Shard shard;
        if (!GetShard(i, &shard) ||
            !AddU32(shard_char_sum, shard.character_count, &shard_char_sum)) {
            Unbind();
            return false;
        }
    }
    if (shard_char_sum != character_count) {
        Unbind();
        return false;
    }
    return true;
}

void StrokeOrderCatalog::Unbind() {
    data_ = nullptr;
    size_ = 0;
    bound_ = false;
    shard_count_ = 0;
    character_count_ = 0;
    entry_offset_ = 0;
    shard_offset_ = 0;
    total_shard_bytes_ = 0;
}

bool StrokeOrderCatalog::Contains(uint32_t codepoint) const { return Find(codepoint, nullptr); }

bool StrokeOrderCatalog::Find(uint32_t codepoint, Entry* out) const {
    if (!bound_) {
        return false;
    }
    uint32_t lo = 0;
    uint32_t hi = character_count_;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        Entry entry;
        if (!GetEntry(mid, &entry)) {
            return false;
        }
        if (entry.codepoint == codepoint) {
            if (out != nullptr) {
                *out = entry;
            }
            return true;
        }
        if (entry.codepoint < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool StrokeOrderCatalog::GetEntry(uint32_t index, Entry* out) const {
    if (!bound_ || out == nullptr || index >= character_count_) {
        return false;
    }
    uint32_t delta = 0;
    uint32_t offset = 0;
    uint32_t cp = 0;
    uint16_t rank = 0;
    uint16_t local_index = 0;
    uint16_t reserved16 = 0;
    if (!MulU32(index, kEntrySize, &delta) || !AddU32(entry_offset_, delta, &offset) ||
        !ReadU32(data_, size_, offset, &cp) || !ReadU16(data_, size_, offset + 4, &rank) ||
        !ReadU16(data_, size_, offset + 8, &local_index) ||
        !ReadU16(data_, size_, offset + 10, &reserved16) || data_[offset + 7] != 0 ||
        reserved16 != 0) {
        return false;
    }
    out->codepoint = cp;
    out->rank = rank;
    out->shard = data_[offset + 6];
    out->local_index = local_index;
    return true;
}

bool StrokeOrderCatalog::GetShard(uint16_t index, Shard* out) const {
    if (!bound_ || out == nullptr || index >= shard_count_) {
        return false;
    }
    uint32_t delta = 0;
    uint32_t offset = 0;
    uint32_t reserved = 0;
    if (!MulU32(index, kShardSize, &delta) || !AddU32(shard_offset_, delta, &offset) ||
        !RangeInSize(offset, kShardSize, size_)) {
        return false;
    }
    std::memcpy(out->name, data_ + offset, kShardNameBytes);
    if (!ReadU32(data_, size_, offset + 16, &out->size) ||
        !ReadU32(data_, size_, offset + 20, &out->crc32) ||
        !ReadU32(data_, size_, offset + 24, &out->character_count) ||
        !ReadU32(data_, size_, offset + 28, &out->first_rank) ||
        !ReadU32(data_, size_, offset + 32, &out->last_rank) ||
        !ReadU32(data_, size_, offset + 36, &reserved) || reserved != 0) {
        return false;
    }
    return true;
}
