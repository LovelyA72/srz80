#pragma once
#include "simulation_controller.hpp"
#include <boundary.hpp>
#include <srz80/tool.h>
#include <map>

namespace srz80::ui {
// GUI-owned copied results, following the input request ledger's lifetime rules
class ToolMemoryReads {
    struct Request {
        void *client;
        uint32_t bytes;
        SimulationController::AsyncReply future;
        std::optional<SimulationController::Reply> result;
        bool released=false;
    };
    std::map<SrhHandle,Request> requests_;
    SrhHandle next_=1;
public:
    void collect() {
        for (auto it=requests_.begin(); it!=requests_.end();) {
            auto &r=it->second;
            if (!r.result && r.future.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                r.result=r.future.get();
            if (r.released && r.result) it=requests_.erase(it);
            else ++it;
        }
    }
    SrhStatus request(SimulationController &controller, void *client, uint64_t generation,
                      const SrhToolMemoryRange *ranges, uint32_t count, SrhHandle *out) {
        if (out) *out=0;
        if (!client || !generation || !ranges || !out || !count || count>SRH_TOOL_MEMORY_MAX_RANGES)
            return SRH_INVALID;
        if (generation!=controller.snapshot()->generation) return SRH_CONFLICT;
        uint32_t total=0;
        std::vector<SimulationController::MemoryRange> owned;
        for (uint32_t i=0; i<count; ++i) {
            const auto &r=ranges[i];
            if (!sdk::valid(&r) || !r.space || !r.size || r.size>SRH_TOOL_MEMORY_MAX_BYTES-total ||
                r.address>UINT64_MAX-(r.size-1) ||
                (i && (r.space<ranges[i-1].space || (r.space==ranges[i-1].space &&
                    (r.address<=ranges[i-1].address || r.address-ranges[i-1].address<ranges[i-1].size)))))
                return SRH_INVALID;
            total+=r.size;
            owned.push_back({r.space,r.address,r.size});
        }
        collect();
        unsigned client_count=0;
        for (const auto &[id,r]:requests_) { (void)id; if (r.client==client) ++client_count; }
        if (client_count>=2 || requests_.size()>=16 || next_==UINT64_MAX) return SRH_UNAVAILABLE;
        const auto id=next_++;
        auto [it,inserted]=requests_.emplace(id,Request{client,total,{},std::nullopt,false});
        (void)inserted;
        try { it->second.future=controller.read_memory_batch_async(std::move(owned),generation); }
        catch (...) { requests_.erase(it); throw; }
        *out=id;
        return SRH_OK;
    }
    SrhStatus poll(void *client, SrhHandle id, SrhToolMemoryResult *out,
                   uint8_t *bytes, SrhStatus *statuses, uint32_t capacity) {
        if (!sdk::valid(out) || ((!bytes || !statuses) && (bytes || statuses || capacity))) return SRH_INVALID;
        collect();
        const auto it=requests_.find(id);
        if (it==requests_.end() || it->second.client!=client || it->second.released) return SRH_NOT_FOUND;
        const auto &r=it->second;
        *out={SRH_INIT(SrhToolMemoryResult),uint32_t(!r.result),SRH_UNAVAILABLE,0,0,r.bytes};
        if (!r.result) return SRH_OK;
        out->status=r.result->status;
        out->generation=r.result->generation;
        out->time_ns=r.result->time_ns;
        out->size=uint32_t(r.result->memory.size());
        if (!bytes) return SRH_OK;
        if (capacity<out->size) return SRH_INVALID;
        for (uint32_t i=0; i<out->size; ++i) {
            bytes[i]=r.result->memory[i].value;
            statuses[i]=r.result->memory[i].status;
        }
        return SRH_OK;
    }
    SrhStatus release(void *client, SrhHandle id) {
        const auto it=requests_.find(id);
        if (it==requests_.end() || it->second.client!=client || it->second.released) return SRH_NOT_FOUND;
        it->second.released=true;
        collect();
        return SRH_OK;
    }
};
}
