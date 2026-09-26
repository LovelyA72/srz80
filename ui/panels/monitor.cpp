#include "../gui.hpp"
#include <algorithm>
#include <string>

namespace srz80::ui {

const char *App::kind_name(uint32_t kind) {
    static const char *names[]{"", "OPCODE", "OPERAND", "DATA READ", "DATA WRITE",
                               "IO READ", "IO WRITE", "INT ACK"};
    return kind < 8 ? names[kind] : "?";
}

void App::monitor() {
    ImGui::Begin("Bus monitor");
    if (!snapshot) {
        ImGui::End();
        return;
    }
    const char *ops[]{"All", "Read", "Write"};
    const char *kinds[]{"All", "Opcode", "Operand", "Data read", "Data write",
                        "I/O read", "I/O write", "Interrupt ack"};
    ImGui::Combo("Operation", &filter_op, ops, 3);
    ImGui::SameLine();
    ImGui::Combo("Kind", &filter_kind, kinds, 8);
    ImGui::InputScalar("Master (0 = all)", ImGuiDataType_U64, &filter_master);
    ImGui::SameLine();
    ImGui::InputScalar("Space (0 = all)", ImGuiDataType_U64, &filter_space);
    ImGui::Checkbox("Group by instruction", &group_trace);
    if (ImGui::Checkbox("Record transactions", &record_trace))
        controller.set_trace_capture(record_trace,
                                     filter_op ? static_cast<uint32_t>(filter_op)
                                               : SRH_READ | SRH_WRITE);
    if (!record_trace)
        ImGui::TextUnformatted("Recording is off; enable it to capture bus transactions.");
    if (ImGui::Button("Clear"))
        controller.clear_trace();
    ImGui::SameLine();
    ImGui::Text("Dropped: %llu", static_cast<unsigned long long>(snapshot->inspection->dropped));
    monitor_view.update(snapshot->inspection, {filter_op, filter_kind, filter_master, filter_space, group_trace});
    const auto &records = monitor_view.records();
    const auto &rows = monitor_view.rows();
    if (ImGui::BeginTable("trace", 10,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        for (const char *name : {"Sequence / parent", "Time ns", "Master 0 tick", "Master", "Space", "Op",
                                 "Kind", "Address", "Data / status", "Responders"})
            ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();
        // The clipper needs one item for every table row.  Instruction
        // separators add rows, so clip a row map rather than the trace list.
        // otherwise its calculated range jumps as separator rows enter and
        // leave view (most visible with a write-only filter).
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto &row = rows[static_cast<size_t>(i)];
                if (row.separator) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::SeparatorText("next instruction");
                    continue;
                }
                const auto &t = records[row.record];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu / %llu", static_cast<unsigned long long>(t.sequence),
                            static_cast<unsigned long long>(t.parent));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(t.time));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(t.ticks[0]));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(t.master));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(t.space));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.operation == SRH_READ ? "READ" : "WRITE");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kind_name(t.kind));
                ImGui::TableNextColumn();
                ImGui::Text("%016llX", static_cast<unsigned long long>(t.address));
                ImGui::TableNextColumn();
                ImGui::Text("%02X / %d", t.value, t.result);
                ImGui::TableNextColumn();
                std::string targets;
                for (auto id : t.responders) {
                    if (!targets.empty())
                        targets += ", ";
                    targets += std::to_string(id);
                }
                ImGui::TextUnformatted(targets.empty() ? "unclaimed" : targets.c_str());
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace srz80::ui
