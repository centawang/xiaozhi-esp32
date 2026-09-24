"""Compile the production Assets lease/unload methods with a tiny mmap/View stub.

No ESP/LVGL runtime is claimed here; the extracted owning pin and boolean unload
logic are the actual product bodies, not a separately reimplemented algorithm.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StrokeOrderAssetsLeaseTest(unittest.TestCase):
    def test_real_lease_methods_gate_unmap_and_close_admission(self):
        source = (ROOT / "main/assets.cc").read_text()
        begin = source.index("bool Assets::UnApplyPartition()")
        end = source.index("void Assets::UseBuiltInTextFontCapability()", begin)
        bodies = source[begin:end]
        prefix = r'''
#include "stroke_order/stroke_order_source_adapter.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#define CONFIG_STROKE_ORDER_LOCAL 1
#define CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500 1
#define ESP_LOGE(...) ((void)0)
struct StrokeOrderView {
    bool drained = false;
    bool SuspendAssets() { return drained; }
    static StrokeOrderView& GetInstance() { static StrokeOrderView v; return v; }
};
struct Assets {
    struct Strategy {
        int unmapped = 0;
        void UnApplyPartition(Assets*) { ++unmapped; }
    };
    std::unique_ptr<Strategy> strategy_{new Strategy};
    mutable std::mutex stroke_mapping_mutex_;
    std::shared_ptr<const uint64_t> stroke_mapping_pin_{new uint64_t(7)};
    uint64_t stroke_mapping_generation_ = 7;
    bool stroke_mapping_accepting_ = true;
    int font_resets = 0;
    std::map<std::string, std::vector<uint8_t>> files;
    void UseBuiltInTextFontCapability() { ++font_resets; }
    bool GetAssetData(const std::string& n, void*& p, size_t& s) {
        auto i = files.find(n);
        if (i == files.end()) return false;
        p = i->second.data(); s = i->second.size(); return true;
    }
    bool UnApplyPartition();
    std::shared_ptr<const StrokeOrderBundleOwner> LeaseStrokeBundle(uint64_t*);
    uint64_t StrokeAssetsGeneration() const;
};
'''
        suffix = r'''
int main() {
    Assets assets;
    const char* names[] = {"stroke_cat.bin", "stroke_pinyin.bin", "so00.bin", "so01.bin",
                           "so02.bin", "so03.bin", "so04.bin", "so05.bin"};
    for (int i = 0; i < 8; ++i) assets.files[names[i]] = {uint8_t(i + 1)};
    uint64_t generation = 99;
    auto owner = assets.LeaseStrokeBundle(&generation);
    assert(owner && generation == 7 && owner->ShardCount() == 6);
    assert(owner->Catalog().data[0] == 1 && owner->Pinyin().data[0] == 2);
    for (int i = 0; i < 6; ++i) assert(owner->Shard(i).data[0] == i + 3);
    assert(!owner->Shard(6).data);
    assert(assets.stroke_mapping_pin_.use_count() == 2);
    assert(!assets.UnApplyPartition()); // worker/borrow not drained
    assert(!assets.stroke_mapping_accepting_ && !assets.StrokeAssetsGeneration());
    assert(!assets.LeaseStrokeBundle(&generation));
    assert(!assets.strategy_->unmapped && !assets.font_resets);
    StrokeOrderView::GetInstance().drained = true;
    assert(!assets.UnApplyPartition()); // even a lease not yet submitted vetoes unmap
    assert(!assets.strategy_->unmapped && !assets.font_resets);
    assert(owner->Shard(5).data[0] == 8);
    owner.reset();
    assert(assets.UnApplyPartition());
    assert(assets.strategy_->unmapped == 1 && assets.font_resets == 1);
    assert(!assets.stroke_mapping_pin_);
    Assets missing;
    for (int i = 0; i < 7; ++i) missing.files[names[i]] = {uint8_t(i + 1)};
    generation = 99;
    assert(!missing.LeaseStrokeBundle(&generation) && generation == 99);
    assert(missing.stroke_mapping_pin_.use_count() == 1);
}
'''
        with tempfile.TemporaryDirectory(prefix="stroke-w7b1-lease-") as tmp:
            directory = Path(tmp)
            harness = directory / "lease.cc"
            harness.write_text(prefix + bodies + suffix)
            exe = directory / "lease"
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-I", str(ROOT / "main")]
            if sys.platform == "darwin":
                sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
                command += ["-isystem", f"{sdk}/usr/include/c++/v1"]
            subprocess.run([*command, str(harness), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
