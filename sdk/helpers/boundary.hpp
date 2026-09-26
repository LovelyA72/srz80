#pragma once
#include <cstddef>
#include <srz80/abi.h>
#include <utility>
namespace srz80::sdk {
template <class F> SrhStatus guard(F &&fn) noexcept {
    try {
        return std::forward<F>(fn)();
    } catch (...) {
        return SRH_ERROR;
    }
}
template <class T> bool valid(const T *p) {
    return p && p->abi_version == SRH_ABI && p->struct_size >= sizeof(T);
}
// Image parts were appended after config_json. Existing hosts that provide the
// pre-image-parts SrhConfig remain valid. Consumers guard the new tail fields.
template <> inline bool valid<SrhConfig>(const SrhConfig *p) {
    return p && p->abi_version == SRH_ABI &&
           p->struct_size >= offsetof(SrhConfig, config_json_size) + sizeof(p->config_json_size);
}
// Image-slot metadata is an optional descriptor tail. Preserve descriptors
// produced by plugins built against the preceding ABI-1 layout.
template <> inline bool valid<SrhCardDescriptor>(const SrhCardDescriptor *p) {
    return p && p->abi_version == SRH_ABI &&
           p->struct_size >= offsetof(SrhCardDescriptor, base_config_key) + sizeof(p->base_config_key);
}
// The original input table ends at input_pop. New consumers must separately
// guard optional subscription fields before reading them.
template <> inline bool valid<SrhHostInputV1>(const SrhHostInputV1 *p) {
    return p && p->abi_version == SRH_ABI &&
           p->struct_size >= offsetof(SrhHostInputV1, input_pop) + sizeof(p->input_pop);
}
// The video registration extension is optional. The original table ends at
// register_video and remains valid for plugins that do not opt into shaders.
template <> inline bool valid<SrhHostVideoV1>(const SrhHostVideoV1 *p) {
    return p && p->abi_version == SRH_ABI &&
           p->struct_size >= offsetof(SrhHostVideoV1, register_video) + sizeof(p->register_video);
}
// The original card function table ends at property_set. All later fields
// are optional and must be guarded independently, including project chunks.
template <> inline bool valid<SrhPlugin>(const SrhPlugin *p) {
    return p && p->abi_version == SRH_ABI &&
           p->struct_size >= offsetof(SrhPlugin, property_set) + sizeof(p->property_set);
}
// Optional tail-field accessor: only read a field when the supplied struct is
// large enough to contain it.  Never decide based on abi_version alone.
template <class T, class M> bool has_field(const T *p, M T::*member) {
    if (!p)
        return false;
    const auto *base = reinterpret_cast<const char *>(p);
    const auto *field = reinterpret_cast<const char *>(&(p->*member));
    return p->struct_size >= static_cast<uint32_t>(field - base) + sizeof(M);
}
} // namespace srz80::sdk
