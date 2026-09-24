#include "simulation_controller.hpp"
#include "simulation_engine.hpp"
#include "tool_services.hpp"
#include <algorithm>

namespace srz80::ui {
namespace {
uint64_t checked_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

// Adapts the stopped-only semantic loader to the engine ABI.  Every call runs
// on the simulation thread while dispatch is quiescent.
class EngineToolMemoryHost final : public ToolMemoryHost {
  public:
    EngineToolMemoryHost(SrzEngine *engine, PcmQueue &pcm) : engine_(engine), pcm_(pcm) {}

    bool space_maximum(SrhHandle space, uint64_t &maximum) const override {
        SrzResult *result = srz80_engine_result_create(engine_);
        if (!result)
            return false;
        bool found = false;
        if (srz80_engine_spaces(engine_, result) == SRH_OK) {
            uint32_t count = 0;
            const SrzSpace *list = srz80_engine_result_spaces(result, &count);
            for (uint32_t index = 0; index < count; ++index) {
                if (list[index].id != space)
                    continue;
                maximum = list[index].maximum;
                found = true;
                break;
            }
        }
        srz80_engine_result_destroy(engine_, result);
        return found;
    }
    bool stopped() const override { return srz80_engine_run_state(engine_) == SRZ_STOPPED; }
    void stop() override {
        srz80_engine_stop(engine_);
        pcm_.clear();
    }
    SrhStatus write(SrhHandle master, SrhHandle space, uint64_t address, uint8_t byte) override {
        return srz80_engine_write(engine_, master, space, address, byte);
    }

  private:
    SrzEngine *engine_;
    PcmQueue &pcm_;
};
} // namespace

void SimulationController::execute(const Properties &request, Reply &reply) {
    reply.properties.clear();
    if (srz80_engine_properties(engine_, request.card, result_) != SRH_OK) {
        reply.error = engine_message();
        return;
    }
    uint32_t count = 0;
    const SrzProperty *list = srz80_engine_result_properties(result_, &count);
    reply.properties.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto &property = list[index];
        UiProperty out;
        out.name = engine_text(property.name);
        out.group = engine_text(property.group);
        out.description = engine_text(property.description);
        out.enum_labels = engine_text(property.enum_labels);
        out.kind = property.kind;
        out.bits = property.bits;
        out.base = property.base;
        out.ui_flags = property.ui_flags;
        out.editable = property.editable != 0;
        out.value = property.value;
        reply.properties.push_back(std::move(out));
    }
}

void SimulationController::execute(const EditProperty &request, Reply &reply) {
    reply.status = srz80_engine_edit_property(engine_, request.card, request.index, &request.value);
    if (reply.status != SRH_OK)
        reply.error = "Register edit rejected";
}

void SimulationController::execute(const ReadMemory &request, Reply &reply) {
    const uint32_t length =
        std::min(request.length, 1024u * 1024);
    reply.memory.reserve(length);
    for (uint64_t offset = 0; offset < length; ++offset) {
        const uint64_t address = checked_add(request.base, offset);
        uint8_t value = 0;
        auto status = srz80_engine_read(engine_, 0, request.space, address, &value, 1);
        reply.memory.push_back({value, status});
    }
}

void SimulationController::execute(const WriteMemory &request, Reply &reply) {
    if (engine_running()) {
        reply.status = SRH_UNAVAILABLE;
        reply.error = "Pause the rack before editing memory";
        return;
    }
    uint64_t maximum = 0;
    if (!space_maximum(request.space, maximum) || request.base > maximum ||
        (!request.bytes.empty() && request.bytes.size() - 1 > maximum - request.base)) {
        reply.status = SRH_INVALID;
        return;
    }
    for (size_t index = 0; index < request.bytes.size(); ++index) {
        const uint64_t address = checked_add(request.base, index);
        auto status = srz80_engine_write(engine_, 0, request.space, address, request.bytes[index]);
        if (status != SRH_OK) {
            reply.status = status;
            reply.error = "Memory write rejected at address " + std::to_string(address);
            break;
        }
    }
    if (!request.bytes.empty())
        ++memory_revision_;
}

void SimulationController::execute(const LoadMemory &request, Reply &reply) {
    // Owned command bytes are adapted to ABI views only here, on the worker.
    EngineToolMemoryHost host{engine_, pcm_};
    std::vector<SrhToolMemorySegment> segments;
    segments.reserve(request.segments.size());
    for (const auto &segment : request.segments)
        segments.push_back({SRH_INIT(SrhToolMemorySegment), segment.address,
                             segment.bytes.data(), segment.bytes.size()});
    uint32_t failed = UINT32_MAX;
    uint64_t written = 0;
    reply.status = load_tool_segments(host, request.space, segments.data(),
        static_cast<uint32_t>(segments.size()), request.reset ? 1u : 0u, &failed, &written);
    reply.memory_load.written = written;
    reply.memory_load.failed_segment = failed;
    if (written)
        ++memory_revision_;
}

void SimulationController::execute(const DisassembleRange &request, Reply &reply) {
    SrzDisassemblyAvailability availability{};
    reply.status = srz80_engine_disassembly_availability(engine_, request.card, &availability);
    reply.disassembly_revision = availability.revision;
    if (reply.status != SRH_OK || availability.state != SRZ_DISASSEMBLY_ENABLED) {
        if (reply.status == SRH_OK) reply.status = SRH_UNAVAILABLE;
        reply.error = availability.message;
        return;
    }
    const uint32_t count = std::min(request.count, 65536u);
    uint64_t maximum = 0;
    if (!space_maximum(request.space, maximum)) {
        reply.status = SRH_INVALID;
        reply.error = "Disassembly address space does not exist";
        return;
    }

    auto decode_at = [&](uint64_t addr, DisasmLine &out) -> bool {
        out.address = addr;
        SrzDisassembly decoded{};
        const auto status =
            srz80_engine_disassemble(engine_, request.card, request.space, addr, &decoded);
        if (status == SRH_UNAVAILABLE)
            return false;
        out.ok = status == SRH_OK && decoded.ok != 0;
        out.instruction_bytes = decoded.instruction_bytes ? decoded.instruction_bytes : 1;
        if (decoded.byte_count)
            out.bytes.assign(decoded.bytes, decoded.bytes + decoded.byte_count);
        out.cycles = decoded.cycles;
        if (out.ok)
            out.text = decoded.text;
        return true;
    };

    std::vector<DisasmLine> backward_lines;
    if (request.backward_count > 0 && request.start > 0) {
        const uint32_t n = std::min(request.backward_count, 1024u);
        const uint64_t target = request.start;

        if (maximum < 65536 && target <= 65536) {
            uint64_t probe = 0;
            std::vector<DisasmLine> chain;
            while (probe < target && probe <= maximum) {
                DisasmLine line;
                if (!decode_at(probe, line))
                    break;
                const uint32_t step = line.instruction_bytes;
                chain.push_back(std::move(line));
                if (probe > UINT64_MAX - step)
                    break;
                probe += step;
            }
            if (probe == target && !chain.empty()) {
                const size_t take = std::min<size_t>(chain.size(), n);
                backward_lines.assign(std::make_move_iterator(chain.end() - take),
                                      std::make_move_iterator(chain.end()));
            }
        } else {
            const uint64_t search_distance = std::min<uint64_t>(
                target, std::max<uint64_t>(64, static_cast<uint64_t>(n) * 4 + 16));
            const uint64_t base_probe = target - search_distance;
            bool synchronized = false;
            for (uint64_t offset = 0; offset < 16 && base_probe + offset < target; ++offset) {
                uint64_t probe = base_probe + offset;
                std::vector<DisasmLine> chain;
                while (probe < target && chain.size() < n * 3) {
                    DisasmLine line;
                    if (!decode_at(probe, line))
                        break;
                    const uint32_t step = line.instruction_bytes;
                    chain.push_back(std::move(line));
                    if (probe > UINT64_MAX - step)
                        break;
                    probe += step;
                }
                if (probe == target && !chain.empty()) {
                    const size_t take = std::min<size_t>(chain.size(), n);
                    backward_lines.assign(std::make_move_iterator(chain.end() - take),
                                          std::make_move_iterator(chain.end()));
                    synchronized = true;
                    break;
                }
            }
            if (!synchronized) {
                uint64_t step_estimate = 4;
                uint64_t probe = target > step_estimate * n ? target - step_estimate * n : 0;
                std::vector<DisasmLine> chain;
                while (probe < target) {
                    DisasmLine line;
                    if (!decode_at(probe, line))
                        break;
                    const uint32_t step = line.instruction_bytes;
                    chain.push_back(std::move(line));
                    if (probe > UINT64_MAX - step)
                        break;
                    probe += step;
                }
                if (!chain.empty()) {
                    const size_t take = std::min<size_t>(chain.size(), n);
                    backward_lines.assign(std::make_move_iterator(chain.end() - take),
                                          std::make_move_iterator(chain.end()));
                }
            }
        }
    }

    std::vector<DisasmLine> forward_lines;
    uint64_t address = request.start;
    for (uint32_t row = 0; row < count && address <= maximum; ++row) {
        DisasmLine line;
        if (!decode_at(address, line)) {
            reply.status = SRH_UNAVAILABLE;
            reply.disasm.clear();
            return;
        }
        const uint32_t step = line.instruction_bytes;
        forward_lines.push_back(std::move(line));
        if (address > UINT64_MAX - step)
            break;
        address += step;
    }

    reply.disasm.clear();
    reply.disasm.reserve(backward_lines.size() + forward_lines.size());
    reply.disasm.insert(reply.disasm.end(),
                        std::make_move_iterator(backward_lines.begin()),
                        std::make_move_iterator(backward_lines.end()));
    reply.disasm.insert(reply.disasm.end(),
                        std::make_move_iterator(forward_lines.begin()),
                        std::make_move_iterator(forward_lines.end()));

    if (srz80_engine_disassembly_availability(engine_, request.card, &availability) != SRH_OK ||
        availability.state != SRZ_DISASSEMBLY_ENABLED || availability.revision != reply.disassembly_revision) {
        reply.disasm.clear();
        reply.status = SRH_UNAVAILABLE;
    }
}

void SimulationController::execute(const TextQuery &request, Reply &reply) {
    uint64_t offset = 0;
    while (reply.bytes.size() < 16 * 1024 * 1024) {
        std::array<uint8_t, 256> chunk{};
        uint32_t size = static_cast<uint32_t>(chunk.size());
        uint32_t total = 0;
        auto status = srz80_engine_text_query(engine_, request.card, offset, chunk.data(), &size, &total);
        if (status != SRH_OK) {
            reply.status = status;
            reply.error = engine_message();
            break;
        }
        if (!size)
            break;
        reply.bytes.insert(reply.bytes.end(), chunk.begin(), chunk.begin() + size);
        offset += size;
        if (offset >= total || total == 0)
            break;
    }
}

void SimulationController::execute(const CardTypes &, Reply &reply) {
    // Descriptor discovery belongs to the engine: it owns plugin loading and
    // the card ABI.  Every string is copied into owned UI values here, so no
    // panel holds a plugin library or a borrowed descriptor afterwards.
    reply.card_types.clear();
    const auto directory = engine_path_text(plugin_directory_);
    if (srz80_engine_discover_plugins(engine_, engine_slice(directory), result_) != SRH_OK) {
        reply.status = SRH_ERROR;
        reply.error = engine_message();
        return;
    }
    uint32_t path_count = 0;
    const char *const *paths = srz80_engine_result_paths(result_, &path_count);
    // Copy the paths first: inspecting a plugin fills its own result slot but
    // must never invalidate the enumeration this loop is walking.
    std::vector<std::string> candidates;
    candidates.reserve(path_count);
    for (uint32_t index = 0; index < path_count; ++index)
        candidates.push_back(engine_text(paths[index]));
    for (const auto &path : candidates) {
        if (path.empty())
            continue;
        // One unreadable or malformed plugin must not hide the rest.
        if (srz80_engine_inspect_plugin(engine_, engine_slice(path), result_) != SRH_OK)
            continue;
        const SrzPluginDescriptor *descriptor = srz80_engine_result_plugin_descriptor(result_);
        if (!descriptor || !descriptor->id || !*descriptor->id)
            continue;
        const std::string id = engine_text(descriptor->id);
        if (std::any_of(reply.card_types.begin(), reply.card_types.end(),
                        [&](const auto &existing) { return existing.id == id; }))
            continue;
        UiCardType type;
        type.id = id;
        const std::string name = engine_text(descriptor->name);
        type.name = name.empty() ? id : name;
        type.category = engine_text(descriptor->category);
        if (type.category.empty())
            type.category = "Misc";
        type.description = engine_text(descriptor->description);
        type.config_json = engine_text(descriptor->default_config_json);
        if (type.config_json.empty())
            type.config_json = "{}";
        type.io_space_config_key = engine_text(descriptor->io_space_config_key);
        type.base_config_key = engine_text(descriptor->base_config_key);
        type.default_base = descriptor->default_base;
        type.default_size = descriptor->default_size;
        type.default_reset_vector = descriptor->default_reset_vector;
        type.default_priority = descriptor->default_priority;
        type.default_clock = descriptor->default_clock;
        type.flags = descriptor->flags;
        uint32_t slot_count = 0;
        const char *const *slots = srz80_engine_result_image_slots(result_, &slot_count);
        for (uint32_t slot = 0; slot < descriptor->image_slot_count; ++slot) {
            const uint32_t index = descriptor->image_slot_offset + slot;
            if (!slots || index >= slot_count)
                break;
            type.image_slots.push_back(engine_text(slots[index]));
        }
        reply.card_types.push_back(std::move(type));
    }
    std::stable_sort(reply.card_types.begin(), reply.card_types.end(),
                     [](const UiCardType &left, const UiCardType &right) {
                         if (left.category != right.category)
                             return left.category < right.category;
                         if (left.name != right.name)
                             return left.name < right.name;
                         return left.id < right.id;
                     });
}

} // namespace srz80::ui
