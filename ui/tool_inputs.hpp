#pragma once
#include "simulation_controller.hpp"
#include <srz80/tool.h>
#include <boundary.hpp>
#include <map>

namespace srz80::ui {
// GUI-owned request ledger. Released but unresolved requests still count toward
// backpressure; a released future is never interpreted as worker cancellation.
class ToolInputs {
    struct Request {
        void *client;
        size_t bytes;
        SimulationController::AsyncReply future;
        bool released = false;
        std::optional<SrhStatus> result;
    };
    std::map<SrhHandle, Request> requests_;
    SrhHandle next_ = 1, next_source_ = 1;
    std::map<void *, uint64_t> sources_;
public:
    void collect() {
        for (auto it = requests_.begin(); it != requests_.end();) {
            auto &r = it->second;
            if (!r.result && r.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                r.result = r.future.get().status;
            if (r.released && r.result) it = requests_.erase(it);
            else ++it;
        }
    }
    SrhStatus submit(SimulationController &controller, void *client, uint64_t generation, uint64_t identity, SrhHandle owner,
                     const char *endpoint, uint64_t time, const uint8_t *bytes, uint64_t size, SrhHandle *out) {
        if (!client || !generation || !owner || !endpoint || !*endpoint || !bytes || !size || size > 65536 || !out)
            return SRH_INVALID;
        std::string name(endpoint);
        if (name.size() > 127) return SRH_INVALID;
        collect();
        size_t count = 0, queued = 0;
        for (const auto &[id, r] : requests_) {
            (void)id;
            if (r.bytes) ++count;
            queued += r.bytes;
        }
        if (count >= 64 || queued + size > 262144 || requests_.size() >= 96 || next_ == UINT64_MAX)
            return SRH_UNAVAILABLE;
        auto id = next_++;
        auto [it, inserted] = requests_.emplace(id, Request{client, static_cast<size_t>(size), {}, false, {}});
        (void)inserted;
        try {
            it->second.future = controller.input_batch_async(std::move(name), time,
                std::vector<uint8_t>(bytes, bytes + size), generation, source(client), identity, owner);
        } catch (...) { requests_.erase(it); throw; }
        *out = id;
        return SRH_OK;
    }
    uint64_t source(void *client) {
        if (auto it = sources_.find(client); it != sources_.end()) return it->second;
        if (sources_.size() >= 256) throw std::runtime_error("Too many input clients");
        return sources_.emplace(client, next_source_++).first->second;
    }
    SrhStatus cancel(SimulationController &controller, void *client, uint64_t generation,
                     uint64_t identity, SrhHandle *out) {
        if (!client || !generation || !out || !identity) return SRH_INVALID;
        collect();
        if (requests_.size() >= 96 || next_ == UINT64_MAX) return SRH_UNAVAILABLE;
        auto id = next_++;
        auto future = controller.input_cancel_async(source(client), identity, generation);
        requests_.emplace(id, Request{client, 0, std::move(future), false, {}});
        *out = id; return SRH_OK;
    }
    SrhStatus poll(void *client, SrhHandle id, SrhToolInputResult *out) {
        if (!sdk::valid(out)) return SRH_INVALID;
        collect();
        auto it = requests_.find(id);
        if (it == requests_.end() || it->second.client != client || it->second.released) return SRH_NOT_FOUND;
        out->pending = !it->second.result;
        out->status = it->second.result.value_or(SRH_UNAVAILABLE);
        return SRH_OK;
    }
    SrhStatus release(void *client, SrhHandle id) {
        auto it = requests_.find(id);
        if (it == requests_.end() || it->second.client != client) return SRH_NOT_FOUND;
        it->second.released = true;
        collect();
        return SRH_OK;
    }
};
}
