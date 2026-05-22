#include "Analyzer.h"

#include "Infrastructure.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] constexpr int utf8_sequence_length(unsigned char lead) noexcept {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead >> 5) == 0x6) {
        return 2;
    }
    if ((lead >> 4) == 0xE) {
        return 3;
    }
    if ((lead >> 3) == 0x1E) {
        return 4;
    }
    return 0;
}

[[nodiscard]] constexpr bool is_utf8_continuation(unsigned char b) noexcept { return (b >> 6) == 0x2; }

enum class AsciiNorm : std::uint8_t { Sep = 0, Lower = 1, Keep = 2, Amp = 3 };

[[nodiscard]] const std::array<AsciiNorm, 256>& ascii_norm_table() {
    static const std::array<AsciiNorm, 256> table = []() {
        std::array<AsciiNorm, 256> t{};
        t.fill(AsciiNorm::Sep);
        for (unsigned c = static_cast<unsigned>('a'); c <= static_cast<unsigned>('z'); ++c) {
            t[c] = AsciiNorm::Lower;
        }
        for (unsigned c = static_cast<unsigned>('A'); c <= static_cast<unsigned>('Z'); ++c) {
            t[c] = AsciiNorm::Lower;
        }
        for (unsigned c = static_cast<unsigned>('0'); c <= static_cast<unsigned>('9'); ++c) {
            t[c] = AsciiNorm::Keep;
        }
        t[static_cast<unsigned>('_')] = AsciiNorm::Keep;
        t[static_cast<unsigned>('&')] = AsciiNorm::Amp;
        return t;
    }();
    return table;
}

void push_utf8_codepoint(std::vector<char>& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
        return;
    }
    if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | static_cast<char>((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>(cp & 0x3F)));
        return;
    }
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | static_cast<char>((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>(cp & 0x3F)));
        return;
    }
    if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | static_cast<char>((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | static_cast<char>(cp & 0x3F)));
    }
}

void push_space_if_needed(std::vector<char>& out, bool& prev_space) {
    if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
    }
}

[[nodiscard]] bool hex_digit_value(char c, unsigned& v) noexcept {
    if (c >= '0' && c <= '9') {
        v = static_cast<unsigned>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        v = static_cast<unsigned>(10 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        v = static_cast<unsigned>(10 + (c - 'A'));
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_numeric_entity(std::string_view in, char32_t& out_cp, std::size_t& consumed) noexcept {
    if (in.size() < 3 || in[0] != '&' || in[1] != '#') {
        return false;
    }
    std::size_t pos = 2;
    const bool hex = pos < in.size() && (in[pos] == 'x' || in[pos] == 'X');
    if (hex) {
        ++pos;
    }
    if (pos >= in.size()) {
        return false;
    }

    char32_t value = 0;
    unsigned digits = 0;
    constexpr unsigned max_digits = 8;

    if (hex) {
        while (pos < in.size() && digits < max_digits) {
            unsigned d = 0;
            if (!hex_digit_value(in[pos], d)) {
                break;
            }
            value = static_cast<char32_t>(value * 16u + d);
            ++pos;
            ++digits;
        }
    } else {
        while (pos < in.size() && digits < max_digits) {
            const char c = in[pos];
            if (c < '0' || c > '9') {
                break;
            }
            value = static_cast<char32_t>(value * 10u + static_cast<unsigned>(c - '0'));
            ++pos;
            ++digits;
        }
    }

    if (digits == 0 || pos >= in.size() || in[pos] != ';') {
        return false;
    }
    ++pos;

    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return false;
    }

    out_cp = value;
    consumed = pos;
    return true;
}

[[nodiscard]] bool parse_named_entity_fast(const char* p, std::size_t n, char32_t& out_cp,
                                           std::size_t& consumed) noexcept {
    if (n >= 5 && p[0] == '&' && p[1] == 'a' && p[2] == 'm' && p[3] == 'p' && p[4] == ';') {
        out_cp = '&';
        consumed = 5;
        return true;
    }
    if (n >= 4 && p[0] == '&' && p[1] == 'l' && p[2] == 't' && p[3] == ';') {
        out_cp = '<';
        consumed = 4;
        return true;
    }
    if (n >= 4 && p[0] == '&' && p[1] == 'g' && p[2] == 't' && p[3] == ';') {
        out_cp = '>';
        consumed = 4;
        return true;
    }
    if (n >= 6 && p[0] == '&' && p[1] == 'q' && p[2] == 'u' && p[3] == 'o' && p[4] == 't' && p[5] == ';') {
        out_cp = '"';
        consumed = 6;
        return true;
    }
    if (n >= 6 && p[0] == '&' && p[1] == 'a' && p[2] == 'p' && p[3] == 'o' && p[4] == 's' && p[5] == ';') {
        out_cp = '\'';
        consumed = 6;
        return true;
    }
    return false;
}

[[nodiscard]] bool decode_entity_prefix(const char* p, std::size_t n, std::vector<char>& out, bool& prev_space,
                                        std::size_t& consumed) noexcept {
    char32_t cp = 0;
    std::size_t used = 0;
    if (n == 0 || p[0] != '&') {
        consumed = 0;
        return false;
    }
    const std::string_view in(p, n);
    if (parse_numeric_entity(in, cp, used)) {
        push_utf8_codepoint(out, cp);
        prev_space = false;
        consumed = used;
        return true;
    }
    if (parse_named_entity_fast(p, n, cp, used)) {
        push_utf8_codepoint(out, cp);
        prev_space = false;
        consumed = used;
        return true;
    }
    consumed = 0;
    return false;
}

[[nodiscard]] std::vector<std::string_view> split_on_spaces(std::string_view stored) {
    std::vector<std::string_view> tokens;
    const char* base = stored.data();
    const std::size_t n = stored.size();
    std::size_t i = 0;
    while (i < n) {
        while (i < n && base[i] == ' ') {
            ++i;
        }
        if (i >= n) {
            break;
        }
        const std::size_t start = i;
        while (i < n && base[i] != ' ') {
            ++i;
        }
        tokens.emplace_back(base + start, i - start);
    }
    return tokens;
}

void append_ascii_byte_fast(std::vector<char>& out, unsigned char uc, bool& prev_space) {
    const AsciiNorm kind = ascii_norm_table()[uc];
    if (kind == AsciiNorm::Lower) {
        const unsigned char lower = (uc >= static_cast<unsigned char>('A') && uc <= static_cast<unsigned char>('Z'))
                                        ? static_cast<unsigned char>(uc | 0x20u)
                                        : uc;
        out.push_back(static_cast<char>(lower));
        prev_space = false;
        return;
    }
    if (kind == AsciiNorm::Keep) {
        out.push_back(static_cast<char>(uc));
        prev_space = false;
        return;
    }
    push_space_if_needed(out, prev_space);
}

void append_utf8_raw(std::string_view raw, std::size_t& i, std::vector<char>& out, bool& prev_space) {
    const unsigned char lead = static_cast<unsigned char>(raw[i]);
    const int len = utf8_sequence_length(lead);
    if (len <= 1 || i + static_cast<std::size_t>(len) > raw.size()) {
        ++i;
        return;
    }
    for (int k = 1; k < len; ++k) {
        if (!is_utf8_continuation(static_cast<unsigned char>(raw[i + static_cast<std::size_t>(k)]))) {
            ++i;
            return;
        }
    }
    for (int k = 0; k < len; ++k) {
        out.push_back(raw[i]);
        ++i;
    }
    prev_space = false;
}

} // namespace

std::vector<std::string_view> Analyzer::normalize_and_tokenize(std::string_view raw, StringArena& arena) {
    thread_local std::vector<char> normalized;
    normalized.clear();
    normalized.reserve(raw.size() + 32);
    bool prev_space = true;

    std::size_t i = 0;
    while (i < raw.size()) {
        const unsigned char uc = static_cast<unsigned char>(raw[i]);
        if (uc < 128) {
            const AsciiNorm kind = ascii_norm_table()[uc];
            if (kind == AsciiNorm::Amp) {
                std::size_t consumed = 0;
                if (decode_entity_prefix(raw.data() + i, raw.size() - i, normalized, prev_space, consumed)) {
                    i += consumed;
                    continue;
                }
                push_space_if_needed(normalized, prev_space);
                ++i;
                continue;
            }
            append_ascii_byte_fast(normalized, uc, prev_space);
            ++i;
            continue;
        }

        if (raw[i] == '&') {
            std::size_t consumed = 0;
            if (decode_entity_prefix(raw.data() + i, raw.size() - i, normalized, prev_space, consumed)) {
                i += consumed;
                continue;
            }
            push_space_if_needed(normalized, prev_space);
            ++i;
            continue;
        }

        append_utf8_raw(raw, i, normalized, prev_space);
    }

    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }

    if (normalized.empty()) {
        return {};
    }

    const std::string_view stored = arena.store(std::string_view(normalized.data(), normalized.size()));
    return split_on_spaces(stored);
}
