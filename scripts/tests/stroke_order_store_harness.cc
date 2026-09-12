#include "stroke_order/stroke_order_store.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kGoldenCodepoint = 0x4E00;
constexpr uint32_t kSecondGeneratedCodepoint = 0x4EBA;
constexpr uint32_t kGoldenHeaderCrc = 0x7CE14721;
constexpr uint32_t kGoldenIndexCrc = 0x6E6CB232;
constexpr uint32_t kGoldenRecordCrc = 0x3BDFC874;

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void WriteU16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
    (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
    (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> ReadBinary(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadHex(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    std::string digits;
    char value = 0;
    while (stream.get(value)) {
        if (std::isxdigit(static_cast<unsigned char>(value)) != 0) {
            digits.push_back(value);
        }
    }
    if (digits.size() % 2 != 0) {
        return {};
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(digits.size() / 2);
    for (size_t i = 0; i < digits.size(); i += 2) {
        try {
            bytes.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
        } catch (...) {
            return {};
        }
    }
    return bytes;
}

void RefreshHeaderCrc(std::vector<uint8_t>* bytes) {
    WriteU32(bytes, 24, StrokeOrderStore::TestOnlyCrc32(bytes->data(), 24));
}

void RefreshIndexCrc(std::vector<uint8_t>* bytes) {
    const uint32_t index_offset = ReadU32(*bytes, 12);
    const uint32_t data_offset = ReadU32(*bytes, 16);
    const size_t length = static_cast<size_t>(data_offset - index_offset);
    WriteU32(bytes, 28, StrokeOrderStore::TestOnlyCrc32(bytes->data() + index_offset, length));
}

void RefreshRecordAndIndexCrc(std::vector<uint8_t>* bytes, uint32_t entry) {
    const size_t entry_offset = StrokeOrderStore::kHeaderSize +
                                static_cast<size_t>(entry) * StrokeOrderStore::kIndexEntrySize;
    const uint32_t record_offset = ReadU32(*bytes, entry_offset + 4);
    const uint32_t record_length = ReadU32(*bytes, entry_offset + 8);
    WriteU32(bytes, entry_offset + 12,
             StrokeOrderStore::TestOnlyCrc32(bytes->data() + record_offset, record_length));
    RefreshIndexCrc(bytes);
}

void ExpectPoint(const StrokeOrderStore::Point& point, uint16_t x, uint16_t y,
                 const char* message) {
    Expect(point.x == x && point.y == y, message);
}

void TestKnownAnswerCrc() {
    static constexpr uint8_t input[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Expect(StrokeOrderStore::TestOnlyCrc32(input, sizeof(input)) == 0xCBF43926,
           "CRC-32/ISO-HDLC known-answer vector");
}

void TestGolden(const std::vector<uint8_t>& golden) {
    Expect(golden.size() == 84, "handwritten golden byte length");
    if (golden.size() != 84) {
        return;
    }
    Expect(ReadU16(golden, 4) == StrokeOrderStore::kFormatVersion, "handwritten format version");
    Expect(ReadU16(golden, 6) == StrokeOrderStore::kCoordMax, "handwritten coordinate maximum");
    Expect(ReadU32(golden, 24) == kGoldenHeaderCrc, "handwritten header CRC constant");
    Expect(ReadU32(golden, 28) == kGoldenIndexCrc, "handwritten index CRC constant");
    Expect(ReadU32(golden, 44) == kGoldenRecordCrc, "handwritten record CRC constant");
    Expect(StrokeOrderStore::TestOnlyCrc32(golden.data(), 24) == kGoldenHeaderCrc,
           "handwritten header CRC recomputation");
    Expect(StrokeOrderStore::TestOnlyCrc32(golden.data() + 32, 16) == kGoldenIndexCrc,
           "handwritten index CRC recomputation");
    Expect(StrokeOrderStore::TestOnlyCrc32(golden.data() + 48, 36) == kGoldenRecordCrc,
           "handwritten record CRC recomputation");

    StrokeOrderStore store;
    Expect(store.Bind(golden.data(), golden.size()), "Bind handwritten golden");
    Expect(store.character_count() == 1, "golden character count");
    Expect(store.Contains(kGoldenCodepoint), "golden Contains");
    uint32_t codepoint = 0;
    Expect(store.GetCodepointAt(0, &codepoint) && codepoint == kGoldenCodepoint,
           "golden GetCodepointAt");
    Expect(store.LoadCharacter(kGoldenCodepoint), "Load handwritten golden");
    Expect(store.stroke_count() == 1, "golden stroke count");

    StrokeOrderStore::StrokeView stroke;
    Expect(store.GetStroke(0, &stroke), "golden GetStroke");
    Expect(stroke.outline_count() == 4, "golden outline count");
    Expect(stroke.median_count() == 2, "golden median count");
    StrokeOrderStore::Point point;
    Expect(stroke.GetOutlinePoint(0, &point), "golden first outline access");
    ExpectPoint(point, 0, 0, "golden minimum coordinate");
    Expect(stroke.GetOutlinePoint(2, &point), "golden boundary outline access");
    ExpectPoint(point, 1024, 1024, "golden maximum coordinate");
    Expect(stroke.GetOutlinePoint(3, &point), "golden closed outline access");
    ExpectPoint(point, 0, 0, "golden outline closure");
    Expect(stroke.GetMedianPoint(0, &point), "golden first median access");
    ExpectPoint(point, 0, 1024, "golden median first boundary");
    Expect(stroke.GetMedianPoint(1, &point), "golden second median access");
    ExpectPoint(point, 1024, 0, "golden median second boundary");
    Expect(!stroke.GetOutlinePoint(4, &point), "outline index bound");
    Expect(!stroke.GetMedianPoint(2, &point), "median index bound");
    Expect(!stroke.GetMedianPoint(0, nullptr), "point output null check");
    StrokeOrderStore::StrokeView empty;
    Expect(!empty.GetOutlinePoint(0, &point), "default view has no byte view");
}

void TestUnaligned(const std::vector<uint8_t>& blob, uint32_t codepoint) {
    std::vector<uint8_t> storage(blob.size() + 1, 0xA5);
    for (size_t i = 0; i < blob.size(); ++i) {
        storage[i + 1] = blob[i];
    }
    StrokeOrderStore store;
    Expect(store.Bind(storage.data() + 1, blob.size()), "Bind unaligned blob");
    Expect(store.LoadCharacter(codepoint), "Load character from unaligned blob");
    StrokeOrderStore::StrokeView stroke;
    StrokeOrderStore::Point point;
    Expect(store.GetStroke(0, &stroke), "GetStroke from unaligned blob");
    Expect(stroke.GetOutlinePoint(0, &point), "decode unaligned outline point");
    Expect(stroke.GetMedianPoint(0, &point), "decode unaligned median point");
}

void TestGenerated(const std::vector<uint8_t>& generated) {
    StrokeOrderStore store;
    Expect(store.Bind(generated.data(), generated.size()), "Bind Python-generated blob");
    Expect(store.character_count() == 2, "Python-generated character count");
    Expect(store.LoadCharacter(kGoldenCodepoint), "Load first Python-generated character");
    StrokeOrderStore::StrokeView stroke;
    StrokeOrderStore::Point point;
    Expect(store.GetStroke(0, &stroke), "Get generated stroke");
    Expect(stroke.GetOutlinePoint(0, &point), "Get generated outline point");
    ExpectPoint(point, 0, 768, "Python y-axis conversion");
    Expect(stroke.GetMedianPoint(1, &point), "Get generated median point");
    ExpectPoint(point, 512, 256, "Python generated median endpoint");
    Expect(store.LoadCharacter(kSecondGeneratedCodepoint),
           "Load second Python-generated character");
}

void TestCorruptionAndState(const std::vector<uint8_t>& golden,
                            const std::vector<uint8_t>& generated) {
    StrokeOrderStore store;
    Expect(store.Bind(golden.data(), golden.size()), "state setup Bind");
    Expect(store.LoadCharacter(kGoldenCodepoint), "state setup Load");

    std::vector<uint8_t> damaged = golden;
    damaged[0] ^= 0x01;
    Expect(!store.Bind(damaged.data(), damaged.size()), "reject bad magic");
    Expect(store.is_bound() && store.loaded_codepoint() == kGoldenCodepoint,
           "failed Bind preserves state");

    damaged = golden;
    damaged[24] ^= 0x01;
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()), "reject header CRC damage");

    damaged = golden;
    damaged[32] ^= 0x01;
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()), "reject index CRC damage");

    damaged = golden;
    damaged.back() ^= 0x01;
    StrokeOrderStore record_damage;
    Expect(record_damage.Bind(damaged.data(), damaged.size()),
           "record corruption does not prevent index bind");
    Expect(!record_damage.LoadCharacter(kGoldenCodepoint), "reject record body corruption");

    damaged = golden;
    WriteU32(&damaged, 44, ReadU32(damaged, 44) ^ 1u);
    RefreshIndexCrc(&damaged);
    StrokeOrderStore record_crc_damage;
    Expect(record_crc_damage.Bind(damaged.data(), damaged.size()),
           "bind with internally protected changed record CRC field");
    Expect(!record_crc_damage.LoadCharacter(kGoldenCodepoint), "reject wrong record CRC field");

    damaged = golden;
    WriteU32(&damaged, 48, kSecondGeneratedCodepoint);
    RefreshRecordAndIndexCrc(&damaged, 0);
    StrokeOrderStore record_header_damage;
    Expect(record_header_damage.Bind(damaged.data(), damaged.size()),
           "bind record-header mismatch fixture");
    Expect(!record_header_damage.LoadCharacter(kGoldenCodepoint),
           "reject record/index codepoint mismatch");

    damaged = golden;
    WriteU32(&damaged, 36, 0xFFFFFFF0u);
    WriteU32(&damaged, 40, 0x20u);
    RefreshIndexCrc(&damaged);
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()),
           "reject wrapping record offset");

    damaged = golden;
    WriteU32(&damaged, 40, 0xFFFFFFF0u);
    RefreshIndexCrc(&damaged);
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()),
           "reject overflowing record length");

    damaged = golden;
    WriteU32(&damaged, 20, 0xFFFFFFFFu);
    RefreshHeaderCrc(&damaged);
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()),
           "reject overflowing data size");

    damaged = golden;
    WriteU32(&damaged, 8, StrokeOrderStore::kMaxCharacters + 1);
    RefreshHeaderCrc(&damaged);
    Expect(!StrokeOrderStore().Bind(damaged.data(), damaged.size()),
           "reject excessive character count");

    damaged = golden;
    WriteU16(&damaged, 52, 0xFFFFu);
    RefreshRecordAndIndexCrc(&damaged, 0);
    StrokeOrderStore stroke_count_damage;
    Expect(stroke_count_damage.Bind(damaged.data(), damaged.size()),
           "bind excessive stroke count fixture");
    Expect(!stroke_count_damage.LoadCharacter(kGoldenCodepoint), "reject excessive stroke count");

    damaged = golden;
    WriteU16(&damaged, 56, 0xFFFFu);
    RefreshRecordAndIndexCrc(&damaged, 0);
    StrokeOrderStore point_count_damage;
    Expect(point_count_damage.Bind(damaged.data(), damaged.size()),
           "bind excessive point count fixture");
    Expect(!point_count_damage.LoadCharacter(kGoldenCodepoint), "reject excessive point count");

    Expect(!StrokeOrderStore().Bind(golden.data(), golden.size() - 1), "reject truncated blob");
    std::vector<uint8_t> oversized(StrokeOrderStore::kMaxFileBytes + 1, 0);
    Expect(!StrokeOrderStore().Bind(oversized.data(), oversized.size()), "reject oversized blob");

    std::vector<uint8_t> mutable_generated = generated;
    StrokeOrderStore load_state;
    Expect(load_state.Bind(mutable_generated.data(), mutable_generated.size()),
           "load-state generated Bind");
    Expect(load_state.LoadCharacter(kGoldenCodepoint), "load-state first character");
    const size_t second_entry = StrokeOrderStore::kHeaderSize + StrokeOrderStore::kIndexEntrySize;
    const uint32_t second_offset = ReadU32(mutable_generated, second_entry + 4);
    mutable_generated[second_offset + 8] ^= 0x01;
    Expect(!load_state.LoadCharacter(kSecondGeneratedCodepoint),
           "failed Load rejects damaged second record");
    Expect(load_state.has_character() && load_state.loaded_codepoint() == kGoldenCodepoint,
           "failed Load preserves loaded character");
    StrokeOrderStore::StrokeView retained;
    StrokeOrderStore::Point retained_point;
    Expect(load_state.GetStroke(0, &retained) && retained.GetMedianPoint(0, &retained_point),
           "failed Load preserves point view");
    ExpectPoint(retained_point, 512, 768, "failed Load preserves point value");
}

void TestSmokeCorpus(const std::vector<uint8_t>& smoke) {
    StrokeOrderStore store;
    Expect(store.Bind(smoke.data(), smoke.size()), "Bind real upstream smoke blob");
    Expect(store.character_count() == 3, "real upstream smoke character count");
    for (uint32_t codepoint : {0x4E00u, 0x4EBAu, 0x53E3u}) {
        Expect(store.Contains(codepoint), "real upstream smoke Contains");
        Expect(store.LoadCharacter(codepoint), "real upstream smoke LoadCharacter");
        Expect(store.stroke_count() > 0, "real upstream smoke has strokes");
        StrokeOrderStore::StrokeView stroke;
        StrokeOrderStore::Point point;
        Expect(store.GetStroke(0, &stroke), "real upstream smoke GetStroke");
        Expect(stroke.GetOutlinePoint(0, &point), "real upstream smoke outline access");
        Expect(stroke.GetMedianPoint(0, &point), "real upstream smoke median access");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: stroke_order_store_harness GENERATED.bin GOLDEN.hex SMOKE.bin\n";
        return 2;
    }
    const std::vector<uint8_t> generated = ReadBinary(argv[1]);
    const std::vector<uint8_t> golden = ReadHex(argv[2]);
    const std::vector<uint8_t> smoke = ReadBinary(argv[3]);
    if (generated.empty() || golden.empty() || smoke.empty()) {
        std::cerr << "failed to read one or more harness inputs\n";
        return 2;
    }

    TestKnownAnswerCrc();
    TestGolden(golden);
    TestUnaligned(golden, kGoldenCodepoint);
    TestUnaligned(generated, kGoldenCodepoint);
    TestGenerated(generated);
    TestCorruptionAndState(golden, generated);
    TestSmokeCorpus(smoke);

    if (failures != 0) {
        std::cerr << failures << " C++ harness assertion(s) failed\n";
        return 1;
    }
    std::cout << "stroke_order_store_harness: PASS\n";
    return 0;
}
