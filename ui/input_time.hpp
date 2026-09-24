#pragma once
#include <cstdint>

namespace srz80::ui {
// Tool input batches carry either UINT64_MAX ("as soon as possible") or an
// absolute simulated timestamp. Only the simulation worker knows the true
// current time; a tool reads simulated time from the published snapshot, which
// always trails a running rack. A batch anchored at that snapshot is therefore
// commonly already behind by the time it executes, so treating "behind worker
// time" as invalid rejects ordinary system input and dense file playback.
// A past timestamp is delivered now, exactly like UINT64_MAX; only a timestamp
// beyond the bounded forward window is invalid.
inline bool normalize_input_time(uint64_t requested, uint64_t now, uint64_t &time) {
    if (requested != UINT64_MAX && requested > now && requested - now > 1000000000ull)
        return false;
    time = requested == UINT64_MAX || requested < now ? now : requested;
    return true;
}
} // namespace srz80::ui
