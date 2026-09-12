#include "stroke_order/stroke_order_candidates.h"
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_pinyin.h"
#include "stroke_order/stroke_order_store.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<uint8_t> ReadBinary(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

class OutOfStorePrimaryProvider : public StrokeOrderCandidateProvider {
public:
    uint32_t AppendHomophones(uint32_t primary, uint32_t* out, uint32_t cap) const override {
        if (primary != 0x9F98 || out == nullptr || cap == 0) {
            return 0;
        }
        out[0] = 0x4E00;
        return 1;
    }
};

void WriteU16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value & 0xFFu);
    (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value & 0xFFu);
    (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    (*bytes)[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    (*bytes)[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void RefreshHeaderCrc(std::vector<uint8_t>* bytes) {
    WriteU32(bytes, 24, StrokeOrderPinyinIndex::TestOnlyCrc32(bytes->data(), 24));
}

void RefreshBodyCrc(std::vector<uint8_t>* bytes) {
    WriteU32(
        bytes, 28,
        StrokeOrderPinyinIndex::TestOnlyCrc32(bytes->data() + StrokeOrderPinyinIndex::kHeaderSize,
                                              bytes->size() - StrokeOrderPinyinIndex::kHeaderSize));
}

std::vector<uint8_t> ReadHex(const std::string& path) {
    std::ifstream stream(path);
    std::string hex;
    stream >> hex;
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto nybble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        const int hi = nybble(hex[i]);
        const int lo = nybble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: stroke_order_pinyin_harness <spy1.hex> <spy1.bin> <sob1.bin>\n";
        return 2;
    }
    const auto golden = ReadHex(argv[1]);
    const auto spy1 = ReadBinary(argv[2]);
    const auto sob1 = ReadBinary(argv[3]);
    Expect(!golden.empty() && golden.size() == 114, "read handwritten SPY1 golden");
    Expect(!spy1.empty() && spy1.size() <= StrokeOrderPinyinIndex::kMaxFileBytes, "read SPY1");
    Expect(!sob1.empty(), "read SOB1");

    Expect(StrokeOrderPinyinIndex::TestOnlyCrc32(reinterpret_cast<const uint8_t*>("123456789"),
                                                 9) == 0xCBF43926u,
           "CRC known answer 123456789");
    Expect(StrokeOrderPinyinIndex::TestOnlyCrc32(golden.data(), 24) == 0x65605A07u,
           "handwritten header CRC");
    Expect(StrokeOrderPinyinIndex::TestOnlyCrc32(golden.data() + 32, golden.size() - 32) ==
               0xC92B5D14u,
           "handwritten body CRC");

    StrokeOrderPinyinIndex index;
    Expect(index.Bind(golden.data(), golden.size()), "bind golden");
    Expect(index.character_count() == 3 && index.Contains(0x4E00) && index.Contains(0x4F0A),
           "golden contains 一/伊");
    uint32_t extras[8] = {};
    uint32_t n = index.AppendHomophones(0x4E00, extras, 8);
    Expect(n == 1 && extras[0] == 0x4F0A, "一 homophone is 伊");
    n = index.AppendHomophones(0x4EBA, extras, 8);
    Expect(n == 0, "人 has no extras in golden");
    uint32_t top[6] = {};
    n = index.AppendTopRanked(top, 6);
    Expect(n == 3 && top[0] == 0x4E00 && top[1] == 0x4EBA && top[2] == 0x4F0A,
           "top ranked by rank");

    std::vector<uint8_t> unaligned(golden.size() + 1, 0xA5);
    std::memcpy(unaligned.data() + 1, golden.data(), golden.size());
    StrokeOrderPinyinIndex unaligned_index;
    Expect(unaligned_index.TestOnlyBindView(unaligned.data() + 1, golden.size()),
           "unaligned direct BindView");
    Expect(unaligned_index.AppendHomophones(0x4E00, extras, 1) == 1 && extras[0] == 0x4F0A,
           "unaligned direct-view homophone");

    std::vector<uint8_t> corrupt = golden;
    corrupt[24] ^= 0xFF;
    Expect(!index.Bind(corrupt.data(), corrupt.size()) && index.is_bound(),
           "header CRC failure preserves previous bind");
    corrupt = golden;
    corrupt[40] ^= 0xFF;
    Expect(!index.Bind(corrupt.data(), corrupt.size()) && index.is_bound(),
           "body CRC failure preserves previous bind");

    struct MutationCase {
        const char* name;
        bool header_change;
        std::function<void(std::vector<uint8_t>*)> mutate;
    };
    const std::vector<MutationCase> mutations = {
        {"char_count_zero", true, [](auto* b) { WriteU32(b, 8, 0); }},
        {"char_count_large", true, [](auto* b) { WriteU32(b, 8, 0xFFFFFFFFu); }},
        {"group_count_zero", true, [](auto* b) { WriteU32(b, 12, 0); }},
        {"group_index_offset", true, [](auto* b) { WriteU32(b, 20, 0xFFFFFFF8u); }},
        {"header_reserved", true, [](auto* b) { WriteU16(b, 6, 1); }},
        {"char_rank_zero", false, [](auto* b) { WriteU16(b, 36, 0); }},
        {"char_rank_duplicate", false, [](auto* b) { WriteU16(b, 48, 1); }},
        {"reading_count_zero", false, [](auto* b) { (*b)[38] = 0; }},
        {"reading_count_large", false,
         [](auto* b) { (*b)[38] = StrokeOrderPinyinIndex::kMaxReadingsPerCharacter + 1; }},
        {"char_reserved", false, [](auto* b) { (*b)[39] = 1; }},
        {"readings_offset_add_overflow", false, [](auto* b) { WriteU32(b, 40, 0xFFFFFFFFu); }},
        {"readings_payload_overlap", false, [](auto* b) { WriteU32(b, 52, 84); }},
        {"member_count_zero", false, [](auto* b) { WriteU16(b, 68, 0); }},
        {"member_count_large", false,
         [](auto* b) { WriteU16(b, 68, StrokeOrderPinyinIndex::kMaxGroupMembers + 1); }},
        {"group_reserved", false, [](auto* b) { WriteU16(b, 70, 1); }},
        {"members_offset_add_overflow", false, [](auto* b) { WriteU32(b, 72, 0xFFFFFFFCu); }},
        {"member_payload_overlap", false, [](auto* b) { WriteU32(b, 80, 90); }},
        {"member_reserved", false, [](auto* b) { WriteU16(b, 96, 1); }},
        {"char_group_membership", false, [](auto* b) { WriteU16(b, 84, 0); }},
        {"member_missing_char", false, [](auto* b) { WriteU32(b, 90, 0x4E01); }},
        {"member_rank_mismatch", false, [](auto* b) { WriteU16(b, 94, 3); }},
    };
    for (const auto& item : mutations) {
        std::vector<uint8_t> damaged = golden;
        item.mutate(&damaged);
        if (item.header_change) {
            RefreshHeaderCrc(&damaged);
        } else {
            RefreshBodyCrc(&damaged);
        }
        StrokeOrderPinyinIndex invalid;
        Expect(!invalid.TestOnlyBindView(damaged.data(), damaged.size()), item.name);
    }

    std::vector<uint8_t> reverse_only = golden;
    WriteU16(&reverse_only, 48, 4);  // 人 char rank.
    WriteU16(&reverse_only, 76, 3);  // Final group gains one member.
    WriteU16(&reverse_only, 94, 4);  // 人 rank in its real group.
    const uint8_t extra_member[] = {0xBA, 0x4E, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    reverse_only.insert(reverse_only.end(), std::begin(extra_member), std::end(extra_member));
    RefreshBodyCrc(&reverse_only);
    Expect(!StrokeOrderPinyinIndex().TestOnlyBindView(reverse_only.data(), reverse_only.size()),
           "group member must reciprocally reference group");

    std::vector<uint8_t> trailing = golden;
    trailing.push_back(0);
    RefreshBodyCrc(&trailing);
    Expect(!StrokeOrderPinyinIndex().TestOnlyBindView(trailing.data(), trailing.size()),
           "trailing unreferenced payload rejected");

    struct ArithmeticCase {
        const char* name;
        uint32_t a;
        uint32_t b;
        bool add_ok;
        bool mul_ok;
    };
    const ArithmeticCase arithmetic[] = {
        {"small", 32, 8, true, true},
        {"add overflow", 0xFFFFFFFFu, 1, false, true},
        {"mul overflow", 0x40000000u, 8, true, false},
    };
    for (const auto& item : arithmetic) {
        uint32_t out = 0;
        Expect(StrokeOrderPinyinIndex::TestOnlyAddU32(item.a, item.b, &out) == item.add_ok,
               item.name);
        Expect(StrokeOrderPinyinIndex::TestOnlyMulU32(item.a, item.b, &out) == item.mul_ok,
               item.name);
    }

    StrokeOrderController controller;
    Expect(controller.BindStore(sob1.data(), sob1.size()), "bind 500 SOB1");
    Expect(controller.is_ready(), "controller ready");
    StrokeOrderStore store;
    Expect(store.Bind(sob1.data(), sob1.size()) && store.character_count() == 500, "store 500");
    for (uint32_t i = 0; i < store.character_count(); ++i) {
        uint32_t cp = 0;
        Expect(store.GetCodepointAt(i, &cp) && store.LoadCharacter(cp) && store.stroke_count() > 0,
               "sequential load");
        if (failures > 8) {
            break;
        }
    }

    StrokeOrderPinyinIndex full;
    Expect(full.Bind(spy1.data(), spy1.size()) && full.character_count() == 500, "bind 500 SPY1");
    StrokeOrderPinyinProvider provider(&full);
    Expect(controller.SetCandidatesFromPrimary(0x4E00, &provider), "primary 一 with provider");
    Expect(controller.candidate_count() >= 1, "store-filtered candidates");
    uint32_t first = 0;
    Expect(controller.GetCandidate(0, &first) && first == 0x4E00, "primary first");
    OutOfStorePrimaryProvider extras_only;
    Expect(controller.SetCandidatesFromPrimary(0x9F98, &extras_only),
           "primary outside store is not rejected before provider extras");
    uint32_t extra_only = 0;
    Expect(controller.candidate_count() == 1 && controller.GetCandidate(0, &extra_only) &&
               extra_only == 0x4E00,
           "final candidates remain store-only");
    Expect(!controller.SetCandidatesFromPrimary(0x9F98, &provider),
           "unsupported primary with empty extras still fails after filter");

    uint32_t mqtt[6] = {};
    n = provider.AppendTopRanked(mqtt, 6);
    Expect(n == 6 && mqtt[0] == 0x4E00, "MQTT TopRanked starts at 一");

    StrokeOrderPinyinIndex missing;
    StrokeOrderPinyinProvider null_provider(&missing);
    Expect(null_provider.AppendHomophones(0x4E00, extras, 6) == 0,
           "no-SPY1 provider is exact-only");
    Expect(controller.SetCandidatesFromPrimary(0x4E00, &null_provider) &&
               controller.candidate_count() == 1,
           "missing SPY1 does not break SOB1 exact-only");

    std::vector<uint8_t> spy_corrupt(spy1.size(), 0);
    Expect(!full.Bind(spy_corrupt.data(), spy_corrupt.size()) && full.is_bound(),
           "corrupt SPY1 does not clobber previous pinyin bind");
    Expect(controller.is_ready(), "corrupt SPY1 cannot unbind SOB1");
    full.Unbind();
    Expect(!full.is_bound() && controller.is_ready(), "SPY1 unbind leaves SOB1 bound");
    Expect(controller.SetCandidatesFromPrimary(0x4E00, &null_provider) &&
               controller.candidate_count() == 1,
           "asset suspend of SPY1 degrades to exact-only");
    Expect(full.Bind(spy1.data(), spy1.size()) && full.character_count() == 500,
           "SPY1 rebind after suspend");
    Expect(controller.SetCandidatesFromPrimary(0x4E00, &provider) &&
               controller.GetCandidate(0, &first) && first == 0x4E00,
           "WS provider path still primary-first after rebind");

    if (failures != 0) {
        std::cerr << "stroke_order_pinyin_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_order_pinyin_harness: PASS\n";
    return 0;
}
