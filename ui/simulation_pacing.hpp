#pragma once
#include <algorithm>
#include <cstdint>

namespace srz80::ui {
// Wall time still owed to this rack. Short execution slices preserve debt;
// prolonged overload is bounded so commands and live edits remain responsive.
class SimulationPacing {
  public:
    static constexpr uint64_t maximum_debt_ns = 20000000;
    uint64_t add_elapsed(uint64_t elapsed) {
        const auto accepted = std::min(elapsed, maximum_debt_ns - pending_);
        pending_ += accepted;
        return elapsed - accepted;
    }
    void advance(uint64_t simulated) { pending_ -= std::min(pending_, simulated); }
    void reset() { pending_ = 0; }
    uint64_t pending() const { return pending_; }
  private:
    uint64_t pending_ = 0;
};
}
