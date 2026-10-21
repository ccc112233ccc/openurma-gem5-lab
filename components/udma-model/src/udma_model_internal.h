// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ubsim/udma_model.h"
#include "ubsim/udma_abi.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>


namespace ubsim::device::detail {
inline constexpr std::uint64_t kStateMagic = 0x3154504355444f55ULL; // OUDCPT1

template <typename T>
bool WritePod(std::ostream& output, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return output.good();
}

template <typename T>
bool ReadPod(std::istream& input, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return input.good();
}

template <typename Map>
bool WritePodMap(std::ostream& output, const Map& map)
{
    const std::uint64_t size = map.size();
    if (!WritePod(output, size)) return false;
    for (const auto& [key, value] : map)
        if (!WritePod(output, key) || !WritePod(output, value)) return false;
    return true;
}

template <typename Map>
bool ReadPodMap(std::istream& input, Map& map)
{
    std::uint64_t size{};
    if (!ReadPod(input, size) || size > (1ULL << 24)) return false;
    map.clear();
    for (std::uint64_t i = 0; i < size; ++i) {
        typename Map::key_type key{};
        typename Map::mapped_type value{};
        if (!ReadPod(input, key) || !ReadPod(input, value)) return false;
        map.emplace(key, value);
    }
    return true;
}
inline constexpr std::uint8_t kOpcodeSend = 1;
inline constexpr std::uint32_t kStatusReady = 1U;
inline constexpr std::uint32_t kStatusSuccess = 0U;
inline constexpr std::uint32_t kStatusDmaError = 1U;
inline constexpr std::uint32_t kInterruptVector = 0U;

inline constexpr std::uint64_t kUbiosRootOffset = 0x10000;
inline constexpr std::uint64_t kUbiosUbcOffset = 0x11000;
inline constexpr std::uint64_t kUbiosUmmuOffset = 0x11c00;
inline constexpr std::uint64_t kUbiosMessageQueueOffset = 0x12000;
inline constexpr std::uint64_t kUbaseCommandSourceOffset = 0x00318004;
inline constexpr std::uint64_t kUbaseCommandQueueOffset = 0x00318400;
inline constexpr std::uint64_t kUmmuOffset = 0x00f00000;
inline constexpr std::uint64_t kUmmuBytes = 0x5000;
inline constexpr std::uint32_t kUmmuCr0 = 0x30;
inline constexpr std::uint32_t kUmmuCr0Ack = 0x34;
inline constexpr std::uint32_t kUmmuGbpa = 0x50;
inline constexpr std::uint32_t kUmmuCommandQueueOffset = 0x100;
inline constexpr std::uint32_t kUmmuCommandQueueStride = 0x10;
inline constexpr std::uint32_t kUmmuCommandQueueProducer = 0x8;
inline constexpr std::uint32_t kUmmuCommandQueueConsumer = 0xc;
inline constexpr std::uint32_t kUmmuCommandQueueEnable = 1U << 31;
inline constexpr std::uint32_t kUmmuReleasePermissionQueue = 0x1178;
inline constexpr std::uint32_t kUmmuReleaseCompletion = 1U;

template <typename T, typename Container>
void StoreLe(Container& bytes, std::size_t offset, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

template <typename T>
T LoadLe(const std::uint8_t* bytes)
{
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(bytes[i]) << (8 * i);
    return value;
}
} // namespace ubsim::device::detail
