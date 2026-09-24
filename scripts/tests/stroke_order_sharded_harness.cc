#include "stroke_order/stroke_order_assets.h"
#include "stroke_order/stroke_order_catalog.h"
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_pinyin.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>());
}

uint32_t ReadU32(const std::vector<uint8_t>& blob, size_t offset) {
    return static_cast<uint32_t>(blob[offset]) | (static_cast<uint32_t>(blob[offset + 1]) << 8) |
           (static_cast<uint32_t>(blob[offset + 2]) << 16) |
           (static_cast<uint32_t>(blob[offset + 3]) << 24);
}

void WriteU32(std::vector<uint8_t>* blob, size_t offset, uint32_t value) {
    (*blob)[offset] = static_cast<uint8_t>(value);
    (*blob)[offset + 1] = static_cast<uint8_t>(value >> 8);
    (*blob)[offset + 2] = static_cast<uint8_t>(value >> 16);
    (*blob)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void WriteU16(std::vector<uint8_t>* blob, size_t offset, uint16_t value) {
    (*blob)[offset] = static_cast<uint8_t>(value);
    (*blob)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void RefreshHeaderCrc(std::vector<uint8_t>* blob) {
    WriteU32(blob, 24, StrokeOrderCatalog::CalculateCrc32(blob->data(), 24));
}

void RefreshBodyCrc(std::vector<uint8_t>* blob) {
    WriteU32(blob, 28,
             StrokeOrderCatalog::CalculateCrc32(blob->data() + StrokeOrderCatalog::kHeaderSize,
                                                blob->size() - StrokeOrderCatalog::kHeaderSize));
}

void ExpectCatalogRejected(const std::vector<uint8_t>& blob, const char* message) {
    StrokeOrderCatalog catalog;
    Expect(!catalog.Bind(blob.data(), blob.size()) && !catalog.is_bound(), message);
}

class MemoryShardSource final : public StrokeOrderShardSource {
public:
    bool AcquireShard(const char* name, const uint8_t** data, size_t* size) override {
        if (active || name == nullptr || data == nullptr || size == nullptr) {
            return false;
        }
        auto found = shards.find(name);
        if (found == shards.end() || found->second.empty()) {
            return false;
        }
        active = true;
        ++acquires;
        ++outstanding;
        max_outstanding = std::max(max_outstanding, outstanding);
        max_size = std::max(max_size, found->second.size());
        *data = found->second.data();
        *size = found->second.size();
        return true;
    }

    void ReleaseShard() override {
        Expect(active && outstanding == 1, "release exactly one acquired shard");
        active = false;
        --outstanding;
        ++releases;
    }

    std::map<std::string, std::vector<uint8_t>> shards;
    size_t max_size = 0;
    uint32_t acquires = 0;
    uint32_t releases = 0;
    uint32_t outstanding = 0;
    uint32_t max_outstanding = 0;
    bool active = false;
};

MemoryShardSource LoadSource(char** argv) {
    MemoryShardSource source;
    for (int i = 0; i < 8; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "so%02d.bin", i);
        source.shards[name] = ReadBinary(argv[i + 4]);
    }
    return source;
}

}  // namespace

int main(int argc, char** argv) {
    static_assert(StrokeOrderPinyinIndex::kMaxCharacters >= 2048);
    static_assert(StrokeOrderPinyinIndex::kMaxGroups >= 1047);
    static_assert(StrokeOrderPinyinIndex::kMaxFileBytes == 65536);
    // Runtime corpus sizes are admitted by the shared source profile contract.
    Expect(StrokeOrderProfileContract(StrokeOrderProfile::Legacy2000, 2000, 8),
           "legacy profile remains exactly 2000/8");
    if (argc != 14) {
        std::cerr << "usage: sharded_harness <catalog> <pinyin> <mismatched-pinyin> <so00> ... "
                     "<so07> <first-cp> <last-cp>\n";
        return 2;
    }
    const auto catalog_blob = ReadBinary(argv[1]);
    const auto pinyin_blob = ReadBinary(argv[2]);
    const auto mismatched_pinyin_blob = ReadBinary(argv[3]);
    Expect(pinyin_blob.size() == 63618 &&
               StrokeOrderPinyinIndex::kMaxFileBytes - pinyin_blob.size() == 1918,
           "SPY1 measured size and 64 KiB headroom");
    MemoryShardSource source = LoadSource(argv);
    const uint32_t first_cp = static_cast<uint32_t>(std::stoul(argv[12], nullptr, 16));
    const uint32_t last_cp = static_cast<uint32_t>(std::stoul(argv[13], nullptr, 16));

    StrokeOrderCatalog catalog;
    Expect(catalog.Bind(catalog_blob.data(), catalog_blob.size()), "bind SCB1");
    Expect(catalog.character_count() == 2000 && catalog.shard_count() == 8, "SCB1 counts");
    Expect(catalog.total_shard_bytes() == 5800492, "SCB1 total shard bytes");
    StrokeOrderCatalog::Entry first;
    StrokeOrderCatalog::Entry last;
    Expect(catalog.Find(first_cp, &first) && first.rank == 1 && first.shard == 0,
           "first official character maps to shard 0");
    Expect(catalog.Find(last_cp, &last) && last.rank == 2000 && last.shard == 7,
           "last official character maps to shard 7");

    {
        auto corrupt = catalog_blob;
        corrupt.back() ^= 1;
        ExpectCatalogRejected(corrupt, "SCB1 body CRC corruption rejected");
    }
    {
        auto duplicate_cp = catalog_blob;
        WriteU32(&duplicate_cp, StrokeOrderCatalog::kHeaderSize + StrokeOrderCatalog::kEntrySize,
                 ReadU32(duplicate_cp, StrokeOrderCatalog::kHeaderSize));
        RefreshBodyCrc(&duplicate_cp);
        ExpectCatalogRejected(duplicate_cp, "SCB1 duplicate codepoint rejected");
    }
    {
        auto duplicate_local = catalog_blob;
        uint32_t first_in_shard = UINT32_MAX;
        uint32_t second_in_shard = UINT32_MAX;
        for (uint32_t i = 0; i < catalog.character_count(); ++i) {
            StrokeOrderCatalog::Entry entry;
            if (!catalog.GetEntry(i, &entry) || entry.shard != 0) {
                continue;
            }
            if (first_in_shard == UINT32_MAX) {
                first_in_shard = i;
            } else {
                second_in_shard = i;
                break;
            }
        }
        const size_t first_offset =
            StrokeOrderCatalog::kHeaderSize + first_in_shard * StrokeOrderCatalog::kEntrySize + 8;
        const size_t second_offset =
            StrokeOrderCatalog::kHeaderSize + second_in_shard * StrokeOrderCatalog::kEntrySize + 8;
        const uint16_t local = static_cast<uint16_t>(duplicate_local[first_offset]) |
                               static_cast<uint16_t>(duplicate_local[first_offset + 1] << 8);
        WriteU16(&duplicate_local, second_offset, local);
        RefreshBodyCrc(&duplicate_local);
        ExpectCatalogRejected(duplicate_local, "SCB1 duplicate local index rejected");
    }
    {
        auto wrong_shard = catalog_blob;
        uint32_t rank_one_index = UINT32_MAX;
        for (uint32_t i = 0; i < catalog.character_count(); ++i) {
            StrokeOrderCatalog::Entry entry;
            if (catalog.GetEntry(i, &entry) && entry.rank == 1) {
                rank_one_index = i;
                break;
            }
        }
        wrong_shard[StrokeOrderCatalog::kHeaderSize +
                    rank_one_index * StrokeOrderCatalog::kEntrySize + 6] = 1;
        RefreshBodyCrc(&wrong_shard);
        ExpectCatalogRejected(wrong_shard, "SCB1 rank outside shard range rejected");
    }
    {
        auto unterminated_name = catalog_blob;
        const size_t shard_offset = StrokeOrderCatalog::kHeaderSize +
                                    catalog.character_count() * StrokeOrderCatalog::kEntrySize;
        std::fill(
            unterminated_name.begin() + static_cast<std::ptrdiff_t>(shard_offset),
            unterminated_name.begin() +
                static_cast<std::ptrdiff_t>(shard_offset + StrokeOrderCatalog::kShardNameBytes),
            static_cast<uint8_t>('a'));
        RefreshBodyCrc(&unterminated_name);
        ExpectCatalogRejected(unterminated_name, "SCB1 unterminated asset name rejected");
    }
    {
        auto wrong_total = catalog_blob;
        WriteU32(&wrong_total, 20, catalog.total_shard_bytes() + 1);
        RefreshHeaderCrc(&wrong_total);
        ExpectCatalogRejected(wrong_total, "SCB1 wrong total shard bytes rejected");
    }
    {
        auto trailing = catalog_blob;
        trailing.push_back(0);
        RefreshBodyCrc(&trailing);
        ExpectCatalogRejected(trailing, "SCB1 trailing data rejected");
    }

    StrokeOrderController controller;
    StrokeOrderPinyinIndex pinyin;
    Expect(RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                   &source, pinyin_blob.data(), pinyin_blob.size()),
           "transactionally bind catalog, pinyin, and eight shards");
    Expect(controller.is_ready() && pinyin.is_bound() && controller.MatchesPinyinIndex(pinyin) &&
               controller.Contains(first_cp) && controller.Contains(last_cp),
           "catalog/pinyin-backed controller ready");
    Expect(source.max_outstanding == 1 && source.outstanding == 0 && !source.active &&
               source.max_size <= StrokeOrderCatalog::kMaxShardBytes &&
               source.acquires == source.releases,
           "validation holds at most one bounded shard view");

    const uint32_t candidates[] = {first_cp, last_cp, first_cp};
    Expect(controller.SetCandidates(candidates, 3) && controller.candidate_count() == 2,
           "cross-shard candidates are loadable and deduplicated");
    Expect(controller.OpenCandidates() && controller.SelectCandidate(1), "select shard-7 glyph");
    std::vector<StrokeOrderController::DecodedStroke> glyph;
    Expect(controller.CopyLoadedGlyph(&glyph) && !glyph.empty(), "selected glyph is owned");
    const size_t stroke_count = glyph.size();
    Expect(source.max_outstanding == 1 && source.outstanding == 0 && !source.active &&
               source.acquires == source.releases,
           "candidate and selected decode release every shard view");

    SuspendStrokeOrderAssets(&controller, &pinyin);
    Expect(!controller.is_ready() && !pinyin.is_bound() &&
               controller.state() == StrokeOrderUiState::Hidden && source.outstanding == 0,
           "asset suspension synchronously invalidates all runtime references");
    Expect(glyph.size() == stroke_count && !glyph.empty(), "owned caller glyph survives unmap");

    Expect(RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                   &source, pinyin_blob.data(), pinyin_blob.size()) &&
               controller.is_ready() && pinyin.is_bound(),
           "complete asset set rebinds after suspension");
    SuspendStrokeOrderAssets(&controller, &pinyin);

    MemoryShardSource missing = LoadSource(argv);
    missing.shards.erase("so03.bin");
    Expect(!RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                    &missing, pinyin_blob.data(), pinyin_blob.size()) &&
               !controller.is_ready() && !pinyin.is_bound() && missing.outstanding == 0,
           "missing shard fails closed and releases its prior view");

    MemoryShardSource wrong = LoadSource(argv);
    wrong.shards["so00.bin"] = wrong.shards["so01.bin"];
    Expect(!RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                    &wrong, pinyin_blob.data(), pinyin_blob.size()) &&
               !controller.is_ready() && !pinyin.is_bound() && wrong.outstanding == 0,
           "swapped shard fails closed");

    auto corrupt_catalog = catalog_blob;
    corrupt_catalog.back() ^= 0x80;
    Expect(!RebindStrokeOrderAssets(&controller, &pinyin, corrupt_catalog.data(),
                                    corrupt_catalog.size(), &source, pinyin_blob.data(),
                                    pinyin_blob.size()) &&
               !controller.is_ready() && !pinyin.is_bound(),
           "catalog corruption fails the complete binding transaction");

    auto corrupt_pinyin = pinyin_blob;
    corrupt_pinyin.back() ^= 0x80;
    Expect(!RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                    &source, corrupt_pinyin.data(), corrupt_pinyin.size()) &&
               !controller.is_ready() && !pinyin.is_bound(),
           "pinyin corruption fails the complete binding transaction");

    Expect(!RebindStrokeOrderAssets(&controller, &pinyin, catalog_blob.data(), catalog_blob.size(),
                                    &source, mismatched_pinyin_blob.data(),
                                    mismatched_pinyin_blob.size()) &&
               !controller.is_ready() && !pinyin.is_bound(),
           "structurally valid but catalog-mismatched pinyin fails closed");

    if (failures != 0) {
        std::cerr << "stroke_order_sharded_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_order_sharded_harness: PASS acquires=" << source.acquires
              << " max_active=" << source.max_outstanding << " max_bytes=" << source.max_size
              << '\n';
    return 0;
}
