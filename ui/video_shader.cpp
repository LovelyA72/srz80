#include "video_shader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <utility>

namespace srz80::ui {
namespace {

struct Candidate { const char *key; SDL_GPUShaderFormat format; };
constexpr Candidate candidates[] = {
    {"spirv", SDL_GPU_SHADERFORMAT_SPIRV},
    {"dxil", SDL_GPU_SHADERFORMAT_DXIL},
    {"dxbc", SDL_GPU_SHADERFORMAT_DXBC},
    {"msl", SDL_GPU_SHADERFORMAT_MSL},
    {"metallib", SDL_GPU_SHADERFORMAT_METALLIB},
};

bool read_binary(const std::filesystem::path &path, std::vector<uint8_t> &out,
                 std::string &error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { error = "cannot open shader binary: " + path.string(); return false; }
    const auto end = stream.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(64 * 1024 * 1024)) {
        error = "shader binary is empty or too large: " + path.string(); return false;
    }
    out.resize(static_cast<size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()))) {
        error = "cannot read shader binary: " + path.string(); return false;
    }
    return true;
}

} // namespace

bool VideoShaderProgram::load(const std::filesystem::path &description,
                               SDL_GPUShaderFormat supported, std::string &error) {
    name.clear();
    format = SDL_GPU_SHADERFORMAT_INVALID;
    entrypoint = "main";
    sampler_count = 1;
    uniform_buffer_count = 0;
    input_chunk = 1;
    output_chunk = 1;
    code.clear();
    textures.clear();
    try {
        std::ifstream stream(description);
        if (!stream) { error = "cannot open shader description: " + description.string(); return false; }
        const auto json = nlohmann::json::parse(stream);
        if (json.value("format", std::string{}) != "srz80-video-shader" ||
            json.value("version", 0) != 1) {
            error = "unsupported shader description format or version";
            return false;
        }
        name = json.value("name", description.stem().string());
        const auto fragment = json.value("fragment", nlohmann::json::object());
        entrypoint = fragment.value("entrypoint", "main");
        sampler_count = fragment.value("samplers", 1u);
        uniform_buffer_count = fragment.value("uniform_buffers", 0u);
        if (sampler_count > 16 || uniform_buffer_count > 16 || entrypoint.empty()) {
            error = "invalid shader resource counts or entry point";
            return false;
        }
        const auto output = json.value("output", nlohmann::json::object());
        input_chunk = output.value("input_chunk", 1u);
        output_chunk = output.value("output_chunk", 1u);
        if (!input_chunk || !output_chunk || input_chunk > 1024 || output_chunk > 1024) {
            error = "invalid shader output chunk sizes";
            return false;
        }
        for (const auto &texture : json.value("textures", nlohmann::json::array())) {
            if (texture.value("format", std::string{}) != "rgba8_uint") {
                error = "unsupported shader texture format";
                return false;
            }
            TextureAsset asset;
            asset.width = texture.value("width", 0u);
            asset.height = texture.value("height", 0u);
            if (!asset.width || !asset.height || asset.width > 16384 || asset.height > 16384) {
                error = "invalid shader texture dimensions";
                return false;
            }
            const auto path = description.parent_path() / texture.value("path", std::string{});
            if (!read_binary(path, asset.pixels, error)) return false;
            const auto expected = static_cast<uint64_t>(asset.width) * asset.height * 4u;
            if (asset.pixels.size() != expected) {
                error = "shader texture size does not match its dimensions: " + path.string();
                return false;
            }
            textures.push_back(std::move(asset));
        }
        if (sampler_count != textures.size() + 1u) {
            error = "shader sampler count must include the source and all declared textures";
            return false;
        }
        for (const auto &candidate : candidates) {
            if (!(supported & candidate.format) || !fragment.contains(candidate.key) ||
                !fragment[candidate.key].is_string()) continue;
            const auto path = description.parent_path() / fragment[candidate.key].get<std::string>();
            if (!read_binary(path, code, error)) return false;
            format = candidate.format;
            return true;
        }
        error = "shader has no binary supported by this GPU renderer";
        return false;
    } catch (const std::exception &exception) {
        error = std::string("invalid shader description: ") + exception.what();
        return false;
    }
}

} // namespace srz80::ui
