#include "rack_thumbnail.hpp"
#include "text_codec.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string_view>

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <miniz.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace srz80::ui {
namespace {

std::string encode_png(const std::vector<uint8_t> &rgba, int width, int height) {
    size_t png_size = 0;
    void *data = tdefl_write_image_to_png_file_in_memory(
        rgba.data(), width, height, 4, width * 4, &png_size);
    if (!data) return {};
    const std::string png(static_cast<const char *>(data), png_size);
    SDL_free(data);
    return "data:image/png;base64," + base64_encode(png);
}

std::vector<uint8_t> resize_rgba(const uint8_t *source, int source_width, int source_height,
                                 int width, int height) {
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const float sy = (y + 0.5f) * source_height / height - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, source_height - 1);
        const int y1 = std::min(y0 + 1, source_height - 1);
        const float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (int x = 0; x < width; ++x) {
            const float sx = (x + 0.5f) * source_width / width - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, source_width - 1);
            const int x1 = std::min(x0 + 1, source_width - 1);
            const float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
            for (int channel = 0; channel < 4; ++channel) {
                const auto sample = [&](int px, int py) {
                    return source[(static_cast<size_t>(py) * source_width + px) * 4 + channel];
                };
                const float top = sample(x0, y0) * (1.0f - fx) + sample(x1, y0) * fx;
                const float bottom = sample(x0, y1) * (1.0f - fx) + sample(x1, y1) * fx;
                result[(static_cast<size_t>(y) * width + x) * 4 + channel] =
                    static_cast<uint8_t>(std::lround(top * (1.0f - fy) + bottom * fy));
            }
        }
    }
    return result;
}

RackThumbnail decode_bytes(const uint8_t *bytes, size_t size, bool encode) {
    RackThumbnail result;
    int channels = 0, source_width = 0, source_height = 0;
    auto *source = size <= static_cast<size_t>(INT_MAX)
                       ? stbi_load_from_memory(bytes, static_cast<int>(size), &source_width,
                                               &source_height, &channels, 4)
                       : nullptr;
    if (!source || source_width <= 0 || source_height <= 0) {
        result.error = "Could not read image. Use a PNG, JPEG, or BMP file";
        stbi_image_free(source);
        return result;
    }
    const float scale = std::min(1.0f, 240.0f / std::max(source_width, source_height));
    result.width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
    result.height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
    result.pixels = resize_rgba(source, source_width, source_height, result.width, result.height);
    stbi_image_free(source);
    if (encode) {
        result.encoded = encode_png(result.pixels, result.width, result.height);
        if (result.encoded.empty()) result.error = "Could not encode image";
    }
    return result;
}

} // namespace

RackThumbnail load_rack_thumbnail(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {{}, 0, 0, {}, "Could not open image"};
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return decode_bytes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), true);
}

RackThumbnail decode_rack_thumbnail(const std::string &encoded) {
    constexpr std::string_view marker = ";base64,";
    const auto marker_at = encoded.find(marker);
    if (marker_at == std::string::npos) return {{}, 0, 0, {}, "Invalid thumbnail data"};
    const auto bytes = base64_decode(encoded.substr(marker_at + marker.size()));
    return decode_bytes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), false);
}

} // namespace srz80::ui
