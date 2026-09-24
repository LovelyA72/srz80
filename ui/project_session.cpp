#include "project_session.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace srz80::ui {
namespace {
auto find_card(auto &records, uint64_t id) {
    return std::find_if(records.begin(), records.end(),
                        [id](const auto &entry) { return entry.first == id; });
}
std::filesystem::path normalize(const std::filesystem::path &path) {
    return path.empty() ? path : std::filesystem::absolute(path).lexically_normal();
}

uint64_t card_number(const nlohmann::json &card, const char *key) {
    if (!card.contains(key)) return 0;
    const auto &value = card.at(key);
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0)
        return static_cast<uint64_t>(value.get<int64_t>());
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        size_t end = 0;
        const auto result = std::stoull(text, &end, 0);
        if (end == text.size()) return result;
    }
    throw std::invalid_argument(std::string("Invalid ") + key + " value");
}

std::vector<std::string> card_images(const nlohmann::json &card) {
    if (card.contains("image")) return {card.at("image").get<std::string>()};
    if (card.contains("images")) return card.at("images").get<std::vector<std::string>>();
    return {};
}

bool same_card_images(const nlohmann::json &card, const std::vector<std::string> &images) {
    auto existing = card_images(card);
    auto requested = images;
    const auto trim_empty_slots = [](auto &paths) {
        while (!paths.empty() && paths.back().empty()) paths.pop_back();
    };
    trim_empty_slots(existing);
    trim_empty_slots(requested);
    return existing == requested;
}

bool same_card_configuration(const nlohmann::json &card,
                             const ProjectSession::CardSettings &settings) {
    return card.value("space", std::string{}) == settings.space_name &&
           card_number(card, "base") == settings.base &&
           card_number(card, "size") == settings.size &&
           card_number(card, "reset_vector") == settings.reset_vector &&
           card.value("priority", int32_t(0)) == settings.priority &&
           same_card_images(card, settings.images) &&
           card.value("config", nlohmann::json::object()) == settings.config;
}
}

nlohmann::json ProjectSession::empty_document() {
    return {{"name", ""}, {"version", 2}, {"seed", 1}, {"time_mode", "project"}, {"epoch_ns", 0},
            {"spaces", nlohmann::json::array({
                {{"name", "cpu0.memory"}, {"maximum", "0xFFFF"}, {"unclaimed", 0}, {"resolver", "priority"}},
                {{"name", "cpu0.io"}, {"maximum", "0xFF"}, {"unclaimed", "0xFF"}, {"resolver", "priority"}}})},
            {"clocks", nlohmann::json::array({0, 0, 0})},
            {"cards", nlohmann::json::array()}};
}

void ProjectSession::install(nlohmann::json document, const std::filesystem::path &path,
                             uint64_t generation,
                             const std::vector<ProjectCardAssociation> &cards, bool saved) {
    const auto &saved_cards = document.at("cards");
    if (!saved_cards.is_array() || saved_cards.size() != cards.size())
        throw std::runtime_error("Runtime card associations do not match the project");
    auto normalized = normalize(path);
    CardRecords active, removed;
    std::vector<uint64_t> order;
    std::unordered_set<uint64_t> ids;
    for (size_t i = 0; i < cards.size(); ++i) {
        const auto &association = cards[i];
        if (!association.id || !ids.insert(association.id).second || !saved_cards[i].is_object())
            throw std::runtime_error("Invalid project card association");
        auto card = saved_cards[i];
        if (association.available) {
            if (association.active) card.erase("removed");
            else card["removed"] = true;
        }
        document["cards"][i] = card;
        (association.active ? active : removed).emplace_back(association.id, std::move(card));
        order.push_back(association.id);
    }
    if (!path.empty()) document["name"] = project_display_name(document, normalized);
    document_ = std::move(document);
    path_ = std::move(normalized);
    active_ = std::move(active);
    removed_ = std::move(removed);
    order_ = std::move(order);
    generation_ = generation;
    document_state_ = DocumentState::loaded;
    installed_revision_ = ++revision_;
    if (saved) saved_revision_ = revision_;
}

bool ProjectSession::commit_save(SaveIdentity captured, const std::filesystem::path &path) {
    if (captured.generation != generation_ || captured.revision > revision_ ||
        captured.revision < saved_revision_ || captured.revision < installed_revision_) return false;
    path_ = normalize(path);
    saved_revision_ = captured.revision;
    return true;
}

void ProjectSession::note_external_edit() { ++revision_; }
void ProjectSession::require_generation(uint64_t generation) const {
    if (!loaded())
        throw std::runtime_error("No project is loaded");
    if (generation != generation_)
        throw std::runtime_error("Project edit belongs to an obsolete rack");
}
void ProjectSession::refresh_cards() {
    auto cards = nlohmann::json::array();
    for (auto id : order_) {
        auto active = find_card(active_, id);
        if (active != active_.end()) cards.push_back(active->second);
        else {
            auto removed = find_card(removed_, id);
            if (removed != removed_.end()) cards.push_back(removed->second);
        }
    }
    document_["cards"] = std::move(cards);
}
void ProjectSession::commit_insert(uint64_t generation, uint64_t id, nlohmann::json card) {
    require_generation(generation);
    if (!id || !card.is_object() || find_card(active_, id) != active_.end() ||
        find_card(removed_, id) != removed_.end())
        throw std::runtime_error("Invalid inserted card association");
    card.erase("removed");
    active_.emplace_back(id, std::move(card));
    order_.push_back(id);
    refresh_cards();
    ++revision_;
}
void ProjectSession::commit_park(uint64_t generation, uint64_t id) {
    require_generation(generation);
    auto it = find_card(active_, id);
    if (it == active_.end()) throw std::runtime_error("Active card configuration is unavailable");
    auto card = it->second;
    card["removed"] = true;
    removed_.emplace_back(id, std::move(card));
    active_.erase(it);
    refresh_cards();
    ++revision_;
}
void ProjectSession::commit_plug(uint64_t generation, uint64_t id) {
    require_generation(generation);
    auto it = find_card(removed_, id);
    if (it == removed_.end()) throw std::runtime_error("Removed card configuration is unavailable");
    auto card = it->second;
    card.erase("removed");
    active_.emplace_back(id, std::move(card));
    removed_.erase(it);
    refresh_cards();
    ++revision_;
}
void ProjectSession::commit_put_away(uint64_t generation, uint64_t id) {
    require_generation(generation);
    auto it = find_card(removed_, id);
    if (it == removed_.end()) throw std::runtime_error("Removed card configuration is unavailable");
    const auto position = std::find(order_.begin(), order_.end(), id);
    if (position != order_.end() && document_.contains("mixer") && document_["mixer"].is_object()) {
        auto sources = document_["mixer"].find("sources");
        if (sources != document_["mixer"].end() && sources->is_array()) {
            const auto removed_index = static_cast<uint32_t>(position - order_.begin());
            for (auto source = sources->begin(); source != sources->end();) {
                if (!source->is_object() || !source->contains("card") || !(*source)["card"].is_number_unsigned()) {
                    ++source;
                } else if ((*source)["card"].get<uint32_t>() == removed_index) {
                    source = sources->erase(source);
                } else {
                    if ((*source)["card"].get<uint32_t>() > removed_index)
                        (*source)["card"] = (*source)["card"].get<uint32_t>() - 1;
                    ++source;
                }
            }
        }
    }
    if (position != order_.end() && document_.contains("video") && document_["video"].is_object()) {
        auto shaders = document_["video"].find("shaders");
        if (shaders != document_["video"].end() && shaders->is_array()) {
            const auto removed_index = static_cast<uint32_t>(position - order_.begin());
            for (auto shader = shaders->begin(); shader != shaders->end();) {
                if (!shader->is_object() || !shader->contains("card") ||
                    !(*shader)["card"].is_number_unsigned()) { ++shader; continue; }
                if ((*shader)["card"].get<uint32_t>() == removed_index)
                    shader = shaders->erase(shader);
                else {
                    if ((*shader)["card"].get<uint32_t>() > removed_index)
                        (*shader)["card"] = (*shader)["card"].get<uint32_t>() - 1;
                    ++shader;
                }
            }
        }
    }
    removed_.erase(it);
    std::erase(order_, id);
    refresh_cards();
    ++revision_;
}
void ProjectSession::commit_card_config(uint64_t generation, uint64_t id, nlohmann::json config) {
    require_generation(generation);
    auto it = find_card(active_, id);
    if (it == active_.end()) throw std::runtime_error("Active card configuration is unavailable");
    if (it->second.value("config", nlohmann::json::object()) == config) return;
    it->second["config"] = std::move(config);
    refresh_cards();
    ++revision_;
}
void ProjectSession::commit_clock(uint64_t generation, uint32_t index, uint32_t hz) {
    require_generation(generation);
    if (index >= 3) throw std::runtime_error("Invalid master clock index");
    auto &clocks = document_["clocks"];
    if (!clocks.is_array()) clocks = nlohmann::json::array({0, 0, 0});
    while (clocks.size() <= index) clocks.push_back(0);
    if (clocks[index] == hz) return;
    clocks[index] = hz;
    ++revision_;
}
void ProjectSession::commit_card_document(uint64_t generation, uint64_t id, nlohmann::json card) {
    require_generation(generation);
    auto it = find_card(active_, id);
    if (it == active_.end()) throw std::runtime_error("Active card configuration is unavailable");
    if (it->second == card) return;
    it->second = std::move(card);
    refresh_cards();
    ++revision_;
}
ProjectSession::RackInfo ProjectSession::rack_info() const {
    RackInfo result;
    if (!loaded() || !document_.is_object()) return result;
    result.name = document_.value("name", std::string{});
    const auto metadata = document_.find("rack_info");
    if (metadata == document_.end() || !metadata->is_object()) return result;
    result.author = metadata->value("author", std::string{});
    result.version = metadata->value("version", std::string{});
    result.notes = metadata->value("notes", std::string{});
    result.thumbnail = metadata->value("thumbnail", std::string{});
    return result;
}
bool ProjectSession::commit_rack_info(RackInfo info) {
    if (!loaded() || rack_info() == info) return false;
    document_["name"] = std::move(info.name);
    auto &metadata = document_["rack_info"];
    if (!metadata.is_object()) metadata = nlohmann::json::object();
    metadata["author"] = std::move(info.author);
    metadata["version"] = std::move(info.version);
    metadata["notes"] = std::move(info.notes);
    metadata["thumbnail"] = std::move(info.thumbnail);
    ++revision_;
    return true;
}
bool ProjectSession::commit_mixer_master(uint32_t percent) {
    if (!loaded()) return false;
    percent = std::min(percent, 100u);
    auto &mixer = document_["mixer"];
    if (!mixer.is_object()) mixer = nlohmann::json::object();
    if (mixer.value("master_volume", 100u) == percent) return false;
    mixer["master_volume"] = percent;
    ++revision_;
    return true;
}
bool ProjectSession::commit_mixer_source(uint64_t owner, std::string name, uint32_t ordinal,
                                         uint32_t percent, bool muted, int32_t pan) {
    if (!loaded()) return false;
    const auto card = std::find(order_.begin(), order_.end(), owner);
    if (card == order_.end()) return false;
    percent = std::min(percent, 150u);
    auto &mixer = document_["mixer"];
    if (!mixer.is_object()) mixer = nlohmann::json::object();
    auto &sources = mixer["sources"];
    if (!sources.is_array()) sources = nlohmann::json::array();
    const auto index = static_cast<uint32_t>(card - order_.begin());
    auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &entry) {
        return entry.is_object() && entry.value("card", UINT32_MAX) == index &&
               entry.value("name", std::string{}) == name && entry.value("ordinal", 0u) == ordinal;
    });
    const nlohmann::json next = {{"card", index}, {"name", std::move(name)},
                                 {"ordinal", ordinal}, {"volume", percent}, {"muted", muted}, {"pan", std::clamp(pan, -64, 63)}};
    if (source != sources.end() && *source == next) return false;
    if (source == sources.end()) sources.push_back(next); else *source = next;
    ++revision_;
    return true;
}
bool ProjectSession::video_shader_enabled(uint64_t owner, uint32_t ordinal) const {
    if (!loaded()) return false;
    const auto card = std::find(order_.begin(), order_.end(), owner);
    if (card == order_.end() || !document_.contains("video") || !document_["video"].is_object())
        return false;
    const auto shaders = document_["video"].find("shaders");
    if (shaders == document_["video"].end() || !shaders->is_array()) return false;
    const auto index = static_cast<uint32_t>(card - order_.begin());
    const auto found = std::find_if(shaders->begin(), shaders->end(), [&](const auto &entry) {
        return entry.is_object() && entry.value("card", UINT32_MAX) == index &&
               entry.value("surface", UINT32_MAX) == ordinal;
    });
    return found != shaders->end() && found->value("enabled", false);
}
bool ProjectSession::commit_video_shader(uint64_t generation, uint64_t owner, uint32_t ordinal,
                                         bool enabled) {
    require_generation(generation);
    if (!loaded()) return false;
    const auto card = std::find(order_.begin(), order_.end(), owner);
    if (card == order_.end()) return false;
    auto &video = document_["video"];
    if (!video.is_object()) video = nlohmann::json::object();
    auto &shaders = video["shaders"];
    if (!shaders.is_array()) shaders = nlohmann::json::array();
    const auto index = static_cast<uint32_t>(card - order_.begin());
    auto found = std::find_if(shaders.begin(), shaders.end(), [&](const auto &entry) {
        return entry.is_object() && entry.value("card", UINT32_MAX) == index &&
               entry.value("surface", UINT32_MAX) == ordinal;
    });
    if (found != shaders.end() && found->value("enabled", false) == enabled) return false;
    const nlohmann::json next = {{"card", index}, {"surface", ordinal}, {"enabled", enabled}};
    if (found == shaders.end()) shaders.push_back(next); else *found = next;
    ++revision_;
    return true;
}
// IDs are materialized only when routing is applied, so opening/cancelling
// settings never edits the project. Reserve deleted IDs with a persisted counter.
std::string ProjectSession::input_card_id(uint64_t owner) const {
    uint64_t next = 1;
    if (document_.contains("input_next_card_id") && document_["input_next_card_id"].is_number_unsigned())
        next = document_["input_next_card_id"].get<uint64_t>();
    std::unordered_set<std::string> used;
    for (const auto *records : {&active_, &removed_})
        for (const auto &[id, card] : *records) {
            (void)id;
            if (card.contains("host_input_id") && card["host_input_id"].is_string())
                used.insert(card["host_input_id"].get<std::string>());
        }
    for (auto id : order_) {
        auto found = find_card(active_, id);
        const auto *card = found != active_.end() ? &found->second : nullptr;
        if (!card) { auto parked = find_card(removed_, id); if (parked != removed_.end()) card = &parked->second; }
        if (!card) continue;
        std::string key;
        if (card->contains("host_input_id") && (*card)["host_input_id"].is_string())
            key = (*card)["host_input_id"].get<std::string>();
        if (key.empty()) {
            do {
                if (next == UINT64_MAX) throw std::runtime_error("Input card ID space exhausted");
                key = "card-" + std::to_string(next++);
            } while (used.contains(key));
            used.insert(key);
        }
        if (id == owner) return key;
    }
    return {};
}
uint64_t ProjectSession::input_card_owner(const std::string &key) const {
    if (key.empty()) return 0;
    uint64_t result = 0;
    for (const auto &[id, card] : active_) {
        (void)card;
        if (input_card_id(id) == key) {
            if (result) return 0; // Malformed duplicate IDs must never redirect input.
            result = id;
        }
    }
    return result;
}
std::vector<InputRoute> ProjectSession::input_routes() const {
    std::vector<InputRoute> routes;
    const auto entries = document_.find("input_routes");
    if (entries == document_.end() || !entries->is_array()) return routes;
    for (const auto &entry : *entries) {
        if (!entry.is_object() || !entry.contains("video_card") || !entry["video_card"].is_string() ||
            !entry.contains("surface") || !entry["surface"].is_number_unsigned() ||
            !entry.contains("keyboard") || !entry["keyboard"].is_string() ||
            !entry.contains("mouse") || !entry["mouse"].is_string() ||
            !entry.contains("relative") || !entry["relative"].is_boolean() ||
            entry["surface"].get<uint64_t>() > UINT32_MAX) continue;
        InputRoute route{entry["video_card"].get<std::string>(), entry["surface"].get<uint32_t>(),
                         entry["keyboard"].get<std::string>(), entry["mouse"].get<std::string>(),
                         entry["relative"].get<bool>()};
        if (std::none_of(routes.begin(), routes.end(), [&](const auto &r) {
            return r.video_card == route.video_card && r.surface == route.surface;
        })) routes.push_back(std::move(route));
    }
    return routes;
}
bool ProjectSession::commit_input_routes(uint64_t generation, const std::vector<InputRoute> &routes) {
    require_generation(generation);
    if (input_routes() == routes) return false;
    std::vector<std::pair<uint64_t, std::string>> keys;
    for (auto id : order_) keys.emplace_back(id, input_card_id(id));
    uint64_t next = document_.value("input_next_card_id", uint64_t(1));
    for (const auto &[id, key] : keys) {
        for (auto *records : {&active_, &removed_}) {
            auto card = find_card(*records, id);
            if (card != records->end()) card->second["host_input_id"] = key;
        }
        if (key.starts_with("card-")) {
            try { next = std::max(next, static_cast<uint64_t>(std::stoull(key.substr(5))) + 1); }
            catch (const std::exception &) { /* User-supplied opaque ID. */ }
        }
    }
    document_["input_next_card_id"] = next;
    auto entries = nlohmann::json::array();
    for (const auto &route : routes)
        entries.push_back({{"video_card", route.video_card}, {"surface", route.surface},
                           {"keyboard", route.keyboard}, {"mouse", route.mouse}, {"relative", route.relative}});
    document_["input_routes"] = std::move(entries);
    refresh_cards();
    ++revision_;
    return true;
}
void ProjectSession::collect_plugin_data(uint64_t generation, const nlohmann::json &data) {
    require_generation(generation);
    bool changed = false;
    for (auto *records : {&active_, &removed_}) {
        for (auto &[id, card] : *records) {
            const auto chunk = data.find(std::to_string(id));
            if (chunk != data.end() && (!card.contains("plugin_data") || card["plugin_data"] != *chunk)) {
                card["plugin_data"] = *chunk;
                changed = true;
            }
        }
    }
    if (changed) { refresh_cards(); ++revision_; }
}
void ProjectSession::collect_tool_state(nlohmann::json states, std::vector<std::string> path_states) {
    tool_path_states_ = std::move(path_states);
    if (states.empty()) {
        if (document_.erase("tool_state")) ++revision_;
    } else if (!document_.contains("tool_state") || document_["tool_state"] != states) {
        document_["tool_state"] = std::move(states);
        ++revision_;
    }
}
} // namespace srz80::ui

namespace srz80::ui {
ProjectSession::~ProjectSession() { shutdown(); }
void ProjectSession::initialize_generation(uint64_t generation) {
    if (generation_ || busy()) throw std::runtime_error("Session already initialized");
    generation_ = generation;
}
bool ProjectSession::blocks_simulation() const {
    if (!busy() || !operation_) return false;
    if (operation_->runtime.kind == ProjectRuntimeRequest::Kind::clock ||
        operation_->runtime.kind == ProjectRuntimeRequest::Kind::card_metadata)
        return false;
    return operation_->action.kind != ActionKind::save || operation_->after_save || path_.empty();
}
bool ProjectSession::transitioning() const {
    if (!operation_) return false;
    const auto kind = operation_->action.kind;
    return kind == ActionKind::new_project || kind == ActionKind::open ||
           kind == ActionKind::close_project ||
           kind == ActionKind::configure || kind == ActionKind::reload_roms ||
           kind == ActionKind::open_state || phase_ == Phase::unresolved;
}
std::vector<ProjectSession::Effect> ProjectSession::take_effects() {
    return std::exchange(effects_, {});
}
void ProjectSession::emit(Effect::Kind kind, const std::string &message) {
    effects_.push_back({kind, operation_id(), operation_ ? operation_->action.path : std::filesystem::path{},
                        0, false, message, {}});
}
void ProjectSession::fail(const std::string &error, bool unresolved) {
    save_conflicts_.clear();
    operation_error_ = error;
    emit(Effect::Kind::error, error);
    if (operation_ && operation_->replacement_announced && !unresolved)
        emit(Effect::Kind::replacement_finished);
    if (unresolved) phase_ = Phase::unresolved;
    else { operation_.reset(); phase_ = Phase::idle; }
}
bool ProjectSession::request(Action action, bool workspace_dirty) {
    if (busy() || stopping_) {
        effects_.push_back({Effect::Kind::error, 0, {}, 0, false, "Project is busy; wait for the current operation", {}});
        return false;
    }
    const auto requested_kind = action.kind;
    if (!loaded() && requested_kind != ActionKind::new_project &&
        requested_kind != ActionKind::open && requested_kind != ActionKind::quit) {
        operation_error_ = "No project is loaded";
        effects_.push_back({Effect::Kind::error, 0, {}, 0, false, operation_error_, {}});
        return false;
    }
    operation_error_.clear();
    operation_ = Operation{next_operation_++, std::move(action), {}, {}, {}, false};
    const auto kind = operation_->action.kind;
    if ((kind == ActionKind::new_project || kind == ActionKind::open ||
         kind == ActionKind::close_project || kind == ActionKind::quit) &&
        (dirty() || workspace_dirty || workspace_.dirty())) {
        phase_ = Phase::awaiting_unsaved;
    } else advance();
    return true;
}
void ProjectSession::resolve_unsaved(uint64_t operation, Decision decision) {
    if (operation != operation_id() || phase_ != Phase::awaiting_unsaved) return;
    if (decision == Decision::cancel) {
        operation_.reset();
        phase_ = Phase::idle;
        emit(Effect::Kind::cancelled);
        return;
    }
    if (decision == Decision::save) {
        operation_->after_save = operation_->action;
        operation_->action = Action{};
        operation_->action.kind = ActionKind::save;
    }
    advance();
}
void ProjectSession::resolve_path(uint64_t operation, const std::filesystem::path &path,
                                   const std::string &error) {
    if (operation != operation_id() || phase_ != Phase::awaiting_path) return;
    if (!error.empty()) { fail(error); return; }
    if (path.empty()) {
        operation_.reset();
        phase_ = Phase::idle;
        emit(Effect::Kind::cancelled);
        return;
    }
    try {
        operation_->action.path = operation_->action.kind == ActionKind::new_project
                                      ? project_save_destination({}, path).root
                                      : path;
    } catch (const std::exception &exception) {
        fail(exception.what());
        return;
    }
    advance();
}
void ProjectSession::finish_save(uint64_t operation, bool success, const std::string &error,
                                  bool workspace_dirty) {
    if (operation != operation_id() || phase_ != Phase::saving) return;
    if (!success || (operation_->after_save && (dirty() || workspace_dirty))) {
        fail(error.empty() ? "Project save did not complete; unsaved changes remain" : error);
        return;
    }
    if (operation_->after_save && !stopping_) {
        operation_->action = std::move(*operation_->after_save);
        operation_->after_save.reset();
        // Saving and the deferred transition have distinct identities, so a
        // late Save As callback cannot answer the subsequent Open dialog.
        operation_->id = next_operation_++;
        advance();
    } else { operation_.reset(); phase_ = Phase::idle; }
}
void ProjectSession::advance() {
    try {
        auto &action = operation_->action;
        if (action.kind == ActionKind::quit) {
            emit(Effect::Kind::quit);
            operation_.reset(); phase_ = Phase::shutdown;
        } else if (action.kind == ActionKind::new_project && action.path.empty()) {
            phase_ = Phase::awaiting_path;
            emit(Effect::Kind::new_project_path);
        } else if (action.kind == ActionKind::save || action.kind == ActionKind::save_as ||
                   action.kind == ActionKind::save_state || action.kind == ActionKind::save_state_as) {
            const bool state = action.kind == ActionKind::save_state || action.kind == ActionKind::save_state_as;
            if (action.path.empty() && action.kind == ActionKind::save_state) action.path = state_path_;
            if (action.path.empty() && action.kind == ActionKind::save) action.path = path_;
            if (action.path.empty()) { phase_ = Phase::awaiting_path; emit(state ? Effect::Kind::save_state_path : Effect::Kind::save_path); }
            else if (state) { phase_ = Phase::saving; emit(Effect::Kind::capture_state); }
            else begin_source_save();
        } else if (action.kind == ActionKind::open || action.kind == ActionKind::open_state) {
            if (action.path.empty()) { phase_ = Phase::awaiting_path; emit(action.kind == ActionKind::open_state ? Effect::Kind::open_state_path : Effect::Kind::open_path); return; }
            action.path = normalize(action.path);
            const auto path = action.path;
            preparation_ = std::async(std::launch::async, [path] {
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Cannot open project: " + path.string());
                return nlohmann::json::parse(input);
            });
            phase_ = Phase::preparing;
        } else {
            prepare_mutation();
        }
    } catch (const std::exception &error) { fail(error.what()); }
}
void ProjectSession::prepare_mutation() {
    auto &op = *operation_;
    const auto &action = op.action;
    auto &request = op.runtime;
    request.operation = op.id;
    request.generation = generation_;
    request.card = action.card;
    request.index = action.index;
    request.hz = action.hz;
    if ((action.kind == ActionKind::park || action.kind == ActionKind::property) &&
        find_card(active_, action.card) == active_.end())
        throw std::runtime_error("Active card configuration is unavailable");
    if ((action.kind == ActionKind::plug || action.kind == ActionKind::put_away) &&
        find_card(removed_, action.card) == removed_.end())
        throw std::runtime_error("Removed card configuration is unavailable");
    using Kind = ProjectRuntimeRequest::Kind;
    switch (action.kind) {
    case ActionKind::new_project:
    case ActionKind::close_project:
        request.kind = Kind::replace;
        request.document = empty_document();
        break;
    case ActionKind::open_state: {
        auto wrapper = std::move(request.document);
        op.restored_project_path = wrapper.value("project_path", path_.string());
        request.execution = wrapper.at("execution").dump();
        request.document = wrapper.at("project");
        request.kind = Kind::restore_state;
        request.directory = default_directory_;
        break;
    }
    case ActionKind::open:
        request.kind = Kind::replace;
        request.directory = action.path.parent_path();
        break;
    case ActionKind::insert: {
        request.kind = Kind::insert;
        request.insertion = action.insertion;
        const auto &card = action.insertion;
        auto config = nlohmann::json::parse(card.config_json);
        if (!config.is_object()) throw std::runtime_error("Plugin configuration must be an object");
        request.document = {{"plugin", card.type}, {"space", action.settings.space_name},
            {"base", card.base}, {"size", card.size}, {"reset_vector", card.reset_vector},
            {"priority", card.priority}, {"clock", card.clock}};
        if (!config.empty()) request.document["config"] = std::move(config);
        if (card.image_paths.size() == 1) request.document["image"] = card.image_paths[0].string();
        else if (!card.image_paths.empty()) {
            request.document["images"] = nlohmann::json::array();
            for (const auto &image : card.image_paths) request.document["images"].push_back(image.string());
        }
        break;
    }
    case ActionKind::park: request.kind = Kind::park; break;
    case ActionKind::plug: request.kind = Kind::plug; break;
    case ActionKind::put_away: request.kind = Kind::put_away; break;
    case ActionKind::clock: request.kind = Kind::clock; break;
    case ActionKind::stop_clocks: request.kind = Kind::stop_clocks; break;
    case ActionKind::property:
        request.kind = Kind::property;
        request.value = action.value;
        break;
    case ActionKind::reload_roms:
        request.kind = Kind::replace;
        request.directory = path_.empty() ? default_directory_ : path_.parent_path();
        request.document = document_;
        request.source_cards = order_;
        break;
    case ActionKind::edit_spaces:
        if (!action.config.is_array() || action.config.empty())
            throw std::runtime_error("At least one memory bus is required");
        {
            std::unordered_set<std::string> names;
            for (const auto &space : action.config) {
                if (!space.is_object() || !space.contains("name") || !space["name"].is_string() ||
                    space["name"].get<std::string>().empty() ||
                    !names.insert(space["name"].get<std::string>()).second)
                    throw std::runtime_error("Memory bus names must be unique and nonempty");
            }
            for (const auto &card : document_.at("cards"))
                if (card.contains("space") && card["space"].is_string() &&
                    !card["space"].get<std::string>().empty() &&
                    !names.contains(card["space"].get<std::string>()))
                    throw std::runtime_error("A card still uses a removed memory bus");
        }
        request.kind = Kind::replace;
        request.directory = path_.empty() ? default_directory_ : path_.parent_path();
        request.document = document_;
        request.document["spaces"] = action.config;
        request.source_cards = order_;
        break;
    case ActionKind::configure:
    case ActionKind::reorder: {
        request.kind = action.kind == ActionKind::reorder ? Kind::reorder : Kind::replace;
        request.directory = path_.empty() ? default_directory_ : path_.parent_path();
        request.document = document_;
        request.source_cards = order_;
        const auto source = find_card(active_, action.card);
        if (source == active_.end()) throw std::runtime_error("Card configuration is unavailable");
        if (action.kind == ActionKind::configure) {
            const auto &settings = action.settings;
            auto card = source->second;
            if (settings.name.empty()) card.erase("name"); else card["name"] = settings.name;
            card["space"] = settings.space_name;
            card["base"] = settings.base; card["size"] = settings.size;
            card["reset_vector"] = settings.reset_vector; card["priority"] = settings.priority;
            card["clock"] = settings.clock;
            card.erase("image");
            card.erase("images");
            if (settings.images.size() == 1) card["image"] = settings.images[0];
            else if (!settings.images.empty()) card["images"] = settings.images;
            if (settings.config.empty()) card.erase("config"); else card["config"] = settings.config;
            auto index = static_cast<size_t>(std::find(order_.begin(), order_.end(), action.card) - order_.begin());
            request.document["cards"][index] = std::move(card);
            if (same_card_configuration(source->second, settings)) {
                request.kind = Kind::card_metadata;
                request.name = settings.name;
                request.index = settings.clock;
                // Keep the original representation of rebuild fields. The
                // metadata path must not rewrite equivalent image/config
                // encodings while committing the edited document.
                auto metadata_card = source->second;
                if (settings.name.empty()) metadata_card.erase("name");
                else metadata_card["name"] = settings.name;
                if (card_number(source->second, "clock") != settings.clock)
                    metadata_card["clock"] = settings.clock;
                request.document["cards"][index] = std::move(metadata_card);
            }
        } else {
            auto active = active_;
            auto from = find_card(active, action.card);
            auto moved = *from;
            active.erase(from);
            auto to = find_card(active, action.target);
            if (to == active.end()) throw std::runtime_error("Reorder target is unavailable");
            if (action.after) ++to;
            active.insert(to, std::move(moved));
            size_t next = 0;
            for (size_t i = 0; i < order_.size(); ++i) {
                if (find_card(active_, order_[i]) == active_.end()) continue;
                request.source_cards[i] = active[next].first;
                request.document["cards"][i] = active[next++].second;
            }
        }
        break;
    }
    default: throw std::runtime_error("Invalid project action");
    }
    if (request.kind == Kind::replace || request.kind == Kind::restore_state) {
        const auto &cards = request.document.at("cards");
        if (!cards.is_array() || std::any_of(cards.begin(), cards.end(), [](const auto &card) { return !card.is_object(); }))
            throw std::runtime_error("Project must contain a card array");
        phase_ = Phase::awaiting_shell;
        op.replacement_announced = true;
        emit(Effect::Kind::before_replace);
    } else submit_runtime();
}
void ProjectSession::confirm_replace(uint64_t operation) {
    if (operation != operation_id() || phase_ != Phase::awaiting_shell || stopping_) return;
    try { submit_runtime(); } catch (const std::exception &error) { fail(error.what()); }
}
void ProjectSession::submit_runtime() {
    if (!runtime_) throw std::runtime_error("Project runtime is unavailable");
    completion_ = runtime_->submit_project(operation_->runtime);
    phase_ = Phase::awaiting_runtime;
}
std::optional<uint32_t> ProjectSession::pending_clock(uint32_t index) const {
    if (!operation_) return {};
    if (operation_->action.kind == ActionKind::stop_clocks) return 0;
    if (operation_->action.kind == ActionKind::clock && operation_->action.index == index)
        return operation_->action.hz;
    return {};
}
void ProjectSession::prepare_save(uint64_t operation, const std::string &error) {
    if (operation != operation_id() || phase_ != Phase::saving || writing_.valid()) return;
    if (!error.empty()) {
        const auto kind = operation_->action.kind;
        fail(error + ((kind == ActionKind::save || kind == ActionKind::save_as)
                         ? "; legacy tool save callbacks may have written files" : ""));
        return;
    }
    try {
        auto &request = operation_->runtime;
        request.operation = operation;
        request.generation = generation_;
        request.kind = operation_->action.kind == ActionKind::save_state || operation_->action.kind == ActionKind::save_state_as
                           ? ProjectRuntimeRequest::Kind::capture_state : ProjectRuntimeRequest::Kind::capture;
        submit_runtime();
    } catch (const std::exception &exception) { fail(exception.what()); }
}
void ProjectSession::start_writing(const ProjectRuntimeResult &result) {
    collect_plugin_data(result.generation, result.document);
    writing_identity_ = save_identity();
    ProjectWriteRequest request;
    request.old_file = path_;
    request.requested = operation_->action.path;
    request.execution_state = operation_->runtime.kind == ProjectRuntimeRequest::Kind::capture_state;
    request.document = document_;
    request.tool_path_states = tool_path_states_;
    writing_tool_state_ = document_.value("tool_state", nlohmann::json::object());
    if (request.execution_state) {
        request.document = {{"project", project_with_absolute_assets(document_, path_)},
                            {"execution", nlohmann::json::parse(result.execution)},
                            {"project_path", path_.string()}};
    } else {
        for (const auto &[path, document] : workspace_.documents())
            if (document->dirty() || document->expected_disk.kind != ProjectFileState::Kind::present ||
                document->expected_disk.text != document->text.data())
                request.sources.push_back({document->id, path, document->text.data(), document->expected_disk});
    }
    if (writer_) writing_ = writer_(std::move(request));
    else writing_ = std::async(std::launch::async, [request = std::move(request)]() mutable {
        return write_project_snapshot(std::move(request));
    });
    if (!writing_.valid()) throw std::runtime_error("Save writer returned no completion");
    phase_ = Phase::saving;
}
void ProjectSession::emit_source_reload(const ProjectFileProbe &probe) {
    Effect effect{};
    effect.kind = Effect::Kind::source_reloaded;
    effect.selected = probe.id;
    effect.path = probe.path;
    effects_.push_back(std::move(effect));
}
void ProjectSession::begin_source_save() {
    operation_error_.clear();
    save_conflicts_.clear();
    auto probes = workspace_.file_probes();
    if (probes.empty()) {
        phase_ = Phase::saving;
        emit(Effect::Kind::save);
        return;
    }
    save_precheck_generation_ = generation_;
    save_precheck_ = std::async(std::launch::async, [probes = std::move(probes)]() mutable {
        return probe_project_sources(std::move(probes));
    });
    phase_ = Phase::checking_sources;
}
void ProjectSession::poll_source_files() {
    if (stopping_ || phase_ != Phase::idle) return;
    if (probing_.valid()) {
        if (probing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto probes = probing_.get();
            if (probing_generation_ == generation_) {
                for (const auto &probe : probes) {
                    if (workspace_.observe_file(probe)) emit_source_reload(probe);
                }
            }
        } catch (const std::exception &error) {
            Effect effect{};
            effect.kind = Effect::Kind::error;
            effect.message = std::string("Cannot check open files: ") + error.what();
            effects_.push_back(std::move(effect));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_probe_ || workspace_.documents().empty()) return;
    next_probe_ = now + std::chrono::seconds(1);
    probing_generation_ = generation_;
    try {
        if (reader_) probing_ = reader_(workspace_.file_probes());
        else probing_ = std::async(std::launch::async, [probes = workspace_.file_probes()]() mutable {
            return probe_project_sources(std::move(probes));
        });
        if (!probing_.valid()) throw std::runtime_error("Source reader returned no completion");
    } catch (const std::exception &error) {
        emit(Effect::Kind::error, std::string("Cannot check open files: ") + error.what());
    }
}
void ProjectSession::resolve_conflicts(uint64_t operation, ConflictDecision decision) {
    if (phase_ != Phase::awaiting_conflict || operation != operation_id()) return;
    if (decision == ConflictDecision::cancel) {
        save_conflicts_.clear();
        operation_.reset();
        phase_ = Phase::idle;
        emit(Effect::Kind::cancelled);
        return;
    }
    // Validate the complete decision before changing any document's baseline.
    for (const auto &conflict : save_conflicts_) {
        const auto *document = workspace_.find(conflict.id);
        if (!document || document->path != conflict.path || document->expected_disk != conflict.expected ||
            conflict.observed.kind == ProjectFileState::Kind::unreadable ||
            (decision == ConflictDecision::reload && conflict.observed.kind != ProjectFileState::Kind::present))
            return;
    }
    for (const auto &conflict : save_conflicts_) {
        if (decision == ConflictDecision::overwrite) workspace_.allow_overwrite(conflict);
        else if (workspace_.reload_file(conflict)) emit_source_reload(conflict);
    }
    save_conflicts_.clear();
    if (decision == ConflictDecision::reload) {
        // Reload cancels this save and any deferred open/new/quit operation.
        operation_.reset();
        phase_ = Phase::idle;
        emit(Effect::Kind::cancelled);
    } else begin_source_save();
}
void ProjectSession::poll() {
    poll_source_files();
    if (phase_ == Phase::checking_sources && save_precheck_.valid() &&
        save_precheck_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            const auto probes = save_precheck_.get();
            if (stopping_) return;
            if (save_precheck_generation_ != generation_) {
                fail("Project changed while checking source files", true);
                return;
            }
            for (const auto &probe : probes) {
                const auto *document = workspace_.find(probe.id);
                if (!document || document->path != probe.path || document->expected_disk != probe.expected) {
                    fail("An open file changed while preparing the save; try again");
                    return;
                }
                if (workspace_.observe_file(probe)) emit_source_reload(probe);
                if (document->external_change) save_conflicts_.push_back(probe);
            }
            if (!save_conflicts_.empty()) {
                phase_ = Phase::awaiting_conflict;
                operation_error_ = "Files changed outside SRZ80";
            } else {
                phase_ = Phase::saving;
                emit(Effect::Kind::save);
            }
        } catch (const std::exception &error) { fail(error.what()); }
    }
    if (phase_ == Phase::saving && writing_.valid() &&
        writing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = writing_.get();
            if (!result.success) {
                if (!result.conflicts.empty()) {
                    if (writing_identity_.generation != generation_) {
                        fail("Project changed while saving; conflict outcome is unresolved", true);
                        return;
                    }
                    save_conflicts_ = std::move(result.conflicts);
                    operation_error_ = result.error;
                    phase_ = Phase::awaiting_conflict;
                } else fail(result.error);
                return;
            }
            const bool state = operation_->runtime.kind == ProjectRuntimeRequest::Kind::capture_state;
            if (state) {
                state_path_ = result.destination.file;
                emit(Effect::Kind::state_saved);
                effects_.back().path = state_path_;
            }
            else {
                if (!commit_save(writing_identity_, result.destination.file)) {
                    fail("Files saved, but session identity changed; outcome unresolved", true); return;
                }
                if (!result.name.empty()) document_["name"] = std::move(result.name);
                // Relocation changes the spelling of path state, not its saved revision.
                // Preserve any tool contributions edited after capture.
                if (document_.contains("tool_state") && document_["tool_state"] == writing_tool_state_)
                    document_["tool_state"] = std::move(result.tool_state);
                for (const auto &source : result.sources)
                    if (auto *document = workspace_.find(source.id)) {
                        document->saved_text = source.text;
                        document->expected_disk = {ProjectFileState::Kind::present, source.text, {}};
                        document->external_change.reset();
                    }
                if (result.destination.create_workspace && !result.destination.old_root.empty())
                    workspace_.relocate(result.destination.old_root, result.destination.root);
                emit(Effect::Kind::saved);
                effects_.back().path = path_;
                effects_.back().previous_path = result.destination.old_root;
            }
            if (state) { operation_.reset(); phase_ = Phase::idle; }
            else finish_save(operation_id(), true, {}, workspace_.dirty());
        } catch (const std::exception &error) { fail(std::string("Files saved, but session commit is unresolved: ") + error.what(), true); }
    }

    if (phase_ == Phase::preparing && preparation_.valid() &&
        preparation_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto doc = preparation_.get();
            if (stopping_) { operation_.reset(); phase_ = Phase::idle; return; }
            operation_->runtime.document = std::move(doc);
            prepare_mutation();
        } catch (const std::exception &error) { fail(error.what()); }
    }
    if (phase_ != Phase::awaiting_runtime || !completion_.valid() ||
        completion_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        auto result = completion_.get();
        auto &op = *operation_;
        if (result.operation != op.id || result.expected_generation != op.runtime.generation) {
            fail("Runtime completion identity mismatch; project outcome is unresolved", true);
            return;
        }
        if (result.status != SRH_OK) {
            fail(result.error.empty() ? "Project operation rejected" : result.error,
                 result.replaced || (result.generation != 0 && result.generation != generation_));
            return;
        }
        const auto action = op.action;
        const auto kind = op.runtime.kind;
        uint64_t selected = action.selected;
        if (kind == ProjectRuntimeRequest::Kind::capture || kind == ProjectRuntimeRequest::Kind::capture_state) {
            if (result.generation != generation_) { fail("Save capture generation mismatch", true); return; }
            try { start_writing(result); } catch (const std::exception &error) { fail(error.what()); }
            return;
        }
        if (kind == ProjectRuntimeRequest::Kind::replace || kind == ProjectRuntimeRequest::Kind::restore_state) {
            if (!result.replaced || result.generation <= generation_) {
                fail("Replacement generation mismatch; project outcome is unresolved", true); return;
            }
            const bool closing = action.kind == ActionKind::close_project;
            const bool clear = action.kind == ActionKind::open ||
                               action.kind == ActionKind::new_project || closing;
            if (!clear && selected) {
                const auto index = std::find(op.runtime.source_cards.begin(), op.runtime.source_cards.end(), selected);
                const auto offset = static_cast<size_t>(index - op.runtime.source_cards.begin());
                selected = offset < result.cards.size() ? result.cards[offset].id : 0;
            }
            const bool state = kind == ProjectRuntimeRequest::Kind::restore_state;
            if (closing) {
                document_ = nlohmann::json::object();
                path_.clear();
                active_.clear();
                removed_.clear();
                order_.clear();
                workspace_.clear();
                state_path_.clear();
                generation_ = result.generation;
                saved_revision_ = revision_;
                installed_revision_ = revision_;
                document_state_ = DocumentState::unloaded;
                emit(Effect::Kind::closed);
                effects_.back().clear_workspace = true;
            } else {
                const auto installed_path = action.kind == ActionKind::new_project
                                                ? std::filesystem::path{}
                                                : (state ? op.restored_project_path : (clear ? action.path : path_));
                install(std::move(result.document), installed_path, result.generation,
                        result.cards, action.kind == ActionKind::open || state);
                if (clear) { workspace_.clear(); state_path_.clear(); }
                if (state) state_path_ = action.path;
                emit(Effect::Kind::installed);
                effects_.back().clear_workspace = clear;
                effects_.back().selected = clear ? 0 : selected;
                effects_.back().path = path_;
            }
            emit(Effect::Kind::replacement_finished);
            if (action.kind == ActionKind::new_project) {
                op.action = {};
                op.action.kind = ActionKind::save;
                op.action.path = action.path;
                begin_source_save();
                return;
            }
        } else {
            if (result.generation != generation_) {
                fail("Edit generation mismatch; project outcome is unresolved", true); return;
            }
            switch (kind) {
            case ProjectRuntimeRequest::Kind::insert: {
                commit_insert(generation_, result.card, op.runtime.document);
                selected = result.card;
                break;
            }
            case ProjectRuntimeRequest::Kind::park:
                commit_park(generation_, action.card); if (selected == action.card) selected = 0; break;
            case ProjectRuntimeRequest::Kind::plug: commit_plug(generation_, action.card); selected = action.card; break;
            case ProjectRuntimeRequest::Kind::put_away:
                commit_put_away(generation_, action.card); if (selected == action.card) selected = 0; break;
            case ProjectRuntimeRequest::Kind::reorder: {
                const auto old_order = order_;
                CardRecords ordered_active;
                for (auto id : op.runtime.source_cards) {
                    const auto card = find_card(active_, id);
                    if (card != active_.end()) ordered_active.push_back(*card);
                }
                active_ = std::move(ordered_active);
                order_ = op.runtime.source_cards;
                if (document_.contains("mixer") && document_["mixer"].is_object()) {
                    auto sources = document_["mixer"].find("sources");
                    if (sources != document_["mixer"].end() && sources->is_array()) {
                        for (auto &source : *sources) {
                            if (!source.is_object() || !source.contains("card") || !source["card"].is_number_unsigned()) continue;
                            const auto old_index = source["card"].get<size_t>();
                            if (old_index >= old_order.size()) continue;
                            const auto next = std::find(order_.begin(), order_.end(), old_order[old_index]);
                            if (next != order_.end()) source["card"] = static_cast<uint32_t>(next - order_.begin());
                        }
                    }
                }
                if (document_.contains("video") && document_["video"].is_object()) {
                    auto shaders = document_["video"].find("shaders");
                    if (shaders != document_["video"].end() && shaders->is_array()) {
                        for (auto &shader : *shaders) {
                            if (!shader.is_object() || !shader.contains("card") ||
                                !shader["card"].is_number_unsigned()) continue;
                            const auto old_index = shader["card"].get<size_t>();
                            if (old_index >= old_order.size()) continue;
                            const auto next = std::find(order_.begin(), order_.end(), old_order[old_index]);
                            if (next != order_.end()) shader["card"] = static_cast<uint32_t>(next - order_.begin());
                        }
                    }
                }
                refresh_cards();
                ++revision_;
                break;
            }
            case ProjectRuntimeRequest::Kind::clock: commit_clock(generation_, action.index, action.hz); break;
            case ProjectRuntimeRequest::Kind::card_metadata:
                commit_card_document(generation_, action.card, op.runtime.document["cards"].at(
                    static_cast<size_t>(std::find(order_.begin(), order_.end(), action.card) - order_.begin())));
                break;
            case ProjectRuntimeRequest::Kind::stop_clocks:
                for (uint32_t i = 0; i < 3; ++i) commit_clock(generation_, i, 0);
                break;
            case ProjectRuntimeRequest::Kind::property: commit_card_config(generation_, action.card, action.config); break;
            default: break;
            }
            emit(Effect::Kind::changed);
            effects_.back().selected = selected;
        }
        operation_.reset(); phase_ = Phase::idle;
    } catch (const std::exception &error) {
        // A broken promise or failure after runtime success is not rollback.
        fail(std::string("Project outcome unresolved: ") + error.what(), true);
    }
}
void ProjectSession::shutdown() {
    if (stopping_) return;
    stopping_ = true;
    if (probing_.valid()) { probing_.wait(); probing_ = {}; }
    if (save_precheck_.valid()) { save_precheck_.wait(); save_precheck_ = {}; }
    if (preparation_.valid()) preparation_.wait();
    if (completion_.valid()) completion_.wait();
    poll();
    if (writing_.valid()) writing_.wait();
    poll();
    effects_.clear();
    operation_.reset();
    phase_ = Phase::shutdown;
}
} // namespace srz80::ui
