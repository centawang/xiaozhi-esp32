#include "stroke_order/stroke_order_candidates.h"
#include "stroke_order/stroke_order_controller.h"
#include "stroke_order/stroke_order_parse.h"
#include "stroke_order/stroke_order_session.h"

#include <cstdint>
#include <cstring>
#include <fstream>
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

bool ParseIs(const char* text, StrokeOrderParse::Status status, uint32_t codepoint) {
    const auto result = StrokeOrderParse::Parse(text);
    return result.status == status && result.codepoint == codepoint;
}

bool ParseBytes(const uint8_t* data, size_t size, StrokeOrderParse::Status status,
                uint32_t codepoint) {
    const auto result = StrokeOrderParse::Parse(reinterpret_cast<const char*>(data), size);
    return result.status == status && result.codepoint == codepoint;
}

class ExtraProvider : public StrokeOrderCandidateProvider {
public:
    uint32_t AppendHomophones(uint32_t primary, uint32_t* out, uint32_t cap) const override {
        if (out == nullptr || cap == 0 || primary != 0x4E00) {
            return 0;
        }
        const uint32_t extras[] = {0x4E00, 0x4EBA, 0x53E3, 0x5929, 0x4EBA};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 5 && n < cap; ++i) {
            out[n++] = extras[i];
        }
        return n;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: stroke_order_session_harness <smoke.bin>\n";
        return 2;
    }
    const auto blob = ReadBinary(argv[1]);
    Expect(!blob.empty(), "read smoke blob");

    Expect(ParseIs(u8"一", StrokeOrderParse::Status::Ok, 0x4E00), "single 一");
    Expect(ParseIs(u8"  人  ", StrokeOrderParse::Status::Ok, 0x4EBA), "trimmed 人");
    Expect(ParseIs(u8"口字", StrokeOrderParse::Status::Ok, 0x53E3), "某字");
    Expect(ParseIs(u8"一怎么写", StrokeOrderParse::Status::Ok, 0x4E00), "某怎么写");
    Expect(ParseIs(u8"人口的口", StrokeOrderParse::Status::Ok, 0x53E3), "某词的某");
    Expect(ParseIs(u8"人口的人", StrokeOrderParse::Status::Ok, 0x4EBA), "某词的某 other char");
    Expect(ParseIs(u8"人口", StrokeOrderParse::Status::Ambiguous, 0), "multiple chars");
    Expect(ParseIs(u8"人口的一", StrokeOrderParse::Status::NoTarget, 0), "的-char not in word");
    Expect(ParseIs("", StrokeOrderParse::Status::Empty, 0), "empty");
    Expect(ParseIs("   ", StrokeOrderParse::Status::Empty, 0), "whitespace only");
    Expect(ParseIs("A", StrokeOrderParse::Status::Ascii, 0), "ascii");
    Expect(ParseIs(u8"一A", StrokeOrderParse::Status::Ascii, 0), "mixed ascii");
    Expect(ParseIs(u8"😀", StrokeOrderParse::Status::Emoji, 0), "emoji");
    Expect(ParseIs(u8"怎么写", StrokeOrderParse::Status::NoTarget, 0), "pattern without target");

    const uint8_t invalid[] = {0xFF, 0xFE};
    Expect(ParseBytes(invalid, sizeof(invalid), StrokeOrderParse::Status::InvalidUtf8, 0),
           "invalid utf-8");
    const uint8_t truncated[] = {0xE4, 0xB8};
    Expect(ParseBytes(truncated, sizeof(truncated), StrokeOrderParse::Status::InvalidUtf8, 0),
           "truncated utf-8");
    const uint8_t overlong[] = {0xC0, 0x80};
    Expect(ParseBytes(overlong, sizeof(overlong), StrokeOrderParse::Status::InvalidUtf8, 0),
           "overlong utf-8");
    std::string too_long(StrokeOrderParse::kMaxBytes + 1, 'a');
    Expect(ParseIs(too_long.c_str(), StrokeOrderParse::Status::TooLong, 0), "overlong ascii bytes");
    std::string many_cjk;
    for (uint32_t i = 0; i < StrokeOrderParse::kMaxCodepoints + 1; ++i) {
        many_cjk += "\xE4\xB8\x80";
    }
    Expect(StrokeOrderParse::Parse(many_cjk.data(), many_cjk.size()).status ==
               StrokeOrderParse::Status::TooLong,
           "too many codepoints");

    StrokeOrderSession session;
    Expect(session.inactive(), "presentation session starts inactive");
    Expect(!session.AcceptStt(1), "inactive presentation rejects STT");
    Expect(!session.BeginConnecting(0), "connecting rejects generation 0");
    Expect(session.BeginConnecting(7), "presentation begins on Connect");
    Expect(session.generation() == 7 && session.connecting(), "Connect page before listen/start");
    Expect(!session.AcceptStt(7), "Connect page does not accept STT");
    Expect(!session.MarkListeningReady(8), "Speak promotion rejects wrong generation");
    Expect(session.MarkListeningReady(7), "listen/start promotes Connect to Speak");
    Expect(session.awaiting_speech(), "Speak page after listen/start");
    Expect(!session.MarkListeningReady(7), "Speak promotion is not idempotent");
    Expect(!session.AcceptStt(8), "presentation rejects wrong generation");
    Expect(session.AcceptStt(7), "presentation accepts matching generation");
    session.MarkCandidates();
    Expect(session.phase() == StrokeOrderVoicePhase::Candidates, "candidates phase");
    session.MarkLocalPlayback();
    Expect(session.phase() == StrokeOrderVoicePhase::LocalPlayback, "local playback phase");
    session.Cancel();
    Expect(session.inactive() && session.generation() == 0,
           "presentation cancel invalidates token");

    StrokeOrderController controller;
    Expect(controller.BindStore(blob.data(), blob.size()), "bind");
    Expect(controller.candidate_count() == 0, "bind does not list library");
    Expect(controller.Contains(0x4E00) && !controller.Contains(0x5929), "store contains 一 not 天");
    uint32_t bounded_inputs[StrokeOrderController::kMaxCandidateInputs + 1] = {};
    for (uint32_t& input : bounded_inputs) {
        input = 0x5929;
    }
    bounded_inputs[StrokeOrderController::kMaxCandidateInputs] = 0x4E00;
    Expect(
        !controller.SetCandidates(bounded_inputs, StrokeOrderController::kMaxCandidateInputs + 1),
        "public candidate scan ignores input beyond hard cap");
    Expect(controller.EnterConnecting(), "connecting ui");
    Expect(controller.state() == StrokeOrderUiState::Connecting, "connecting state");
    Expect(controller.IsExclusiveTouch(), "connecting exclusive");
    Expect(controller.EnterAwaitingSpeech(), "awaiting ui");
    Expect(controller.state() == StrokeOrderUiState::AwaitingSpeech, "awaiting state");
    Expect(controller.IsExclusiveTouch(), "awaiting exclusive");
    Expect(!controller.OpenCandidates(), "no candidates yet");

    ExtraProvider extras;
    Expect(controller.SetCandidatesFromPrimary(0x4E00, &extras), "primary plus extras");
    Expect(controller.candidate_count() == 3, "extras filtered to store chars, max 6");
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    uint32_t c2 = 0;
    Expect(controller.GetCandidate(0, &c0) && c0 == 0x4E00, "recognized first");
    Expect(controller.GetCandidate(1, &c1) && c1 == 0x4EBA, "extra 人");
    Expect(controller.GetCandidate(2, &c2) && c2 == 0x53E3, "extra 口");
    Expect(controller.OpenCandidates(), "open filtered candidates");
    Expect(controller.EnterNoMatch() && controller.state() == StrokeOrderUiState::NoMatch,
           "no match page");
    Expect(controller.EnterTimedOut() && controller.state() == StrokeOrderUiState::TimedOut,
           "timeout page");
    Expect(controller.EnterError() && controller.state() == StrokeOrderUiState::Error,
           "error page");
    Expect(controller.Exit() && controller.state() == StrokeOrderUiState::Hidden, "cleanup hidden");
    Expect(!controller.IsExclusiveTouch(), "exit releases exclusive touch");

    if (failures != 0) {
        std::cerr << "stroke_order_session_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "stroke_order_session_harness: PASS\n";
    return 0;
}
