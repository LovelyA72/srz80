#include "simulation_controller.hpp"
#include "input_time.hpp"
#include "simulation_engine.hpp"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace srz80::ui {

namespace {
// Releases a candidate that was not installed, including on an exception path.
struct CandidateGuard {
    SrzEngine *candidate;
    ~CandidateGuard() {
        if (candidate)
            srz80_engine_destroy(candidate);
    }
    SrzEngine *release() {
        SrzEngine *result = candidate;
        candidate = nullptr;
        return result;
    }
};

std::vector<SrzImagePart> read_image_parts(const std::vector<std::filesystem::path> &paths,
                                           std::vector<std::vector<uint8_t>> &storage) {
    storage.clear();
    storage.reserve(paths.size());
    for (const auto &path : paths) {
        storage.emplace_back();
        if (path.empty())
            continue;
        const auto size = std::filesystem::file_size(path);
        auto &bytes = storage.back();
        if (size > bytes.max_size() ||
            size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::length_error("Image file exceeds host addressable size: " + path.string());
        bytes.resize(static_cast<size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input || (!bytes.empty() &&
            !input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))))
            throw std::runtime_error("Cannot read ROM: " + path.string());
    }
    std::vector<SrzImagePart> parts;
    parts.reserve(storage.size());
    for (const auto &bytes : storage)
        parts.push_back({SRZ_INIT(SrzImagePart), bytes.data(), bytes.size()});
    return parts;
}

// Project mixer entries deliberately use card position/name/ordinal instead of
// engine handles: cards and sources receive fresh handles on every load.
void apply_project_mixer(SrzEngine *engine, SrzResult *result, const nlohmann::json &document,
                         const std::vector<ProjectCardAssociation> &cards) {
    const auto mixer = document.find("mixer");
    if (mixer == document.end() || !mixer->is_object()) return;
    if (const auto master = mixer->find("master_volume"); master != mixer->end() && master->is_number_unsigned())
        srz80_engine_audio_set_master_volume(engine, std::min(master->get<uint32_t>(), 100u));
    const auto entries = mixer->find("sources");
    if (entries == mixer->end() || !entries->is_array() ||
        srz80_engine_audio_sources(engine, result) != SRH_OK) return;
    uint32_t count = 0;
    const auto sources = srz80_engine_result_audio_sources(result, &count);
    std::map<std::pair<Handle, std::string>, std::vector<Handle>> by_owner_name;
    for (uint32_t i = 0; i < count; ++i)
        by_owner_name[{sources[i].owner, engine_text(sources[i].name)}].push_back(sources[i].id);
    for (const auto &entry : *entries) {
        if (!entry.is_object() || !entry.contains("card") || !entry["card"].is_number_unsigned() ||
            !entry.contains("name") || !entry["name"].is_string()) continue;
        const auto card = entry["card"].get<size_t>();
        const auto ordinal = entry.value("ordinal", 0u);
        if (card >= cards.size()) continue;
        const auto found = by_owner_name.find({cards[card].id, entry["name"].get<std::string>()});
        if (found == by_owner_name.end() || ordinal >= found->second.size()) continue;
        const auto source = found->second[ordinal];
        if (const auto volume = entry.find("volume"); volume != entry.end() && volume->is_number_unsigned())
            (void)srz80_engine_audio_set_source_volume(engine, source, std::min(volume->get<uint32_t>(), 150u));
        if (const auto pan = entry.find("pan"); pan != entry.end() && pan->is_number_integer())
            (void)srz80_engine_audio_set_source_pan(engine, source,
                pan->is_number_unsigned() ? static_cast<int32_t>(std::min(pan->get<uint64_t>(), uint64_t{63}))
                                         : static_cast<int32_t>(std::clamp(pan->get<int64_t>(), int64_t{-64}, int64_t{63})));
        if (const auto muted = entry.find("muted"); muted != entry.end() && muted->is_boolean())
            (void)srz80_engine_audio_set_source_muted(engine, source, muted->get<bool>() ? 1u : 0u);
    }
}
} // namespace

void SimulationController::process_command(Command &command) {
    if (!engine_)
        throw std::runtime_error("Simulation engine is unavailable");
    Reply reply;
    reply.seq = command.seq;
    reply.status = SRH_OK;
    if (project_scoped(command.payload) && command.generation != generation_) {
        reply.status = SRH_CONFLICT;
        reply.error = "Request belongs to an obsolete project generation";
        finish_command(command, std::move(reply));
        return;
    }
    try {
        std::visit([&](const auto &request) { execute(request, reply); }, command.payload);
    } catch (const std::exception &e) {
        reply.status = SRH_ERROR;
        reply.error = e.what();
    }
    std::visit([&](const auto &request) {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, LegacyCommand>) {
            invalidate(request, reply);
        } else if constexpr (std::is_same_v<T, EditProperty> ||
                             std::is_same_v<T, WriteMemory> || std::is_same_v<T, LoadMemory>) {
            if constexpr (std::is_same_v<T, EditProperty>)
                if (reply.status == SRH_OK) ++memory_revision_;
            force_video_publish_ = true;
            force_inspection_publish_ = true;
            force_control_publish_ = true;
        }
    }, command.payload);
    finish_command(command, std::move(reply));
}

void SimulationController::execute(const ProjectRuntimeRequest &request, Reply &reply) {
    using Kind = ProjectRuntimeRequest::Kind;
    if (request.kind == Kind::capture || request.kind == Kind::capture_state) {
        reply.project.document = nlohmann::json::object();
        if (srz80_engine_plugin_project_data(engine_, result_) != SRH_OK)
            throw std::runtime_error(engine_message());
        uint32_t count = 0;
        const SrzPluginData *chunks = srz80_engine_result_plugin_data(result_, &count);
        for (uint32_t index = 0; index < count; ++index)
            reply.project.document[std::to_string(chunks[index].owner)] = {
                {"encoding", "hex"}, {"data", engine_text(chunks[index].hex)}};
        if (request.kind == Kind::capture_state) {
            std::string execution;
            std::string error;
            if (!read_state(true, execution, error))
                throw std::runtime_error(error);
            reply.project.execution = std::move(execution);
        }
        return;
    }
    if (request.kind == Kind::reorder) {
        reply.status = srz80_engine_reorder_cards(
            engine_, request.source_cards.data(), static_cast<uint32_t>(request.source_cards.size()));
        if (reply.status == SRH_OK) {
            force_inspection_publish_ = true;
            force_control_publish_ = true;
        } else {
            reply.error = "Rack order no longer matches the active project";
        }
        return;
    }
    if (request.kind == Kind::replace || request.kind == Kind::restore_state) {
        auto document = request.document;
        if (!request.source_cards.empty()) {
            if (request.source_cards.size() != document.at("cards").size())
                throw std::runtime_error("Rebuild source associations do not match candidate cards");
            std::map<Handle, std::string> data;
            if (srz80_engine_plugin_project_data(engine_, result_) != SRH_OK)
                throw std::runtime_error(engine_message());
            uint32_t chunk_count = 0;
            const SrzPluginData *chunks = srz80_engine_result_plugin_data(result_, &chunk_count);
            for (uint32_t index = 0; index < chunk_count; ++index)
                data.emplace(chunks[index].owner, engine_text(chunks[index].hex));
            const auto current = copy_cards(true);
            for (size_t i = 0; i < request.source_cards.size(); ++i) {
                const auto id = request.source_cards[i];
                if (std::none_of(current.begin(), current.end(), [id](const auto &card) { return card.id == id; }))
                    throw std::runtime_error("Rebuild source card is no longer available");
                if (auto chunk = data.find(id); chunk != data.end())
                    document["cards"][i]["plugin_data"] = {{"encoding", "hex"}, {"data", chunk->second}};
            }
        }
        CandidateGuard candidate{srz80_engine_candidate_create(engine_)};
        if (!candidate.candidate)
            throw std::runtime_error("Cannot create a project candidate");
        const auto directory = engine_path_text(request.directory);
        const auto json = document.dump();
        if (srz80_engine_load_project_json(candidate.candidate, engine_slice(json),
                                           engine_slice(directory), SrzSlice{}, SRZ_STOPPED) != SRH_OK)
            throw std::runtime_error(engine_last_message(candidate.candidate));
        if (request.kind == Kind::restore_state) {
            if (srz80_engine_load_state(candidate.candidate, engine_slice(request.execution)) != SRH_OK)
                throw std::runtime_error(engine_last_message(candidate.candidate));
            srz80_engine_pause(candidate.candidate);
        }
        // Complete fallible result preparation before replacing the active rack.
        std::vector<ProjectCardAssociation> cards;
        if (!install_candidate(candidate.release(), &cards, reply.error)) {
            reply.status = SRH_ERROR;
            return;
        }
        reply.project.cards = std::move(cards);
        apply_project_mixer(engine_, result_, document, reply.project.cards);
        reply.project.document = std::move(document);
        reply.project.replaced = true;
        return;
    }
    if (request.kind == Kind::property) {
        execute(EditProperty{request.card, request.index, request.value}, reply);
        if (reply.status == SRH_OK) ++memory_revision_;
        force_inspection_publish_ = true;
        force_control_publish_ = true;
        return;
    }
    if (request.kind == Kind::card_metadata) {
        reply.status = srz80_engine_set_card_name(engine_, request.card, engine_slice(request.name));
        if (reply.status == SRH_OK)
            reply.status = srz80_engine_set_card_clock(engine_, request.card, request.index);
        if (reply.status != SRH_OK) reply.error = engine_message();
        force_inspection_publish_ = true;
        force_control_publish_ = true;
        return;
    }
    LegacyCommand command;
    command.handle1 = request.card;
    command.clock = request.index;
    command.hz = request.hz;
    switch (request.kind) {
    case Kind::insert: command.kind = CmdKind::AddCard; command.card = request.insertion; break;
    case Kind::park: command.kind = CmdKind::Park; break;
    case Kind::plug: command.kind = CmdKind::Plug; break;
    case Kind::put_away: command.kind = CmdKind::PutAway; break;
    case Kind::clock:
        if (request.index >= 3 || request.hz > 50000000)
            throw std::runtime_error("Clock must be 0..50 MHz with index 0..2");
        command.kind = CmdKind::Frequency; break;
    case Kind::stop_clocks: command.kind = CmdKind::StopClocks; break;
    default: throw std::runtime_error("Unsupported project command");
    }
    execute(command, reply);
    invalidate(command, reply);
    reply.project.card = reply.handle1;
}

void SimulationController::execute(const InputBatch &request, Reply &reply) {
    if (request.source) {
        const auto found = input_identities_.find(request.source);
        const auto identity = found == input_identities_.end() ? 0 : found->second;
        if (request.identity < identity || (!request.cancel && identity && request.identity != identity)) {
            reply.status = SRH_CONFLICT;
            return;
        }
        if (request.cancel) {
            srz80_engine_cancel_input_source(engine_, request.source);
            input_identities_[request.source] = request.identity;
            if (!request.release || request.bytes.empty()) return;
        }
    } else if (request.cancel) {
        reply.status = SRH_INVALID;
        return;
    }
    if (engine_stopped() && !request.release) { reply.status = SRH_CONFLICT; return; }
    const auto now = srz80_engine_now(engine_);
    uint64_t time = now;
    if (!normalize_input_time(request.timestamp, now, time)) { reply.status = SRH_INVALID; return; }
    reply.status = srz80_engine_enqueue_input_batch(
        engine_, engine_slice(request.endpoint), time, request.bytes.data(), request.bytes.size(),
        request.release ? (request.source ^ (uint64_t(1) << 62)) : request.source, request.expected_owner);
    if (reply.status == SRH_OK && request.source) input_identities_[request.source] = request.identity;
    force_provider_publish_ = true;
}
void SimulationController::execute(const LegacyCommand &command, Reply &reply) {
    switch (command.kind) {
    case CmdKind::PluginProjectData:
        if (srz80_engine_plugin_project_data(engine_, result_) != SRH_OK) {
            reply.status = SRH_ERROR;
            reply.error = engine_message();
            break;
        }
        {
            uint32_t count = 0;
            const SrzPluginData *chunks = srz80_engine_result_plugin_data(result_, &count);
            for (uint32_t index = 0; index < count; ++index)
                reply.json[std::to_string(chunks[index].owner)] = {
                    {"encoding", "hex"}, {"data", engine_text(chunks[index].hex)}};
        }
        break;
    case CmdKind::ProviderCommand:
        reply.status = srz80_engine_provider_command(engine_, command.handle1, command.clock,
                                                     command.value1, engine_slice(command.text1));
        if (reply.status != SRH_OK)
            reply.error = "Provider command rejected (stale revision, invalid data, or unsupported run state)";
        break;
    case CmdKind::Pause:
        srz80_engine_pause(engine_);
        break;
    case CmdKind::Resume:
        reply.status = srz80_engine_resume(engine_);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::Stop:
        reply.status = srz80_engine_stop(engine_);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        reset_audio();
        break;
    case CmdKind::Reset:
        reply.status = srz80_engine_reset(engine_, command.flag ? 1u : 0u);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        reset_audio();
        break;
    case CmdKind::Step:
        if (srz80_engine_step(engine_, command.clock) != SRH_OK)
            reply.status = SRH_UNAVAILABLE;
        break;
    case CmdKind::Frequency:
        reply.status = srz80_engine_frequency(engine_, command.clock, command.hz);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::StopClocks:
        reply.status = srz80_engine_stop_clocks(engine_);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::EnqueueInput:
        reply.status = srz80_engine_enqueue_input(engine_, engine_slice(command.text1),
                                                  command.value1, static_cast<uint8_t>(command.value2));
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::ClearTrace:
        srz80_engine_clear_trace(engine_);
        break;
    case CmdKind::SetTraceCapture:
        srz80_engine_set_trace_capture(engine_, command.flag ? 1u : 0u, command.hz);
        break;
    case CmdKind::ClearLogs:
        srz80_engine_clear_logs(engine_);
        break;
    case CmdKind::AddBreakpoint:
        srz80_engine_add_breakpoint(engine_, command.handle1, command.handle2, command.value1,
                                    command.value2, command.hz);
        break;
    case CmdKind::RemoveBreakpoint:
        reply.status = srz80_engine_remove_breakpoint(engine_, command.value1);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::SetBreakpointEnabled:
        reply.status = srz80_engine_set_breakpoint_enabled(engine_, command.value1,
                                                           command.flag ? 1u : 0u);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::AudioMasterVolume:
        srz80_engine_audio_set_master_volume(engine_, static_cast<uint32_t>(command.value1));
        break;
    case CmdKind::AudioSampleRate:
        reply.status = srz80_engine_audio_set_sample_rate(
            engine_, static_cast<uint32_t>(command.value1));
        if (reply.status == SRH_OK)
            reset_audio();
        else
            reply.error = engine_message();
        break;
    case CmdKind::AudioResampling:
        reply.status = srz80_engine_audio_set_resampling(
            engine_, static_cast<SrzAudioResampling>(command.value1));
        if (reply.status == SRH_OK)
            reset_audio();
        else
            reply.error = engine_message();
        break;
    case CmdKind::AudioSourceVolume:
        if (srz80_engine_audio_set_source_volume(engine_, command.handle1,
                                                 static_cast<uint32_t>(command.value1)) != SRH_OK)
            reply.status = SRH_NOT_FOUND;
        break;
    case CmdKind::AudioSourcePan:
        if (srz80_engine_audio_set_source_pan(engine_, command.handle1,
                                              static_cast<int32_t>(command.value1) - 64) != SRH_OK)
            reply.status = SRH_NOT_FOUND;
        break;
    case CmdKind::AudioSourceMuted:
        if (srz80_engine_audio_set_source_muted(engine_, command.handle1, command.flag ? 1u : 0u) != SRH_OK)
            reply.status = SRH_NOT_FOUND;
        break;
    case CmdKind::AudioSoftwareClipping:
        srz80_engine_audio_set_software_clipping(engine_, command.flag ? 1u : 0u);
        break;
    case CmdKind::AudioDcOffsetCorrection:
        srz80_engine_audio_set_dc_offset_correction(engine_, command.flag ? 1u : 0u);
        break;
    case CmdKind::AudioQueueCapacity:
        srz80_engine_audio_set_queue_capacity(engine_, command.value1);
        pcm_.set_capacity(std::max<uint64_t>(command.value1, 256));
        break;
    case CmdKind::Log:
        srz80_engine_log(engine_, engine_slice(command.text1));
        break;
    case CmdKind::LoadProjectJson: {
        CandidateGuard candidate{srz80_engine_candidate_create(engine_)};
        if (!candidate.candidate)
            throw std::runtime_error("Cannot create a project candidate");
        const auto directory = engine_path_text(command.path1);
        if (srz80_engine_load_project_json(candidate.candidate, engine_slice(command.text1),
                                           engine_slice(directory), SrzSlice{}, SRZ_STOPPED) != SRH_OK)
            throw std::runtime_error(engine_last_message(candidate.candidate));
        if (!install_candidate(candidate.release(), nullptr, reply.error)) {
            reply.status = SRH_ERROR;
            break;
        }
        break;
    }
    case CmdKind::NewProject: {
        CandidateGuard candidate{srz80_engine_candidate_create(engine_)};
        if (!candidate.candidate)
            throw std::runtime_error("Cannot create a project candidate");
        if (srz80_engine_new_project(candidate.candidate) != SRH_OK)
            throw std::runtime_error(engine_last_message(candidate.candidate));
        if (!install_candidate(candidate.release(), nullptr, reply.error)) {
            reply.status = SRH_ERROR;
            break;
        }
        break;
    }
    case CmdKind::RestoreState: {
        const auto project = nlohmann::json::parse(command.text1);
        const auto &cards = project.at("cards");
        if (!cards.is_array())
            throw std::runtime_error("State snapshot has no card list");
        const auto current_cards = copy_cards(true);
        bool same_structure = static_cast<bool>(engine_) && current_cards.size() == cards.size();
        if (same_structure) {
            size_t index = 0;
            for (const auto &card : cards) {
                if (index >= current_cards.size() ||
                    current_cards[index].type != card.value("plugin", std::string())) {
                    same_structure = false;
                    break;
                }
                ++index;
            }
        }
        if (same_structure) {
            reply.status = srz80_engine_load_state(engine_, engine_slice(command.text2));
            if (reply.status != SRH_OK)
                reply.error = engine_message();
        } else {
            CandidateGuard candidate{srz80_engine_candidate_create(engine_)};
            if (!candidate.candidate)
                throw std::runtime_error("Cannot create a project candidate");
            const auto directory = engine_path_text(command.path1);
            if (srz80_engine_load_project_json(candidate.candidate, engine_slice(command.text1),
                                               engine_slice(directory), SrzSlice{},
                                               SRZ_PAUSED) != SRH_OK)
                throw std::runtime_error(engine_last_message(candidate.candidate));
            if (srz80_engine_load_state(candidate.candidate, engine_slice(command.text2)) != SRH_OK)
                throw std::runtime_error(engine_last_message(candidate.candidate));
            if (!install_candidate(candidate.release(), nullptr, reply.error)) {
                reply.status = SRH_ERROR;
                break;
            }
        }
        if (command.flag)
            srz80_engine_pause(engine_);
        reset_audio();
        ++generation_;
        ++memory_revision_;
        apply_runtime_options();
        break;
    }
    case CmdKind::SaveState:
        if (!read_state(command.flag, reply.text1, reply.error))
            reply.status = SRH_ERROR;
        break;
    case CmdKind::Park:
        reply.status = srz80_engine_park(engine_, command.handle1);
        if (reply.status != SRH_OK)
            reply.error = "Could not park card " + std::to_string(command.handle1);
        break;
    case CmdKind::Plug:
        reply.status = srz80_engine_plug(engine_, command.handle1);
        if (reply.status != SRH_OK)
            reply.error = "Could not plug card " + std::to_string(command.handle1);
        break;
    case CmdKind::PutAway:
        reply.status = srz80_engine_put_away(engine_, command.handle1);
        if (reply.status != SRH_OK)
            reply.error = "Could not put away card " + std::to_string(command.handle1);
        break;
    case CmdKind::Remove:
        reply.status = srz80_engine_remove(engine_, command.handle1);
        if (reply.status != SRH_OK)
            reply.error = "Could not remove card " + std::to_string(command.handle1);
        break;
    case CmdKind::AddCard: {
        const auto &request = command.card.value();
        const auto &plugin_dir = request.plugin_directory.empty() ? plugin_directory_ : request.plugin_directory;
        SrzCardRequest abi{};
        abi.abi_version = SRZ80_ENGINE_ABI;
        abi.struct_size = static_cast<uint32_t>(sizeof(SrzCardRequest));
        abi.space = request.space;
        abi.base = request.base;
        abi.size = request.size;
        abi.reset_vector = request.reset_vector;
        abi.priority = request.priority;
        abi.clock = request.clock;
        abi.type = engine_slice(request.type);
        abi.config_json = engine_slice(request.config_json);
        const auto plugin_directory = engine_path_text(plugin_dir);
        abi.plugin_directory = engine_slice(plugin_directory);
        std::vector<std::vector<uint8_t>> image_storage;
        const auto parts = read_image_parts(request.image_paths, image_storage);
        abi.images = parts.empty() ? nullptr : parts.data();
        abi.image_count = static_cast<uint32_t>(parts.size());
        reply.status = srz80_engine_add_card(engine_, &abi, &reply.handle1);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    }
    case CmdKind::ConfigGet: {
        const auto fallback = command.text2;
        reply.text1 = engine_config_value(engine_, command.text1, fallback);
        break;
    }
    case CmdKind::ConfigEntryGet:
        reply.status = engine_config_entry(engine_, command.text1, reply.text1);
        break;
    case CmdKind::ConfigEntrySet:
        reply.status = srz80_engine_config_entry_set(engine_, engine_slice(command.text1),
                                                     engine_slice(command.text2));
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::ConfigSet:
        reply.status = srz80_engine_config_set(engine_, engine_slice(command.text1),
                                               engine_slice(command.text2));
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::ConfigLoad:
        reply.status = srz80_engine_load_config(engine_);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    case CmdKind::ConfigSave:
        reply.status = srz80_engine_save_config(engine_);
        if (reply.status != SRH_OK)
            reply.error = engine_message();
        break;
    }
}

void SimulationController::invalidate(const LegacyCommand &command, const Reply &reply) {
    // Apply publication policy once, after the complete command boundary.
    // Read-only requests don't force copies; mixer/clock changes need control
    // data only. Lifecycle and memory changes also invalidate video.
    switch (command.kind) {
    case CmdKind::Stop: case CmdKind::Reset: case CmdKind::Step:
    case CmdKind::LoadProjectJson: case CmdKind::NewProject: case CmdKind::RestoreState:
    case CmdKind::Park: case CmdKind::Plug: case CmdKind::PutAway: case CmdKind::Remove:
    case CmdKind::AddCard:
        if (reply.status == SRH_OK)
            ++memory_revision_;
        force_video_publish_ = true;
        force_inspection_publish_ = true;
        force_control_publish_ = true;
        break;
    case CmdKind::AddBreakpoint: case CmdKind::RemoveBreakpoint: case CmdKind::SetBreakpointEnabled:
    case CmdKind::ConfigEntrySet:
    case CmdKind::ClearTrace: case CmdKind::SetTraceCapture: case CmdKind::ClearLogs: case CmdKind::Log:
        force_inspection_publish_ = true;
        break;
    case CmdKind::Pause: case CmdKind::Resume: case CmdKind::Frequency: case CmdKind::StopClocks:
    case CmdKind::AudioMasterVolume: case CmdKind::AudioSampleRate: case CmdKind::AudioResampling:
    case CmdKind::AudioSourceVolume: case CmdKind::AudioSourcePan: case CmdKind::AudioSourceMuted:
    case CmdKind::AudioSoftwareClipping: case CmdKind::AudioDcOffsetCorrection: case CmdKind::AudioQueueCapacity:
        force_control_publish_ = true;
        break;
    case CmdKind::ProviderCommand:
        force_provider_publish_ = true;
        inspection_dirty_ = true;
        break;
    default:
        break;
    }
}

} // namespace srz80::ui
