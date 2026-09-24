#include "text_codec.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>

namespace srz80::ui {
namespace {
const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

std::string base64_encode(const std::string &input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t n = (static_cast<uint8_t>(input[i]) << 16) |
                     (static_cast<uint8_t>(input[i + 1]) << 8) |
                     static_cast<uint8_t>(input[i + 2]);
        output += base64_chars[(n >> 18) & 63];
        output += base64_chars[(n >> 12) & 63];
        output += base64_chars[(n >> 6) & 63];
        output += base64_chars[n & 63];
        i += 3;
    }
    size_t remaining = input.size() - i;
    if (remaining == 1) {
        uint32_t n = static_cast<uint8_t>(input[i]) << 16;
        output += base64_chars[(n >> 18) & 63];
        output += base64_chars[(n >> 12) & 63];
        output += '=';
        output += '=';
    } else if (remaining == 2) {
        uint32_t n = (static_cast<uint8_t>(input[i]) << 16) |
                     (static_cast<uint8_t>(input[i + 1]) << 8);
        output += base64_chars[(n >> 18) & 63];
        output += base64_chars[(n >> 12) & 63];
        output += base64_chars[(n >> 6) & 63];
        output += '=';
    }
    return output;
}

std::string base64_decode(const std::string &input) {
    int table[256];
    std::fill(std::begin(table), std::end(table), -1);
    for (int i = 0; i < 64; ++i)
        table[static_cast<uint8_t>(base64_chars[i])] = i;
    std::string output;
    int accumulator = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=')
            break;
        if (c == '\r' || c == '\n' || std::isspace(c))
            continue;
        int value = table[c];
        if (value < 0)
            continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output += static_cast<char>((accumulator >> bits) & 0xFF);
        }
    }
    return output;
}

} // namespace srz80::ui
