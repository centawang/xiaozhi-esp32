#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Strict UTF-8 utterance parser for the local 笔划 prototype.
 * Counts Unicode code points, not UTF-8 bytes. Host-testable.
 */
struct StrokeOrderParse {
    enum class Status : uint8_t {
        Ok = 0,
        Empty,
        InvalidUtf8,
        TooLong,
        Ascii,
        Emoji,
        NoTarget,
        Ambiguous,
    };

    struct Result {
        Status status = Status::Empty;
        uint32_t codepoint = 0;
    };

    static constexpr size_t kMaxBytes = 96;
    static constexpr uint32_t kMaxCodepoints = 32;
    static constexpr uint32_t kMinCjk = 0x4E00;
    static constexpr uint32_t kMaxCjk = 0x9FFF;
    static constexpr uint32_t kZi = 0x5B57;   // 字
    static constexpr uint32_t kZen = 0x600E;  // 怎
    static constexpr uint32_t kMe = 0x4E48;   // 么
    static constexpr uint32_t kXie = 0x5199;  // 写
    static constexpr uint32_t kDe = 0x7684;   // 的

    static Result Parse(const char* data, size_t size);
    static Result Parse(const char* data);

private:
    static bool IsContinuation(uint8_t byte);
    static size_t DecodeOne(const uint8_t* data, size_t size, uint32_t* out);
    static bool IsWhitespace(uint32_t cp);
    static bool IsCjkPunctuation(uint32_t cp);
    static bool IsAsciiNonspace(uint32_t cp);
    static bool IsEmoji(uint32_t cp);
    static bool IsCjk(uint32_t cp);
    static bool PrefixContains(const uint32_t* cps, uint32_t count, uint32_t needle);
};

inline bool StrokeOrderParse::IsContinuation(uint8_t byte) { return (byte & 0xC0) == 0x80; }

inline size_t StrokeOrderParse::DecodeOne(const uint8_t* data, size_t size, uint32_t* out) {
    if (data == nullptr || size == 0 || out == nullptr) {
        return 0;
    }
    const uint8_t lead = data[0];
    if (lead < 0x80) {
        *out = lead;
        return 1;
    }
    if (lead < 0xC2) {
        return 0;
    }
    if (lead < 0xE0) {
        if (size < 2 || !IsContinuation(data[1])) {
            return 0;
        }
        const uint32_t cp =
            (static_cast<uint32_t>(lead & 0x1F) << 6) | static_cast<uint32_t>(data[1] & 0x3F);
        if (cp < 0x80) {
            return 0;
        }
        *out = cp;
        return 2;
    }
    if (lead < 0xF0) {
        if (size < 3 || !IsContinuation(data[1]) || !IsContinuation(data[2])) {
            return 0;
        }
        const uint32_t cp = (static_cast<uint32_t>(lead & 0x0F) << 12) |
                            (static_cast<uint32_t>(data[1] & 0x3F) << 6) |
                            static_cast<uint32_t>(data[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        *out = cp;
        return 3;
    }
    if (lead < 0xF5) {
        if (size < 4 || !IsContinuation(data[1]) || !IsContinuation(data[2]) ||
            !IsContinuation(data[3])) {
            return 0;
        }
        const uint32_t cp = (static_cast<uint32_t>(lead & 0x07) << 18) |
                            (static_cast<uint32_t>(data[1] & 0x3F) << 12) |
                            (static_cast<uint32_t>(data[2] & 0x3F) << 6) |
                            static_cast<uint32_t>(data[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) {
            return 0;
        }
        *out = cp;
        return 4;
    }
    return 0;
}

inline bool StrokeOrderParse::IsWhitespace(uint32_t cp) {
    return cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x20 || cp == 0xA0 || cp == 0x3000;
}

inline bool StrokeOrderParse::IsCjkPunctuation(uint32_t cp) {
    return cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF01 ||
           cp == 0xFF1F || cp == 0xFEFF;
}

inline bool StrokeOrderParse::IsAsciiNonspace(uint32_t cp) {
    return cp < 0x80 && !IsWhitespace(cp);
}

inline bool StrokeOrderParse::IsEmoji(uint32_t cp) {
    if (cp == 0x200D || cp == 0xFE0F) {
        return true;
    }
    if (cp >= 0x1F000 && cp <= 0x1FAFF) {
        return true;
    }
    if (cp >= 0x2600 && cp <= 0x27BF) {
        return true;
    }
    return false;
}

inline bool StrokeOrderParse::IsCjk(uint32_t cp) { return cp >= kMinCjk && cp <= kMaxCjk; }

inline bool StrokeOrderParse::PrefixContains(const uint32_t* cps, uint32_t count, uint32_t needle) {
    if (cps == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (cps[i] == needle) {
            return true;
        }
    }
    return false;
}

inline StrokeOrderParse::Result StrokeOrderParse::Parse(const char* data) {
    if (data == nullptr) {
        return Result{Status::Empty, 0};
    }
    size_t size = 0;
    while (data[size] != '\0') {
        if (size >= kMaxBytes) {
            return Result{Status::TooLong, 0};
        }
        size += 1;
    }
    return Parse(data, size);
}

inline StrokeOrderParse::Result StrokeOrderParse::Parse(const char* data, size_t size) {
    if (data == nullptr || size == 0) {
        return Result{Status::Empty, 0};
    }
    if (size > kMaxBytes) {
        return Result{Status::TooLong, 0};
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    uint32_t decoded[kMaxCodepoints] = {};
    uint32_t decoded_count = 0;
    size_t offset = 0;
    while (offset < size) {
        uint32_t cp = 0;
        const size_t used = DecodeOne(bytes + offset, size - offset, &cp);
        if (used == 0) {
            return Result{Status::InvalidUtf8, 0};
        }
        offset += used;
        if (IsWhitespace(cp) || IsCjkPunctuation(cp)) {
            continue;
        }
        if (decoded_count >= kMaxCodepoints) {
            return Result{Status::TooLong, 0};
        }
        decoded[decoded_count] = cp;
        decoded_count += 1;
    }

    if (decoded_count == 0) {
        return Result{Status::Empty, 0};
    }

    bool ascii = false;
    bool emoji = false;
    for (uint32_t i = 0; i < decoded_count; ++i) {
        if (IsAsciiNonspace(decoded[i])) {
            ascii = true;
        }
        if (IsEmoji(decoded[i])) {
            emoji = true;
        }
    }
    if (emoji) {
        return Result{Status::Emoji, 0};
    }
    if (ascii) {
        return Result{Status::Ascii, 0};
    }

    // 单字
    if (decoded_count == 1 && IsCjk(decoded[0])) {
        return Result{Status::Ok, decoded[0]};
    }
    // 某字
    if (decoded_count == 2 && IsCjk(decoded[0]) && decoded[1] == kZi) {
        return Result{Status::Ok, decoded[0]};
    }
    // 某怎么写
    if (decoded_count == 4 && IsCjk(decoded[0]) && decoded[1] == kZen && decoded[2] == kMe &&
        decoded[3] == kXie) {
        return Result{Status::Ok, decoded[0]};
    }
    // 某词的某: CJK+ 的 CJK, and the final character must appear in the prefix.
    if (decoded_count >= 3 && decoded[decoded_count - 2] == kDe &&
        IsCjk(decoded[decoded_count - 1])) {
        const uint32_t prefix_count = decoded_count - 2;
        bool prefix_all_cjk = prefix_count > 0;
        for (uint32_t i = 0; i < prefix_count; ++i) {
            if (!IsCjk(decoded[i])) {
                prefix_all_cjk = false;
                break;
            }
        }
        const uint32_t target = decoded[decoded_count - 1];
        if (prefix_all_cjk && PrefixContains(decoded, prefix_count, target)) {
            return Result{Status::Ok, target};
        }
        return Result{Status::NoTarget, 0};
    }

    uint32_t content_cjk = 0;
    for (uint32_t i = 0; i < decoded_count; ++i) {
        const uint32_t cp = decoded[i];
        if (IsCjk(cp) && cp != kZi && cp != kZen && cp != kMe && cp != kXie && cp != kDe) {
            content_cjk += 1;
        }
    }
    if (content_cjk >= 2) {
        return Result{Status::Ambiguous, 0};
    }
    return Result{Status::NoTarget, 0};
}
