#pragma once
#include <boundary.hpp>
#include <srz80/tool.h>
#include <cstdint>
#include <limits>

namespace srz80::ui {
// Narrow host view the semantic memory loader needs.  It is deliberately free
// of ImGui, SDL and engine types so host policy stays directly testable.
class ToolMemoryHost {
  public:
    virtual ~ToolMemoryHost() = default;
    // Reports an address space's highest address, or false when it is unknown.
    virtual bool space_maximum(SrhHandle space, uint64_t &maximum) const = 0;
    virtual bool stopped() const = 0;
    virtual void stop() = 0;
    virtual SrhStatus write(SrhHandle master, SrhHandle space, uint64_t address, uint8_t byte) = 0;
};

inline SrhStatus load_tool_segments(ToolMemoryHost &host, SrhHandle space,
    const SrhToolMemorySegment *segments, uint32_t count, uint32_t reset,
    uint32_t *failed, uint64_t *written) noexcept {
    if (failed) *failed = UINT32_MAX;
    if (written) *written = 0;
    return sdk::guard([&]() -> SrhStatus {
        if (!failed || !written || !segments || !count || count > SIZE_MAX / sizeof(*segments) || reset > 1) return SRH_INVALID;
        uint64_t maximum = 0;
        if (!host.space_maximum(space, maximum)) return SRH_INVALID;
        uint64_t total = 0, previous_end = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto &s = segments[i];
            if (!sdk::valid(&s) || !s.bytes || !s.size || s.size > SIZE_MAX ||
                s.address > maximum || s.size - 1 > maximum - s.address ||
                (i && s.address <= previous_end) || s.size > UINT64_MAX - total) {
                *failed = i; return SRH_INVALID;
            }
            previous_end = s.address + s.size - 1;
            total += s.size;
        }
        if (!host.stopped()) return SRH_UNAVAILABLE;
        if (reset) host.stop();
        for (uint32_t i = 0; i < count; ++i) {
            *failed = i;
            const auto &s = segments[i];
            for (uint64_t offset = 0; offset < s.size; ++offset) {
                auto status = host.write(0, space, s.address + offset, s.bytes[static_cast<size_t>(offset)]);
                if (status != SRH_OK) return status;
                ++*written;
            }
        }
        *failed = UINT32_MAX;
        return SRH_OK;
    });
}
}
