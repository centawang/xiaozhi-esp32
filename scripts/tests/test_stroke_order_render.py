"""Real LVGL software/RGB565 offscreen regression for the production View.

The existing fence harness does not compile the View. Here we extract complete
production method bodies verbatim (not a model of the dirty/admission logic),
include the real View header and link the real controller/clock/coordinator.
Only the ESP/Application/Display boundary is stubbed. A firmware build is still
needed for the complete ESP translation unit and physical flush/input behavior.
All generated sources, LVGL objects and fixtures live in TemporaryDirectory.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_stroke_order_ui import ROOT, package_smoke_corpus


LVGL = ROOT / "managed_components/lvgl__lvgl"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def prepare(directory):
    directory = Path(directory)
    source = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
    names = [
        "void StrokeOrderView::HideEntryLocked", "void StrokeOrderView::RequestAbortLocked",
        "void StrokeOrderView::StopAnimTimer", "bool StrokeOrderView::SyncAnimTimer",
        "bool StrokeOrderView::EnsureOverlay", "void StrokeOrderView::DestroyOverlay",
        "bool StrokeOrderView::CacheCandidateGlyph", "bool StrokeOrderView::CacheLoadedGlyph",
        "bool StrokeOrderView::RenderCandidates", "bool StrokeOrderView::RenderAnimationPage",
        "bool StrokeOrderView::RenderErrorPage", "bool StrokeOrderView::RenderStatusPage",
        "void StrokeOrderView::DrawTianGrid", "void StrokeOrderView::DrawGlyphOutlines",
        "void StrokeOrderView::DrawStrokeOutline", "void StrokeOrderView::DrawStartMarker",
        "void StrokeOrderView::RedrawCanvas", "void StrokeOrderView::UpdateControlLabels",
        "bool StrokeOrderView::HandleControlLocked", "void StrokeOrderView::HandleStatePresentationLocked",
        "void StrokeOrderView::DisarmClick", "uint32_t StrokeOrderView::IndexFromUserData",
        "void StrokeOrderView::CandidateClicked", "void StrokeOrderView::ControlClicked",
        "void StrokeOrderView::OverlayDeleted", "void StrokeOrderView::EntryDeleted",
        "void StrokeOrderView::CandidateDraw", "void StrokeOrderView::AnimTimerCb",
    ]
    helpers = "\n".join(function(source, name) for name in
                        ["lv_color_t MixLight", "void DrawLine", "void StyleControl"])
    (directory / "production.inc").write_text(
        "constexpr int kGridInset = 12, kOutlineWidth = 2, kMarkerRadius = 5;\n" + helpers +
        "\n".join(function(source, name) for name in names))
    (directory / "display.h").write_text('''#pragma once
#include <lvgl.h>
class LvglTheme {
public:
    lv_color_t bg = lv_color_white(), fg = lv_color_black();
    lv_color_t background_color() const { return bg; }
    lv_color_t text_color() const { return fg; }
    lv_color_t border_color() const { return fg; }
    lv_color_t assistant_bubble_color() const { return bg; }
};
class Display {
public:
    LvglTheme theme;
    LvglTheme* GetTheme() { return &theme; }
};
''')
    (directory / "lv_conf.h").write_text('''#pragma once
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_LOG 0
#define LV_USE_SYSMON 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
''')
    cxx_headers = ""
    if shutil.which("xcrun"):
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        cxx_headers = f'target_include_directories(render SYSTEM PRIVATE "{sdk}/usr/include/c++/v1")'
    sources = [ROOT / "main/stroke_order" / (name + ".cc") for name in
               ("stroke_order_controller", "stroke_order_catalog", "stroke_order_pinyin",
                "stroke_order_store", "stroke_round_coordinator")]
    (directory / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(stroke_render_test C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "{directory}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{LVGL}" lvgl)
add_executable(render "{ROOT}/scripts/tests/stroke_order_render_harness.cc"
 {' '.join('"' + str(s) + '"' for s in sources)})
target_include_directories(render PRIVATE "{directory}" "{ROOT}/main")
target_compile_definitions(render PRIVATE HAVE_LVGL=1 CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500=0)
target_compile_options(render PRIVATE -Wall -Wextra -Werror -UNDEBUG -fsanitize=undefined -fno-sanitize-recover=all)
target_link_options(render PRIVATE -fsanitize=undefined)
target_link_libraries(render PRIVATE lvgl)
{cxx_headers}
''')
    # Decode only the selected real Level1_3500 records, then repackage their
    # unchanged raw geometry as SOB1 for the render-only Controller seam.
    from stroke_order import scb2, sob2, dzz1, heatshrink
    from stroke_order.convert import pack_characters
    from stroke_order.v2 import HEADER_SIZE, crc
    import struct
    corpus = ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500"
    catalog = scb2.validate_scb2((corpus / "stroke_cat.bin").read_bytes())
    characters = []
    for char in "一乙人口顺赢囊":
        entry = next(e for e in catalog["characters"] if e["codepoint"] == ord(char))
        shard = (corpus / catalog["shards"][entry["shard"]]["name"]).read_bytes()
        cp, off, stored, transformed, raw_len, stored_crc, raw_crc, rank, _ = sob2.INDEX.unpack_from(
            shard, HEADER_SIZE + entry["local"] * sob2.INDEX.size)
        payload = shard[off:off + stored]
        assert crc(payload) == stored_crc and cp == ord(char) and rank == entry["rank"]
        raw = dzz1.decode(heatshrink.decode(payload, transformed), raw_len)
        assert crc(raw) == raw_crc and struct.unpack_from("<I", raw)[0] == cp
        characters.append({"codepoint": cp, "character": char,
                           "strokes": [{"outline": o, "median": m}
                                       for o, m in dzz1.inspect_raw(raw)]})
    (directory / "real3500.sob1").write_bytes(pack_characters(characters))
    return package_smoke_corpus(directory / "assets")["bin_path"]


class StrokeOrderRenderTest(unittest.TestCase):
    def test_real_lvgl_pixels_render_admission_and_counts(self):
        if not LVGL.is_dir():
            self.skipTest("resolved LVGL dependency unavailable; run IDF dependency resolution first")
        if not shutil.which("cmake"):
            self.fail("cmake is required for real LVGL rendering test")
        with tempfile.TemporaryDirectory(prefix="stroke-render-test-") as tmp:
            fixture = prepare(tmp)
            commands = [
                ["cmake", "-S", tmp, "-B", tmp + "/build", "-DCMAKE_BUILD_TYPE=Debug"],
                ["cmake", "--build", tmp + "/build", "-j", "8", "--target", "render"],
                [tmp + "/build/render", fixture,
                 str(ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000/so06.sob1"),
                 tmp + "/real3500.sob1"],
            ]
            for command in commands:
                run = subprocess.run(command, text=True, capture_output=True, timeout=240,
                                     env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
                self.assertEqual(run.returncode, 0, " ".join(command) + "\n" + run.stdout + run.stderr)
            print(run.stdout, end="")
            self.assertIn("stroke_order_render_harness: PASS", run.stdout)


if __name__ == "__main__":
    unittest.main()
