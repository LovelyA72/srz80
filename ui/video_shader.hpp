#pragma once

#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace srz80::ui {

// A portable description points at backend-specific precompiled fragment
// binaries. The host never compiles or executes plugin-provided source code.
struct VideoShaderProgram {
    struct TextureAsset {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };

    std::string name;
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
    std::string entrypoint = "main";
    uint32_t sampler_count = 1;
    uint32_t uniform_buffer_count = 0;
    uint32_t input_chunk = 1;
    uint32_t output_chunk = 1;
    std::vector<TextureAsset> textures;
    std::vector<uint8_t> code;

    bool load(const std::filesystem::path &description, SDL_GPUShaderFormat supported,
              std::string &error);
    bool valid() const { return format != SDL_GPU_SHADERFORMAT_INVALID && !code.empty(); }
};

} // namespace srz80::ui
