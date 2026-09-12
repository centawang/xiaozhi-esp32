import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _host_compiler():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise AssertionError("no host C++ compiler found for protocol selection harness")
    return compiler


def _compile_with_fallback(common):
    attempts = [common]
    xcrun = shutil.which("xcrun")
    if xcrun:
        sdk = subprocess.run(
            [xcrun, "--show-sdk-path"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if sdk.returncode == 0:
            attempts.append(
                common[:1] + ["-isystem", f"{sdk.stdout.strip()}/usr/include/c++/v1"] + common[1:]
            )

    failures = []
    for command in attempts:
        run = subprocess.run(command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run.returncode == 0:
            return
        failures.append(f"{' '.join(command)}\n{run.stderr}")
    raise AssertionError("failed to compile protocol selection harness:\n" + "\n".join(failures))


def _compile_protocol_selection_harness(output: Path):
    command = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main/protocols"),
        str(ROOT / "scripts/tests/protocol_selection_harness.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(command)


class ProtocolSelectionTest(unittest.TestCase):
    def test_production_helper_matrix_and_stroke_websocket_preference(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "protocol_selection_harness"
            _compile_protocol_selection_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("protocol_selection_harness: PASS", run.stdout)

    def test_application_initialize_protocol_uses_helper(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/protocols/protocol_selection.h").read_text(encoding="utf-8")
        self.assertIn('#include "protocol_selection.h"', application)
        start = application.index("void Application::InitializeProtocol()")
        connected = application.index("protocol_->OnConnected", start)
        init = application[start:connected]
        self.assertIn("SelectProtocolTransport", init)
        self.assertIn("HasMqttConfig", init)
        self.assertIn("HasWebsocketConfig", init)
        self.assertIn("prefer_websocket_for_stroke_voice", init)
        self.assertIn("CONFIG_STROKE_ORDER_LOCAL", init)
        self.assertIn("ProtocolTransportName", init)
        self.assertIn("Stroke voice availability", init)
        self.assertNotIn("if (ota_->HasMqttConfig())", init)
        self.assertIn("prefer_websocket_for_stroke_voice && input.has_websocket_config", header)
        self.assertNotIn("endpoint", init.lower())
        self.assertNotIn("token", init.lower())

    def test_mqtt_stroke_voice_capability_stays_disabled(self):
        mqtt = (ROOT / "main/protocols/mqtt_protocol.h").read_text(encoding="utf-8")
        websocket = (ROOT / "main/protocols/websocket_protocol.h").read_text(encoding="utf-8")
        self.assertIn("SupportsCorrelatedSessionOpen() const override { return false; }", mqtt)
        self.assertIn("SupportsStrokeVoiceRouting() const override { return false; }", mqtt)
        self.assertIn("SupportsCorrelatedSessionOpen() const override { return true; }", websocket)
        self.assertIn("SupportsStrokeVoiceRouting() const override { return true; }", websocket)


if __name__ == "__main__":
    unittest.main()
