#pragma once

#include <boundary.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

// Card snapshot framing. Project documents and core payloads do not carry this
// header. Change a card's payload version whenever its serialized layout changes.
namespace srz80::sdk::state {
inline constexpr std::array<uint8_t, 8> magic{'S', 'R', 'Z', '8', '0', 'S', 'T', 0};
inline constexpr uint16_t envelope_version = 1;
inline constexpr uint16_t header_size = 28;

template <class T> void put(uint8_t *out, T value) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        out[i] = static_cast<uint8_t>(bits >> (8 * i));
}
template <class T> T get(const uint8_t *in) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    using U = std::make_unsigned_t<T>;
    U bits = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        bits |= U(in[i]) << (8 * i);
    return std::bit_cast<T>(bits);
}

// FNV-1a detects accidental corruption; this is not an authentication code.
inline uint32_t checksum(std::span<const uint8_t> bytes) {
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

// Field archives encode integers little-endian, bool as 0/1, and IEEE floating
// point as their bit patterns. Enumerations must be encoded with an explicit
// fixed-width integer and range-checked by the payload decoder.
class Writer {
public:
    std::vector<uint8_t> bytes;
    template <class T> void operator()(const T &value) {
        if constexpr (std::is_same_v<T, bool>) {
            bytes.push_back(value ? 1 : 0);
        } else if constexpr (std::is_integral_v<T>) {
            const size_t offset = bytes.size();
            bytes.resize(offset + sizeof(T));
            put(bytes.data() + offset, value);
        } else if constexpr (std::is_floating_point_v<T>) {
            static_assert(std::numeric_limits<T>::is_iec559);
            if constexpr (sizeof(T) == 4) (*this)(std::bit_cast<uint32_t>(value));
            else { static_assert(sizeof(T) == 8); (*this)(std::bit_cast<uint64_t>(value)); }
        } else {
            for (const auto &element : value) (*this)(element);
        }
    }
    template <class... T> void fields(const T &...values) { ((*this)(values), ...); }
};

class Reader {
    std::span<const uint8_t> bytes_;
    size_t offset_ = 0;
    bool valid_ = true;
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool finished() const { return valid_ && offset_ == bytes_.size(); }
    bool valid() const { return valid_; }
    template <class T> void operator()(T &value) {
        if constexpr (std::is_arithmetic_v<T>) {
            if (!valid_ || sizeof(T) > bytes_.size() - offset_) { valid_ = false; return; }
            if constexpr (std::is_same_v<T, bool>) {
                const auto byte = bytes_[offset_];
                if (byte > 1) { valid_ = false; return; }
                value = byte != 0;
            } else if constexpr (std::is_integral_v<T>) {
                value = get<T>(bytes_.data() + offset_);
            } else {
                static_assert(std::numeric_limits<T>::is_iec559);
                if constexpr (sizeof(T) == 4) value = std::bit_cast<T>(get<uint32_t>(bytes_.data() + offset_));
                else { static_assert(sizeof(T) == 8); value = std::bit_cast<T>(get<uint64_t>(bytes_.data() + offset_)); }
            }
            offset_ += sizeof(T);
        } else {
            for (auto &element : value) (*this)(element);
        }
    }
    template <class... T> void fields(T &...values) { ((*this)(values), ...); }
};

inline SrhStatus copy_payload(std::span<const uint8_t> payload, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    const auto capacity = buffer ? *size : 0;
    *size = payload.size();
    if (!buffer) return SRH_OK;
    if (capacity < payload.size()) return SRH_UNAVAILABLE;
    if (!payload.empty()) std::memcpy(buffer, payload.data(), payload.size());
    return SRH_OK;
}

// The payload callbacks use the ordinary ABI buffer contract, but are private
// implementation functions. Only these framed callbacks go in SrhPlugin.
// LoadPayload must validate into temporary state before committing to the card.
template <SrhSaveState SavePayload, SrhLoadState LoadPayload, uint32_t PayloadVersion = 1>
struct Callbacks {
    static SrhStatus SRH_CALL save(void *context, uint8_t *buffer, uint64_t *size) noexcept {
        return guard([&]() -> SrhStatus {
            if (!context || !size) return SRH_INVALID;
            uint64_t payload_size = 0;
            const auto status = SavePayload(context, nullptr, &payload_size);
            if (status != SRH_OK) return status;
            if (payload_size > std::numeric_limits<size_t>::max() - header_size ||
                payload_size > UINT64_MAX - header_size) return SRH_ERROR;
            const uint64_t required = header_size + payload_size;
            const uint64_t capacity = buffer ? *size : 0;
            *size = required;
            if (!buffer) return SRH_OK;
            if (capacity < required) return SRH_UNAVAILABLE;
            uint64_t written = payload_size;
            const auto saved = SavePayload(context, buffer + header_size, &written);
            if (saved != SRH_OK) return saved;
            if (written != payload_size) return SRH_ERROR;
            std::copy(magic.begin(), magic.end(), buffer);
            put(buffer + 8, envelope_version);
            put(buffer + 10, header_size);
            put(buffer + 12, PayloadVersion);
            put(buffer + 16, payload_size);
            put(buffer + 24, checksum({buffer + header_size, static_cast<size_t>(payload_size)}));
            return SRH_OK;
        });
    }
    static SrhStatus SRH_CALL load(void *context, const uint8_t *buffer, uint64_t size) noexcept {
        return guard([&]() -> SrhStatus {
            if (!context || !buffer || size < header_size || size > std::numeric_limits<size_t>::max())
                return SRH_INVALID;
            if (!std::equal(magic.begin(), magic.end(), buffer) ||
                get<uint16_t>(buffer + 8) != envelope_version ||
                get<uint16_t>(buffer + 10) != header_size ||
                get<uint32_t>(buffer + 12) != PayloadVersion ||
                get<uint64_t>(buffer + 16) != size - header_size)
                return SRH_INVALID;
            const std::span<const uint8_t> payload(buffer + header_size, static_cast<size_t>(size - header_size));
            if (get<uint32_t>(buffer + 24) != checksum(payload)) return SRH_INVALID;
            return LoadPayload(context, payload.data(), payload.size());
        });
    }
};
} // namespace srz80::sdk::state
