// Host admission driver using the unmodified production W4a reader and C decoder.
#include "stroke_order/stroke_order_v2.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::string directory = argv[1];
    auto read = [&](const std::string& name) {
        std::ifstream file(directory + "/" + name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    };
    auto cat = read("stroke_cat.bin");
    auto py = read("stroke_pinyin.bin");
    std::vector<uint8_t> storage[6];
    stroke_order_v2::Blob shards[6];
    for (size_t i = 0; i < 6; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "so%02u.bin", static_cast<unsigned>(i));
        storage[i] = read(name);
        shards[i] = {storage[i].data(), storage[i].size()};
    }
    if (!stroke_order_v2::ValidateBundle({cat.data(), cat.size()}, {py.data(), py.size()}, shards,
                                         6, stroke_order_v2::ValidationMode::Deep))
        return 1;
    std::puts("W4a ValidateBundle PASS 3500 6 stored<=16384");
    return 0;
}
