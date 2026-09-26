import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from package_stroke_order_smoke import package_smoke_corpus  # noqa: E402
from stroke_order.unpack import validate_blob  # noqa: E402


def _host_compiler():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise AssertionError("no host C++ compiler found for stroke-order harness")
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
    raise AssertionError("failed to compile stroke-order harness:\n" + "\n".join(failures))


def _compile_controller_harness(output: Path):
    common = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/stroke_order_controller_harness.cc"),
        str(ROOT / "main/stroke_order/stroke_order_controller.cc"),
        str(ROOT / "main/stroke_order/stroke_order_catalog.cc"),
        str(ROOT / "main/stroke_order/stroke_order_pinyin.cc"),
        str(ROOT / "main/stroke_order/stroke_order_store.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(common)


def _compile_lifecycle_harness(output: Path):
    command = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/stroke_order_lifecycle_harness.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(command)


def _compile_audio_generation_harness(output: Path):
    command = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/audio_stream_generation_harness.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(command)


def _compile_audio_processor_postcondition_harness(output: Path):
    command = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/audio_processor_postcondition_harness.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(command)


def _compile_listening_mode_selection_harness(output: Path):
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
        str(ROOT / "scripts/tests/listening_mode_selection_harness.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(command)


def _compile_round_integration_harness(output: Path):
    common = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-pthread",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/stroke_round_integration_harness.cc"),
        str(ROOT / "main/stroke_order/stroke_round_coordinator.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(common)


def _compile_session_harness(output: Path):
    common = [
        _host_compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=undefined",
        "-fno-sanitize-recover=all",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "scripts/tests/stroke_order_session_harness.cc"),
        str(ROOT / "main/stroke_order/stroke_order_controller.cc"),
        str(ROOT / "main/stroke_order/stroke_order_catalog.cc"),
        str(ROOT / "main/stroke_order/stroke_order_pinyin.cc"),
        str(ROOT / "main/stroke_order/stroke_order_store.cc"),
        "-o",
        str(output),
    ]
    _compile_with_fallback(common)


class StrokeOrderUiTest(unittest.TestCase):
    def test_package_smoke_corpus_is_offline_three_chars_not_release(self):
        source = Path(SCRIPTS / "package_stroke_order_smoke.py").read_text(encoding="utf-8")
        self.assertNotIn("urllib", source)
        self.assertNotIn("requests", source)
        self.assertIn("upstream_smoke", source)
        self.assertNotRegex(source, r"[\"']all\.json[\"']")
        with tempfile.TemporaryDirectory() as directory:
            result = package_smoke_corpus(directory)
            blob = Path(result["bin_path"]).read_bytes()
            parsed = validate_blob(blob, load_all=True)
            self.assertEqual(
                [item["codepoint"] for item in parsed["characters"]],
                [0x4E00, 0x4EBA, 0x53E3],
            )
            manifest = result["manifest"]
            self.assertTrue(manifest["not_a_release_library"])
            self.assertEqual(manifest["characters"], ["一", "人", "口"])
            self.assertTrue((Path(directory) / "ARPHICPL.TXT").is_file())
            self.assertTrue((Path(directory) / "NOTICE.md").is_file())
            notice = (Path(directory) / "NOTICE.md").read_text(encoding="utf-8")
            self.assertIn("not a 500-character or release", notice)

    def test_package_failure_keeps_previous_complete_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "assets"
            output.mkdir()
            (output / "old.marker").write_text("old", encoding="utf-8")
            with mock.patch(
                "package_stroke_order_smoke.shutil.copyfile",
                side_effect=OSError("injected copy failure"),
            ):
                with self.assertRaises(OSError):
                    package_smoke_corpus(output)
            self.assertEqual((output / "old.marker").read_text(encoding="utf-8"), "old")
            self.assertEqual(
                list(Path(directory).glob(".assets.staging-*")),
                [],
                "failed staging directories must be removed",
            )

    def test_cmake_declares_all_atomic_package_outputs(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("OUTPUT ${STROKE_ORDER_ASSET_OUTPUTS}", cmake)
        self.assertIn("stroke_cat.bin", cmake)
        self.assertIn("stroke_pinyin.bin", cmake)
        self.assertIn("so00.bin", cmake)
        self.assertIn("so07.bin", cmake)
        self.assertIn("ARPHICPL.TXT", cmake)
        self.assertIn("UNICODE-LICENSE.txt", cmake)
        self.assertIn("NOTICE.md", cmake)
        self.assertIn("selection-2000.csv", cmake)
        self.assertIn("charset-2000.txt", cmake)
        self.assertIn("SHA256SUMS", cmake)
        self.assertIn("package_stroke_order_2000.py", cmake)
        self.assertIn("stroke_order/stroke_order_assets.cc", cmake)
        for dependency in (
            "stroke_order/catalog.py",
            "stroke_order/constants.py",
            "stroke_order/pinyin.py",
            "stroke_order/pinyin_constants.py",
            "stroke_order/select.py",
            "stroke_order/unpack.py",
        ):
            self.assertIn(dependency, cmake)
        self.assertIn("DEFAULT_ASSETS_EXTRA_FILES_DEPENDENCIES ${STROKE_ORDER_ASSET_OUTPUTS}", cmake)

    def test_controller_state_idempotent_cancel_and_glyph_direction(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            packaged = package_smoke_corpus(tmp / "assets")
            executable = tmp / "stroke_order_controller_harness"
            _compile_controller_harness(executable)
            run = subprocess.run(
                [str(executable), packaged["bin_path"]],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_order_controller_harness: PASS", run.stdout)

    def test_production_lifecycle_touch_snapshot_and_sequence_harness(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "stroke_order_lifecycle_harness"
            _compile_lifecycle_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_order_lifecycle_harness: PASS", run.stdout)

    def test_session_parse_token_candidates_and_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            packaged = package_smoke_corpus(tmp / "assets")
            executable = tmp / "stroke_order_session_harness"
            _compile_session_harness(executable)
            run = subprocess.run(
                [str(executable), packaged["bin_path"]],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_order_session_harness: PASS", run.stdout)

    def test_round_route_fake_protocol_audio_and_ordinary_bypass(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "stroke_round_integration_harness"
            _compile_round_integration_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_round_integration_harness: PASS", run.stdout)

    def test_async_cancel_sources_fence_before_deferred_work(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        websocket = (ROOT / "main/protocols/websocket_protocol.cc").read_text(encoding="utf-8")
        mqtt = (ROOT / "main/protocols/mqtt_protocol.h").read_text(encoding="utf-8")
        harness = (ROOT / "scripts/tests/stroke_round_integration_harness.cc").read_text(
            encoding="utf-8"
        )

        network = application[
            application.index("board.SetNetworkEventCallback") : application.index(
                "// Start network asynchronously"
            )
        ]
        scanning = network[
            network.index("case NetworkEvent::Scanning") : network.index(
                "case NetworkEvent::Connecting"
            )
        ]
        disconnected = network[
            network.index("case NetworkEvent::Disconnected") : network.index(
                "case NetworkEvent::WifiConfigModeEnter"
            )
        ]
        self.assertLess(scanning.index("PublishStrokeCancelFence"), scanning.index("Schedule("))
        self.assertLess(
            scanning.index("PublishStrokeCancelFence"),
            scanning.index("MAIN_EVENT_NETWORK_DISCONNECTED"),
        )
        self.assertLess(
            disconnected.index("PublishStrokeCancelFence"),
            disconnected.index("MAIN_EVENT_NETWORK_DISCONNECTED"),
        )

        network_error = application[
            application.index("protocol_->OnNetworkError") : application.index(
                "protocol_->OnIncomingAudio"
            )
        ]
        self.assertLess(
            network_error.index("PublishStrokeCancelFence"),
            network_error.index("MAIN_EVENT_ERROR"),
        )
        reset = application[application.index("void Application::ResetProtocol()") :]
        self.assertLess(reset.index("PublishStrokeCancelFence"), reset.index("Schedule("))
        replace = application[
            application.index("void Application::RequestStartStrokeRound") : application.index(
                "void Application::RequestAbortStrokeRound"
            )
        ]
        self.assertLess(replace.index("PublishCancelFence"), replace.index("MAIN_EVENT_STROKE_START"))

        channel_close = application[
            application.index("protocol_->OnAudioChannelClosed") : application.index(
                "protocol_->OnIncomingJson"
            )
        ]
        self.assertIn("MatchStrokeOpenAttempt(info.open_attempt_id)", channel_close)
        # A fence-only scheduled abort runs after START. Use the publisher that
        # also invalidates the bounded pending/in-flight start transaction.
        self.assertLess(channel_close.index("RequestAbortStrokeRound"), channel_close.index("Schedule("))
        self.assertNotIn("stroke_round_.PublishCancelFence", channel_close)
        final_gate = application[
            application.index("void Application::StartListeningAudio()") : application.index(
                "void Application::ConfigureWakeWordForListening()"
            )
        ]
        self.assertLess(final_gate.index("HasCancelFence"), final_gate.index("SendStartListening"))
        self.assertLess(
            final_gate.index("HasCancelFence"), final_gate.index("EnableVoiceProcessing(true)")
        )
        self.assertLess(
            final_gate.index("SendStartListening"), final_gate.index("EnableVoiceProcessing(true)")
        )
        self.assertLess(
            final_gate.index("SendStartListening"), final_gate.index("MarkListeningStarted")
        )
        self.assertLess(
            final_gate.index("EnableVoiceProcessing(true)"),
            final_gate.index("ShowListeningFromMain"),
        )

        self.assertIn("ReserveAudioChannelOpenAttempt", websocket)
        self.assertIn(".open_attempt_id = generation", websocket)
        self.assertIn("SupportsCorrelatedSessionOpen() const override { return false; }", mqtt)
        self.assertIn("SupportsStrokeVoiceRouting() const override { return false; }", mqtt)
        for source in (
            "ChannelClosed",
            "NetworkDisconnected",
            "NetworkError",
            "ResetProtocol",
            "ReplaceRound",
        ):
            self.assertIn(f"FakeCancelSource::{source}", harness)
        self.assertIn("after_open_before_bind", harness)
        self.assertIn("STATE_CHANGED-first final gate rejects", harness)

    def test_listening_mode_helper_autostop_for_stroke_manual_for_generation_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "listening_mode_selection_harness"
            _compile_listening_mode_selection_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("listening_mode_selection_harness: PASS", run.stdout)

        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        helper = (ROOT / "main/protocols/listening_mode_selection.h").read_text(encoding="utf-8")
        self.assertIn('#include "listening_mode_selection.h"', application)
        start = application.index("void Application::HandleStartListeningRequest")
        stop = application.index("void Application::HandleStopListeningEvent")
        request = application[start:stop]
        self.assertIn("ListeningModeForStartGeneration", request)
        self.assertIn("ContinueOpenAudioChannel(start_mode, expected_generation)", request)
        self.assertIn("SetListeningMode(start_mode)", request)
        self.assertNotIn("kListeningModeManualStop", request)
        self.assertIn("generation == 0 ? QueuedStartListeningMode::ManualStop", helper)
        self.assertIn(": QueuedStartListeningMode::AutoStop", helper)

        default_mode = application[
            application.index("ListeningMode Application::GetDefaultListeningMode") : application.index(
                "void Application::Reboot()"
            )
        ]
        self.assertIn("kListeningModeAutoStop", default_mode)
        self.assertIn("kListeningModeRealtime", default_mode)
        self.assertNotIn("ListeningModeForStartGeneration", default_mode)

        toggle = application[
            application.index("void Application::HandleToggleChatEvent") : application.index(
                "void Application::ContinueOpenAudioChannel"
            )
        ]
        self.assertIn("GetDefaultListeningMode()", toggle)
        self.assertNotIn("ListeningModeForStartGeneration", toggle)

        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text(encoding="utf-8")
        start_voice = view[
            view.index("bool StrokeOrderView::StartVoiceSessionFromMain") : view.index(
                "bool StrokeOrderView::ShowListeningFromMain"
            )
        ]
        show_listen = view[
            view.index("bool StrokeOrderView::ShowListeningFromMain") : view.index(
                "bool StrokeOrderView::StartLocalCandidateSessionFromMain"
            )
        ]
        self.assertIn("BeginConnecting", start_voice)
        self.assertIn("RenderConnecting", start_voice)
        self.assertNotIn("RenderAwaitingSpeech", start_voice)
        self.assertIn('RenderStatusPage("Connect"', view)
        self.assertIn("MarkListeningReady", show_listen)
        self.assertIn("RenderAwaitingSpeech", show_listen)
        self.assertIn('RenderStatusPage("Speak"', view)
        self.assertNotIn("session_id", start_voice.lower())
        self.assertNotIn("token", start_voice.lower())
        self.assertNotIn("http", start_voice.lower())

        protocol = (ROOT / "main/protocols/protocol.cc").read_text(encoding="utf-8")
        self.assertIn("bool Protocol::SendStartListening", protocol)
        self.assertIn("return SendText(message)", protocol)
        start_audio = application[
            application.index("void Application::StartListeningAudio()") : application.index(
                "void Application::ConfigureWakeWordForListening()"
            )
        ]
        self.assertIn("protocol_->SendStartListening(listening_mode_)", start_audio)
        self.assertLess(start_audio.index("SendStartListening"), start_audio.index("ShowListeningFromMain"))

    def test_start_result_policy_is_used_by_production_failure_paths(self):
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        application_header = (ROOT / "main/application.h").read_text(encoding="utf-8")
        audio_source = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        audio_header = (ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
        policy = (ROOT / "main/protocols/listening_start_policy.h").read_text(encoding="utf-8")

        self.assertIn('#include "listening_start_policy.h"', application)
        self.assertIn("EvaluateListeningStartResult", application)
        self.assertIn("ListeningStartDisposition::RecoverOrdinary", policy)
        self.assertIn("ListeningStartDisposition::AbortStroke", policy)
        self.assertIn("ListeningStartDisposition::AbandonFencedStroke", policy)
        self.assertIn("bool EnableVoiceProcessing(bool enable);", audio_header)
        self.assertIn("bool AudioService::EnableVoiceProcessing(bool enable)", audio_source)
        self.assertIn("return IsAudioProcessorRunning();", audio_source)
        self.assertIn("audio_engine_->IsVoiceProcessingEnabled()", audio_source)
        self.assertIn("void RecoverOrdinaryListeningStartFailure();", application_header)

        start_audio = application[
            application.index("void Application::StartListeningAudio()") : application.index(
                "void Application::ConfigureWakeWordForListening()"
            )
        ]
        self.assertLess(start_audio.index("HasCancelFence"), start_audio.index("SendStartListening"))
        self.assertLess(start_audio.index("SendStartListening"), start_audio.index("EnableVoiceProcessing(true)"))
        self.assertLess(start_audio.index("EnableVoiceProcessing(true)"), start_audio.index("IsAudioProcessorRunning"))
        self.assertLess(start_audio.index("IsAudioProcessorRunning"), start_audio.index("MarkListeningStarted"))
        self.assertLess(start_audio.index("MarkListeningStarted"), start_audio.index("ShowListeningFromMain"))

        wake = application[
            application.index("void Application::HandleWakeWordDetectedEvent()") : application.index(
                "void Application::BeginWakeWordInvoke"
            )
        ]
        failed_send = wake[wake.index("SendStartListening") :]
        self.assertIn("RecoverOrdinaryListeningStartFailure", failed_send)
        self.assertLess(failed_send.index("RecoverOrdinaryListeningStartFailure"), failed_send.index("ResetDecoder"))

    def test_audio_stream_generation_rejects_stale_inflight_work(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "audio_stream_generation_harness"
            _compile_audio_generation_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("audio_stream_generation_harness: PASS", run.stdout)

    def test_audio_processor_postcondition_binds_engine_and_service_event(self):
        audio_source = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        audio_header = (ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
        helper = (ROOT / "main/audio/audio_processor_postcondition.h").read_text(encoding="utf-8")
        harness = (ROOT / "scripts/tests/audio_processor_postcondition_harness.cc").read_text(
            encoding="utf-8"
        )

        self.assertIn('#include "audio_processor_postcondition.h"', audio_source)
        self.assertIn("bool IsAudioProcessorRunning() const;", audio_header)
        self.assertNotIn("xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING);", audio_header)
        self.assertIn("AudioProcessorIsRunning", helper)
        self.assertIn("VoiceProcessingEnableMayPublishEvent", helper)
        self.assertIn("VoiceProcessingDisableKeepServiceEvent", helper)
        self.assertIn("VoiceProcessingDisableSucceeded", helper)
        self.assertIn("IsVoiceProcessingEnabled()", helper)

        running = audio_source[
            audio_source.index("bool AudioService::IsAudioProcessorRunning()") : audio_source.index(
                "bool AudioService::EnableVoiceProcessing"
            )
        ]
        self.assertIn("audio_engine_.get()", running)
        self.assertIn("engine->IsVoiceProcessingEnabled()", running)
        self.assertIn("AudioProcessorIsRunning", running)
        self.assertGreater(
            running.rfind("service_stopped_.load()"),
            running.find("engine->IsVoiceProcessingEnabled()"),
            "Stop must be observed after engine/event snapshots",
        )

        enable = audio_source[
            audio_source.index("bool AudioService::EnableVoiceProcessing") : audio_source.index(
                "void AudioService::EnableAudioTesting"
            )
        ]
        disable = enable[: enable.index("ResetDecoder()")]
        enable_true = enable[enable.index("audio_engine_->EnableVoiceProcessing(true)") :]
        self.assertIn("audio_engine_->IsVoiceProcessingEnabled()", disable)
        self.assertIn("VoiceProcessingDisableKeepServiceEvent", disable)
        self.assertIn("VoiceProcessingDisableSucceeded", disable)
        self.assertLess(
            disable.find("EnableVoiceProcessing(false)"),
            disable.find("IsVoiceProcessingEnabled()"),
        )
        self.assertIn("audio_engine_->IsVoiceProcessingEnabled()", enable_true)
        self.assertIn("VoiceProcessingEnableMayPublishEvent", enable_true)
        self.assertLess(
            enable_true.find("IsVoiceProcessingEnabled()"),
            enable_true.find("xEventGroupSetBits"),
        )
        self.assertLess(
            enable_true.find("IsVoiceProcessingEnabled()"),
            enable_true.find("return IsAudioProcessorRunning();"),
        )
        self.assertGreater(
            enable.count("audio_engine_->IsVoiceProcessingEnabled()"),
            1,
            "enable and disable must both query the real engine",
        )

        self.assertIn("IsVoiceProcessingEnabled()", harness)
        self.assertIn("engine false + attempted service event is not running", harness)
        self.assertIn("engine true but service event false is not running", harness)
        self.assertIn("stopped service is not running", harness)
        self.assertIn("disable returns false when the engine stays enabled", harness)

        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "audio_processor_postcondition_harness"
            _compile_audio_processor_postcondition_harness(executable)
            run = subprocess.run(
                [str(executable)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("audio_processor_postcondition_harness: PASS", run.stdout)


if __name__ == "__main__":
    unittest.main()
