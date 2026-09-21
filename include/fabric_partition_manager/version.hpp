// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Version constants. include/fabric_partition_manager/version.hpp is the
// authoritative source of the product version; CMakeLists.txt fails
// configuration when the CMake project version disagrees with it.
#ifndef FABRIC_PARTITION_MANAGER_VERSION_HPP
#define FABRIC_PARTITION_MANAGER_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace fabric_partition_manager {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version_string = "1.0.0";
inline constexpr std::string_view product_name = "Fabric Partition Manager";

// Durable store format version. Bumped whenever the on-disk record or snapshot
// encoding changes in a way older or newer builds cannot interpret.
inline constexpr std::uint32_t durable_format_version = 1;

// Framed wire protocol version.
inline constexpr std::uint16_t wire_protocol_version = 1;

// Policy schema version carried inside every durable policy definition.
inline constexpr std::uint16_t policy_schema_version = 1;

// The version the linked library was built from. Equal to version_string for
// this build; exposed as a function so a consumer can bind to the compiled
// artifact rather than to a header constant.
[[nodiscard]] std::string_view linked_version() noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_VERSION_HPP
