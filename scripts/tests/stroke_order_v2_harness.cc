#include "stroke_order/stroke_order_v2.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace stroke_order_v2;
using Bytes = std::vector<uint8_t>;
static Bytes Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), {});
}
static Blob View(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }
static void Require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
static void Write32(std::ofstream& file, uint32_t value) {
    uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                        uint8_t(value >> 24)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
}
static void AllocationTests(Blob cat_bytes, Blob py_bytes, Blob shard_bytes) {
    Catalog cat;
    Pinyin py;
    Shard shard;
    Require(cat.Bind(cat_bytes) && py.Bind(py_bytes) && shard.Bind(shard_bytes), "alloc baseline");
    for (int i = 0; i < 1; ++i) {
        FailAllocationAfter(i);
        Require(!cat.Bind(cat_bytes), "catalog alloc failure");
        FailAllocationAfter(i);
        Require(!shard.Bind(shard_bytes), "shard alloc failure");
    }
    for (int i = 0; i < 2; ++i) {
        FailAllocationAfter(i);
        Require(!py.Bind(py_bytes), "pinyin alloc failure");
    }
    FailAllocationAfter(-1);
    Glyph glyph;
    Require(shard.Decode(0, &glyph), "initial decode");
    auto old = glyph.raw.get();
    auto cp = glyph.codepoint;
    for (int i = 0; i < 3; ++i) {
        FailAllocationAfter(i);
        Require(!shard.Decode(0, &glyph), "decode alloc failure");
        Require(glyph.raw.get() == old && glyph.codepoint == cp, "decode failure atomicity");
    }
    FailAllocationAfter(-1);
    Require(py.Matches(cat) && shard.Decode(0, &glyph), "allocation recovery");
}
int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    std::string mode = argv[1];
    if (mode == "bundle" || mode == "structural") {
        if (argc != 4 && argc != 5)
            return 2;
        std::string directory = argv[2];
        auto cb = Read(directory + "/stroke_cat.bin"), pb = Read(directory + "/stroke_pinyin.bin");
        Catalog cat;
        Pinyin py;
        Require(cat.Bind(View(cb)) && py.Bind(View(pb), cat.corpus()) && py.Matches(cat),
                "metadata");
        std::vector<Bytes> storage;
        std::vector<Blob> shards;
        for (unsigned i = 0; i < cat.shard_count(); ++i) {
            char name[24];
            std::snprintf(name, sizeof(name), "/so%02u.bin", i);
            storage.push_back(Read(directory + name));
        }
        for (const auto& bytes : storage)
            shards.push_back(View(bytes));
        ResetWorkCounters();
        Require(ValidateBundle(View(cb), View(pb), shards.data(), shards.size(),
                               ValidationMode::Structural), "structural bundle");
        Require(GetWorkCounters().decodes == 0 &&
                    GetWorkCounters().owned_allocations == 3 + shards.size(),
                "structural work bounded by shard count");
        if (mode == "structural")
            return 0;
        ResetWorkCounters();
        Require(ValidateBundle(View(cb), View(pb), shards.data(), shards.size(),
                               ValidationMode::Deep), "full bundle");
        Require(GetWorkCounters().decodes == 3500, "deep admission decodes all glyphs");
        Require(!cat.ValidateShards(shards.data(), shards.size() - 1), "missing shard");
        Require(!cat.ValidateShards(shards.data(), shards.size() + 1), "extra shard");
        Require(!cat.Bind({}) && !py.Bind({}), "invalid rebind");
        Require(py.Matches(cat), "failed rebind preserves metadata");
        Bytes unaligned_cat = cb, unaligned_py = pb, unaligned_shard = storage[0];
        unaligned_cat.insert(unaligned_cat.begin(), 0);
        unaligned_py.insert(unaligned_py.begin(), 0);
        unaligned_shard.insert(unaligned_shard.begin(), 0);
        Catalog uc;
        Pinyin up;
        Shard us;
        Glyph ug;
        Require(uc.Bind({unaligned_cat.data() + 1, cb.size()}) &&
                    up.Bind({unaligned_py.data() + 1, pb.size()}, uc.corpus()) && up.Matches(uc) &&
                    us.Bind({unaligned_shard.data() + 1, storage[0].size()}, uc.corpus()) &&
                    us.Decode(0, &ug),
                "unaligned readers");
        AllocationTests(View(cb), View(pb), shards[0]);
        std::ofstream output(argv[3], std::ios::binary);
        uint32_t total = 0;
        for (auto bytes : shards) {
            Shard shard;
            Require(shard.Bind(bytes, cat.corpus()), "shard");
            for (uint32_t i = 0; i < shard.count(); ++i) {
                Record r;
                Glyph glyph;
                Require(shard.GetRecord(i, &r) && shard.Decode(i, &glyph), "glyph");
                Write32(output, r.rank);
                Write32(output, r.stored);
                Write32(output, r.transformed);
                Write32(output, glyph.size);
                output.write(reinterpret_cast<const char*>(glyph.raw.get()), glyph.size);
                for (uint16_t s = 0; s < glyph.stroke_count; ++s) {
                    StrokeOrderStore::Point point;
                    Require(glyph.strokes[s].GetOutlinePoint(0, &point), "shared view");
                    Require(
                        !glyph.strokes[s].GetMedianPoint(glyph.strokes[s].median_count(), &point),
                        "view bound");
                }
                ++total;
            }
        }
        Require(total == 3500 && output.good(), "output");
        for (uint32_t i = 0; i < 3500; ++i) {
            CatalogEntry entry;
            Require(cat.GetEntry(i, &entry), "catalog entry");
            uint32_t candidates[6];
            size_t count = py.Homophones(entry.codepoint, candidates, 100);
            Require(count <= 6, "top6");
            std::cout << "P " << entry.codepoint << ' ' << py.ReadingCount(entry.codepoint);
            for (size_t j = 0; j < count; ++j)
                std::cout << ' ' << candidates[j];
            std::cout << '\n';
        }
        uint32_t maximum = 0;
        for (uint32_t g = 0; g < py.group_count(); ++g)
            maximum = std::max(maximum, py.GroupSize(g));
        std::cout << "PASS 3500 " << shards.size() << ' ' << maximum << '\n';
        return 0;
    }
    auto bytes = Read(argv[2]);
    if (mode == "shard") {
        Shard shard;
        if (!shard.Bind(View(bytes)))
            return 1;
        Glyph glyph;
        for (uint32_t i = 0; i < shard.count(); ++i)
            if (!shard.Decode(i, &glyph))
                return 1;
        return 0;
    }
    if (mode == "catalog") {
        Catalog cat;
        return cat.Bind(View(bytes)) ? 0 : 1;
    }
    if (mode == "pinyin") {
        Pinyin py;
        return py.Bind(View(bytes)) ? 0 : 1;
    }
    if ((mode == "dzz" || mode == "hs") && argc == 4) {
        size_t size = std::strtoul(argv[3], nullptr, 10);
        if (size > 16388)
            return 1;
        Bytes output(size);
        bool ok = mode == "dzz" ? DecodeDzz1(View(bytes), output.data(), size)
                                : DecodeHeatshrink(View(bytes), output.data(), size);
        return ok ? 0 : 1;
    }
    return 2;
}
