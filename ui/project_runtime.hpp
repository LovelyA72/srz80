#pragma once
#include <srz80/abi.h>
#include <cstdint>
#include <filesystem>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace srz80::ui {
struct ProjectCardAssociation { uint64_t id; bool active; bool available = true; };
struct ProjectCardRequest {
    std::string type;
    uint64_t space = 0, base = 0, size = 0, reset_vector = 0;
    int32_t priority = 0;
    uint32_t clock = 0;
    std::string config_json = "{}";
    std::vector<std::filesystem::path> image_paths;
    std::filesystem::path plugin_directory;
};
struct ProjectRuntimeRequest {
    enum class Kind { replace, insert, park, plug, put_away, reorder, card_metadata, clock, stop_clocks, property, capture, capture_state, restore_state };
    Kind kind = Kind::replace;
    uint64_t operation = 0, generation = 0, card = 0;
    nlohmann::json document;
    std::string execution;
    std::string name;
    std::filesystem::path directory;
    // For reordering/rebuilds, each candidate card names its source in the current rack.
    // The worker collects opaque data before replacement, in the same command.
    std::vector<uint64_t> source_cards;
    ProjectCardRequest insertion;
    uint32_t index = 0, hz = 0;
    SrhValue value{};
};
struct ProjectRuntimeResult {
    uint64_t operation = 0, expected_generation = 0, generation = 0, card = 0;
    SrhStatus status = SRH_OK;
    bool replaced = false;
    std::string error;
    nlohmann::json document;
    std::string execution;
    std::vector<ProjectCardAssociation> cards;
};
class ProjectRuntime {
  public:
    virtual ~ProjectRuntime() = default;
    virtual std::future<ProjectRuntimeResult> submit_project(ProjectRuntimeRequest request) = 0;
};
} // namespace srz80::ui
