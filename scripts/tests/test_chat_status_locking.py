"""Real LVGL + production display declarations and verbatim locking method bodies.

Only hardware constructors, unrelated virtuals, initial widget layout and NVS are
stubbed. In particular SetTextFont, SetTheme, LCD/OLED theme application, SetStatus,
the board subclass and DisplayLockGuard are NOT sequential policy substitutes.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def definition(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + (";" if signature.startswith("class ") else "")


class ChatStatusLockingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lvgl = ROOT / "managed_components/lvgl__lvgl"
        if not lvgl.is_dir() or not shutil.which("cmake") or not shutil.which("c++"):
            raise unittest.SkipTest("requires resolved LVGL, cmake and a host C++ compiler")
        cls.temp = tempfile.TemporaryDirectory(prefix="chat-status-lock-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        stubs = work / "stubs"
        stubs.mkdir()
        for name, content in {
            "esp_log.h": '#include <cstdio>\n#define ESP_LOGW(...) ((void)0)\n'
                         '#define ESP_LOGE(...) std::fputs("Failed to lock display\\n", stderr)\n',
            "esp_pm.h": "using esp_pm_lock_handle_t = void*;\n",
            "esp_timer.h": "using esp_timer_handle_t = void*;\n",
            "esp_lcd_panel_io.h": "using esp_lcd_panel_io_handle_t = void*;\n",
            "esp_lcd_panel_ops.h": "using esp_lcd_panel_handle_t = void*;\n",
            "esp_heap_caps.h": '#include <cstdlib>\n#define MALLOC_CAP_INTERNAL 1\n'
                               '#define MALLOC_CAP_8BIT 2\n#define MALLOC_CAP_SPIRAM 4\n'
                               'inline void* heap_caps_malloc(size_t n, unsigned) { return malloc(n); }\n'
                               'inline void heap_caps_free(void* p) { free(p); }\n',
        }.items():
            (stubs / name).write_text("#pragma once\n" + content)
        (work / "lv_conf.h").write_text(
            "#pragma once\n#define LV_COLOR_DEPTH 16\n"
            "#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB\n"
            "#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB\n"
            "#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB\n"
            "#define LV_USE_OS LV_OS_NONE\n#define LV_USE_THORVG_INTERNAL 0\n")
        methods = []
        for path, signatures in {
            "main/display/lvgl_display/lvgl_display.cc": [
                "bool LvglDisplay::SetTextFont(", "void LvglDisplay::SetTheme(",
                "void LvglDisplay::SetThemeLocked(", "void LvglDisplay::SetStatus(",
                "void LvglDisplay::SetStatusLocked("],
            "main/display/lcd_display.cc": ["void LcdDisplay::SetThemeLocked("],
            "main/display/oled_display.cc": ["void OledDisplay::SetThemeLocked("],
            "main/display/display.cc": ["void Display::SetTheme("],
            "main/boards/m5stack/core-s3/m5stack_core_s3.cc": ["class CoreS3ChatDisplay "],
        }.items():
            methods.extend(definition(path, sig) for sig in signatures)
        production = "\n\n".join(methods)
        # Mutations reproduce each rejected locking shape, demonstrating sensitivity.
        mutations = {
            "normal": production,
            "nested": production.replace("SetThemeLocked(current_theme_)",
                                           "SetTheme(current_theme_)"),
            "split": production.replace(
                "protected:\n    void SetStatusLocked(const char* status) override",
                "public:\n    void SetStatus(const char* status) override").replace(
                'LvglDisplay::SetStatusLocked(status != nullptr ? status : "");',
                'LvglDisplay::SetStatus(status != nullptr ? status : "");\n'
                '        DisplayLockGuard lock(this);'),
        }
        for mode, source in mutations.items():
            if mode != "normal":
                assert source != production, mode
            (work / f"production-{mode}.inc").write_text(source)
        cmake = [
            "cmake_minimum_required(VERSION 3.20)",
            "project(chat_status_lock_test LANGUAGES C CXX ASM)",
            "set(CMAKE_CXX_STANDARD 17)",
            'set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)',
            'set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)',
            f'add_subdirectory("{lvgl}" lvgl)',
            "find_package(Threads REQUIRED)",
        ]
        for style in (0, 1):
            for mode in mutations:
                name = f"chat_{style}_{mode}"
                cmake += [
                    f'add_executable({name} "{ROOT}/scripts/tests/chat_status_locking_harness.cc" '
                    f'"{ROOT}/main/display/lvgl_display/lvgl_theme.cc")',
                    f'target_include_directories({name} PRIVATE "{work}" "{stubs}" '
                    f'"{ROOT}/main/display" "{ROOT}/main/display/lvgl_display")',
                    f'target_compile_definitions({name} PRIVATE CONFIG_USE_WECHAT_MESSAGE_STYLE={style} '
                    f'PRODUCTION_INCLUDE="production-{mode}.inc")',
                    f'target_compile_options({name} PRIVATE -Wall -Wextra -Werror '
                    '-Wno-unused-parameter -fsanitize=undefined -fno-sanitize-recover=all)',
                    f'target_link_options({name} PRIVATE -fsanitize=undefined)',
                    f'target_link_libraries({name} PRIVATE lvgl Threads::Threads)',
                ]
        # Some macOS CLT installs do not discover libc++ headers automatically.
        if shutil.which("xcrun"):
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            cmake.insert(3, f'add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-isystem{sdk}/usr/include/c++/v1>")')
        cmake.insert(3, 'add_compile_options("$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-inconsistent-missing-override>")')
        (work / "CMakeLists.txt").write_text("\n".join(cmake))
        cls.build = work / "build"
        for command in (["cmake", "-S", str(work), "-B", str(cls.build)],
                        ["cmake", "--build", str(cls.build), "-j", "6"]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)

    def run_case(self, mode, case, success=True):
        for style in (0, 1):
            with self.subTest(style=style, mode=mode, case=case):
                run = subprocess.run([str(self.build / f"chat_{style}_{mode}"), case],
                                     capture_output=True, text=True, timeout=20,
                                     env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
                if success:
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn("PASS", run.stdout)
                else:
                    self.assertNotEqual(run.returncode, 0, "mutation escaped detection")
                    expected = "Failed to lock display" if mode == "nested" else "inconsistent status commit"
                    self.assertIn(expected, run.stderr)

    def test_nonrecursive_font_virtual_chain_lcd_oled_and_cores3(self):
        self.run_case("normal", "font")

    def test_controlled_concurrent_status_commits(self):
        self.run_case("normal", "status")

    def test_role_filter_theme_and_overlay(self):
        self.run_case("normal", "policy")

    def test_rejects_original_nested_font_lock(self):
        self.run_case("nested", "font", success=False)

    def test_rejects_split_status_transaction(self):
        self.run_case("split", "status", success=False)
