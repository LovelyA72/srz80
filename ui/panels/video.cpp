#include "../gui.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace srz80::ui {
namespace {

uint32_t scaled_dimension(uint32_t dimension, float scale) {
    const auto scaled = static_cast<double>(dimension) * static_cast<double>(scale);
    const auto rounded = std::max<double>(1.0, std::round(scaled));
    return static_cast<uint32_t>(std::min(rounded,
                                           static_cast<double>(std::numeric_limits<int>::max())));
}

float cosine_fraction(float fraction) {
    return (1.0f - std::cos(fraction * 3.14159265358979323846f)) * 0.5f;
}

std::vector<uint8_t> cosine_scale(const std::vector<uint8_t> &source,
                                  uint32_t source_width, uint32_t source_height,
                                  uint32_t target_width, uint32_t target_height) {
    std::vector<uint8_t> output(static_cast<size_t>(target_width) * target_height * 4u);
    const auto sample = [&](int x, int y, int channel) -> float {
        x = std::clamp(x, 0, static_cast<int>(source_width) - 1);
        y = std::clamp(y, 0, static_cast<int>(source_height) - 1);
        return static_cast<float>(source[(static_cast<size_t>(y) * source_width +
                                          static_cast<size_t>(x)) * 4u +
                                         static_cast<size_t>(channel)]);
    };

    for (uint32_t y = 0; y < target_height; ++y) {
        const float source_y = (static_cast<float>(y) + 0.5f) * source_height /
                               static_cast<float>(target_height) - 0.5f;
        const int y0 = static_cast<int>(std::floor(source_y));
        const float y_fraction = cosine_fraction(source_y - static_cast<float>(y0));
        for (uint32_t x = 0; x < target_width; ++x) {
            const float source_x = (static_cast<float>(x) + 0.5f) * source_width /
                                   static_cast<float>(target_width) - 0.5f;
            const int x0 = static_cast<int>(std::floor(source_x));
            const float x_fraction = cosine_fraction(source_x - static_cast<float>(x0));
            const size_t output_offset = (static_cast<size_t>(y) * target_width + x) * 4u;
            for (int channel = 0; channel < 4; ++channel) {
                const float top = sample(x0, y0, channel) * (1.0f - x_fraction) +
                                  sample(x0 + 1, y0, channel) * x_fraction;
                const float bottom = sample(x0, y0 + 1, channel) * (1.0f - x_fraction) +
                                     sample(x0 + 1, y0 + 1, channel) * x_fraction;
                output[output_offset + static_cast<size_t>(channel)] = static_cast<uint8_t>(
                    std::clamp(std::lround(top * (1.0f - y_fraction) + bottom * y_fraction),
                               0L, 255L));
            }
        }
    }
    return output;
}

void video_shader_callback(const ImDrawList *, const ImDrawCmd *command) {
    auto *data = static_cast<VideoShaderDrawData *>(command->UserCallbackData);
    if (!data || !data->renderer || !data->state || !data->source_texture ||
        !data->output_texture) return;
    auto *previous_target = SDL_GetRenderTarget(data->renderer);
    if (!SDL_SetRenderTarget(data->renderer, data->output_texture)) return;
    SDL_SetGPURenderState(data->renderer, data->state);
    SDL_SetGPURenderStateFragmentUniforms(data->state, 0, &data->uniforms, sizeof(data->uniforms));
    SDL_RenderTexture(data->renderer, data->source_texture, nullptr, nullptr);
    SDL_SetGPURenderState(data->renderer, nullptr);
    SDL_SetRenderTarget(data->renderer, previous_target);
}

bool upload_shader_texture(SDL_GPUDevice *device, const VideoShaderProgram::TextureAsset &asset,
                           SDL_GPUTexture *&texture, std::string &error) {
    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = asset.width;
    texture_info.height = asset.height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    texture = SDL_CreateGPUTexture(device, &texture_info);
    if (!texture) { error = std::string("cannot create shader texture: ") + SDL_GetError(); return false; }

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<uint32_t>(asset.pixels.size());
    auto *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (!transfer) { error = std::string("cannot create shader texture upload: ") + SDL_GetError(); return false; }
    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (!mapped) {
        error = std::string("cannot map shader texture upload: ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }
    std::memcpy(mapped, asset.pixels.data(), asset.pixels.size());
    SDL_UnmapGPUTransferBuffer(device, transfer);
    auto *commands = SDL_AcquireGPUCommandBuffer(device);
    auto *copy = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
    if (!copy) {
        error = std::string("cannot begin shader texture upload: ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }
    SDL_GPUTextureTransferInfo source{transfer, 0, asset.width, asset.height};
    SDL_GPUTextureRegion destination{texture, 0, 0, 0, 0, 0,
                                     asset.width, asset.height, 1};
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    if (!submitted) { error = std::string("cannot submit shader texture upload: ") + SDL_GetError(); return false; }
    return true;
}

} // namespace

void App::destroy_video_shader() {
    if (video_gpu_state) SDL_DestroyGPURenderState(video_gpu_state);
    video_gpu_state = nullptr;
    if (renderer) {
        if (auto *device = SDL_GetGPURendererDevice(renderer)) {
            for (auto *sampler : video_shader_samplers) SDL_ReleaseGPUSampler(device, sampler);
            for (auto *texture : video_shader_textures) SDL_ReleaseGPUTexture(device, texture);
        }
    }
    video_shader_samplers.clear();
    video_shader_textures.clear();
    if (video_gpu_shader && renderer) {
        if (auto *device = SDL_GetGPURendererDevice(renderer))
            SDL_ReleaseGPUShader(device, video_gpu_shader);
    }
    video_gpu_shader = nullptr;
    video_shader_program = {};
}

void App::reload_video_shader() {
    if (video_shader_loaded_path == video_shader_path &&
        (video_shader_path.empty() || video_gpu_state || !video_shader_error.empty())) return;
    video_shader_loaded_path = video_shader_path;
    video_shader_error.clear();
    destroy_video_shader();
    if (video_shader_path.empty() || !renderer) return;
    auto *device = SDL_GetGPURendererDevice(renderer);
    if (!device) {
        video_shader_error = "custom shaders require the SDL GPU renderer; select 'gpu' and restart";
        return;
    }
    const auto description = std::filesystem::path(video_shader_path).is_absolute()
                                 ? std::filesystem::path(video_shader_path)
                                 : base / video_shader_path;
    if (!video_shader_program.load(description, SDL_GetGPUShaderFormats(device), video_shader_error)) return;
    SDL_GPUShaderCreateInfo info{};
    info.code_size = video_shader_program.code.size();
    info.code = video_shader_program.code.data();
    info.entrypoint = video_shader_program.entrypoint.c_str();
    info.format = video_shader_program.format;
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = video_shader_program.sampler_count;
    info.num_uniform_buffers = video_shader_program.uniform_buffer_count;
    video_gpu_shader = SDL_CreateGPUShader(device, &info);
    if (!video_gpu_shader) {
        video_shader_error = std::string("cannot create shader: ") + SDL_GetError();
        destroy_video_shader();
        return;
    }
    std::vector<SDL_GPUTextureSamplerBinding> bindings;
    for (const auto &asset : video_shader_program.textures) {
        SDL_GPUTexture *texture = nullptr;
        if (!upload_shader_texture(device, asset, texture, video_shader_error)) {
            if (texture) SDL_ReleaseGPUTexture(device, texture);
            destroy_video_shader();
            return;
        }
        SDL_GPUSamplerCreateInfo sampler_info{};
        sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        auto *sampler = SDL_CreateGPUSampler(device, &sampler_info);
        if (!sampler) {
            SDL_ReleaseGPUTexture(device, texture);
            video_shader_error = std::string("cannot create shader sampler: ") + SDL_GetError();
            destroy_video_shader();
            return;
        }
        video_shader_textures.push_back(texture);
        video_shader_samplers.push_back(sampler);
        bindings.push_back({texture, sampler});
    }
    SDL_GPURenderStateCreateInfo state_info{};
    state_info.fragment_shader = video_gpu_shader;
    state_info.num_sampler_bindings = static_cast<int>(bindings.size());
    state_info.sampler_bindings = bindings.data();
    video_gpu_state = SDL_CreateGPURenderState(renderer, &state_info);
    if (!video_gpu_state) {
        video_shader_error = std::string("cannot create shader render state: ") + SDL_GetError();
        destroy_video_shader();
    }
}

void App::video() {
    reload_video_shader();
    if (!ImGui::Begin("Video")) {
        ImGui::End();
        return;
    }
    if (!host_input.error().empty()) ImGui::TextDisabled("%s", host_input.error().c_str());
    if (!snapshot) {
        ImGui::End();
        return;
    }
    auto surfaces = snapshot->inspection->video_surfaces;
    std::set<srz80::Handle> active;
    for (const auto &surface : surfaces)
        active.insert(surface.id);
    for (auto it = video_textures.begin(); it != video_textures.end();) {
        if (!active.contains(it->first)) {
            if (it->second.texture)
                SDL_DestroyTexture(it->second.texture);
            if (it->second.cosine_texture)
                SDL_DestroyTexture(it->second.cosine_texture);
            if (it->second.shader_texture)
                SDL_DestroyTexture(it->second.shader_texture);
            it = video_textures.erase(it);
        } else {
            ++it;
        }
    }
    if (surfaces.empty())
        ImGui::TextDisabled("No active video surfaces.");
    for (const auto &surface : surfaces) {
        ImGui::PushID(static_cast<int>(surface.id));
        ImGui::Text("Card %llu  Surface %llu  %ux%u", static_cast<unsigned long long>(surface.owner),
                    static_cast<unsigned long long>(surface.id), surface.width, surface.height);
        if (host_input.owns(surface.id)) {
            ImGui::SameLine();
            ImGui::TextDisabled("Captured · Ctrl+Alt to release");
        }
        auto &entry = video_textures[surface.id];
        const uint32_t ordinal = [&]() {
            uint32_t value = 0;
            for (const auto &prior : surfaces) {
                if (prior.id == surface.id) break;
                if (prior.owner == surface.owner) ++value;
            }
            return value;
        }();
        const auto found = snapshot->video->video_frames.find(surface.id);
        const bool has_scanout = found != snapshot->video->video_frames.end() && found->second.has_scanout;
        const bool shader_allowed = (surface.flags & SRH_VIDEO_ALLOW_SHADER) != 0;
        if (!entry.shader_state_initialized || entry.surface_ordinal != ordinal ||
            entry.width != surface.width || entry.height != surface.height) {
            entry.surface_ordinal = ordinal;
            entry.shader_state_initialized = true;
            entry.shader_enabled = shader_allowed &&
                                   project_session.video_shader_enabled(surface.owner, ordinal);
        }
        if (!shader_allowed) entry.shader_enabled = false;
        if (shader_allowed) {
            ImGui::BeginDisabled(!has_scanout);
            bool enabled = entry.shader_enabled;
            if (ImGui::Checkbox("Shader", &enabled)) {
                entry.shader_enabled = enabled;
                try {
                    project_session.commit_video_shader(project_session.generation(), surface.owner,
                                                        ordinal, enabled);
                } catch (const std::exception &exception) { error = exception.what(); }
            }
            ImGui::EndDisabled();
            if (!has_scanout) ImGui::TextDisabled("Shader requires card scanout timing");
        } else {
            ImGui::TextDisabled("Shader not permitted by plugin");
        }
        if (!video_shader_path.empty() && !video_gpu_state && !video_shader_error.empty())
            ImGui::TextDisabled("Shader unavailable: %s", video_shader_error.c_str());
        if ((!entry.texture || entry.width != surface.width || entry.height != surface.height) &&
            renderer) {
            if (entry.texture)
                SDL_DestroyTexture(entry.texture);
            if (entry.cosine_texture)
                SDL_DestroyTexture(entry.cosine_texture);
            if (entry.shader_texture)
                SDL_DestroyTexture(entry.shader_texture);
            entry.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              static_cast<int>(surface.width),
                                              static_cast<int>(surface.height));
            entry.width = surface.width;
            entry.height = surface.height;
            entry.cosine_width = 0;
            entry.cosine_height = 0;
            entry.shader_width = 0;
            entry.shader_height = 0;
            entry.bytes = static_cast<size_t>(surface.width) * surface.height * 4u;
            entry.publication = 0;
            entry.cosine_publication = 0;
            entry.cosine_texture = nullptr;
            entry.shader_texture = nullptr;
        }

        if (found != snapshot->video->video_frames.end() && found->second.rgba &&
            found->second.status == SRH_OK && found->second.total == entry.bytes) {
            const bool uploaded = entry.texture && (entry.publication == snapshot->video->sequence ||
                SDL_UpdateTexture(entry.texture, nullptr, found->second.rgba->data(),
                                  static_cast<int>(surface.width * 4u)));
            if (uploaded) {
                entry.publication = snapshot->video->sequence;
                const uint32_t target_width = scaled_dimension(surface.width, video_scale);
                const uint32_t target_height = scaled_dimension(surface.height, video_scale);
                const uint32_t shader_groups = std::max(
                    1u, surface.width / video_shader_program.input_chunk +
                            (surface.width % video_shader_program.input_chunk != 0u));
                const uint32_t shader_width =
                    shader_groups * video_shader_program.output_chunk;
                const uint32_t shader_input_width = surface.width;
                const bool shader_active = entry.shader_enabled && has_scanout && video_gpu_state;
                const bool cosine = video_scale_filter == "Cosine" && !shader_active;
                SDL_Texture *display_texture = entry.texture;
                bool image_drawn = false;
                if (cosine) {
                    if (!entry.cosine_texture || entry.cosine_width != target_width ||
                        entry.cosine_height != target_height) {
                        if (entry.cosine_texture)
                            SDL_DestroyTexture(entry.cosine_texture);
                        entry.cosine_texture = SDL_CreateTexture(
                            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                            static_cast<int>(target_width), static_cast<int>(target_height));
                        entry.cosine_width = target_width;
                        entry.cosine_height = target_height;
                        entry.cosine_publication = 0;
                        if (entry.cosine_texture)
                            SDL_SetTextureScaleMode(entry.cosine_texture, SDL_SCALEMODE_NEAREST);
                    }
                    if (entry.cosine_texture && entry.cosine_publication != snapshot->video->sequence) {
                        const auto scaled = cosine_scale(*found->second.rgba, surface.width, surface.height,
                                                         target_width, target_height);
                        if (SDL_UpdateTexture(entry.cosine_texture, nullptr, scaled.data(),
                                              static_cast<int>(target_width * 4u)))
                            entry.cosine_publication = snapshot->video->sequence;
                    }
                    display_texture = entry.cosine_texture;
                } else if (entry.texture) {
                    SDL_SetTextureScaleMode(entry.texture, video_scale_filter == "Linear"
                                                         ? SDL_SCALEMODE_LINEAR
                                                         : SDL_SCALEMODE_NEAREST);
                }
                if (display_texture && shader_active) {
                    if (!entry.shader_texture || entry.shader_width != shader_width ||
                        entry.shader_height != surface.height) {
                        if (entry.shader_texture) SDL_DestroyTexture(entry.shader_texture);
                        entry.shader_texture = SDL_CreateTexture(
                            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                            static_cast<int>(shader_width), static_cast<int>(surface.height));
                        entry.shader_width = shader_width;
                        entry.shader_height = surface.height;
                        if (entry.shader_texture)
                            SDL_SetTextureScaleMode(entry.shader_texture, SDL_SCALEMODE_LINEAR);
                    }
                    entry.shader_draw.renderer = renderer;
                    entry.shader_draw.state = video_gpu_state;
                    entry.shader_draw.source_texture = display_texture;
                    entry.shader_draw.output_texture = entry.shader_texture;
                    entry.shader_draw.uniforms = video_shader_uniforms(
                        surface.width, surface.height, shader_input_width, found->second);
                    const auto position = ImGui::GetCursorScreenPos();
                    const auto size = ImVec2(static_cast<float>(target_width),
                                             static_cast<float>(target_height));
                    auto *draw_list = ImGui::GetWindowDrawList();
                    if (entry.shader_texture) {
                        draw_list->AddCallback(video_shader_callback, &entry.shader_draw);
                        draw_list->AddImage(
                            ImTextureID(reinterpret_cast<uintptr_t>(entry.shader_texture)), position,
                            ImVec2(position.x + size.x, position.y + size.y));
                        ImGui::Dummy(size);
                        image_drawn = true;
                    } else {
                        ImGui::TextDisabled("Shader output texture creation failed.");
                    }
                } else if (display_texture) {
                    ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(display_texture)),
                                 ImVec2(static_cast<float>(target_width),
                                        static_cast<float>(target_height)));
                    image_drawn = true;
                } else {
                    ImGui::TextDisabled("Video texture creation failed.");
                }
                if (image_drawn && ImGui::IsItemVisible()) {
                    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                    const float border = shader_active ? 0.0f : ImGui::GetStyle().ImageBorderSize;
                    const auto clip_min = ImGui::GetWindowDrawList()->GetClipRectMin();
                    const auto clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
                    const auto window_id = ImGui::GetCurrentWindow()->ID;
                    host_input.surface({surface.id, surface.owner, ordinal, surface.width, surface.height,
                        {min.x + border, min.y + border, max.x - min.x - 2 * border, max.y - min.y - 2 * border},
                        {clip_min.x, clip_min.y, clip_max.x - clip_min.x, clip_max.y - clip_min.y},
                        [window_id](float x, float y) {
                            ImGuiWindow *hovered = nullptr;
                            ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &hovered, nullptr);
                            return hovered && hovered->ID == window_id && !ImGui::GetTopMostPopupModal();
                        },
                        [window_id] {
                            ImGui::ClearActiveID();
                            ImGui::FocusWindow(ImGui::FindWindowByID(window_id));
                        }});
                }
            } else {
                ImGui::TextDisabled("Video texture update failed.");
            }
        } else {
            ImGui::TextDisabled("Video unavailable (status %d).",
                                found != snapshot->video->video_frames.end()
                                    ? static_cast<int>(found->second.status)
                                    : static_cast<int>(SRH_UNAVAILABLE));
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace srz80::ui
