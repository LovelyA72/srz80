#pragma once
#include "ui_snapshot.hpp"

namespace srz80::ui {
struct TraceFilter {
    int operation = 0, kind = 0;
    Handle master = 0, space = 0;
    bool group = false;
    bool operator==(const TraceFilter &) const = default;
};
struct TraceRow { size_t record; bool separator; };

// Only row indices are rebuilt. The retained immutable publication owns the
// records, including responder lists, and remains valid across worker updates.
class TraceView {
  public:
    void update(std::shared_ptr<const UiInspectionSnapshot> source, TraceFilter filter) {
        if (source_ == source && filter_ == filter)
            return;
        source_ = std::move(source);
        filter_ = filter;
        rows_.clear();
        rows_.reserve(source_->trace.size() * (filter.group ? 2 : 1));
        uint64_t last_instruction = UINT64_MAX;
        for (size_t i = 0; i < source_->trace.size(); ++i) {
            const auto &t = source_->trace[i];
            if ((filter.operation && t.operation != static_cast<uint32_t>(filter.operation)) ||
                (filter.kind && t.kind != static_cast<uint32_t>(filter.kind)) ||
                (filter.master && t.master != filter.master) || (filter.space && t.space != filter.space))
                continue;
            if (filter.group && t.instruction && last_instruction != UINT64_MAX && t.instruction != last_instruction)
                rows_.push_back({i, true});
            if (t.instruction)
                last_instruction = t.instruction;
            rows_.push_back({i, false});
        }
    }
    const auto &rows() const { return rows_; }
    const auto &records() const { return source_->trace; }

  private:
    std::shared_ptr<const UiInspectionSnapshot> source_;
    TraceFilter filter_;
    std::vector<TraceRow> rows_;
};
}
