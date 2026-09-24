#include "gui.hpp"
#include <algorithm>
#include <iterator>

namespace srz80::ui {
namespace {
// Offset at which the last `max_lines` lines of `text` begin.  A line ends at
// '\n', '\r' or a "\r\n" pair, and a trailing break yields an empty final line,
// matching how the console splits the transcript.  Returns 0 when the whole
// text fits.
size_t retained_text_offset(const std::vector<uint8_t> &text, int max_lines) {
    size_t remaining = static_cast<size_t>(std::max(max_lines, 0));
    size_t i = text.size();
    while (remaining > 0 && i > 0) {
        const uint8_t byte = text[i - 1];
        if (byte == '\n') {
            const size_t after = i;
            --i;
            if (i > 0 && text[i - 1] == '\r')
                --i;
            --remaining;
            if (remaining == 0)
                return after;
        } else if (byte == '\r') {
            const size_t after = i;
            --i;
            --remaining;
            if (remaining == 0)
                return after;
        } else {
            --i;
        }
    }
    return 0;
}
} // namespace

void App::refresh_memory_cache() {
    if (!snapshot)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (memory_request.valid()) {
        if (memory_request.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto reply = memory_request.get();
        if (reply.status == SRH_OK && reply.generation == snapshot->generation &&
            memory_cache_space == space && memory_cache_base == memory_base &&
            memory_cache_length == memory_length && reply.memory_revision == controller.memory_revision()) {
            memory_cache = std::move(reply.memory);
            memory_cache_time = now;
        }
    }
    const bool same = memory_cache_space == space && memory_cache_base == memory_base &&
                      memory_cache_length == memory_length && memory_request_generation == snapshot->generation;
    if (same && now - memory_cache_time < std::chrono::milliseconds(snapshot_interval_ms()))
        return;
    if (!same)
        memory_cache.clear();
    memory_cache_space = space;
    memory_cache_base = memory_base;
    memory_cache_length = memory_length;
    memory_request_generation = snapshot->generation;
    memory_request = controller.read_memory_async(space, memory_base,
        static_cast<uint32_t>(std::max(memory_length, 0)), snapshot->generation);
}

void App::refresh_console_text() {
    const auto now = std::chrono::steady_clock::now();
    if (console_request.valid()) {
        if (console_request.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto reply = console_request.get();
        if (reply.status == SRH_OK && reply.generation == snapshot->generation &&
            console_request_card == console_text_card) {
            console_text_cache = std::move(reply.bytes);
            const size_t retained = retained_text_offset(console_text_cache, console_max_lines);
            if (retained > 0)
                console_text_cache.erase(
                    console_text_cache.begin(),
                    console_text_cache.begin() +
                        static_cast<std::vector<uint8_t>::difference_type>(retained));
            console_text_time = now;
            console_text_dirty = false;
        }
    }
    if (!console_text_dirty && console_request_generation == snapshot->generation &&
        now - console_text_time < std::chrono::milliseconds(snapshot_interval_ms()))
        return;
    console_request_card = console_text_card;
    console_request_generation = snapshot->generation;
    console_request = controller.read_text_async(console_text_card, snapshot->generation);
}

void App::queue_disassembly(srz80::Handle card, srz80::Handle memory, uint64_t start,
                             uint32_t count, DisasmRequestKind kind, bool anchor_known,
                             uint32_t backward_count) {
    const auto availability = snapshot->disassembly.find(card);
    if (availability == snapshot->disassembly.end() || availability->second.state != SRZ_DISASSEMBLY_ENABLED)
        return;
    if (disasm_request.valid())
        return;
    disasm_request_availability_revision = availability->second.revision;
    disasm_request_kind = kind;
    disasm_request_append = (kind == DisasmRequestKind::Append);
    disasm_request_card = card;
    disasm_request_space = memory;
    disasm_request_start = start;
    disasm_request_view = disasm_start;
    disasm_request_anchor_known = anchor_known;
    disasm_request_generation = snapshot->generation;
    disasm_request_revision = controller.memory_revision();
    disasm_request = controller.disassemble_async(card, memory, start, count, snapshot->generation, backward_count);
}

void App::poll_disassembly(srz80::Handle memory) {
    if (!disasm_request.valid() ||
        disasm_request.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    auto reply = disasm_request.get();
    const auto availability = snapshot->disassembly.find(selected);
    if (availability == snapshot->disassembly.end() || !availability->second.accepts(disasm_request_availability_revision, reply.disassembly_revision))
        return;
    if (reply.status != SRH_OK || reply.generation != snapshot->generation ||
        disasm_request_generation != snapshot->generation || disasm_request_card != selected ||
        disasm_request_space != memory || disasm_request_revision != controller.memory_revision())
        return;
    if (disasm_request_kind == DisasmRequestKind::Append) {
        if (!disasm_cache.empty() && !reply.disasm.empty()) {
            const auto last_addr = disasm_cache.back().address;
            for (auto &line : reply.disasm) {
                if (line.address > last_addr) {
                    disasm_cache.push_back(std::move(line));
                }
            }
        }
    } else if (disasm_request_kind == DisasmRequestKind::Prepend) {
        if (!disasm_cache.empty() && !reply.disasm.empty()) {
            const auto first_addr = disasm_cache.front().address;
            std::vector<DisasmLine> prepended;
            for (auto &line : reply.disasm) {
                if (line.address < first_addr) {
                    prepended.push_back(std::move(line));
                }
            }
            if (!prepended.empty()) {
                const size_t count = prepended.size();
                disasm_cache.insert(disasm_cache.begin(),
                                    std::make_move_iterator(prepended.begin()),
                                    std::make_move_iterator(prepended.end()));
                disasm_view_row += count;
            }
        }
    } else {
        if (!reply.disasm.empty()) {
            disasm_cache = std::move(reply.disasm);
            disasm_anchor_known = disasm_request_anchor_known;
            const auto next = std::upper_bound(disasm_cache.begin(), disasm_cache.end(), disasm_request_start,
                                               [](uint64_t value, const DisasmLine &line) {
                                                   return value < line.address;
                                               });
            disasm_view_row = (next == disasm_cache.begin()) ? 0 : static_cast<size_t>(next - disasm_cache.begin() - 1);
            if (disasm_view_row < disasm_cache.size()) {
                disasm_start = disasm_cache[disasm_view_row].address;
            }
        }
    }
    disasm_running_refresh_at_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(snapshot_interval_ms());
}

void App::poll_clipboard() {
    if (!clipboard_request.valid() ||
        clipboard_request.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    auto reply = clipboard_request.get();
    if (reply.generation != snapshot->generation)
        return;
    if (reply.status != SRH_OK) {
        error = reply.error;
        return;
    }
    std::string text;
    if (clipboard_is_memory) {
        if (reply.memory.size() != clipboard_size ||
            std::any_of(reply.memory.begin(), reply.memory.end(), [](const auto &cell) { return !cell.available(); })) {
            error = "Selection contains unavailable memory";
            return;
        }
        text.reserve(reply.memory.size() * 3);
        static constexpr char digits[] = "0123456789ABCDEF";
        for (const auto &cell : reply.memory) {
            if (!text.empty()) text.push_back(' ');
            text.push_back(digits[cell.value >> 4]);
            text.push_back(digits[cell.value & 15]);
        }
    } else {
        for (uint8_t byte : reply.bytes)
            text += escape_transcript_byte(byte);
    }
    ImGui::SetClipboardText(text.c_str());
    error.clear();
}
}
